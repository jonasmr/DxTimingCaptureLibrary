// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

// Optional no-op base implementations of the callback interfaces and the
// timestamp converter.
//
// DxEtwEventHandler requires every callback interface to be non-null (see
// DxEtwEventHandler.h), even for categories you do not consume. Rather than stub
// ~40 virtual methods by hand, derive from the classes here and override only
// the methods you care about; the rest do nothing. Out-parameter ids are handed
// back as monotonically increasing values so the cross-callback correlation the
// library performs has well-defined, unique inputs.
//
// These are a convenience, not a requirement - a consumer may implement the
// interfaces directly.

#include <atomic>

#include <DxEtwLibrary/EtwCallbacks.h>
#include <DxEtwLibrary/DxEtwEventHandler.h> // TimestampConverter

class NoOpApiObjectCallbacks : public ApiObjectCallbacks
{
    std::atomic<UINT64> m_nextObjectId{ 1 };

public:
    HRESULT OnDeviceCreation(INT64, const DirectX::Etw::DeviceInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnDescriptorHeapCreation(INT64, UINT64, UINT32, UINT32, const D3D12_DESCRIPTOR_HEAP_DESC*, const DirectX::Etw::ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnCommittedResourceCreation(INT64, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const DirectX::Etw::ObjectPlacementInfo*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnPlacedResourceCreation(INT64, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const DirectX::Etw::ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnReservedResourceCreation(INT64, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const DirectX::Etw::ReservedResourceInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnHeapCreation(INT64, UINT64, UINT32, UINT32, const D3D12_HEAP_DESC*, const DirectX::Etw::ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnPipelineStateCreation(INT64, UINT64, UINT32, UINT32, const DirectX::Etw::ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnStateObjectCreation(INT64, UINT64, UINT32, UINT32, const DirectX::Etw::ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnCommandAllocatorCreation(INT64, UINT64, UINT32, UINT32, D3D12_COMMAND_LIST_TYPE, const DirectX::Etw::ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnMetaCommandCreation(INT64, UINT64, UINT32, UINT32, REFGUID, const DirectX::Etw::ObjectPlacementInfo*, UINT64* objectId) override { *objectId = ++m_nextObjectId; return S_OK; }
    HRESULT OnObjectDestruction(INT64, DirectX::Etw::ApiObjectType, UINT64) override { return S_OK; }
    HRESULT UpdateApiObjectName(INT64, DirectX::Etw::ApiObjectType, UINT64, const wchar_t*) override { return S_OK; }
    HRESULT UpdateApiObjectSizeAndAddress(INT64, DirectX::Etw::ApiObjectType, UINT64, UINT64, UINT64) override { return S_OK; }
};

class NoOpPixCounterCallbacks : public PixCounterCallbacks
{
    std::atomic<UINT64> m_nextId{ 1 };

public:
    HRESULT StorePixCounterInfo(UINT64, UINT32, PCWSTR, PCWSTR, PCWSTR, DirectX::Etw::CounterFlags, double, double, UINT64* counterId) override { *counterId = ++m_nextId; return S_OK; }
    HRESULT StorePixCounterInfoWithDefinition(UINT64, UINT32, PCWSTR, PCWSTR, PCWSTR, PCWSTR, DirectX::Etw::CounterFlags, double, double, UINT64* counterId) override { *counterId = ++m_nextId; return S_OK; }
    HRESULT StorePixCounterBudget(UINT64, UINT64, DirectX::Etw::CounterBudgetType, DirectX::Etw::CounterBudgetSource) override { return S_OK; }
    HRESULT StorePixCounter(const CounterDataPoint*, UINT32) override { return S_OK; }
    HRESULT StorePixCounterGroup(UINT64, PCWSTR, PCWSTR, UINT64* groupId) override { *groupId = ++m_nextId; return S_OK; }
};

class NoOpResidencyEventCallbacks : public ResidencyEventCallbacks
{
public:
    HRESULT StoreResidenceOperation(const DirectX::Etw::ResidencyOperation*) override { return S_OK; }
    HRESULT StoreDemotedAllocationData(const DirectX::Etw::DemotedAllocation*, UINT32) override { return S_OK; }
    HRESULT StoreAllocationMigrationData(const DirectX::Etw::AllocationMigration*, UINT32) override { return S_OK; }
};

class NoOpPixEventCallbacks : public PixEventCallbacks
{
    std::atomic<UINT64> m_nextEventId{ 1 };

public:
    HRESULT StorePixEvents(UINT32, UINT32, const PixCpuEvent*, UINT64* pContextIds, UINT32 count) override
    {
        for (UINT32 i = 0; i < count; ++i)
        {
            pContextIds[i] = ++m_nextEventId;
        }
        return S_OK;
    }
    HRESULT StorePixGpuEvents(const DirectX::Etw::GpuEvent*, UINT32) override { return S_OK; }
    HRESULT FinalizeGpuEventsForCpuEventIds(const UINT64*, UINT32) override { return S_OK; }
    HRESULT StoreMemoryEvent(const DirectX::Etw::PixMemoryEvent*) override { return S_OK; }
};

class NoOpMonitorEventCallbacks : public MonitorEventCallbacks
{
    std::atomic<UINT64> m_nextMonitorId{ 1 };

public:
    HRESULT AddMonitor(wchar_t const*, wchar_t const*, UINT64* monitorId) override { *monitorId = ++m_nextMonitorId; return S_OK; }
    HRESULT UpdateMonitor(UINT64, wchar_t const*, wchar_t const*) override { return S_OK; }
    HRESULT StoreVSync(UINT64, INT64) override { return S_OK; }
};

class NoOpGpuTimingsCallbacks : public GpuTimingsCallbacks
{
    std::atomic<UINT64> m_nextExecutionId{ 1 };
    std::atomic<UINT64> m_nextWorkId{ 1 };
    std::atomic<UINT64> m_nextMarkerId{ 1 };

public:
    HRESULT DefineGpuExecution(UINT32, UINT32, UINT64, INT64, UINT64, UINT64* gpuExecutionId) override { *gpuExecutionId = ++m_nextExecutionId; return S_OK; }
    HRESULT CompleteGpuExecution(UINT64) override { return S_OK; }
    HRESULT StoreGpuWork2(UINT64, UINT64, UINT32, UINT64, INT64, INT64, UINT64* gpuWorkId) override { *gpuWorkId = ++m_nextWorkId; return S_OK; }
    HRESULT StoreApiMarker(UINT32, UINT32, PCWSTR, INT64, UINT64* apiMarkerId) override { *apiMarkerId = ++m_nextMarkerId; return S_OK; }
};

class NoOpDxgkObjectCallbacks : public DxgkObjectCallbacks
{
    std::atomic<UINT64> m_nextId{ 1 };

public:
    HRESULT DefineHardwareAdapter(UINT64* adapterId) override { *adapterId = ++m_nextId; return S_OK; }
    HRESULT UpdateHardwareAdapter(UINT64, PCWSTR) override { return S_OK; }
    HRESULT DefineHardwareCommandQueue(UINT64, UINT64* hardwareCommandQueueId) override { *hardwareCommandQueueId = ++m_nextId; return S_OK; }
    HRESULT UpdateHardwareCommandQueue(UINT64, PCWSTR) override { return S_OK; }
    HRESULT DefineApiCommandQueue(UINT32, UINT64, PCWSTR, INT64, UINT64* apiCommandQueueId) override { *apiCommandQueueId = ++m_nextId; return S_OK; }
    HRESULT UpdateApiCommandQueue(UINT64, PCWSTR) override { return S_OK; }
    HRESULT EndApiCommandQueue(UINT64, INT64) override { return S_OK; }
};

class NoOpPipelineStateEventCallbacks : public PipelineStateEventCallbacks
{
public:
    HRESULT AddPsoCompilation(UINT32, UINT32, UINT64, INT64, INT64) override { return S_OK; }
};

class NoOpRuntimeFailureCallbacks : public RuntimeFailureCallbacks
{
public:
    HRESULT OnD3D12JournalEntry(INT64, UINT32, UINT32, UINT32, std::string_view) override { return S_OK; }
};

// Passes the incoming ETW timestamp through unchanged (identity conversion).
// Adequate for tests; a real consumer must convert QPC ticks to nanoseconds
// (see TimestampConverter and the sample's TimestampConverterImpl).
class NoOpTimestampConverter : public TimestampConverter
{
public:
    LONGLONG GetLastEventTimeStamp() const override { return 0; }
    LONGLONG ConvertClockToTimeStamp(LONGLONG count) const override { return count; }
    INT64 GetHighPerformanceFrequency() const override { return 10000000; }
};
