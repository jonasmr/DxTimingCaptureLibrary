// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <string_view>

#include <DxEtwLibrary/Types.h>

// Consumer callback interfaces. The library calls these as it decodes and
// correlates ETW events; a consumer implements them to store the data wherever
// it likes (see <DxEtwLibrary/NoOpCallbacks.h> for no-op bases to derive from).
//
// Threading: all callbacks are invoked synchronously on the thread that calls
// DxEtwEventHandler::HandleEventRecord / OnDataComplete (i.e. your ETW consumer
// thread). The library holds no locks and assumes a single such thread; any
// storage written from a callback that other threads also touch must be guarded
// by the consumer.
//
// Return-value contract: return S_OK on success. Returning a failure HRESULT is
// treated as fatal - the library throws an exception carrying that HRESULT out of
// HandleEventRecord / OnDataComplete and lets it propagate to the consumer (the
// same way a decode failure on malformed event data propagates). The consumer
// owns the try/catch around those calls (it owns the ProcessTrace loop) and
// decides what a failure means; a propagating exception must not be allowed to
// unwind through ETW. Methods with an _Out_ id parameter must set it on S_OK: the
// library feeds these ids into later callbacks, so a unique, stable, non-zero
// value is required (a monotonically increasing counter works).

class PixCounterCallbacks
{
public:
    struct CounterDataPoint
    {
        UINT64 CounterId;
        INT64 Timestamp; // Time (in ns) since the beginning of the capture session
        double Value;
    };

    virtual ~PixCounterCallbacks() = default;

    virtual HRESULT StorePixCounterInfo(
        UINT64 groupId,
        UINT32 processId,
        PCWSTR name,
        PCWSTR description,
        PCWSTR units,
        DirectX::Etw::CounterFlags flags,
        double min,
        double max,
        UINT64* counterId) = 0;

    virtual HRESULT StorePixCounterInfoWithDefinition(
        UINT64 groupId,
        UINT32 processId,
        PCWSTR name,
        PCWSTR description,
        PCWSTR counterDefinition,
        PCWSTR units,
        DirectX::Etw::CounterFlags flags,
        double min,
        double max,
        UINT64* counterId) = 0;

    virtual HRESULT StorePixCounterBudget(UINT64 counterId, UINT64 budgetCounterId, DirectX::Etw::CounterBudgetType budgetType, DirectX::Etw::CounterBudgetSource budgetSource) = 0;

    virtual HRESULT StorePixCounter(const _In_count_(count) CounterDataPoint* dataPoints, UINT32 count) = 0;

    virtual HRESULT StorePixCounterGroup(UINT64 parentGroupId, PCWSTR name, PCWSTR description, UINT64* groupId) = 0;
};

class ResidencyEventCallbacks
{
public:
    virtual ~ResidencyEventCallbacks() = default;

    virtual HRESULT StoreResidenceOperation(const DirectX::Etw::ResidencyOperation* data) = 0;
    virtual HRESULT StoreDemotedAllocationData(const DirectX::Etw::DemotedAllocation* data, UINT32 count) = 0;
    virtual HRESULT StoreAllocationMigrationData(const DirectX::Etw::AllocationMigration* data, UINT32 count) = 0;
};

class PixEventCallbacks
{
public:
    virtual ~PixEventCallbacks() = default;

    virtual HRESULT StorePixEvents(
        UINT32 processId,
        UINT32 threadId,
        _In_count_(count) const PixCpuEvent* pEvents,
        _In_count_(count) UINT64* pContextIds,
        UINT32 count) = 0;
    virtual HRESULT StorePixGpuEvents(_In_count_(count) const DirectX::Etw::GpuEvent* pGpuEvents, UINT32 count) = 0;
    virtual HRESULT FinalizeGpuEventsForCpuEventIds(_In_count_(count) const UINT64* pEventIds, UINT32 count) = 0;

    virtual HRESULT StoreMemoryEvent(const DirectX::Etw::PixMemoryEvent* data) = 0;
};

class MonitorEventCallbacks
{
public:
    virtual ~MonitorEventCallbacks() = default;

    virtual HRESULT AddMonitor(wchar_t const* monitorDescription, wchar_t const* adapterDescription, UINT64* monitorId) = 0;
    virtual HRESULT UpdateMonitor(UINT64 monitorId, wchar_t const* monitorDescription, wchar_t const* adapterDescription) = 0;

    virtual HRESULT StoreVSync(UINT64 monitorId, INT64 timestamp) = 0;
};

class GpuTimingsCallbacks
{
public:
    virtual ~GpuTimingsCallbacks() = default;

    virtual HRESULT DefineGpuExecution(
        UINT32 processId,
        UINT32 threadId,
        UINT64 apiCommandQueueId,
        INT64 cpuSubmitTime,
        UINT64 presentToken,
        UINT64* gpuExecutionId) = 0;

    virtual HRESULT CompleteGpuExecution(UINT64 gpuExecutionId) = 0;

    virtual HRESULT StoreGpuWork2(
        UINT64 hardwareCommandQueueId,
        UINT64 gpuExecutionId,
        UINT32 commandListIndexInExecution,
        UINT64 apiMarkerId,
        INT64 beginTimestamp,
        INT64 endTimestamp,
        UINT64* gpuWorkId) = 0;

    virtual HRESULT StoreApiMarker(UINT32 processId, UINT32 threadId, PCWSTR name, INT64 timeStamp, UINT64* apiMarkerId) = 0;
};

class DxgkObjectCallbacks
{
public:
    virtual ~DxgkObjectCallbacks() = default;

    virtual HRESULT DefineHardwareAdapter(UINT64* adapterId) = 0;
    virtual HRESULT UpdateHardwareAdapter(UINT64 adapterId, PCWSTR name) = 0;

    virtual HRESULT DefineHardwareCommandQueue(UINT64 adapterId, UINT64* hardwareCommandQueueId) = 0;
    virtual HRESULT UpdateHardwareCommandQueue(UINT64 hardwareCommandQueueId, PCWSTR name) = 0;

    virtual HRESULT DefineApiCommandQueue(
        UINT32 processId,
        UINT64 hardwareAdapterId,
        PCWSTR commandQueueType,
        INT64 beginTimestamp,
        UINT64* apiCommandQueueId) = 0;
    virtual HRESULT UpdateApiCommandQueue(UINT64 apiCommandQueueId, PCWSTR name) = 0;
    virtual HRESULT EndApiCommandQueue(UINT64 apiCommandQueueId, INT64 endTimestamp) = 0;
};

class PipelineStateEventCallbacks
{
public:
    virtual ~PipelineStateEventCallbacks() = default;

    virtual HRESULT AddPsoCompilation(UINT32 processId, UINT32 threadId, UINT64 apiObjectId, INT64 startTimestamp, INT64 endTimestamp) = 0;
};

class ApiObjectCallbacks
{
public:
    virtual ~ApiObjectCallbacks() = default;

    virtual HRESULT OnDeviceCreation(
        INT64 timestamp,
        const DirectX::Etw::DeviceInfo* device,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnDescriptorHeapCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_DESCRIPTOR_HEAP_DESC* desc,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    // A committed resource owns an implicit heap, so it carries that heap's
    // properties and flags along with its GPU virtual address range.
    virtual HRESULT OnCommittedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        const D3D12_HEAP_PROPERTIES* heapProperties,
        D3D12_HEAP_FLAGS heapFlags,
        _Out_ UINT64* objectId) = 0;

    // A placed resource lives within a separately-created heap; it only adds a
    // GPU virtual address range on top of the resource description.
    virtual HRESULT OnPlacedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    // A reserved (tiled) resource has no backing memory at creation, so it has
    // no GPU virtual address; it carries tiling information instead.
    virtual HRESULT OnReservedResourceCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_RESOURCE_DESC* desc,
        const DirectX::Etw::ReservedResourceInfo* reserved,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnHeapCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const D3D12_HEAP_DESC* desc,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnPipelineStateCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnStateObjectCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnCommandAllocatorCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        D3D12_COMMAND_LIST_TYPE commandListType,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnMetaCommandCreation(
        INT64 timestamp,
        UINT64 deviceId,
        UINT32 processId,
        UINT32 threadId,
        REFGUID commandId,
        const DirectX::Etw::ObjectPlacementInfo* placement,
        _Out_ UINT64* objectId) = 0;

    virtual HRESULT OnObjectDestruction(INT64 timestamp, DirectX::Etw::ApiObjectType type, UINT64 objectId) = 0;

    virtual HRESULT UpdateApiObjectName(INT64 timestamp, DirectX::Etw::ApiObjectType type, UINT64 objectId, const wchar_t* name) = 0;
    virtual HRESULT UpdateApiObjectSizeAndAddress(INT64 timestamp, DirectX::Etw::ApiObjectType type, UINT64 objectId, UINT64 allocationSize, UINT64 baseAddress) = 0;
};

// Optional interface for the D3D12 runtime's "failure journal" - the runtime's
// running record of its most recent internal failures: validation errors,
// driver-reported errors, out-of-memory, and command-list removals (for example a
// CopyBufferRegion rejected while recording, which is surfaced to the app only
// later at Close()).
//
// This is the one optional callback interface: nothing else the library reports
// depends on it, so you may pass null for it when constructing DxEtwEventHandler,
// in which case these failures are not reported.
class RuntimeFailureCallbacks
{
public:
    virtual ~RuntimeFailureCallbacks() = default;

    // Called once per journaled failure.
    //   timestamp - when the failure was recorded, on the nanosecond timeline the
    //               library uses for all callbacks.
    //   index     - the runtime's journal slot; it wraps, so it is not a unique id.
    //   code      - the failing HRESULT or NTSTATUS (e.g. E_INVALIDARG 0x80070057).
    //   threadId  - the OS thread that hit the failure.
    //   message   - a short description; may be empty or truncated to 63 characters.
    //
    // The same failure may be reported more than once (once when it happens, and
    // again if the trace replays recent history when a session starts);
    // de-duplicate on (code, threadId, message) if you need each failure once.
    virtual HRESULT OnD3D12JournalEntry(
        INT64 timestamp,
        UINT32 index,
        UINT32 code,
        UINT32 threadId,
        std::string_view message) = 0;
};
