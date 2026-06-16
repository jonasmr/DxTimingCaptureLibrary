// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <DxEtwLibrary/EtwDispatcher.h>
#include <DxEtwLibrary/EtwCallbacks.h>
#include <DxEtwLibrary/EtwDiagnostics.h>

class DxEtwProcessor;

// Converts the raw timestamps carried by ETW event records into the nanosecond
// timeline the library hands to the callbacks.
//
// IMPORTANT: the consumer must open the ETW session in raw-QPC mode -
// PROCESS_TRACE_MODE_RAW_TIMESTAMP on the trace, and Wnode.ClientContext = 1
// (QPC) on the session. In that mode EVENT_RECORD::EventHeader.TimeStamp is a
// QPC tick count, which is what ConvertClockToTimeStamp receives. Without
// raw-QPC mode ProcessTrace delivers FILETIME-based timestamps and the
// conversion overflows / produces garbage.
class TimestampConverter
{
public:
    virtual ~TimestampConverter() = default;

    // Timestamp (in nanoseconds) of the most recently processed event, or 0 if
    // none has been processed yet. The library uses this as "now" when flushing
    // correlation state on completion (emitting a final memory-counter sample,
    // timing out unmatched GPU markers), so an implementation that always
    // returns 0 disables those final flushes.
    virtual LONGLONG GetLastEventTimeStamp() const = 0;

    // Converts a raw QPC tick count (EventHeader.TimeStamp in raw-QPC mode) to a
    // nanosecond timestamp on the library's timeline. A typical implementation is
    // TicksToNanoseconds(count, GetHighPerformanceFrequency()).
    virtual LONGLONG ConvertClockToTimeStamp(LONGLONG count) const = 0;

    // The QPC frequency in ticks per second (QueryPerformanceFrequency).
    virtual INT64 GetHighPerformanceFrequency() const = 0;
};

class DxEtwEventHandler
{
    std::unique_ptr<DxEtwProcessor> m_processor;
    std::unique_ptr<DirectX::Etw::EtwDispatcher<DxEtwProcessor>> m_dispatcher;
    DirectX::Etw::Options m_options;
    bool m_gpuOnlyEvents;
    DWORD m_targetProcessId;

public:
    // The eight callback interfaces and the timestampConverter are REQUIRED: the
    // constructor throws (E_INVALIDARG) if any of them is null. They cannot be
    // omitted individually because the library passes ids minted by one callback
    // into another (for example a command-queue id from DxgkObjectCallbacks is
    // handed to GpuTimingsCallbacks), so a null callback would corrupt another's
    // data, not merely suppress its own. Supply a no-op implementation for any
    // category you do not care about (see <DxEtwLibrary/NoOpCallbacks.h> for
    // ready-made no-op bases).
    //
    // Two arguments are genuinely optional - pass null for either:
    //   - diagnosticsSink: null gives a no-op sink.
    //   - runtimeFailureCallbacks: surfaces the D3D12 runtime's failure journal
    //     entries (EventD3D12JournalEntry); null means those events are ignored.
    //     Unlike the eight above it mints no ids fed to other callbacks, so it is
    //     safe to omit.
    //
    // targetProcessId selects the single process whose D3D12/DXGK work is
    // decoded; the library correlates one target process at a time. gpuOnlyEvents
    // links GPU events to their originating CPU PIX markers when true.
    DxEtwEventHandler(
        const DirectX::Etw::Options& options,
        bool gpuOnlyEvents,
        std::unique_ptr<ApiObjectCallbacks> apiObjectCallbacks,
        std::unique_ptr<PixCounterCallbacks> pixCounterCallbacks,
        std::unique_ptr<ResidencyEventCallbacks> residencyEventCallbacks,
        std::unique_ptr<PixEventCallbacks> pixEventCallbacks,
        std::unique_ptr<MonitorEventCallbacks> monitorEventCallbacks,
        std::unique_ptr<GpuTimingsCallbacks> gpuTimingsCallbacks,
        std::unique_ptr<DxgkObjectCallbacks> dxgkObjectCallbacks,
        std::unique_ptr<PipelineStateEventCallbacks> pipelineStateEventCallbacks,
        std::unique_ptr<DirectX::Etw::DiagnosticsSink> diagnosticsSink,
        std::unique_ptr<RuntimeFailureCallbacks> runtimeFailureCallbacks,
        std::unique_ptr<TimestampConverter> timestampConverter,
        DWORD targetProcessId);

    virtual ~DxEtwEventHandler();

    // Decodes and correlates a single ETW event record, invoking the relevant
    // callbacks. Call this from your ETW consumer's EventRecordCallback for every
    // delivered record; records from providers the library does not handle are
    // ignored.
    //
    // This can throw: a malformed/truncated payload, or a callback that returns a
    // failure HRESULT, surfaces as an exception and propagates to the caller (a
    // partial event surfaces nothing to the callbacks before it throws). Because
    // this runs inside ETW's ProcessTrace callback (C frames), the consumer MUST
    // wrap the call in try/catch so a propagating exception never unwinds through
    // ProcessTrace - catch it, drop that event, and decide whether to continue
    // (see the sample's OnEvent and the functional test's SimpleEtwSession).
    void HandleEventRecord(PEVENT_RECORD record);

    // Flushes any remaining correlation state, invoking the callbacks for data
    // that was still pending. Call once after ProcessTrace returns (the trace has
    // drained). Unlike HandleEventRecord, exceptions are not swallowed here.
    void OnDataComplete();

    // Records ETW data-loss totals for the trace so they can be surfaced through
    // the diagnostics sink and queried via GetLostEventStatistics. The counts in
    // logfile.LogfileHeader (EventsLost / BuffersLost) are cumulative for the
    // session; the consumer typically calls this from its ETW BufferCallback
    // and/or once after ProcessTrace returns.
    void ReportTraceStatistics(const EVENT_TRACE_LOGFILE& logfile);

    // Returns the cumulative ETW data-loss totals last seen via
    // ReportTraceStatistics. Non-zero values mean some captured data is missing.
    DirectX::Etw::LostEventStatistics GetLostEventStatistics() const;
};
