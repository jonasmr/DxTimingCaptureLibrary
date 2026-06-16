// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <DxEtwLibrary/NoOpCallbacks.h>

#include "PerfettoTraceWriter.h"

// Callback implementations that forward the DxEtwLibrary events into a
// PerfettoTraceWriter. Each derives from the matching no-op base and overrides
// only the methods that map onto the Perfetto timeline; the _Out_ id methods are
// overridden so every id comes from the writer's single shared counter (which
// keeps ids globally unique - convenient for keying Perfetto tracks).

class PerfettoApiObjectCallbacks : public NoOpApiObjectCallbacks
{
    PerfettoTraceWriter& m_w;

    HRESULT Create(const char* track, INT64 ts, std::wstring label, UINT64* objectId)
    {
        // The library may pass a null out-pointer when it does not need the id
        // back; keep a local id for our own bookkeeping and only write if asked.
        uint64_t id = m_w.NextId();
        if (objectId) *objectId = id;
        m_w.ObjectCreated(id, track, ts, std::move(label));
        return S_OK;
    }

public:
    explicit PerfettoApiObjectCallbacks(PerfettoTraceWriter& w) : m_w(w) {}

    HRESULT OnDeviceCreation(INT64 ts, const DirectX::Etw::DeviceInfo*, UINT64* id) override
    {
        return Create("Devices", ts, L"Device", id);
    }
    HRESULT OnDescriptorHeapCreation(INT64 ts, UINT64, UINT32, UINT32, const D3D12_DESCRIPTOR_HEAP_DESC*, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("Descriptor Heaps", ts, L"Descriptor Heap", id);
    }
    HRESULT OnCommittedResourceCreation(INT64 ts, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const DirectX::Etw::ObjectPlacementInfo*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, UINT64* id) override
    {
        return Create("Resources", ts, L"Committed Resource", id);
    }
    HRESULT OnPlacedResourceCreation(INT64 ts, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("Resources", ts, L"Placed Resource", id);
    }
    HRESULT OnReservedResourceCreation(INT64 ts, UINT64, UINT32, UINT32, const D3D12_RESOURCE_DESC*, const DirectX::Etw::ReservedResourceInfo*, UINT64* id) override
    {
        return Create("Resources", ts, L"Reserved Resource", id);
    }
    HRESULT OnHeapCreation(INT64 ts, UINT64, UINT32, UINT32, const D3D12_HEAP_DESC*, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("Heaps", ts, L"Heap", id);
    }
    HRESULT OnPipelineStateCreation(INT64 ts, UINT64, UINT32, UINT32, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("Pipeline States", ts, L"Pipeline State", id);
    }
    HRESULT OnStateObjectCreation(INT64 ts, UINT64, UINT32, UINT32, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("State Objects", ts, L"State Object", id);
    }
    HRESULT OnCommandAllocatorCreation(INT64 ts, UINT64, UINT32, UINT32, D3D12_COMMAND_LIST_TYPE, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("Command Allocators", ts, L"Command Allocator", id);
    }
    HRESULT OnMetaCommandCreation(INT64 ts, UINT64, UINT32, UINT32, REFGUID, const DirectX::Etw::ObjectPlacementInfo*, UINT64* id) override
    {
        return Create("Meta Commands", ts, L"Meta Command", id);
    }
    HRESULT OnObjectDestruction(INT64 ts, DirectX::Etw::ApiObjectType, UINT64 objectId) override
    {
        m_w.ObjectDestroyed(objectId, ts);
        return S_OK;
    }
    HRESULT UpdateApiObjectName(INT64, DirectX::Etw::ApiObjectType, UINT64 objectId, const wchar_t* name) override
    {
        m_w.ObjectNamed(objectId, name);
        return S_OK;
    }
};

class PerfettoDxgkObjectCallbacks : public NoOpDxgkObjectCallbacks
{
    PerfettoTraceWriter& m_w;

public:
    explicit PerfettoDxgkObjectCallbacks(PerfettoTraceWriter& w) : m_w(w) {}

    HRESULT DefineHardwareAdapter(UINT64* adapterId) override
    {
        if (adapterId) *adapterId = m_w.NextId();
        return S_OK;
    }
    HRESULT DefineHardwareCommandQueue(UINT64, UINT64* hwQueueId) override
    {
        if (hwQueueId) *hwQueueId = m_w.NextId();
        return S_OK;
    }
    HRESULT UpdateHardwareCommandQueue(UINT64 hwQueueId, PCWSTR name) override
    {
        m_w.NameHwQueue(hwQueueId, name);
        return S_OK;
    }
    HRESULT DefineApiCommandQueue(UINT32, UINT64, PCWSTR type, INT64 beginTs, UINT64* apiQueueId) override
    {
        uint64_t id = m_w.NextId();
        if (apiQueueId) *apiQueueId = id;
        m_w.DefineApiQueue(id, beginTs, type);
        return S_OK;
    }
    HRESULT UpdateApiCommandQueue(UINT64 apiQueueId, PCWSTR name) override
    {
        m_w.NameApiQueue(apiQueueId, name);
        return S_OK;
    }
    HRESULT EndApiCommandQueue(UINT64 apiQueueId, INT64 endTs) override
    {
        m_w.EndApiQueue(apiQueueId, endTs);
        return S_OK;
    }
};

class PerfettoGpuTimingsCallbacks : public NoOpGpuTimingsCallbacks
{
    PerfettoTraceWriter& m_w;

public:
    explicit PerfettoGpuTimingsCallbacks(PerfettoTraceWriter& w) : m_w(w) {}

    HRESULT DefineGpuExecution(UINT32, UINT32, UINT64, INT64, UINT64, UINT64* gpuExecutionId) override
    {
        if (gpuExecutionId) *gpuExecutionId = m_w.NextId();
        return S_OK;
    }
    HRESULT StoreGpuWork2(UINT64 hwQueueId, UINT64, UINT32, UINT64 apiMarkerId, INT64 beginTs, INT64 endTs, UINT64* gpuWorkId) override
    {
        // gpuWorkId may be null (the library does not always want the id back).
        if (gpuWorkId) *gpuWorkId = m_w.NextId();
        m_w.EmitGpuWork(hwQueueId, apiMarkerId, beginTs, endTs);
        return S_OK;
    }
    HRESULT StoreApiMarker(UINT32, UINT32, PCWSTR name, INT64 ts, UINT64* apiMarkerId) override
    {
        uint64_t id = m_w.NextId();
        if (apiMarkerId) *apiMarkerId = id;
        m_w.RegisterApiMarker(id, name, ts);
        return S_OK;
    }
};

class PerfettoPipelineStateEventCallbacks : public NoOpPipelineStateEventCallbacks
{
    PerfettoTraceWriter& m_w;

public:
    explicit PerfettoPipelineStateEventCallbacks(PerfettoTraceWriter& w) : m_w(w) {}

    HRESULT AddPsoCompilation(UINT32, UINT32 threadId, UINT64, INT64 startTs, INT64 endTs) override
    {
        m_w.EmitPsoCompile(threadId, startTs, endTs);
        return S_OK;
    }
};
