// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <cstdlib> // strtoul

#include <DxEtwLibrary/DxEtwLibrary.h>

#include "EtwConfig.h"
#include "PrintfCallbacks.h"

static std::wstring g_etwSessionName;
static std::atomic<bool> g_running(true);
static TRACEHANDLE g_sessionHandle = 0;

std::unique_ptr<DxEtwEventHandler> g_dxEtwEventHandler;

void WINAPI OnEvent(EVENT_RECORD* record)
{
    // HandleEventRecord throws on a malformed event or a callback that returns a
    // failure HRESULT. This runs inside ProcessTrace's C call frames, where
    // letting an exception unwind is undefined behaviour, so the consumer must
    // contain it here: drop the event and keep processing the trace.
    try
    {
        g_dxEtwEventHandler->HandleEventRecord(record);
    }
    catch (...)
    {
        printf("Dropped an event: HandleEventRecord threw.\n");
    }
}

// ETW calls this once per delivered buffer. The logfile header carries the
// running EventsLost / BuffersLost totals, so this is where we feed them to the
// handler; it reports any newly observed loss through the diagnostics sink.
ULONG WINAPI OnBuffer(PEVENT_TRACE_LOGFILE logfile)
{
    g_dxEtwEventHandler->ReportTraceStatistics(*logfile);
    return TRUE; // keep processing
}

static void ConsumerThread()
{
    EVENT_TRACE_LOGFILE trace = {};
    trace.LoggerName = const_cast<LPWSTR>(g_etwSessionName.c_str());
    trace.ProcessTraceMode =
        PROCESS_TRACE_MODE_REAL_TIME |
        PROCESS_TRACE_MODE_EVENT_RECORD |
        // Keep EventHeader.TimeStamp in the session's raw QPC domain rather than letting
        // ProcessTrace convert it to FILETIME (100 ns since 1601), which the ticks->ns
        // converter would overflow.
        PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    trace.EventRecordCallback = OnEvent;
    trace.BufferCallback = OnBuffer;

    TRACEHANDLE traceHandle = OpenTrace(&trace);
    if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        printf("OpenTrace failed: %lu\n", GetLastError());
        DebugBreak();
    }

    // This blocks until the session stops
    ULONG status = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
    if (status != ERROR_SUCCESS)
    {
        printf("ProcessTrace failed: %lu\n", status);
        DebugBreak();
    }

    // Report the final loss totals once processing has drained.
    g_dxEtwEventHandler->ReportTraceStatistics(trace);

    status = CloseTrace(traceHandle);
    if (status != ERROR_SUCCESS)
    {
        printf("CloseTrace failed: %lu\n", status);
        DebugBreak();
    }
}

BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT)
    {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

int __cdecl main(int argc, char** argv)
{
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // The library decodes events for a single target process: the process whose
    // D3D12 work you want to observe. Pass its PID as the first command-line
    // argument; with no argument the sample observes its own process. (A single
    // target PID is required because WinPixEventDecoder currently decodes one
    // process at a time.)
    DWORD targetProcessId = GetCurrentProcessId();
    if (argc > 1)
    {
        targetProcessId = static_cast<DWORD>(strtoul(argv[1], nullptr, 10));
        if (targetProcessId == 0)
        {
            printf("Usage: %s [targetProcessId]\n", argv[0]);
            return 1;
        }
    }
    printf("Observing process %lu\n", targetProcessId);

    g_etwSessionName = L"DxEtwLibrary.sample.session" + std::to_wstring(GetCurrentProcessId());

    Options options{};
    options.CaptureGpuMemoryUsage = true;

    g_dxEtwEventHandler = std::make_unique<DxEtwEventHandler>(
        options,
        true,
        std::make_unique<ApiObjectCallbacksPrintf>(),
        std::make_unique<PixCounterCallbacksPrintf>(),
        std::make_unique<ResidencyEventCallbacksPrintf>(),
        std::make_unique<PixEventCallbacksPrintf>(),
        std::make_unique<MonitorCallbacksPrintf>(),
        std::make_unique<GpuTimingsCallbacksPrintf>(),
        std::make_unique<DxgkObjectCallbacksPrintf>(),
        std::make_unique<PipelineStateEventCallbacksPrintf>(),
        std::make_unique<DiagnosticsSinkPrintf>(),
        std::make_unique<RuntimeFailureCallbacksPrintf>(),
        std::make_unique<TimestampConverterImpl>(),
        targetProcessId
    );

    // Start Trace
    EventTraceProperties properties{};
  
    auto u = ::StartTraceW(&g_sessionHandle, g_etwSessionName.c_str(), properties);
    if (u != ERROR_SUCCESS)
    {
        printf("StartTraceW failed: %lu\n", u);
        DebugBreak();
    }

    EnableD3D12Provider(g_sessionHandle, options.CaptureGpuMemoryUsage);
    EnableDxgkProvider(g_sessionHandle, options.CaptureGpuTiming, options.CaptureGpuMemoryUsage);

    std::thread consumer(ConsumerThread);

    printf("Listening for events (Ctrl+C to stop)...\n");

    // Wait until Ctrl+C
    while (g_running)
    {
        Sleep(200);
    }

    printf("Stopping session...\n");

    u = ControlTraceW(
        g_sessionHandle,
        g_etwSessionName.c_str(),
        properties,
        EVENT_TRACE_CONTROL_STOP
    );
    if (u != ERROR_SUCCESS)
    {
        printf("ControlTraceW failed: %lu\n", u);
        DebugBreak();
    }

    consumer.join();

    printf("Done\n");
    return 0;
}
