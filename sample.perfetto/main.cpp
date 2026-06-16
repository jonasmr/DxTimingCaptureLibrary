// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//
// Like the printf sample, this listens for D3D12 / DXGK ETW events for a target
// process; instead of printing them it writes them into a Perfetto trace
// (dxetw.perfetto-trace) that opens directly in https://ui.perfetto.dev.

#include "pch.h"

#include <cstdlib> // strtoul

#include <DxEtwLibrary/DxEtwLibrary.h>

#include "EtwConfig.h"               // shared with the printf sample (sample/)
#include "PerfettoCallbacks.h"
#include "PerfettoTraceWriter.h"
#include "TimestampConverterImpl.h"

using namespace DirectX::Etw;

static std::wstring g_etwSessionName;
static std::atomic<bool> g_running(true);
static TRACEHANDLE g_sessionHandle = 0;

std::unique_ptr<DxEtwEventHandler> g_dxEtwEventHandler;

void WINAPI OnEvent(EVENT_RECORD* record)
{
    // HandleEventRecord runs inside ProcessTrace's C frames; an exception must not
    // unwind through ETW, so contain it here (drop the event, keep going).
    try
    {
        g_dxEtwEventHandler->HandleEventRecord(record);
    }
    catch (...)
    {
        printf("Dropped an event: HandleEventRecord threw.\n");
    }
}

ULONG WINAPI OnBuffer(PEVENT_TRACE_LOGFILE logfile)
{
    g_dxEtwEventHandler->ReportTraceStatistics(*logfile);
    return TRUE;
}

static void ConsumerThread()
{
    EVENT_TRACE_LOGFILE trace = {};
    trace.LoggerName = const_cast<LPWSTR>(g_etwSessionName.c_str());
    trace.ProcessTraceMode =
        PROCESS_TRACE_MODE_REAL_TIME |
        PROCESS_TRACE_MODE_EVENT_RECORD |
        PROCESS_TRACE_MODE_RAW_TIMESTAMP; // keep raw QPC timestamps
    trace.EventRecordCallback = OnEvent;
    trace.BufferCallback = OnBuffer;

    TRACEHANDLE traceHandle = OpenTrace(&trace);
    if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        printf("OpenTrace failed: %lu\n", GetLastError());
        return;
    }

    ULONG status = ProcessTrace(&traceHandle, 1, nullptr, nullptr); // blocks until the session stops
    if (status != ERROR_SUCCESS)
        printf("ProcessTrace failed: %lu\n", status);

    g_dxEtwEventHandler->OnDataComplete(); // flush pending correlation
    g_dxEtwEventHandler->ReportTraceStatistics(trace);

    CloseTrace(traceHandle);
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

    // Usage: DxEtwLibrary.sample.perfetto.exe [targetProcessId] [seconds]
    //   targetProcessId - process to observe (default: this process)
    //   seconds         - auto-stop after N seconds (default: run until Ctrl+C)
    DWORD targetProcessId = GetCurrentProcessId();
    if (argc > 1)
    {
        targetProcessId = static_cast<DWORD>(strtoul(argv[1], nullptr, 10));
        if (targetProcessId == 0)
        {
            printf("Usage: %s [targetProcessId] [seconds]\n", argv[0]);
            return 1;
        }
    }
    unsigned int runSeconds = (argc > 2) ? static_cast<unsigned int>(strtoul(argv[2], nullptr, 10)) : 0;
    printf("Observing process %lu, writing dxetw.perfetto-trace\n", targetProcessId);

    g_etwSessionName = L"DxEtwLibrary.sample.perfetto.session" + std::to_wstring(GetCurrentProcessId());

    auto writer = std::make_unique<PerfettoTraceWriter>("dxetw.perfetto-trace");
    PerfettoTraceWriter& w = *writer;

    Options options{};
    options.CaptureGpuMemoryUsage = true;
    options.CaptureGpuTiming = true; // GPU work spans

    g_dxEtwEventHandler = std::make_unique<DxEtwEventHandler>(
        options,
        true,
        std::make_unique<PerfettoApiObjectCallbacks>(w),
        std::make_unique<NoOpPixCounterCallbacks>(),
        std::make_unique<NoOpResidencyEventCallbacks>(),
        std::make_unique<NoOpPixEventCallbacks>(),
        std::make_unique<NoOpMonitorEventCallbacks>(),
        std::make_unique<PerfettoGpuTimingsCallbacks>(w),
        std::make_unique<PerfettoDxgkObjectCallbacks>(w),
        std::make_unique<PerfettoPipelineStateEventCallbacks>(w),
        nullptr, // diagnostics sink (optional)
        nullptr, // runtime failure callbacks (optional)
        std::make_unique<TimestampConverterImpl>(),
        targetProcessId);

    EventTraceProperties properties{};
    auto u = ::StartTraceW(&g_sessionHandle, g_etwSessionName.c_str(), properties);
    if (u != ERROR_SUCCESS)
    {
        printf("StartTraceW failed: %lu\n", u);
        return 1;
    }

    EnableD3D12Provider(g_sessionHandle, options.CaptureGpuMemoryUsage);
    EnableDxgkProvider(g_sessionHandle, options.CaptureGpuTiming, options.CaptureGpuMemoryUsage);

    std::thread consumer(ConsumerThread);

    if (runSeconds)
        printf("Listening for %u seconds (or Ctrl+C)...\n", runSeconds);
    else
        printf("Listening for events (Ctrl+C to stop)...\n");

    unsigned int elapsedMs = 0;
    while (g_running)
    {
        Sleep(200);
        elapsedMs += 200;
        if (runSeconds && elapsedMs >= runSeconds * 1000)
            break;
    }

    printf("Stopping session...\n");
    u = ControlTraceW(g_sessionHandle, g_etwSessionName.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    if (u != ERROR_SUCCESS)
        printf("ControlTraceW failed: %lu\n", u);

    consumer.join();

    writer->Finish();
    printf("Done\n");
    return 0;
}
