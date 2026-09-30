# DxTimingCaptureLibrary

DxTimingCaptureLibrary is a consumer of Windows ETW (Event Tracing for Windows) events that's designed to make it easy for you to interpret DirectX ETW data in your applications.

The library decodes and correlates events from D3D12, the DirectX graphics kernel (DXGK), the PIX runtime, and DirectStorage, then delivers the results through straightforward callbacks. A game engine, profiler, or other D3D12 tool can use that data without parsing raw ETW payloads itself.

We created DxTimingCaptureLibrary by extracting code from PIX Timing Captures and moving it into a standalone project. All DirectX data shown in the PIX Timing Capture UI is available through DxTimingCaptureLibrary.

> **Note:** Standalone builds currently use a stub PIX event decoder
> (`lib/PixEventDecoderStub.cpp`, selected by `UseStubPixEventDecoder` in
> `Directory.Build.props`). With the stub, PIX runtime events (CPU/GPU markers,
> custom counters, and PIX memory events) are not decoded. All other providers
> are fully supported.

## What does the library give you?

- **GPU timing data**: top-of-pipe and bottom-of-pipe times for GPU work, command-list spans, and submission details.
- **GPU engine activity**: see other processes' GPU work and how it relates to your process's work.
- **Memory and residency events**: make-resident, evict, page-in, page-out, migration, segment changes - all attributed to specific D3D12 objects. Also see per-adapter memory budgets and usage.
- **D3D12 objects**: creation/destruction times, names, descriptions, placement, size, and GPU virtual addresses for resources, heaps, pipeline states, and other API objects.
- **PIX runtime data**: custom counters, memory events, and CPU/GPU markers.
- **DirectStorage**: files, queues, reads, status notifications, fence signals, event signals, and submissions.
- **D3D12 runtime failures**: detailed runtime error logs with timestamps, error codes, thread IDs, and messages.
- **Pipeline-state compilation**: start and end timing for PSO compilation.
- **Displays**: monitor information and VSync timestamps.
- **Diagnostics**: ETW data-loss notifications and recoverable processing problems.

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

The library handles events from Direct3D 12, the DirectX graphics kernel (LDDMCore/DXGK), the PIX event runtime, DirectStorage, and the ETW system-config provider. Events from anything else are ignored.

## Creating the handler

Add the callbacks you care about to `DxTimingCaptureEventCallbacks`, then pass the struct to `DxTimingCaptureEventHandler::Create()`. Every callback is optional; empty slots use no-op implementations. The handler does not own callback objects, so they must outlive it.

`DxTimingCaptureLibraryOptions` controls the more expensive tracking work. Set `TrackApiObjects`, `TrackGpuTiming`, and/or `TrackGpuEngineActivity` to match the data you want and the ETW provider keywords you enable.

The target process ID scopes API object, residency, and runtime-failure data. GPU timing can include multiple processes, while GPU engine activity is always machine-wide.

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

### ETW settings

Your ETW session must use raw QPC timestamps: set `PROCESS_TRACE_MODE_RAW_TIMESTAMP` on the trace and `Wnode.ClientContext = 1` on the session. Without this, the timing data returned by the library will be wrong. The [basic sample](samples/basic) shows the complete setup.

Provider GUIDs and keyword flags for session setup (`EnableTraceEx2`) are in `<DxTimingCaptureLibrary/EtwProviders.h>`. Include it where you configure the session; the handler itself doesn't need it.

Starting a real-time ETW session and enabling the DXGK provider requires administrator rights or membership in the Performance Log Users group.

## Using the library

The simplest option is to add `lib/DxTimingCaptureLibrary.lib.vcxproj` to your solution and reference it from your project. Otherwise, link `DxTimingCaptureLibrary.lib` (built to `x64\<Configuration>\lib\`; Debug uses the debug DLL CRT `/MDd`, Release uses `/MD`, so match your configuration) and add these include directories:

- `include/`: the public headers (`DxTimingCaptureLibrary/DxTimingCaptureLibrary.h` is the entry point)
- `third_party/PixEventDecoder/include/`: decoded PIX event types used in the public callbacks
- The D3D12 Agility SDK headers (`Microsoft.Direct3D.D3D12` NuGet package, `build/native/include`), for `d3d12.h` and `D3D12Events.h`

If you include `EtwProviders.h`, also add `third_party/dxgk/` and `third_party/directstorage/`.

To process a recorded `.etl` file instead of a live session, see the [ETL processor sample](samples/etlprocessor).

## Samples

### [Basic event logging](samples/basic)

This is the best place to start. It provides straightforward, `printf`-based implementations of the callback interfaces and shows the complete setup: creating a real-time ETW session, enabling the DirectX providers, feeding events to the library, handling errors, and reporting data loss.

Run it with an optional target process ID:

```text
DxTimingCaptureLibrary.sample.exe [processId]
```

### [D3D12 journal entries](samples/journal)

This sample implements the runtime-failure callback and deliberately records an invalid `CopyBufferRegion` call. The D3D12 runtime rejects the call and writes a journal entry, which the library decodes and prints. Journal entries provide more detail than the normal D3D12 API error alone, work in retail D3D12 environments, and do not require the D3D12 debug layer.

### [Perfetto](samples/perfetto)

This sample turns the library's GPU timing callbacks into a [Perfetto](https://perfetto.dev) trace file. The trace shows each command list's top-of-pipeline and end-of-pipeline timestamps on the GPU timeline and connects that work to the CPU-side marker where it was recorded. It can capture a live session or process an existing ETL file.

### [Memory map](samples/memorymap)

This sample listens for resource creation and residency events and uses them to build a live memory visualization. It shows resource and heap usage in system and video memory while accounting for paging, eviction, migration, placed resources, and aliasing.

## Building

### Prerequisites

- Windows 10 or 11, x64
- Visual Studio 2022 with the **Desktop development with C++** workload (v143 toolset)
- Windows SDK 10.0.26100.0
- NuGet access to nuget.org. The build restores `Microsoft.Direct3D.D3D12` 1.619.5 and `Microsoft.Direct3D.DirectStorage` 1.3.0.

Open [`DxTimingCaptureLibrary.sln`](DxTimingCaptureLibrary.sln) in Visual Studio and build the x64 Debug or Release configuration. The solution includes the `DxTimingCaptureLibrary` static library, samples, and tests. You can also build from a Developer Command Prompt:

```text
msbuild DxTimingCaptureLibrary.sln /restore /p:Configuration=Debug /p:Platform=x64
```

The main test projects live under [`test.unit/`](test.unit) and [`test.functional/`](test.functional). Samples with their own behavior keep their tests nearby; for example, see [`samples/perfetto/test.unit`](samples/perfetto/test.unit) and [`samples/perfetto/test.functional`](samples/perfetto/test.functional). Unit tests are deterministic and need no special hardware. Functional tests run against a live ETW session, so they need a D3D12-capable GPU and administrator rights, and the DirectStorage tests also need DirectStorage support.

## License

MIT — see [`LICENSE`](LICENSE). Third-party components are listed in [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).
