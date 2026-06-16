// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

class DxEtwProcessor
{
    Options m_options;
    bool m_gpuOnlyEvents; // If true (default new behavior) link GPU events with CPU markers

    std::unique_ptr<ApiObjectCallbacks> m_apiObjectCallbacks;
    std::unique_ptr<PixCounterCallbacks> m_pixCounterCallbacks;
    std::unique_ptr<ResidencyEventCallbacks> m_residencyEventCallbacks;
    std::unique_ptr<PixEventCallbacks> m_pixEventCallbacks;
    std::unique_ptr<MonitorEventCallbacks> m_monitorEventCallbacks;
    std::unique_ptr<GpuTimingsCallbacks> m_gpuTimingsCallbacks;
    std::unique_ptr<DxgkObjectCallbacks> m_dxgkObjectCallbacks;
    std::unique_ptr<PipelineStateEventCallbacks> m_pipelineStateEventCallbacks;

    // Optional (may be null): surfaces runtime failure journal entries.
    std::unique_ptr<RuntimeFailureCallbacks> m_runtimeFailureCallbacks;

    std::unique_ptr<TimestampConverter> m_timestampConverter;
    DWORD m_targetProcessId;

    // Never null: substituted with a NullDiagnosticsSink when the consumer
    // supplies none, so every call site can dereference it unconditionally.
    std::unique_ptr<DiagnosticsSink> m_diagnosticsSink;

    // Cumulative ETW data-loss totals as last reported via ReportTraceStatistics.
    LostEventStatistics m_lostEventStatistics;

    std::unordered_map<PixEventBlockId, int64_t> m_pixEventBlockIdToLastTimestampMap;
    std::unordered_map<std::wstring, PixCounterId> m_customCounterNameToIdMap;
    UINT64 m_customCounterGroup;
    DxgkContexts m_dxgkContexts;
    std::map<uint32_t, PerProcessData> m_perProcessData;
    AdapterTracker m_adapters;

public:
    DxEtwProcessor(
        const Options& options,
        bool gpuOnlyEvents,
        std::unique_ptr<ApiObjectCallbacks> apiObjectCallbacks,
        std::unique_ptr<PixCounterCallbacks> pixCounterCallbacks,
        std::unique_ptr<ResidencyEventCallbacks> residencyEventCallbacks,
        std::unique_ptr<PixEventCallbacks> pixEventCallbacks,
        std::unique_ptr<MonitorEventCallbacks> monitorEventCallbacks,
        std::unique_ptr<GpuTimingsCallbacks> gpuTimingsCallbacks,
        std::unique_ptr<DxgkObjectCallbacks> dxgkObjectCallbacks,
        std::unique_ptr<PipelineStateEventCallbacks> pipelineStateEventCallbacks,
        std::unique_ptr<DiagnosticsSink> diagnosticsSink,
        std::unique_ptr<RuntimeFailureCallbacks> runtimeFailureCallbacks,
        std::unique_ptr<TimestampConverter> timestampConverter,
        DWORD targetProcessId)
        : m_options(options)
        , m_gpuOnlyEvents(gpuOnlyEvents)
        , m_timestampConverter(std::move(timestampConverter))
        , m_targetProcessId(targetProcessId)
        , m_diagnosticsSink(diagnosticsSink ? std::move(diagnosticsSink) : std::make_unique<NullDiagnosticsSink>())
        , m_customCounterGroup(0)
    {
        // All event callbacks and the timestamp converter are required. They are
        // not independently optional: the library mints object/queue ids in one
        // callback and feeds them to another (e.g. a command-queue id from
        // DxgkObjectCallbacks is passed to GpuTimingsCallbacks), so a missing
        // callback would corrupt another's data rather than simply drop its own.
        // Reject null here with a clear error instead of crashing later. The
        // diagnostics sink is the one exception — it is genuinely optional and
        // defaults to a no-op above.
        ThrowIf(apiObjectCallbacks == nullptr, E_INVALIDARG, L"apiObjectCallbacks is required and must not be null.");
        ThrowIf(pixCounterCallbacks == nullptr, E_INVALIDARG, L"pixCounterCallbacks is required and must not be null.");
        ThrowIf(residencyEventCallbacks == nullptr, E_INVALIDARG, L"residencyEventCallbacks is required and must not be null.");
        ThrowIf(pixEventCallbacks == nullptr, E_INVALIDARG, L"pixEventCallbacks is required and must not be null.");
        ThrowIf(monitorEventCallbacks == nullptr, E_INVALIDARG, L"monitorEventCallbacks is required and must not be null.");
        ThrowIf(gpuTimingsCallbacks == nullptr, E_INVALIDARG, L"gpuTimingsCallbacks is required and must not be null.");
        ThrowIf(dxgkObjectCallbacks == nullptr, E_INVALIDARG, L"dxgkObjectCallbacks is required and must not be null.");
        ThrowIf(pipelineStateEventCallbacks == nullptr, E_INVALIDARG, L"pipelineStateEventCallbacks is required and must not be null.");
        ThrowIf(m_timestampConverter == nullptr, E_INVALIDARG, L"timestampConverter is required and must not be null.");

        m_apiObjectCallbacks = std::move(apiObjectCallbacks);
        m_pixCounterCallbacks = std::move(pixCounterCallbacks);
        m_residencyEventCallbacks = std::move(residencyEventCallbacks);
        m_pixEventCallbacks = std::move(pixEventCallbacks);
        m_monitorEventCallbacks = std::move(monitorEventCallbacks);
        m_gpuTimingsCallbacks = std::move(gpuTimingsCallbacks);
        m_dxgkObjectCallbacks = std::move(dxgkObjectCallbacks);
        m_pipelineStateEventCallbacks = std::move(pipelineStateEventCallbacks);

        // Optional: not validated above because a null is a valid choice.
        m_runtimeFailureCallbacks = std::move(runtimeFailureCallbacks);
    }

    TimestampConverter* GetTimestampConverter() const
    {
        return m_timestampConverter.get();
    }

    void OnComplete()
    {
        // Write all remaining gpu timing data ignoring any outstanding PIX
        // events.
        ProcessPendingSubmissions(true);

        // Write all gpu memory counters one last time to ensure we have multiple points
        // to plot a line on a graph.
        m_adapters.ReportLastMemoryCounterValues(m_timestampConverter->GetLastEventTimeStamp(), m_pixCounterCallbacks.get());

        ProcessDeferredEntries();
    }

    // Records ETW data-loss totals from a trace's logfile header. The counts are
    // cumulative for the session, so this is called repeatedly (e.g. from the
    // consumer's ETW BufferCallback, or once after ProcessTrace); each newly
    // observed loss is reported to the diagnostics sink exactly once.
    void ReportTraceStatistics(const EVENT_TRACE_LOGFILE& logfile)
    {
        const uint64_t eventsLost = logfile.LogfileHeader.EventsLost;
        const uint64_t buffersLost = logfile.LogfileHeader.BuffersLost;

        if (eventsLost > m_lostEventStatistics.EventsLost)
        {
            m_diagnosticsSink->OnDiagnostic(
                DiagnosticSeverity::Error,
                DiagnosticCode::EventsLost,
                std::format(
                    L"ETW reported {} lost event(s) ({} total for this trace). Some captured data is missing.",
                    eventsLost - m_lostEventStatistics.EventsLost,
                    eventsLost));
            m_lostEventStatistics.EventsLost = eventsLost;
        }

        if (buffersLost > m_lostEventStatistics.BuffersLost)
        {
            m_diagnosticsSink->OnDiagnostic(
                DiagnosticSeverity::Error,
                DiagnosticCode::BuffersLost,
                std::format(
                    L"ETW reported {} lost buffer(s) ({} total for this trace). Some captured data is missing.",
                    buffersLost - m_lostEventStatistics.BuffersLost,
                    buffersLost));
            m_lostEventStatistics.BuffersLost = buffersLost;
        }
    }

    LostEventStatistics GetLostEventStatistics() const
    {
        return m_lostEventStatistics;
    }

private:
    friend class EtwDispatcher<DxEtwProcessor>;

    PerProcessData& GetPerProcessData(EVENT_RECORD* record)
    {
        return GetPerProcessData(record->EventHeader.ProcessId);
    }

    PerProcessData& GetPerProcessData(uint32_t processId)
    {
        auto perProcessData = m_perProcessData.try_emplace(processId, processId, m_pipelineStateEventCallbacks.get(), m_diagnosticsSink.get());
        return perProcessData.first->second;
    }

    void ProcessPendingSubmissions(bool ignoreMissingPixEvents)
    {
        // Loop all processes and process any pending submissions
        for (auto& processEntry : m_perProcessData)
        {
            processEntry.second.ProcessPendingSubmissions(m_timestampConverter.get(), &m_adapters, &m_dxgkContexts, m_gpuTimingsCallbacks.get(), m_pixEventCallbacks.get(), ignoreMissingPixEvents, m_diagnosticsSink.get());
        }
    }

    void ProcessDeferredEntries()
    {
        for (auto& processEntry : m_perProcessData)
        {
            processEntry.second.ProcessDeferredEntries(m_apiObjectCallbacks.get());
        }
    }

    template <typename TEventArgs>
    void OnD3D12Event_Start(EVENT_RECORD* record, TEventArgs args)
    {}

    template <typename TEventArgs>
    void OnD3D12Event_Stop(EVENT_RECORD* record, TEventArgs args)
    {}

    template <typename TEventArgs>
    void OnD3D12Event_DCStart(EVENT_RECORD* record, TEventArgs args)
    {}

    template <typename TEventArgs>
    void OnD3D12Event_Info(EVENT_RECORD* record, TEventArgs args)
    {}

    template <>
    void OnD3D12Event_Info(EVENT_RECORD* record, D3D12JournalEntryArgs args)
    {
        // Not gated on CaptureGpuMemoryUsage: supplying the callback is itself the opt-in.
        if (m_runtimeFailureCallbacks == nullptr || record->EventHeader.ProcessId != m_targetProcessId)
        {
            return;
        }

        INT64 timestamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        ThrowFailure(m_runtimeFailureCallbacks->OnD3D12JournalEntry(
            timestamp,
            args.index,
            args.code,
            args.threadId,
            args.message));
    }

    template <>
    void OnD3D12Event_Info(EVENT_RECORD* record, D3D12NamedObjectArgs args)
    {
        GetPerProcessData(record).UpdateName(m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart), args.object, args.name, m_dxgkObjectCallbacks.get(), m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Info(EVENT_RECORD* record, D3D12NamedObjectWideArgs args)
    {
        GetPerProcessData(record).UpdateNameWide(m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart), args.object, args.name, m_dxgkObjectCallbacks.get(), m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Info(EVENT_RECORD* record, D3D12RenameObjectArgs args)
    {
        GetPerProcessData(record).UpdateName(m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart), args.object, args.newName, m_dxgkObjectCallbacks.get(), m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Info(EVENT_RECORD* record, D3D12RenameObjectWideArgs args)
    {
        GetPerProcessData(record).UpdateNameWide(m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart), args.object, args.newName, m_dxgkObjectCallbacks.get(), m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12DeviceArgs args)
    {
        auto& perProcessData = GetPerProcessData(record);
        perProcessData.UpdateD3D12Device(args.device, args.kmDevice, args.umDeviceVersion);

        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        uint64_t timestamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        perProcessData.StartNewD3D12Device(args.device, args.featureLevel, args.kmAdapter, args.umAdapter, args.umAdapterVersion, args.kmDevice, args.umDeviceVersion, timestamp, record->EventHeader.ThreadId, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12DeviceArgs args)
    {
        auto& perProcessData = GetPerProcessData(record);
        perProcessData.UpdateD3D12Device(args.device, args.kmDevice, args.umDeviceVersion);

        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        uint64_t timestamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        perProcessData.DestroyD3D12Device(args.device, args.kmDevice, args.umDeviceVersion, timestamp, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_DCStart(EVENT_RECORD* record, D3D12DeviceArgs args)
    {
        auto& perProcessData = GetPerProcessData(record);
        perProcessData.UpdateD3D12Device(args.device, args.kmDevice, args.umDeviceVersion);

        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        uint64_t timestamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        perProcessData.StartNewD3D12Device(args.device, args.featureLevel, args.kmAdapter, args.umAdapter, args.umAdapterVersion, args.kmDevice, args.umDeviceVersion, timestamp, record->EventHeader.ThreadId, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Info(EVENT_RECORD* record, D3D12CommandListArgs args)
    {
        GetPerProcessData(record).StartNewCommandList(args.commandList, args.device, args.sequenceNumber, true, m_pixEventCallbacks.get(), m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12CommandListArgs args)
    {
        GetPerProcessData(record).StartNewCommandList(args.commandList, args.device, args.sequenceNumber, false, m_pixEventCallbacks.get(), m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12CommandListArgs args)
    {
        GetPerProcessData(record).DestroyCommandList(args.commandList, args.device, args.sequenceNumber, m_pixEventCallbacks.get(), m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_DCStart(EVENT_RECORD* record, D3D12CommandListArgs args)
    {
        // We can ignore commandlists being reported during capture state. This is
        // because the D3D12 runtime would not have any runtime marker data or
        // tracked history buffers for them.
        // The D3D12 runtime only starts properly generating history buffers on the
        // next commandlist RESET operation.
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12CommandQueueArgs args)
    {
        GetPerProcessData(record).StartNewCommandQueue(
            args.commandQueue,
            args.device,
            (D3D12_COMMAND_LIST_TYPE)args.commandListType,
            m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart),
            &m_adapters,
            &m_dxgkContexts,
            m_dxgkObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12CommandQueueArgs args)
    {
        GetPerProcessData(record).DestroyCommandQueue(
            args.commandQueue,
            args.device,
            m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart),
            m_dxgkObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_DCStart(EVENT_RECORD* record, D3D12CommandQueueArgs args)
    {
        GetPerProcessData(record).StartNewCommandQueue(args.commandQueue,
            args.device,
            (D3D12_COMMAND_LIST_TYPE)args.commandListType,
            m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart),
            &m_adapters,
            &m_dxgkContexts,
            m_dxgkObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12HeapArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        uint64_t timestamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        GetPerProcessData(record).StartNewD3D12Heap(
            args.device,
            args.heap,
            args.sizeInBytes,
            args.alignment,
            args.type,
            args.cpuPageProperty,
            args.memoryPoolPreference,
            args.creationNodeMask,
            args.visibleNodeMask,
            args.flags,
            args.conjoinedResource,
            args.kmAllocation,
            timestamp,
            record->EventHeader.ProcessId,
            record->EventHeader.ThreadId,
            m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12HeapArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        uint64_t timestamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        GetPerProcessData(record).DestroyD3D12Heap(args.heap, timestamp, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_DCStart(EVENT_RECORD* record, D3D12HeapArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        uint64_t timestamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        GetPerProcessData(record).StartNewD3D12Heap(
            args.device,
            args.heap,
            args.sizeInBytes,
            args.alignment,
            args.type,
            args.cpuPageProperty,
            args.memoryPoolPreference,
            args.creationNodeMask,
            args.visibleNodeMask,
            args.flags,
            args.conjoinedResource,
            args.kmAllocation,
            timestamp,
            record->EventHeader.ProcessId,
            record->EventHeader.ThreadId,
            m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12ResourceArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        uint64_t timestamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        GetPerProcessData(record).StartNewD3D12Resource(
            args.device,
            args.resource,
            args.umResource,
            args.dimension,
            args.width,
            args.height,
            args.depth,
            args.mipLevels,
            args.arraySize,
            args.planeCount,
            args.format,
            args.sampleCount,
            args.sampleQuality,
            args.layout,
            args.flags,
            args.heapType,
            args.heap,
            args.immutableHeapOffset,
            args.placedAlignment,
            args.placedSize,
            args.numTilesForResource,
            args.numPackedMips,
            args.numTilesForPackedMips,
            args.immutableBuffer,
            args.immutableBufferOffset,
            timestamp,
            record->EventHeader.ProcessId,
            record->EventHeader.ThreadId,
            m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12ResourceArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        uint64_t timestamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        GetPerProcessData(record).DestroyD3D12Resource(args.resource, timestamp, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_DCStart(EVENT_RECORD* record, D3D12ResourceArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        uint64_t timestamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        GetPerProcessData(record).StartNewD3D12Resource(
            args.device,
            args.resource,
            args.umResource,
            args.dimension,
            args.width,
            args.height,
            args.depth,
            args.mipLevels,
            args.arraySize,
            args.planeCount,
            args.format,
            args.sampleCount,
            args.sampleQuality,
            args.layout,
            args.flags,
            args.heapType,
            args.heap,
            args.immutableHeapOffset,
            args.placedAlignment,
            args.placedSize,
            args.numTilesForResource,
            args.numPackedMips,
            args.numTilesForPackedMips,
            args.immutableBuffer,
            args.immutableBufferOffset,
            timestamp,
            record->EventHeader.ProcessId,
            record->EventHeader.ThreadId,
            m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12GraphicsPipelineStateArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;
        
        GetPerProcessData(record).StartNewD3D12GraphicsPipelineState(args, GetTimestamp(record), record->EventHeader.ProcessId, record->EventHeader.ThreadId, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12GraphicsPipelineStateArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).DestroyD3D12GraphicsPipelineState(args, GetTimestamp(record), m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_DCStart(EVENT_RECORD* record, D3D12GraphicsPipelineStateArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).StartNewD3D12GraphicsPipelineState(args, GetTimestamp(record), record->EventHeader.ProcessId, record->EventHeader.ThreadId, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12StateObjectArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).StartNewD3D12StateObject(args, GetTimestamp(record), record->EventHeader.ProcessId, record->EventHeader.ThreadId, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12StateObjectArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).DestroyD3D12StateObject(args, GetTimestamp(record), m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_DCStart(EVENT_RECORD* record, D3D12StateObjectArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).StartNewD3D12StateObject(args, GetTimestamp(record), record->EventHeader.ProcessId, record->EventHeader.ThreadId, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12CommandAllocatorArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).StartNewD3D12CommandAllocator(args, GetTimestamp(record), record->EventHeader.ProcessId, record->EventHeader.ThreadId, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12CommandAllocatorArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).DestroyD3D12CommandAllocator(args, GetTimestamp(record), m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_DCStart(EVENT_RECORD* record, D3D12CommandAllocatorArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).StartNewD3D12CommandAllocator(args, GetTimestamp(record), record->EventHeader.ProcessId, record->EventHeader.ThreadId, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12DescriptorHeapArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).StartNewD3D12DescriptorHeap(args, GetTimestamp(record), record->EventHeader.ProcessId, record->EventHeader.ThreadId, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12DescriptorHeapArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).DestroyD3D12DescriptorHeap(args, GetTimestamp(record), m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_DCStart(EVENT_RECORD* record, D3D12DescriptorHeapArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).StartNewD3D12DescriptorHeap(args, GetTimestamp(record), record->EventHeader.ProcessId, record->EventHeader.ThreadId, m_apiObjectCallbacks.get());
    }


    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12MetaCommandArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).StartNewD3D12MetaCommand(args, GetTimestamp(record), record->EventHeader.ProcessId, record->EventHeader.ThreadId, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12MetaCommandArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).DestroyD3D12MetaCommand(args, GetTimestamp(record), m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_DCStart(EVENT_RECORD* record, D3D12MetaCommandArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).StartNewD3D12MetaCommand(args, GetTimestamp(record), record->EventHeader.ProcessId, record->EventHeader.ThreadId, m_apiObjectCallbacks.get());
    }

    template <>
    void OnD3D12Event_Info(EVENT_RECORD* record, D3D12AllocationInfoArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(record).AddAllocationInfos(
            args.device,
            args.object,
            args.numVirtualAddressInfos,
            args.virtualAddressInfos,
            args.numKMTInfos,
            args.kmtInfos,
            m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart),
            m_residencyEventCallbacks.get(),
            m_apiObjectCallbacks.get()
        );
    }

    template <>
    void OnD3D12Event_Info(EVENT_RECORD* record, D3D12RuntimeMarkerDataArgs args)
    {
        GetPerProcessData(record).UpdateRuntimeMarkerData(
            m_timestampConverter.get(),
            m_gpuTimingsCallbacks.get(),
            record->EventHeader.ProcessId,
            args.cpuFrequency,
            args.firstApiSequenceNumber,
            args.commandList,
            args.cpuTimeHigh,
            args.threadIdCount,
            args.threadIds,
            args.dataSize,
            args.data);
    }

    template <>
    void OnD3D12Event_Info(EVENT_RECORD* record, D3D12CommandBufferSubmissionArgs args)
    {
        GetPerProcessData(record).UpdateCommandBufferSubmission(
            record,
            &m_dxgkContexts,
            args.commandQueue,
            args.contextCount,
            args.contexts,
            args.loopIteration,
            args.submitCommandCbSequence,
            args.firstApiSequenceNumberHigh,
            args.completedApiSequenceNumberSize,
            args.completedApiSequenceNumbers,
            args.commandList);
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12ExecuteCommandListArgs args)
    {
        uint64_t cpuTimeStamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        GetPerProcessData(record).StartExecuteCommandList(record, cpuTimeStamp, args.commandQueue, args.commandList);
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12ExecuteCommandListArgs args)
    {
        GetPerProcessData(record).StopExecuteCommandList(record);
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12ExecuteCommandListsArgs args)
    {
        uint64_t cpuTimeStamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        GetPerProcessData(record).StartExecuteCommandLists(record, cpuTimeStamp, args.commandQueue, args.commandLists);
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12ExecuteCommandListsArgs args)
    {
        GetPerProcessData(record).StopExecuteCommandLists(record, args.commandLists);
    }


    template <typename TEventArgs>
    void OnDxgkEvent_Start(EVENT_RECORD* record, TEventArgs args)
    {}

    template <typename TEventArgs>
    void OnDxgkEvent_Stop(EVENT_RECORD* record, TEventArgs args)
    {}

    template <typename TEventArgs>
    void OnDxgkEvent_DCStart(EVENT_RECORD* record, TEventArgs args)
    {}

    template <typename TEventArgs>
    void OnDxgkEvent_Info(EVENT_RECORD* record, TEventArgs args)
    {}

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkVSyncDPCArgs args)
    {
        uint64_t cpuTimeStamp = m_timestampConverter->ConvertClockToTimeStamp(args.frameQPCTime);
        m_adapters.AddVsyncEvent(args.vidPnTargetId, args.dxgAdapter, cpuTimeStamp, m_monitorEventCallbacks.get());
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkCalibrateGpuClockArgs args)
    {
        m_dxgkContexts.AddClockCalibration(
            args.adapter,
            args.nodeOrdinal,
            args.engineOrdinal,
            args.gpuFrequency,
            args.gpuClock,
            args.cpuClock,
            m_diagnosticsSink.get());

        // Process pending submissions every time we recieve a clock calibration event.
        // This acts like a heart beat while the capture is running.  It is also
        // better to calculate gpu timings after collecting clock data as it ensures
        // we are in a better position to create accurate timestamps.
        ProcessPendingSubmissions(false);
    }

    template <>
    void OnD3D12Event_Info(EVENT_RECORD* record, D3D12HistoryBufferCompletionArgs args)
    {
        // HW scheduling uses a HW Queue instead of the broadcast context.
        // Everything else is the same so reuse the existing History buffer handler.
        HandleWriteHistoryBuffer(
            record,
            args.hwQueueHandle,
            args.renderCbSequence,
            args.precision,
            args.historyBufferSize,
            args.historyBuffer);
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12CreatePipelineStateObjectArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
        {
            return;
        }

        GetPerProcessData(record).StartCompilationEvent(ApiObjectType::PipelineState, record, GetTimestamp(record));
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12CreateStateObjectArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
        {
            return;
        }

        GetPerProcessData(record).StartCompilationEvent(ApiObjectType::StateObject, record, GetTimestamp(record));
    }

    template <>
    void OnD3D12Event_Start(EVENT_RECORD* record, D3D12AddToStateObjectArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
        {
            return;
        }

        // AddToStateObject calls result in a new StateObject creation, so we can treat it the same as CreateStateObject
        GetPerProcessData(record).StartCompilationEvent(ApiObjectType::StateObject, record, GetTimestamp(record));
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12CreatePipelineStateObjectArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
        {
            return;
        }

        GetPerProcessData(record).StopCompilationEvent(ApiObjectType::PipelineState, record, GetTimestamp(record), m_pipelineStateEventCallbacks.get());
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12CreateStateObjectArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
        {
            return;
        }

        GetPerProcessData(record).StopCompilationEvent(ApiObjectType::StateObject, record, GetTimestamp(record), m_pipelineStateEventCallbacks.get());
    }

    template <>
    void OnD3D12Event_Stop(EVENT_RECORD* record, D3D12AddToStateObjectArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
        {
            return;
        }

        GetPerProcessData(record).StopCompilationEvent(ApiObjectType::StateObject, record, GetTimestamp(record), m_pipelineStateEventCallbacks.get());
    }

    template <>
    void OnD3D12Event_Info(EVENT_RECORD* record, D3D12CacheStatisticsArgs args)
    {
        // CaptureGpuMemoryUsage is really "capture API objects"
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
        {
            return;
        }

        GetPerProcessData(record).CaptureCompilationCacheStatistics(record, std::move(args));
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkHistoryBufferArgs args)
    {
        HandleWriteHistoryBuffer(
            record,
            args.context,
            args.renderCbSequence,
            args.precision,
            args.historyBufferSize,
            args.historyBuffer);
    }

    void HandleWriteHistoryBuffer(
        EVENT_RECORD* record,
        uint64_t context,
        uint32_t renderCbSequence,
        uint32_t precision,
        uint32_t historyBufferSize,
        uint8_t* historyBuffer)
    {
        uint64_t cpuClock = record->EventHeader.TimeStamp.QuadPart;
        for (auto& entry : m_perProcessData)
        {
            entry.second.UpdateHistoryBuffers(
                &m_dxgkContexts,
                cpuClock,
                context,
                renderCbSequence,
                precision,
                historyBufferSize,
                historyBuffer);
        }
    }

    template <>
    void OnDxgkEvent_Start(EVENT_RECORD* record, DxgkHwQueueArgs args)
    {
        m_dxgkContexts.UpdateHwQueue(args.parentDxgContext, args.hwQueueHandle, args.hwQueue);
    }

    template <>
    void OnDxgkEvent_Stop(EVENT_RECORD* record, DxgkHwQueueArgs args)
    {
        m_dxgkContexts.UpdateHwQueue(args.parentDxgContext, args.hwQueueHandle, args.hwQueue);
    }

    template <>
    void OnDxgkEvent_DCStart(EVENT_RECORD* record, DxgkHwQueueArgs args)
    {
        m_dxgkContexts.UpdateHwQueue(args.parentDxgContext, args.hwQueueHandle, args.hwQueue);
    }

    template <>
    void OnDxgkEvent_Start(EVENT_RECORD* record, DxgkContextArgs args)
    {
        m_dxgkContexts.UpdateContext(args.context, args.contextHandle, args.device, args.nodeOrdinal, args.engineAffinity);
    }

    template <>
    void OnDxgkEvent_Stop(EVENT_RECORD* record, DxgkContextArgs args)
    {
        m_dxgkContexts.UpdateContext(args.context, args.contextHandle, args.device, args.nodeOrdinal, args.engineAffinity);
    }

    template <>
    void OnDxgkEvent_DCStart(EVENT_RECORD* record, DxgkContextArgs args)
    {
        m_dxgkContexts.UpdateContext(args.context, args.contextHandle, args.device, args.nodeOrdinal, args.engineAffinity);
    }

    template <>
    void OnDxgkEvent_Start(EVENT_RECORD* record, DxgkDeviceArgs args)
    {
        m_dxgkContexts.UpdateDevice(args.device, args.dxgAdapter, args.processId, args.thunkHandle);
    }

    template <>
    void OnDxgkEvent_Stop(EVENT_RECORD* record, DxgkDeviceArgs args)
    {
        m_dxgkContexts.UpdateDevice(args.device, args.dxgAdapter, args.processId, args.thunkHandle);
    }

    template <>
    void OnDxgkEvent_DCStart(EVENT_RECORD* record, DxgkDeviceArgs args)
    {
        m_dxgkContexts.UpdateDevice(args.device, args.dxgAdapter, args.processId, args.thunkHandle);
        
        // Once we get dxgAdapter information on rundown, we should attempt to try and write
        // any pending command queues that needed that information.
        // These DXG adapter report events are guaranteed to be sent before any gpu timing
        // traffic, so this is the best place to ensure that all pending command queues are
        // written to the database.

        // processId is logged as a uint64, but it's really a uint32, so it is safe to
        // cast it here to access the per process data.
        GetPerProcessData(static_cast<uint32_t>(args.processId)).DefinePendingCommandQueues(&m_adapters, &m_dxgkContexts, m_dxgkObjectCallbacks.get());
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkNodeMetadataArgs args)
    {
        m_adapters.UpdateDxgiAdapterHardwareQueue(args.dxgAdapter, args.nodeOrdinal, args.engineType, args.friendlyName, m_dxgkObjectCallbacks.get());
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkDpiReportAdapterArgs args)
    {
        // These are defined in wingdi.w but are stripped out of the public header wingdi.h
        // so they are defined here to make this logic more readable.
        enum DISPLAYCONFIG_BUSTYPE
        {
            DISPLAYCONFIG_BUSTYPE_UNDEFINED = 0,
            DISPLAYCONFIG_BUSTYPE_PCI = 1,
            DISPLAYCONFIG_BUSTYPE_ACPI = 2,
            DISPLAYCONFIG_BUSTYPE_ROOT = 3,
            DISPLAYCONFIG_BUSTYPE_VMBUS = 4,
            DISPLAYCONFIG_BUSTYPE_FORCE_UINT32 = 0xFFFFFFFF
        };

        constexpr int MAX_ADAPTER_PNP_STRING = 128;
        wchar_t pnpDeviceId[MAX_ADAPTER_PNP_STRING] = {};

        // Only PCI and ACPI are handled here. If the BusType is root, we also check explicitely for MSBDA
        switch ((DISPLAYCONFIG_BUSTYPE)args.busType)
        {
        case DISPLAYCONFIG_BUSTYPE_PCI:
        {
            ThrowIf(swprintf_s(pnpDeviceId,
                _countof(pnpDeviceId),
                L"PCI\\VEN_%4.4X&DEV_%4.4X&SUBSYS_%4.4X%4.4X&REV_%2.2X",
                args.vendorId,
                args.deviceId,
                args.subSystemId,
                args.subVendorId,
                args.revisionId) == -1);
        }
        break;
        case DISPLAYCONFIG_BUSTYPE_ACPI:
        {
            char* acpiAsString = reinterpret_cast<char*>(&args.vendorId);
            ThrowIf(swprintf_s(pnpDeviceId,
                _countof(pnpDeviceId),
                L"ACPI\\%c%c%c%c%c%c%c%c",
                acpiAsString[0],
                acpiAsString[1],
                acpiAsString[2],
                acpiAsString[3],
                acpiAsString[4],
                acpiAsString[5],
                acpiAsString[6],
                acpiAsString[7]) == -1);
        }
        break;
        case DISPLAYCONFIG_BUSTYPE_ROOT:
        {
            if (args.vendorId == 0x1414 &&
                args.deviceId == 0x8C &&
                args.subVendorId == 0x0 &&
                args.subSystemId == 0x0 &&
                args.revisionId == 0x0)
            {
                ThrowIf(swprintf_s(pnpDeviceId, _countof(pnpDeviceId), L"ROOT\\BASICDISPLAY") == -1);
            }
        }
        break;
        // The following types are still tracked as adapters in PIX but the PnP device id is not parsed.
        // An optional friendly name will not be provided for these adapters.
        // This matches GPUView's behavior.
        case DISPLAYCONFIG_BUSTYPE_UNDEFINED: // specified for indirect display drivers
        case DISPLAYCONFIG_BUSTYPE_VMBUS:     // specified for VGPU and Hyper-V drivers
            ThrowIf(swprintf_s(pnpDeviceId, _countof(pnpDeviceId), L"ROOT\\") == -1);
            break;
        }

        m_adapters.UpdateDxgkAdapter(args.dxgAdapter, args.adapterLuid, pnpDeviceId, m_dxgkObjectCallbacks.get(), m_pixCounterCallbacks.get());
    }

    template <>
    void OnDxgkEvent_Start(EVENT_RECORD* record, DxgkDeviceAllocationArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(static_cast<uint32_t>(args.processId))
            .StartNewDeviceAllocation(args.vidMmAlloc, args.vidMmGlobalAlloc, args.thunkAllocation, GetTimestamp(record));
    }

    template <>
    void OnDxgkEvent_DCStart(EVENT_RECORD* record, DxgkDeviceAllocationArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(static_cast<uint32_t>(args.processId))
            .StartNewDeviceAllocation(args.vidMmAlloc, args.vidMmGlobalAlloc, args.thunkAllocation, GetTimestamp(record));
    }

    template <>
    void OnDxgkEvent_Stop(EVENT_RECORD* record, DxgkDeviceAllocationArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(static_cast<uint32_t>(args.processId))
            .DestroyDeviceAllocation(args.vidMmAlloc, args.vidMmGlobalAlloc);
    }


    template <>
    void OnDxgkEvent_Start(EVENT_RECORD* record, DxgkAdapterAllocationArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(static_cast<uint32_t>(args.hProcessId))
            .StartNewAdapterAllocation(
                args.pDxgAdapter,
                args.preferredSegment,
                args.hVidMmGlobalAlloc);
    }

    template <>
    void OnDxgkEvent_DCStart(EVENT_RECORD* record, DxgkAdapterAllocationArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(static_cast<uint32_t>(args.hProcessId))
            .StartNewAdapterAllocation(
                args.pDxgAdapter,
                args.preferredSegment,
                args.hVidMmGlobalAlloc);
    }

    template <>
    void OnDxgkEvent_Stop(EVENT_RECORD* record, DxgkAdapterAllocationArgs args)
    {
        if (!m_options.CaptureGpuMemoryUsage || record->EventHeader.ProcessId != m_targetProcessId)
            return;

        GetPerProcessData(static_cast<uint32_t>(args.hProcessId))
            .DestroyAdapterAllocation(args.hVidMmGlobalAlloc);
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkVidMmProcessBudgetChangeArgs args)
    {
        if (args.processId != m_targetProcessId)
            return;

        GetPerProcessData(args.processId).ReportAdapterMemoryBudgetChange(
            m_adapters.GetMemoryCounterWriter(args.dxgAdapter),
            args.newBudget,
            args.memorySegmentGroup,
            m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart),
            m_pixCounterCallbacks.get());
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkVidMmProcessUsageChangeArgs args)
    {
        if (args.processId != m_targetProcessId)
            return;

        GetPerProcessData(args.processId).ReportAdapterMemoryUsageChange(
            m_adapters.GetMemoryCounterWriter(args.dxgAdapter),
            args.newUsage,
            args.memorySegmentGroup,
            m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart),
            m_pixCounterCallbacks.get());
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkVidMmProcessCommitmentChangeArgs args)
    {
        if (args.processId != m_targetProcessId)
            return;

        GetPerProcessData(args.processId).ReportAdapterMemoryCommitmentChange(
            m_adapters.GetMemoryCounterWriter(args.dxgAdapter),
            args.newCommitment,
            args.memorySegmentGroup,
            m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart),
            m_pixCounterCallbacks.get());
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkVidMmProcessDemotedCommitmentChangeArgs args)
    {
        if (args.processId != m_targetProcessId)
            return;

        GetPerProcessData(args.processId).ReportAdapterMemoryDemotedCommitmentChange(
            m_adapters.GetMemoryCounterWriter(args.dxgAdapter),
            args.newCommitment,
            args.priorityClass,
            m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart),
            m_pixCounterCallbacks.get());
    }

    void ReportPagingOperation(
        uint64_t allocationGlobalHandle,
        DXGK_MEMORY_TRANSFER_DIRECTION transferDirection,
        uint64_t allocationOffset,
        uint64_t timestamp)
    {
        if (transferDirection == DXGK_MEMORY_TRANSFER_DIRECTION::DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL)
            return;
        
        // For large allocations, DXGK will send multiple PagingOpVirtualTransfer events. Allocations are paged
        // at the granularity of adapter allocations so we can ignore all the subsequent events
        if (allocationOffset > 0)
            return;

        // Paging events always originate from the System process, but the other DXGK and D3D12 events we need come
        // from their respective processes. Right now we only care about paging events that involve the target process.
        GetPerProcessData(m_targetProcessId).AddPagingOperation(allocationGlobalHandle, transferDirection, timestamp, m_residencyEventCallbacks.get());
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkPagingOpVirtualTransferArgs args)
    {
        uint64_t timestamp = GetTimestamp(record);
        GetPerProcessData(record).ReportAdapterPagingActivity(
            m_adapters.GetMemoryCounterWriter(args.dxgAdapter),
            args.transferSize,
            args.transferDirection,
            timestamp,
            m_pixCounterCallbacks.get());

        // Note: we don't filter by m_targetProcessId here since these events always come from the system process
        if (!m_options.CaptureGpuMemoryUsage)
            return;

        ReportPagingOperation(
            args.hAllocationGlobalHandle,
            args.transferDirection,
            args.allocationOffset,
            timestamp);
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkVidMmMakeResidentArgs args)
    {
        // Note: we don't filter by m_targetProcessId here since these events always come from the system process
        if (!m_options.CaptureGpuMemoryUsage)
            return;

        GetPerProcessData(record).ReportMakeResident(args.vidMmAlloc, args.residencyCount, GetTimestamp(record), m_residencyEventCallbacks.get());
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkVidMmEvictArgs args)
    {
        // Note: we don't filter by m_targetProcessId here since these events always come from the system process
        if (!m_options.CaptureGpuMemoryUsage)
            return;

        GetPerProcessData(record).ReportEvict(args.vidMmAlloc, args.residencyCount, GetTimestamp(record), m_residencyEventCallbacks.get());
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkReportSegmentArgs args)
    {
        m_adapters.UpdateSegment(args.dxgAdapter, args.segmentId, args.memorySegmentGroup);

        if (!m_options.CaptureGpuMemoryUsage)
            return;
        GetPerProcessData(m_targetProcessId).TryInsertDeferredDemotedAllocations(m_adapters, args.dxgAdapter, m_residencyEventCallbacks.get());
    }

    void HandleAllocationSegmentInfo(uint64_t dxgAdapter, uint64_t allocationGlobalHandle, uint32_t segmentId, uint64_t timestamp)
    {
        // Note: we don't filter by m_targetProcessId here since these events always come from the system process
        if (!m_options.CaptureGpuTiming)
            return;

        auto& targetProcessData = GetPerProcessData(m_targetProcessId);
        targetProcessData.HandleAllocationSegmentInfo(
            m_adapters,
            dxgAdapter,
            allocationGlobalHandle,
            segmentId,
            timestamp,
            m_residencyEventCallbacks.get());
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkPagingOpVirtualFillArgs args)
    {
        HandleAllocationSegmentInfo(args.dxgAdapter, args.allocationGlobalHandle, args.segmentId, GetTimestamp(record));
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkPagingOpSysmemCommitArgs args)
    {
        HandleAllocationSegmentInfo(args.dxgAdapter, args.allocationGlobalHandle, args.segmentId, GetTimestamp(record));
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkPagingOpMapApertureSegmentArgs args)
    {
        HandleAllocationSegmentInfo(args.dxgAdapter, args.allocationGlobalHandle, args.segmentId, GetTimestamp(record));
    }

    template <>
    void OnDxgkEvent_Info(EVENT_RECORD* record, DxgkMigrateAllocationArgs args)
    {
        // Note: we don't filter by m_targetProcessId here since these events always come from the system process
        if (!m_options.CaptureGpuTiming)
            return;

        auto& targetProcessData = GetPerProcessData(m_targetProcessId);
        targetProcessData.StartMigrateAllocation(
            args.allocationGlobalHandle,
            GetTimestamp(record));
    }

    template <>
    void OnDxgkEvent_Stop(EVENT_RECORD* record, DxgkCompleteAllocationMigrationArgs args)
    {
        // Note: we don't filter by m_targetProcessId here since these events always come from the system process
        if (!m_options.CaptureGpuTiming)
            return;

        auto& targetProcessData = GetPerProcessData(m_targetProcessId);
        targetProcessData.StopMigrateAllocation(
            args.allocationGlobalHandle,
            args.status,
            GetTimestamp(record),
            m_residencyEventCallbacks.get());
    }

    void OnEventWritePIXRecordTimingBlock_v1(
        EVENT_RECORD* record,
        uint32_t extraData,
        uint32_t bufferSize,
        uint8_t* buffer)
    {
        UNREFERENCED_PARAMETER(extraData); // Currently unused by the block decoder

        auto ignoreEventContexts = !m_options.CaptureGpuTiming; // If we are not capturing GPU timings, event Contexts should be ignored
        auto decodedData = PixEventDecoder::DecodeTimingBlock(ignoreEventContexts, m_gpuOnlyEvents, bufferSize, buffer, [=](uint64_t time) { return m_timestampConverter->ConvertClockToTimeStamp(time); });
        if (decodedData.Events.empty())
            return;

        const int64_t firstTimestampInBlock = decodedData.Events.front().Timestamp;
        const int64_t lastTimestampInBlock = decodedData.Events.back().Timestamp;

        // Decoded event data blocks always belong to the same process id and thread id,
        // so we assume that here and create a unique id for that block.
        const uint32_t blockProcessId = decodedData.ProcessId;
        const uint32_t blockThreadId = decodedData.ThreadId;
        const auto blockId = PixEventBlockId(((uint64_t)blockProcessId) << 32 | blockThreadId);

        auto itLastTimestamp = m_pixEventBlockIdToLastTimestampMap.find(blockId);
        if (itLastTimestamp == m_pixEventBlockIdToLastTimestampMap.end())
        {
            // The first new block we see we give it a 0 timestamp to ensure
            // it always starts before any following blocks
            itLastTimestamp = m_pixEventBlockIdToLastTimestampMap.emplace(blockId, 0).first;
        }

        // If the first timestamp in this block is < the last timestamp of a previous matching block, skip it.
        // This can be caused by the PIX runtime writing a buffer to ETW more than once.
        if (firstTimestampInBlock < itLastTimestamp->second)
        {
            return;
        }

        // Save the last timestamp in this block
        itLastTimestamp->second = lastTimestampInBlock;

        // Write all events from the WinPixEventRuntime to the database and collect the
        // returned event ids to be used for associating gpu events.
        size_t totalEvents = decodedData.Events.size();
        std::vector<uint64_t> eventIds(totalEvents);
        ThrowFailure(m_pixEventCallbacks->StorePixEvents(blockProcessId, blockThreadId, decodedData.Events.data(), eventIds.data(), (UINT32)totalEvents));

        // If we are not capturing GPU timings, we should not hold onto the PIX events
        if (!m_options.CaptureGpuTiming)
            return;

        // Find the process for which these events belong and collect them for
        // later processing
        auto& perProcessData = GetPerProcessData(blockProcessId);
        for (size_t i = 0; i < totalEvents; i++)
        {
            // Only track events that are associated with a context.
            if (!decodedData.Events[i].HasContext)
                continue;

            auto& winPixEvent = decodedData.Events[i];
            WinPixEventEntry entry = { WinPixEventId(eventIds[i]), winPixEvent.Type, winPixEvent.Timestamp };
            perProcessData.AddPixEvent(blockThreadId, WinPixEventContextId(decodedData.D3D12Contexts[i]), std::move(entry));
        }

        // Process pending submissions as PIX events are a common cause for holding up processing
        ProcessPendingSubmissions(false);
    }

    void OnEventWritePIXRecordTimingBlock_v2(
        EVENT_RECORD* record,
        uint32_t extraData,
        uint32_t bufferSize,
        uint8_t* buffer)
    {
        // For now, this can just be exactly the same as V1 events, since the decoder supports both V1 and V2 events.
        // If/when we add new functionality to the V2 events then we might decouple the functions.
        return OnEventWritePIXRecordTimingBlock_v1(record, extraData, bufferSize, buffer);
    }

    void OnEventWritePIXReportCounterData(
        EVENT_RECORD* record,
        float value,
        std::wstring_view name)
    {
        PixCounterCallbacks::CounterDataPoint dataPoint = { UINT64_MAX, m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart), value };

        auto counter = m_customCounterNameToIdMap.find(name.data());
        if (counter != m_customCounterNameToIdMap.end())
        {
            dataPoint.CounterId = counter->second.Value;
        }
        else
        {
            if (m_customCounterGroup == 0)
            {
                ThrowFailure(m_pixCounterCallbacks->StorePixCounterGroup(0, L"Custom counters", L"", &m_customCounterGroup));
            }

            constexpr double infinity = std::numeric_limits<double>::infinity();
            ThrowFailure(m_pixCounterCallbacks->StorePixCounterInfo(m_customCounterGroup, record->EventHeader.ProcessId, name.data(), L"", L"", CounterFlags::IsFractional, -infinity, infinity, &dataPoint.CounterId));
            m_customCounterNameToIdMap[name.data()] = PixCounterId(dataPoint.CounterId);
        }

        ThrowFailure(m_pixCounterCallbacks->StorePixCounter(&dataPoint, 1));
    }

    void OnPnpDeviceDescription(
        EVENT_RECORD* record,
        std::wstring_view deviceId,
        std::wstring_view deviceDescription)
    {
        m_adapters.UpdateDeviceDescription(deviceId, deviceDescription, m_dxgkObjectCallbacks.get(), m_monitorEventCallbacks.get(), m_pixCounterCallbacks.get());
    }

    inline uint64_t GetTimestamp(const EVENT_RECORD* record)
    {
        return m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
    }

    void OnEventWritePIXRecordMemoryAllocationEvent(
        EVENT_RECORD* record,
        UINT16 allocatorId,
        UINT64 baseAddress,
        UINT64 size,
        UINT64 metadata)
    {
        PixMemoryEvent data{};
        data.Timestamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        data.ProcessId = record->EventHeader.ProcessId;
        data.ThreadId = record->EventHeader.ThreadId;
        data.AllocatorId = allocatorId;
        data.BaseAddress = baseAddress;
        data.Size = size;
        data.UserData = metadata;
        data.Operation = MemoryOperation::Allocate;

        ThrowFailure(m_pixEventCallbacks->StoreMemoryEvent(&data));
    }

    void OnEventWritePIXRecordMemoryFreeEvent(
        EVENT_RECORD* record,
        UINT16 allocatorId,
        UINT64 baseAddress,
        UINT64 size,
        UINT64 metadata)
    {
        PixMemoryEvent data{};
        data.Timestamp = m_timestampConverter->ConvertClockToTimeStamp(record->EventHeader.TimeStamp.QuadPart);
        data.ProcessId = record->EventHeader.ProcessId;
        data.ThreadId = record->EventHeader.ThreadId;
        data.AllocatorId = allocatorId;
        data.BaseAddress = baseAddress;
        data.Size = size;
        data.UserData = metadata;
        data.Operation = MemoryOperation::Free;

        ThrowFailure(m_pixEventCallbacks->StoreMemoryEvent(&data));
    }
};
