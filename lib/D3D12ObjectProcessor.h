// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

#include "AllocationTracker.h"
#include <DxEtwLibrary/EtwCallbacks.h>

// Internal: how a resource obtained its backing memory, and the placement data
// carried for it across the ETW events that describe its creation. This is the
// library's bookkeeping form; the public callbacks (see EtwCallbacks.h) split it
// into per-placement methods so each only sees the fields it actually populates.
enum class ResourcePlacement
{
    Committed,
    Placed,
    Reserved, // aka tiled
};

struct ResourcePlacementInfo
{
    UINT64 GpuVirtualAddress;
    UINT64 GpuVirtualSize;
    ResourcePlacement Placement;

    D3D12_HEAP_PROPERTIES CommittedHeapProperties;
    D3D12_HEAP_FLAGS CommittedHeapFlags;

    UINT32 NumTilesForResource;
    UINT32 NumPackedMips;
    UINT32 NumTilesForPackedMips;
};

class D3D12ObjectProcessor
{
public:
    // Public forward declarations
    struct HeapEntry;
    struct ResourceEntry;
    struct PipelineStateEntry;
    struct StateObjectEntry;
    struct CommandAllocatorEntry;
    struct DescriptorHeapEntry;
    struct MetaCommandEntry;

private:
    const AllocationTracker& m_allocationTracker;

    std::unordered_map<uint64_t, UINT64> m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Max];
    std::unordered_map<uint64_t, AllocationTracker::Handle> m_objectAddressToAllocationInfoHandle;

    // Data carried from a heap to its conjoined (committed) resource.
    struct ConjoinedHeapData
    {
        UINT64 GpuVirtualBaseAddress;
        UINT64 GpuVirtualSize;
        UINT64 Alignment;
        D3D12_HEAP_PROPERTIES Properties;
        D3D12_HEAP_FLAGS Flags;
    };

    // map conjoined resource to heapData
    std::unordered_map<uint64_t, ConjoinedHeapData> m_heapDataByConjoinedResource;
    // and heap address to conjoined resource
    std::unordered_map<uint64_t, uint64_t> m_heapAddressToConjoinedResource;

    // stores the placed resources object addresses that are waiting for a heap (used to lookup into m_deferredResourceEntryByObjectAddress)
    std::unordered_map<uint64_t, std::vector<uint64_t>> m_deferredPlacedResourcesByHeapObjectAddress;

    using DeviceObjectAddress = uint64_t;
    using ObjectAddress = uint64_t;
    std::unordered_map<DeviceObjectAddress, std::unordered_map<ApiObjectType, std::vector<ObjectAddress>>> m_objectsMissingDeviceId;

    using DeferredEntry = std::variant<
        HeapEntry,
        ResourceEntry,
        PipelineStateEntry,
        StateObjectEntry,
        CommandAllocatorEntry,
        DescriptorHeapEntry,
        MetaCommandEntry
    >;
    std::unordered_map<ObjectAddress, DeferredEntry> m_deferredEntriesByObjectAddress;

    struct EntryBase
    {
        uint64_t Timestamp = 0;
        uint64_t ObjectAddress = 0;
        uint32_t ProcessId = 0;
        uint32_t ThreadId = 0;
    };

public:
    struct ApiObjectInfo
    {
        uint64_t Id;
        ApiObjectType Type;
    };

    struct DeviceEntry : EntryBase
    {
        DeviceInfo Data;
    };

    struct HeapEntry : EntryBase
    {
        // Use this constant to indicate a value is invalid (e.g. Data.GpuVirtualBaseAddress not set yet)
        static const uint64_t InvalidValue = UINT64_MAX;

        std::optional<uint64_t> DeviceId;
        D3D12_HEAP_DESC Desc;
        ObjectPlacementInfo Placement;
        uint64_t ConjoinedResource;

        HeapEntry();

        bool IsComplete() const;
    };

    // Contains all information needed to insert a Resource API Object into the database.
    struct ResourceEntry : EntryBase
    {
        // Use this constant to indicate a value is invalid (e.g. Data.GpuVirtualBaseAddress not set yet)
        static const uint64_t InvalidValue = UINT64_MAX;

        std::optional<uint64_t> DeviceId;
        D3D12_RESOURCE_DESC Desc;
        ResourcePlacementInfo Placement;
        ResourceHeapType ResourceHeapType;
        struct ResourceHeapData
        {
            uint64_t HeapObjectAddress;

            uint64_t ImmutableHeapOffset;
            uint64_t PlacedAlignment;
            uint64_t PlacedSize;

            uint32_t NumTilesForResource;
            uint32_t NumPackedMips;
            uint32_t NumTilesForPackedMips;
        };
        ResourceHeapData ResourceHeapData;

        ResourceEntry();

        // Sometimes we have to wait for multiple events to come in before we have all the data necessary.
        // Right now the two cases are
        // 1. The device hasn't been reported yet
        // 2. The heap information isn't available yet (either CreateHeap is needed for committed resources or ReportHeap is needed for placed resources)
        //    We use the GpuVirtualBaseAddress as the proxy for whether we have heap information.
        bool IsComplete() const;
    };

    struct PipelineStateEntry : EntryBase
    {
        static const uint64_t InvalidValue = UINT64_MAX;

        std::optional<uint64_t> DeviceId;
        uint64_t GpuVirtualBaseAddress;
        uint64_t Size;

        PipelineStateEntry();

        bool IsComplete() const;
    };

    struct StateObjectEntry : EntryBase
    {
        static const uint64_t InvalidValue = UINT64_MAX;

        std::optional<uint64_t> DeviceId;
        uint64_t GpuVirtualBaseAddress;
        uint64_t Size;

        StateObjectEntry();

        bool IsComplete() const;
    };

    struct CommandAllocatorEntry : EntryBase
    {
        static const uint64_t InvalidValue64 = UINT64_MAX;
        static const uint32_t InvalidValue32 = UINT32_MAX;

        std::optional<uint64_t> DeviceId;
        uint64_t GpuVirtualBaseAddress;
        uint64_t Size;
        uint32_t CommandListType;

        CommandAllocatorEntry();

        bool IsComplete() const;
    };

    struct DescriptorHeapEntry : EntryBase
    {
        static const uint64_t InvalidValue64 = UINT64_MAX;
        static const uint32_t InvalidValue32 = UINT32_MAX;

        std::optional<uint64_t> DeviceId;
        uint64_t GpuVirtualBaseAddress;
        uint64_t Size;
        uint32_t DescriptorHeapType;
        uint32_t NumDescriptors;
        uint32_t DescriptorHeapFlags;
        uint32_t NodeMask;

        DescriptorHeapEntry();

        bool IsComplete() const;
    };

    struct MetaCommandEntry : EntryBase
    {
        static const uint64_t InvalidValue64 = UINT64_MAX;

        std::optional<uint64_t> DeviceId;
        uint64_t GpuVirtualBaseAddress;
        uint64_t Size;

        GUID CommandId;

        MetaCommandEntry();

        bool IsComplete() const;
    };


    struct ApiObjectInsertedEventArgs
    {
        ApiObjectType ObjectType;
        uint64_t ObjectId;
        uint32_t ThreadId;
    };
    std::function<void(ApiObjectInsertedEventArgs)> OnApiObjectInserted;

    D3D12ObjectProcessor(const AllocationTracker& allocationTracker);

    void AddAllocationInfo(AllocationTracker::Handle allocationInfoHandle, uint64_t timestamp, ApiObjectCallbacks* callbacks);
    void AddDevice(const DeviceEntry& deviceEntry, ApiObjectCallbacks* callbacks);
    void AddHeap(HeapEntry heapEntry, uint64_t device, ApiObjectCallbacks* callbacks);
    void AddResource(ResourceEntry resourceEntry, uint64_t device, ApiObjectCallbacks* callbacks);
    void AddPipelineState(PipelineStateEntry entry, uint64_t device, ApiObjectCallbacks* callbacks);
    void AddStateObject(StateObjectEntry entry, uint64_t device, ApiObjectCallbacks* callbacks);
    void AddCommandAllocator(CommandAllocatorEntry entry, uint64_t device, ApiObjectCallbacks* callbacks);
    void AddDescriptorHeap(DescriptorHeapEntry entry, uint64_t device, ApiObjectCallbacks* callbacks);
    void AddMetaCommand(MetaCommandEntry entry, uint64_t device, ApiObjectCallbacks* callbacks);

    void UpdateName(uint64_t objectAddress, std::wstring_view name, uint64_t timestamp, ApiObjectCallbacks* callbacks);

    void DestroyApiObject(ApiObjectType objectType, uint64_t objectAddress, uint64_t timestamp, ApiObjectCallbacks* callbacks);

    void ProcessDeferredEntries(ApiObjectCallbacks* callbacks);

    // Gets the owning D3D12 object(s) for the given vidMm allocation.
    // For global vidMm allocations there may be multiple owning D3D12 objects. For non-global vidMm allocations there should be only one.
    // If the object isn't found, the returned vector is empty.
    // 
    // For resources, the allocation will always be for a heap, but for some APIs (like MakeResident and Evict)
    // the target object is the owning object. E.g. for an implied heap this function will return the
    // information for the associated committed resource.
    // For reference the events are linked like so:
    // Find DeviceAllocation via vidMmAlloc or vidMmGlobalAlloc
    // Find AllocationInfo via DeviceAllocation.ThunkAllocation <-> AllocationInfo.KMTInfos[0].kmAllocation
    // Then find D3D12 object via AllocationInfo.pObject
    std::vector<ApiObjectInfo> TryGetOwningApiObjectInfos(uint64_t vidMmAlloc, bool isAllocGlobal) const;

    std::optional<ApiObjectInfo> TryGetOwningApiObjectInfoByObjectAddress(uint64_t objectAddress) const;

private:
    // Fetches the first virtual address from the allocation info at the given index (in m_allocationInfos)
    uint64_t GetGpuVirtualAddressFromAllocationInfoHandle(AllocationTracker::Handle handle);

    void InsertD3D12Heap(const HeapEntry& entry, ApiObjectCallbacks* callbacks);
    void InsertD3D12Resource(const ResourceEntry& entry, ApiObjectCallbacks* callbacks);
    void InsertD3D12PipelineState(const PipelineStateEntry& entry, ApiObjectCallbacks* callbacks);
    void InsertD3D12StateObject(const StateObjectEntry& entry, ApiObjectCallbacks* callbacks);
    void InsertD3D12CommandAllocator(const CommandAllocatorEntry& entry, ApiObjectCallbacks* callbacks);
    void InsertD3D12DescriptorHeap(const DescriptorHeapEntry& entry, ApiObjectCallbacks* callbacks);
    void InsertD3D12MetaCommand(const MetaCommandEntry& entry, ApiObjectCallbacks* callbacks);

    void UpdateApiObjectSizeAndAddress(
        uint64_t timestamp,
        ApiObjectType objectType,
        UINT64 id,
        uint64_t memoryUsage,
        uint64_t baseAddress,
        ApiObjectCallbacks* callbacks) const;
};
