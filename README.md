# DxEtwLibrary

DxEtwLibrary is an in-process consumer of Windows ETW (Event Tracing for Windows)
events that decodes and **correlates** Direct3D 12, DirectX graphics-kernel
(DXGK), and PIX-runtime events into a usable model of an application's GPU work.
A game engine (or any D3D12 app) can use it to understand its own runtime
behavior — object lifetimes, GPU timing, memory residency, custom counters and
PIX markers — without having to parse raw ETW payloads itself.

You bring an ETW session and a set of callbacks; the library does the decoding
and correlation and hands you structured data through those callbacks.

## What you get

The library surfaces, through consumer-supplied callbacks:

- **D3D12 API objects** — devices, resources (committed / placed / reserved),
  heaps, descriptor heaps, command allocators, pipeline states, state objects,
  meta commands: creation, destruction, naming, and GPU virtual address/size.
- **GPU timing** — per-command-list GPU work spans correlated from DXGK history
  buffers and D3D12/PIX markers, plus API command-queue lifetimes.
- **Memory & residency** — make-resident / evict / page-in / page-out, allocation
  migration, per-adapter memory budget/usage counters, and PIX custom memory
  events.
- **PIX runtime data** — custom counters and CPU/GPU PIX markers.
- **Monitors / vsync** — monitor enumeration and vsync timestamps.
- **Diagnostics** — data-loss totals and recoverable-problem reports.

## Architecture at a glance

```
your ETW session  ─►  ProcessTrace  ─►  EventRecordCallback
                                              │  (your code calls)
                                              ▼
                              DxEtwEventHandler::HandleEventRecord
                                              │
                                              ▼
                          EtwDispatcher  (per-provider decode)
                                              │
                                              ▼
                              DxEtwProcessor  (correlation)
                                              │
                                              ▼
                                   your callback interfaces
```

The library handles events from four providers: Direct3D 12
(`Microsoft-Windows-DX`), the DirectX graphics kernel (LDDMCore/DXGK), the PIX
event runtime, and the ETW system-config provider. Events from anything else are
ignored.

## The construction contract

You construct a single `DxEtwEventHandler` (see
[`include/DxEtwLibrary/DxEtwEventHandler.h`](include/DxEtwLibrary/DxEtwEventHandler.h)),
supplying:

- **Eight callback interfaces** (all required, see
  [`EtwCallbacks.h`](include/DxEtwLibrary/EtwCallbacks.h)):
  `ApiObjectCallbacks`, `PixCounterCallbacks`, `ResidencyEventCallbacks`,
  `PixEventCallbacks`, `MonitorEventCallbacks`, `GpuTimingsCallbacks`,
  `DxgkObjectCallbacks`, `PipelineStateEventCallbacks`.
- A **`TimestampConverter`** (required) — converts the session's raw QPC tick
  timestamps to nanoseconds.
- A **`DiagnosticsSink`** (optional — pass `nullptr` for a no-op).
- A **`RuntimeFailureCallbacks`** (optional — pass `nullptr` to ignore) — surfaces
  the D3D12 runtime's failure journal entries (`EventD3D12JournalEntry`): the broad
  signal for "something failed inside the D3D12 runtime", such as a command list
  removed because a recording-time call (e.g. `CopyBufferRegion`) was rejected.
- A **target process id** — the process whose D3D12 *object* state (resources, heaps,
  descriptor heaps, residency) is tracked. It does **not** filter GPU timing:
  `GpuTimingsCallbacks` decodes command-list execution for *every* D3D12 process on
  the machine, which is why `OnGpuExecutionBegin` and `OnApiMarker` carry a
  `processId`. `OnGpuWork` does not, so a consumer that wants a single-process
  timeline must record `gpuExecutionId` → `processId` at `OnGpuExecutionBegin` and
  filter on it — see `PerfettoGpuTimingsCallbacks` in
  `samples/perfetto/PerfettoCallbacks.h`. PIX takes the other route: it persists
  every process's executions into the capture and filters to the target process in
  its UI.

**Why every callback is required:** the library mints ids in one callback (e.g. a
command-queue id from `DxgkObjectCallbacks`) and feeds them into another (e.g.
`GpuTimingsCallbacks`). A null callback would corrupt a *sibling's* data, not just
suppress its own, so the constructor rejects nulls with `E_INVALIDARG`.

You don't have to implement everything by hand: derive from the no-op bases in
[`NoOpCallbacks.h`](include/DxEtwLibrary/NoOpCallbacks.h) and override only the
methods you care about.

**Callback contract:** callbacks run synchronously on your ETW consumer thread
(the library holds no locks and assumes a single such thread). Return `S_OK`;
returning a failure `HRESULT` makes the library throw out of `HandleEventRecord`,
and it **propagates to you** (as does a decode failure on malformed/truncated
event data) — so wrap that call in a `try`/`catch` (it runs inside ETW's
`ProcessTrace`). See the header comment in `EtwCallbacks.h` for the full contract.

## Quickstart

```cpp
#include <DxEtwLibrary/DxEtwLibrary.h>
#include <DxEtwLibrary/NoOpCallbacks.h>

// 1. Implement the categories you care about; no-op the rest.
class MyApiObjects : public NoOpApiObjectCallbacks { /* override what you need */ };

// 2. A QPC -> nanoseconds converter (see the sample's TimestampConverterImpl).
class MyClock : public NoOpTimestampConverter { /* real conversion here */ };

DirectX::Etw::Options options{};
options.CaptureGpuMemoryUsage = true;

auto handler = std::make_unique<DxEtwEventHandler>(
    options,
    /*gpuOnlyEvents*/ true,
    std::make_unique<MyApiObjects>(),
    std::make_unique<NoOpPixCounterCallbacks>(),
    std::make_unique<NoOpResidencyEventCallbacks>(),
    std::make_unique<NoOpPixEventCallbacks>(),
    std::make_unique<NoOpMonitorEventCallbacks>(),
    std::make_unique<NoOpGpuTimingsCallbacks>(),
    std::make_unique<NoOpDxgkObjectCallbacks>(),
    std::make_unique<NoOpPipelineStateEventCallbacks>(),
    /*diagnosticsSink*/ nullptr,
    /*runtimeFailureCallbacks*/ nullptr,
    std::make_unique<MyClock>(),
    targetProcessId);

// 3. Drive an ETW real-time session and feed every record to the handler:
//      OpenTrace + ProcessTrace, with EventRecordCallback calling
//      handler->HandleEventRecord(record).
// 4. After ProcessTrace returns: handler->OnDataComplete();
```

A complete, runnable consumer — ETW session setup, provider enabling, the consume
loop, and a real QPC converter — is in [`samples/basic/`](samples/basic). Run it
with an optional target PID: `DxEtwLibrary.sample.exe [targetProcessId]`. A second
sample, [`samples/perfetto/`](samples/perfetto), writes the same events into a
[Perfetto](https://perfetto.dev) trace.

### Required ETW session mode

The session **must** be opened in raw-QPC timestamp mode:
`PROCESS_TRACE_MODE_RAW_TIMESTAMP` on the trace and `Wnode.ClientContext = 1`
(QPC) on the session. In that mode `EventHeader.TimeStamp` is a QPC tick count,
which is what `TimestampConverter::ConvertClockToTimeStamp` receives. Without it,
`ProcessTrace` delivers FILETIME-based timestamps and the tick→ns conversion
produces garbage. See `samples/basic/EtwConfig.h` and `samples/basic/main.cpp`.

## Data loss & diagnostics

Under a real-time session you can lose events. Feed the trace's logfile header to
`ReportTraceStatistics()` (typically from your ETW `BufferCallback` and once after
`ProcessTrace` returns); newly observed loss is reported through the
`DiagnosticsSink`, and the cumulative totals are available via
`GetLostEventStatistics()`. The sink also reports recoverable correlation problems
(unmatched GPU markers, backwards clock calibration, dropped PSO compilations,
...). See [`EtwDiagnostics.h`](include/DxEtwLibrary/EtwDiagnostics.h) for the full
`DiagnosticCode` catalog. Non-zero loss means some of the data below is missing.

## Building

In-tree (within the PIX repo) the library builds with the rest of the solution as
the `DxEtwLibrary` static library; the samples and tests are separate projects
under [`samples/`](samples), [`test.unit/`](test.unit), and
[`test.functional/`](test.functional). A sample with behaviour of its own carries
its own tests next to it - see
[`samples/perfetto/test.unit`](samples/perfetto/test.unit) and
[`samples/perfetto/test.functional`](samples/perfetto/test.functional), which
respectively drive the sample's classes directly and run the built sample against
a live ETW session. The functional ones need administrator rights to start a
session, and skip rather than fail without them.

> **Standalone build status:** the public headers currently reach a few Windows
> SDK/WDK and D3D event headers through the surrounding PIX source tree
> (`<external/wdk/...>`, `<external/d3d/...>`) and the build is MSBuild-only. A
> self-contained build (vendored/SDK headers + a CMake/vcpkg target) is still in
> progress; see `THIRD-PARTY-NOTICES.md`. Until then, building outside the PIX
> tree requires reproducing those include roots.

The standalone exception path is selected by *not* defining `UsePixUtility`; in
that configuration the library throws a self-contained
`DirectX::Etw::EtwException` (derived from `std::exception`) and has no dependency
on PIX-internal utilities.

## License

MIT — see [`LICENSE`](LICENSE). Third-party components are listed in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).
