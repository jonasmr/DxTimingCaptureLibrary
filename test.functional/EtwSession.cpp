// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "EtwSession.h"

#include <atomic>

#include <DxEtwLibrary/DxEtwEventHandler.h>
#include <DxEtwLibrary/TicksToNanoseconds.h>

using namespace DirectX::Etw;

namespace
{
    void ThrowIfFailed(ULONG error, const char* operation)
    {
        if (error != ERROR_SUCCESS)
        {
            throw std::system_error(
                static_cast<int>(error),
                std::system_category(),
                operation);
        }
    }
}

struct EventTraceProperties
{
    static constexpr size_t MAX_SESSION_NAME_LENGTH = 1024;
    static constexpr size_t MAX_LOG_FILE_NAME_LENGTH = 1024;

    //
    // https://docs.microsoft.com/en-us/windows/desktop/ETW/event-trace-properties
    //
    // m_properties contains offsets to the m_loggerName and m_logFileName
    // relative to the start of m_properties.
    //
    // Both the logger name and file name are max length 1024.  The logger
    // name must be before the log file name in memory (!).  We must fill in
    // the log file name, but StartTrace will fill in the session name.
    //
    EVENT_TRACE_PROPERTIES Properties;
    wchar_t LoggerName[MAX_SESSION_NAME_LENGTH];
    wchar_t LogFileName[MAX_LOG_FILE_NAME_LENGTH];

    EventTraceProperties()
        : Properties{}
        , LoggerName{}
        , LogFileName{}
    {
        Properties.Wnode.BufferSize = sizeof(EventTraceProperties);
        Properties.Wnode.ClientContext = 1; // From the documentation: a value of 1 specifies QPC clock resolution.
        Properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        Properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        Properties.EnableFlags = 0u;

        Properties.MaximumFileSize = 0;

        Properties.LogFileNameOffset = 0;

        // Use the maximum buffer size for improved performance and reduced likelihood of lost events
        // Use lower MinimumBuffers so that ETW uses less memory when it does not need it.
        // Use High MaximumBuffers so that we can still support very high data loads.
        Properties.BufferSize = 1024;
        Properties.MinimumBuffers = 64;
        Properties.MaximumBuffers = 1290;

        Properties.LoggerNameOffset = FIELD_OFFSET(EventTraceProperties, LoggerName);
        // The LoggerName itself is filled in by StartTrace
    }

    EVENT_TRACE_PROPERTIES* operator ->()
    {
        return &Properties;
    }

    operator EVENT_TRACE_PROPERTIES* ()
    {
        return &Properties;
    }
};

// The ETW consumer callback is a C-style function pointer, so it cannot capture
// state. We thread the owning session through EVENT_TRACE_LOGFILE::Context, which
// ETW hands back on each record as EVENT_RECORD::UserContext.
static void WINAPI OnEvent(EVENT_RECORD* record)
{
    static_cast<SimpleEtwSession*>(record->UserContext)->OnEventRecord(record);
}

void SimpleEtwSession::OnEventRecord(_EVENT_RECORD* record)
{
    try
    {
        m_handler->HandleEventRecord(record);
    }
    catch (...)
    {
        // A real consumer must not let an exception unwind through ProcessTrace (it
        // runs through C code). Capture the first one so the rest of the trace still
        // drains and the test can assert on it after the session ends.
        if (!m_consumerException)
        {
            m_consumerException = std::current_exception();
        }
    }
}

void SimpleEtwSession::ConsumerThread()
{
    EVENT_TRACE_LOGFILE trace = {};
    trace.LoggerName = const_cast<LPWSTR>(m_sessionName.c_str());
    trace.ProcessTraceMode =
        PROCESS_TRACE_MODE_REAL_TIME |
        PROCESS_TRACE_MODE_EVENT_RECORD |
        // Without this, ProcessTrace converts EventHeader.TimeStamp to FILETIME
        // (100 ns since 1601) regardless of the session's QPC clock, which a naive
        // ticks->ns converter would overflow. Asking for raw timestamps keeps them in
        // the session's QPC domain (Wnode.ClientContext == 1) so TimestampConverterImpl
        // converts them to a sane absolute nanosecond value.
        PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    trace.EventRecordCallback = OnEvent;
    trace.Context = this;

    TRACEHANDLE traceHandle = OpenTrace(&trace);
    if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        ThrowIfFailed(GetLastError(), "OpenTrace");
    }

    ULONG status = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
    ThrowIfFailed(status, "ProcessTrace");

    // ProcessTrace has returned, so every captured record has been delivered. Surface
    // the session's cumulative data-loss totals and flush any entries the correlation
    // logic was holding until end-of-stream. Doing this here (rather than on the test
    // thread) keeps all handler interaction on a single thread.
    m_handler->ReportTraceStatistics(trace);
    m_lostEventStatistics = m_handler->GetLostEventStatistics();
    m_handler->OnDataComplete();

    status = CloseTrace(traceHandle);
    ThrowIfFailed(status, "CloseTrace");
}

class MonitorCallbacksDummy : public MonitorEventCallbacks
{
    std::atomic<UINT64> m_nextMonitorId{ 1 };

public:
    HRESULT AddMonitor(wchar_t const* monitorDescription, wchar_t const* adapterDescription, UINT64* monitorId) override
    {
        *monitorId = ++m_nextMonitorId;
        return S_OK;
    }

    HRESULT UpdateMonitor(UINT64 monitorId, wchar_t const* monitorDescription, wchar_t const* adapterDescription) override
    {
        return S_OK;
    }

    HRESULT StoreVSync(UINT64 monitorId, INT64 timestamp) override
    {
        return S_OK;
    }
};

class DxgkObjectCallbacksDummy : public DxgkObjectCallbacks
{
    std::atomic<UINT64> m_nextAdapterId{ 1 };
    std::atomic<UINT64> m_nextHardwareCommandQueueId{ 1 };
    std::atomic<UINT64> m_nextApiCommandQueueId{ 1 };

public:
    HRESULT DefineHardwareAdapter(UINT64* adapterId) override
    {
        *adapterId = ++m_nextAdapterId;
        return S_OK;
    }

    HRESULT UpdateHardwareAdapter(UINT64 adapterId, PCWSTR name) override
    {
        return S_OK;
    }

    HRESULT DefineHardwareCommandQueue(UINT64 adapterId, UINT64* hardwareCommandQueueId) override
    {
        *hardwareCommandQueueId = ++m_nextHardwareCommandQueueId;
        return S_OK;
    }

    HRESULT UpdateHardwareCommandQueue(UINT64 hardwareCommandQueueId, PCWSTR name) override
    {
        return S_OK;
    }

    HRESULT DefineApiCommandQueue(
        UINT32 processId,
        UINT64 hardwareAdapterId,
        PCWSTR commandQueueType,
        INT64 beginTimestamp,
        UINT64* apiCommandQueueId) override
    {
        *apiCommandQueueId = ++m_nextApiCommandQueueId;
        return S_OK;
    }

    HRESULT UpdateApiCommandQueue(UINT64 apiCommandQueueId, PCWSTR name) override
    {
        return S_OK;
    }

    HRESULT EndApiCommandQueue(UINT64 apiCommandQueueId, INT64 endTimestamp) override
    {
        return S_OK;
    }
};

class ResidencyEventCallbacksDummy : public ResidencyEventCallbacks
{
public:
    HRESULT StoreResidenceOperation(const ResidencyOperation* data) override
    {
        return S_OK;
    }

    HRESULT StoreDemotedAllocationData(const DemotedAllocation* data, UINT32 count) override
    {
        return S_OK;
    }

    HRESULT StoreAllocationMigrationData(const AllocationMigration* data, UINT32 count) override
    {
        return S_OK;
    }
};


class ApiObjectCallbacksDummy: public ApiObjectCallbacks
{
    std::atomic<UINT64> m_nextObjectId{ 1 };

public:
    HRESULT OnDeviceCreation(INT64 timestamp, const DeviceInfo* device, _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        return S_OK;
    }

    HRESULT OnDescriptorHeapCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_DESCRIPTOR_HEAP_DESC* desc,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        return S_OK;
    }

    HRESULT OnCommittedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const ObjectPlacementInfo* placement,
        const D3D12_HEAP_PROPERTIES* heapProperties,
        D3D12_HEAP_FLAGS heapFlags,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        return S_OK;
    }

    HRESULT OnPlacedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        return S_OK;
    }

    HRESULT OnReservedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const ReservedResourceInfo* reserved,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        return S_OK;
    }

    HRESULT OnHeapCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_HEAP_DESC* desc,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        return S_OK;
    }

    HRESULT OnPipelineStateCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        return S_OK;
    }

    HRESULT OnStateObjectCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        return S_OK;
    }

    HRESULT OnCommandAllocatorCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        D3D12_COMMAND_LIST_TYPE commandListType,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        return S_OK;
    }

    HRESULT OnMetaCommandCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        REFGUID commandId,
        const ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) override
    {
        *objectId = ++m_nextObjectId;
        return S_OK;
    }

    HRESULT OnObjectDestruction(INT64 timestamp, ApiObjectType type, UINT64 objectId) override
    {
        return S_OK;
    }

    HRESULT UpdateApiObjectName(INT64 timestamp, ApiObjectType type, UINT64 objectId, const wchar_t* name) override
    {
        return S_OK;
    }

    HRESULT UpdateApiObjectSizeAndAddress(INT64 timestamp, ApiObjectType type, UINT64 objectId, UINT64 allocationSize, UINT64 baseAddress) override
    {
        return S_OK;
    }
};

class PixCounterCallbacksDummy : public PixCounterCallbacks
{
    std::atomic<UINT64> m_nextCounterId{ 1 };
    std::atomic<UINT64> m_nextGroupId{ 1 };

public:
    HRESULT StorePixCounterInfo(
        UINT64 groupId,
        UINT32 processId,
        PCWSTR name,
        PCWSTR description,
        PCWSTR units,
        CounterFlags flags,
        double min,
        double max,
        UINT64* counterId) override
    {
        *counterId = ++m_nextCounterId;
        return S_OK;
    }

    HRESULT StorePixCounterInfoWithDefinition(
        UINT64 groupId,
        UINT32 processId,
        PCWSTR name,
        PCWSTR description,
        PCWSTR counterDefinition,
        PCWSTR units,
        CounterFlags flags,
        double min,
        double max,
        UINT64* counterId) override
    {
        *counterId = ++m_nextCounterId;
        return S_OK;
    }

    HRESULT StorePixCounterBudget(UINT64 counterId, UINT64 budgetCounterId, CounterBudgetType budgetType, CounterBudgetSource budgetSource) override
    {
        return S_OK;
    }

    HRESULT StorePixCounter(const _In_count_(count) CounterDataPoint* dataPoints, UINT32 count) override
    {
        return S_OK;
    }

    HRESULT StorePixCounterGroup(UINT64 parentGroupId, PCWSTR name, PCWSTR description, UINT64* groupId) override
    {
        *groupId = ++m_nextGroupId;
        return S_OK;
    }
};

class PipelineStateEventCallbacksDummy : public PipelineStateEventCallbacks
{
public:
    HRESULT AddPsoCompilation(UINT32 processId, UINT32 threadId, UINT64 apiObjectId, INT64 startTimestamp, INT64 endTimestamp) override
    {
        return S_OK;
    }
};

class PixEventCallbacksDummy : public PixEventCallbacks
{
    std::atomic<UINT64> m_nextEventId{ 1 };

public:
    HRESULT StorePixEvents(UINT32 processId, UINT32 threadId, const PixCpuEvent* pEvents, UINT64* pContextIds, UINT32 count) override
    {
        for (UINT32 i = 0; i < count; ++i)
        {
            pContextIds[i] = ++m_nextEventId;
        }
        return S_OK;
    }

    HRESULT StorePixGpuEvents(const GpuEvent* pGpuEvents, UINT32 count) override { return S_OK; }
    HRESULT FinalizeGpuEventsForCpuEventIds(const UINT64* pEventIds, UINT32 count) override { return S_OK; }
    HRESULT StoreMemoryEvent(const PixMemoryEvent* data) override { return S_OK; }
};

class GpuTimingsCallbacksDummy : public GpuTimingsCallbacks
{
    std::atomic<UINT64> m_nextExecutionId{ 1 };
    std::atomic<UINT64> m_nextWorkId{ 1 };
    std::atomic<UINT64> m_nextMarkerId{ 1 };

public:
    HRESULT DefineGpuExecution(UINT32 processId, UINT32 threadId, UINT64 apiCommandQueueId, INT64 cpuSubmitTime, UINT64 presentToken, UINT64* gpuExecutionId) override
    {
        *gpuExecutionId = ++m_nextExecutionId;
        return S_OK;
    }

    HRESULT CompleteGpuExecution(UINT64 gpuExecutionId) override { return S_OK; }

    HRESULT StoreGpuWork2(UINT64 hardwareCommandQueueId, UINT64 gpuExecutionId, UINT32 commandListIndexInExecution, UINT64 apiMarkerId, INT64 beginTimestamp, INT64 endTimestamp, UINT64* gpuWorkId) override
    {
        *gpuWorkId = ++m_nextWorkId;
        return S_OK;
    }

    HRESULT StoreApiMarker(UINT32 processId, UINT32 threadId, PCWSTR name, INT64 timeStamp, UINT64* apiMarkerId) override
    {
        *apiMarkerId = ++m_nextMarkerId;
        return S_OK;
    }
};

class TimestampConverterImpl : public TimestampConverter
{
    LARGE_INTEGER m_frequency;

public:
    TimestampConverterImpl()
    {
        QueryPerformanceFrequency(&m_frequency);
    }

    virtual LONGLONG GetLastEventTimeStamp() const override
    {
        return 0;
    }

    virtual LONGLONG ConvertClockToTimeStamp(LONGLONG count) const override
    {
        // The session uses QPC timestamps (Wnode.ClientContext == 1), so `count` is a
        // QPC tick count. The library forwards whatever this returns straight to the
        // callbacks' `timestamp` parameter and treats it as nanoseconds, so convert
        // ticks -> ns here.
        return static_cast<LONGLONG>(TicksToNanoseconds(static_cast<uint64_t>(count), static_cast<uint64_t>(m_frequency.QuadPart)));
    }

    virtual INT64 GetHighPerformanceFrequency() const override
    {
        return m_frequency.QuadPart;
    }
};

static void EnableProvider(TRACEHANDLE traceHandle, GUID const& providerGuid, ULONGLONG keyword, UCHAR level)
{
    ENABLE_TRACE_PARAMETERS enableTraceParameters = { ENABLE_TRACE_PARAMETERS_VERSION_2, 0 };

    ULONG constexpr timeout = 5000; // 5 seconds, seems a reasonable default

    auto err = EnableTraceEx2(
        traceHandle,
        &providerGuid,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        level,
        keyword,
        0,
        timeout,
        &enableTraceParameters);

    ThrowIfFailed(err, "EnableTraceEx2 (enable provider)");

    // Do rundown events too
    err = EnableTraceEx2(
        traceHandle,
        &providerGuid,
        EVENT_CONTROL_CODE_CAPTURE_STATE,
        TRACE_LEVEL_VERBOSE,
        0,
        0,
        INFINITE, // synchronous
        nullptr);

    ThrowIfFailed(err, "EnableTraceEx2 (capture state)");
}

static void EnableD3D12Provider(TRACEHANDLE sessionHandle, bool captureGpuMemoryUsage)
{
    ULONGLONG d3d12Keywords = D3D12_ETW_LOG_FLAGS_RENDER_OPERATION_MARKERS |
        D3D12_ETW_LOG_FLAGS_DRIVER_CUSTOM_MARKERS |
        D3D12_ETW_LOG_FLAGS_BUNDLE_MARKERS |
        D3D12_ETW_LOG_FLAGS_NAMES |
        D3D12_ETW_LOG_FLAGS_DEVICES |
        D3D12_ETW_LOG_FLAGS_OBJECT_LIFETIME |
        D3D12_ETW_LOG_FLAGS_JOURNAL_ENTRIES; // runtime failure journal entries

    if (captureGpuMemoryUsage)
    {
        // D3D12_ETW_LOG_FLAGS_RESOURCES is required for tracking API objects in general.
        // D3D12_ETW_LOG_APIS is required for PSO compilation timing events.
        d3d12Keywords |= D3D12_ETW_LOG_FLAGS_RESOURCES | D3D12_ETW_LOG_APIS;
    }

    EnableProvider(sessionHandle, Direct3D12EtwProviderGuid, d3d12Keywords, TRACE_LEVEL_RESERVED6);
}

static void EnableDxgkProvider(TRACEHANDLE sessionHandle, bool captureGpuTiming, bool captureGpuMemoryUsage)
{
    ULONGLONG dxgKernelKeywords = DXGK_KEYWORD_LOG_FLAGS_LONG_HAUL |
        DXGK_KEYWORD_LOG_FLAGS_BASE |
        DXGK_KEYWORD_LOG_FLAGS_PROFILER;

    if (captureGpuTiming)
    {
        dxgKernelKeywords |= DXGK_KEYWORD_LOG_FLAGS_HISTORY_BUFFER;
    }

    if (captureGpuMemoryUsage)
    {
        dxgKernelKeywords |= DXGK_KEYWORD_LOG_FLAGS_RESOURCE;   // DeviceAllocation, AdapterAllocation, PagingOpVirtualTransfer
    }

    if (captureGpuMemoryUsage || captureGpuTiming)
    {
        dxgKernelKeywords |= DXGK_KEYWORD_LOG_FLAGS_ALLOCATIONS_REFERENCES;  // VidMmMakeResident, VidMmEvict
    }

    EnableProvider(sessionHandle, DxgkControlGuid, dxgKernelKeywords, TRACE_LEVEL_VERBOSE);
}

void SimpleEtwSession::Begin()
{
    // Give every session instance a unique name so leftover state from a prior test
    // (or another instance) cannot collide on StartTrace.
    static std::atomic<unsigned long long> s_sessionCounter{ 0 };
    m_sessionName = L"DxEtwLibrary.test.functional.session."
        + std::to_wstring(GetCurrentProcessId())
        + L"." + std::to_wstring(++s_sessionCounter);

    Options options{};
    options.CaptureGpuMemoryUsage = true;

    m_handler = std::make_unique<DxEtwEventHandler>(
        options,
        true,
        m_apiObjectCallbacks == nullptr ? std::make_unique<ApiObjectCallbacksDummy>() : std::move(m_apiObjectCallbacks),
        std::make_unique<PixCounterCallbacksDummy>(),
        std::make_unique<ResidencyEventCallbacksDummy>(),
        std::make_unique<PixEventCallbacksDummy>(),
        std::make_unique<MonitorCallbacksDummy>(),
        std::make_unique<GpuTimingsCallbacksDummy>(),
        std::make_unique<DxgkObjectCallbacksDummy>(),
        std::make_unique<PipelineStateEventCallbacksDummy>(),
        nullptr, // diagnostics sink (defaults to a no-op)
        std::move(m_runtimeFailureCallbacks),
        std::make_unique<TimestampConverterImpl>(),
        GetCurrentProcessId()
    );

    EventTraceProperties properties;
    TRACEHANDLE sessionHandle = 0;
    auto u = ::StartTraceW(&sessionHandle, m_sessionName.c_str(), properties);
    ThrowIfFailed(u, "StartTraceW");
    m_sessionHandle = sessionHandle;

    EnableD3D12Provider(sessionHandle, options.CaptureGpuMemoryUsage);
    EnableDxgkProvider(sessionHandle, options.CaptureGpuTiming, options.CaptureGpuMemoryUsage);

    m_thread = std::thread(&SimpleEtwSession::ConsumerThread, this);

    m_running = true;
}

void SimpleEtwSession::End()
{
    EventTraceProperties properties;
    auto u = ControlTraceW(
        static_cast<TRACEHANDLE>(m_sessionHandle),
        m_sessionName.c_str(),
        properties,
        EVENT_TRACE_CONTROL_STOP
    );
    ThrowIfFailed(u, "ControlTraceW");

    m_thread.join();

    m_running = false;
}
