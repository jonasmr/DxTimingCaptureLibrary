# DxTimingCaptureLibrary

DxTimingCaptureLibrary is an in-process consumer of Windows ETW (Event Tracing for Windows)
events that decodes and correlates D3D12, DirectX graphics-kernel
(DXGK), and PIX-runtime events into useable callbacks.
A game engine (or any D3D12 app) can use it to understand its own runtime
behavior without having to parse raw ETW payloads itself.

You create an ETW session (the samples can help you!) and implement the callbacks. The library does the decoding and correlation, and hands you easy-to-use data through the callbacks.

## What you get

The library surfaces data including:

- **D3D12 API object descs/data** — devices, resources (committed / placed / reserved),
  heaps, descriptor heaps, command allocators, pipeline states, state objects,
  meta commands: creation, destruction, naming, and GPU virtual address/size.
- **GPU timing** — per-command-list GPU work spans correlated from DXGK history
  buffers and D3D12/PIX markers.
- **Memory & residency** — make-resident / evict / page-in / page-out, allocation
  migration, per-adapter memory budget/usage counters, and PIX custom memory
  events via WinPixEventRuntime APIs.
- **PIX runtime data** — custom counters and CPU/GPU PIX markers.
- **Monitors / vsync** — monitor enumeration and vsync timestamps.
- **Diagnostics** — data-loss totals and recoverable-problem reports.

## Architecture at a glance

```
your ETW session  ─►  ProcessTrace  ─►  EventRecordCallback
                                              │  (your code calls)
                                              ▼
                     DxTimingCaptureEventHandler::HandleEventRecord
                                              │
                                              ▼
                          EtwDispatcher  (per-provider decode)
                                              │
                                              ▼
                       DxTimingCaptureProcessor  (correlation)
                                              │
                                              ▼
                                   your callback interfaces
```

The library handles events from Direct3D 12, the DirectX graphics kernel
(LDDMCore/DXGK), the PIX event runtime, DirectStorage, and the ETW system-config
provider. Events from anything else are ignored.

## Creating the handler

Put the callbacks you care about in a `DxTimingCaptureEventCallbacks` struct and
call `DxTimingCaptureEventHandler::Create()`. Every callback is optional; the
library fills empty slots with no-op implementations. Callback objects are not
owned by the handler, so keep them alive until the handler is destroyed.

`DxTimingCaptureLibraryOptions` controls the more expensive tracking work. Set
`TrackApiObjects`, `TrackGpuTiming`, and/or `TrackGpuEngineActivity` to match the
data you want and the ETW provider keywords you enable. The target process ID
selects the process whose DirectX work is decoded.

For a live raw-QPC session, `Create()` supplies the timestamp converter for you.
The overload that accepts a `TimestampConverter` is mainly useful for offline
ETL processing or another clock domain. See
[`DxTimingCaptureEventHandler.h`](include/DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h)
for the full API.

## Quickstart

```cpp
#include <cstdio>

#include <DxTimingCaptureLibrary/DxTimingCaptureLibrary.h>
#include <DxTimingCaptureLibrary/NoOpCallbacks.h>

using namespace DirectX::Etw;

class MyGpuTimings : public NoOpGpuTimingsCallbacks
{
    // Override the GpuTimingsCallbacks methods you need.
};

MyGpuTimings gpuTimings;

DxTimingCaptureLibraryOptions options{};
options.TrackGpuTiming = true;

DxTimingCaptureEventCallbacks callbacks{};
callbacks.GpuTimingsCallbacks = &gpuTimings;

auto handler = DxTimingCaptureEventHandler::Create(
    targetProcessId,
    options,
    callbacks);

// From your ETW EventRecordCallback:
try
{
    handler->HandleEventRecord(record);
}
catch (...)
{
    // Do not let a C++ exception unwind through ProcessTrace's C frames.
    std::fputs("Dropped an event: HandleEventRecord threw.\n", stderr);
}

// After ProcessTrace returns:
handler->OnDataComplete();
```

### Required ETW session mode

The session **must** be opened in raw-QPC timestamp mode:
`PROCESS_TRACE_MODE_RAW_TIMESTAMP` on the trace and `Wnode.ClientContext = 1`
(QPC) on the session. In that mode `EventHeader.TimeStamp` is a QPC tick count,
which is what the default `QpcTimestampConverter` expects. Without it,
`ProcessTrace` delivers FILETIME-based timestamps and the tick→ns conversion
produces garbage. See `samples/basic/EtwConfig.h` and `samples/basic/main.cpp`.

## Samples

### [Basic event logging](samples/basic)

This is the best place to start. It provides straightforward, `printf`-based
implementations of the callback interfaces and shows the complete setup:
creating a real-time ETW session, enabling the DirectX providers, feeding events
to the library, handling errors, and reporting data loss.

Run it with an optional target process ID:

```text
DxTimingCaptureLibrary.sample.exe [processId]
```

### [Perfetto](samples/perfetto)

This sample turns the library's GPU timing callbacks into a
[Perfetto](https://perfetto.dev) trace file. The trace shows each command list's
top-of-pipeline and end-of-pipeline timestamps on the GPU timeline and connects
that work to the CPU-side marker where it was recorded. It can capture a live
session or process an existing ETL file.

### [Memory map](samples/memorymap)

This sample listens for resource creation and residency events and uses them to
build a live memory visualization. It shows resource and heap usage in system
and video memory while accounting for paging, eviction, migration, placed
resources, and aliasing.

## Data loss & diagnostics

Under a real-time session you can lose events. Feed the trace's logfile header to
`ReportTraceStatistics()` (typically from your ETW `BufferCallback` and once after
`ProcessTrace` returns). Newly observed loss is reported through the
`DiagnosticsSink`. The sink also reports recoverable correlation problems
(unmatched GPU markers, backwards clock calibration, dropped PSO compilations,
...). See [`EtwDiagnostics.h`](include/DxTimingCaptureLibrary/EtwDiagnostics.h)
for the full `DiagnosticCode` catalog. A loss diagnostic means some capture data
is missing.

## Building

Open [`DxTimingCaptureLibrary.sln`](DxTimingCaptureLibrary.sln) in Visual Studio
and build the x64 Debug or Release configuration. The solution includes the
`DxTimingCaptureLibrary` static library, samples, and tests.

The main test projects live under [`test.unit/`](test.unit) and
[`test.functional/`](test.functional). Samples with their own behavior keep
their tests nearby; for example, see
[`samples/perfetto/test.unit`](samples/perfetto/test.unit) and
[`samples/perfetto/test.functional`](samples/perfetto/test.functional). The
functional tests run against a live ETW session.

## License

MIT — see [`LICENSE`](LICENSE). Third-party components are listed in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).
