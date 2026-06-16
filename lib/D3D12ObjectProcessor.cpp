// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "D3D12ObjectProcessor.h"

D3D12ObjectProcessor::HeapEntry::HeapEntry()
{
    Timestamp = InvalidValue;
    ObjectAddress = InvalidValue;
    ConjoinedResource = InvalidValue;
    Desc = {};
    Placement = {};
    Placement.GpuVirtualAddress = HeapEntry::InvalidValue;
}

bool D3D12ObjectProcessor::HeapEntry::IsComplete() const
{
    return DeviceId.has_value()
        && Placement.GpuVirtualAddress != InvalidValue;
}

D3D12ObjectProcessor::ResourceEntry::ResourceEntry()
{
    Timestamp = InvalidValue;
    ObjectAddress = InvalidValue;
    ResourceHeapType = ResourceHeapType::ImplicitResource; // this is essentially invalid for our purposes
    ResourceHeapData = { 0 };
    Desc = {};
    Placement = {};
    Placement.GpuVirtualAddress = InvalidValue;
}

// Sometimes we have to wait for multiple events to come in before we have all the data necessary.
// Right now the two cases are
// 1. The device hasn't been reported yet
// 2. The heap information isn't available yet (either CreateHeap is needed for committed resources or ReportHeap is needed for placed resources)
//    We use the GpuVirtualBaseAddress as the proxy for whether we have heap information.
bool D3D12ObjectProcessor::ResourceEntry::IsComplete() const
{
    // TODO we don't track mappings for reserved resources so we assume they are complete
    // without a valid GpuVirtualBaseAddress. Revisit this once we do decide to fully support reserved resources.
    return DeviceId.has_value()
        && (Placement.Placement == ResourcePlacement::Reserved || Placement.GpuVirtualAddress != InvalidValue);
}

D3D12ObjectProcessor::PipelineStateEntry::PipelineStateEntry()
{
    Timestamp = InvalidValue;
    DeviceId = std::nullopt;
    ObjectAddress = InvalidValue;
    GpuVirtualBaseAddress = InvalidValue;
    Size = 0;
}

bool D3D12ObjectProcessor::PipelineStateEntry::IsComplete() const
{
    // Not all PipelineState objects result in allocations. If an allocation does occur the memory usage will be updated via UpdateD3D12PipelineState.
    return DeviceId.has_value();
}

D3D12ObjectProcessor::StateObjectEntry::StateObjectEntry()
{
    Timestamp = InvalidValue;
    DeviceId = std::nullopt;
    ObjectAddress = InvalidValue;
    GpuVirtualBaseAddress = InvalidValue;
    Size = 0;
}

bool D3D12ObjectProcessor::StateObjectEntry::IsComplete() const
{
    // Not all StateObject objects result in allocations. If an allocation does occur the memory usage will be updated via UpdateD3D12StateObject.
    return DeviceId.has_value();
}

D3D12ObjectProcessor::CommandAllocatorEntry::CommandAllocatorEntry()
{
    Timestamp = InvalidValue64;
    DeviceId = std::nullopt;
    ObjectAddress = InvalidValue64;
    GpuVirtualBaseAddress = InvalidValue64;
    Size = 0;   // see comment in IsComplete
    CommandListType = InvalidValue32;
}

bool D3D12ObjectProcessor::CommandAllocatorEntry::IsComplete() const
{
    // CommandAllocators don't necesssarily have backing allocations. If any are made, we'll update the size later.
    return DeviceId.has_value();
}

D3D12ObjectProcessor::DescriptorHeapEntry::DescriptorHeapEntry()
{
    Timestamp = InvalidValue64;
    DeviceId = std::nullopt;
    ObjectAddress = InvalidValue64;
    Size = InvalidValue64;    // All descriptor heaps should have some non-zero size (numDescriptors == 0 doesn't make much sense; nvidia drivers crash if you try!)
    DescriptorHeapType = InvalidValue32;
    NumDescriptors = InvalidValue32;
    DescriptorHeapFlags = InvalidValue32;
    NodeMask = InvalidValue32;
}

bool D3D12ObjectProcessor::DescriptorHeapEntry::IsComplete() const
{
    return DeviceId.has_value() && Size != InvalidValue64;
}

D3D12ObjectProcessor::MetaCommandEntry::MetaCommandEntry()
{
    Timestamp = InvalidValue64;
    DeviceId = std::nullopt;
    ObjectAddress = InvalidValue64;
    Size = 0;
    CommandId = { 0 };
}

bool D3D12ObjectProcessor::MetaCommandEntry::IsComplete() const
{
    return DeviceId.has_value();
}

D3D12ObjectProcessor::D3D12ObjectProcessor(const AllocationTracker& allocationTracker)
    : m_allocationTracker(allocationTracker)
{}

void D3D12ObjectProcessor::AddAllocationInfo(AllocationTracker::Handle allocationInfoHandle, uint64_t timestamp, ApiObjectCallbacks* callbacks)
{
    const AllocationInfo& info = m_allocationTracker.GetAllocationInfo(allocationInfoHandle);

    // this method assumes only non-empty AllocationInfos
    assert(info.virtualAddressInfos.size() > 0);

    // Keep a mapping of object address to allocation info index for easy lookups
    m_objectAddressToAllocationInfoHandle[info.object] = allocationInfoHandle;

    // Process any deferred insertions that depend on AllocationInfo
    const auto deferredEntryIt = m_deferredEntriesByObjectAddress.find(info.object);
    if (deferredEntryIt != m_deferredEntriesByObjectAddress.end())
    {
        auto& deferredEntry = deferredEntryIt->second;
        if (auto heapEntry = std::get_if<HeapEntry>(&deferredEntry))
        {
            uint64_t gpuVirtualBaseAddress = info.virtualAddressInfos[0].startAddress;
            heapEntry->Placement.GpuVirtualAddress = gpuVirtualBaseAddress;
            if (heapEntry->IsComplete())
            {
                InsertD3D12Heap(*heapEntry, callbacks);
            }
        }
        else if (auto descriptorHeapEntry = std::get_if<DescriptorHeapEntry>(&deferredEntry))
        {
            uint64_t gpuVirtualBaseAddress = info.virtualAddressInfos[0].startAddress;
            descriptorHeapEntry->GpuVirtualBaseAddress = gpuVirtualBaseAddress;
            descriptorHeapEntry->Size = m_allocationTracker.GetMemoryUsage(allocationInfoHandle);
            if (descriptorHeapEntry->IsComplete())
            {
                InsertD3D12DescriptorHeap(*descriptorHeapEntry, callbacks);
            }
        }

        // Add any other API objects that require an AllocationInfo here
    }
    else
    {
        // Some API objects may receive multiple AllocationInfo events. If this happens we'll need to
        // update the existing database entry. Each AllocationInfo contains a complete snapshot of the memory allocations,
        // so we only need to look at the most recent event.
        {
            const auto existingIdIt = m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::PipelineState].find(info.object);
            if (existingIdIt != m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::PipelineState].end())
            {
                uint64_t gpuVirtualBaseAddress = info.virtualAddressInfos[0].startAddress;
                UpdateApiObjectSizeAndAddress(timestamp, ApiObjectType::PipelineState, existingIdIt->second, m_allocationTracker.GetMemoryUsage(allocationInfoHandle), gpuVirtualBaseAddress, callbacks);
            }
        }

        {
            const auto existingIdIt = m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::StateObject].find(info.object);
            if (existingIdIt != m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::StateObject].end())
            {
                uint64_t gpuVirtualBaseAddress = info.virtualAddressInfos[0].startAddress;
                UpdateApiObjectSizeAndAddress(timestamp, ApiObjectType::StateObject, existingIdIt->second, m_allocationTracker.GetMemoryUsage(allocationInfoHandle), gpuVirtualBaseAddress, callbacks);
            }
        }

        {
            const auto existingIdIt = m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::CommandAllocator].find(info.object);
            if (existingIdIt != m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::CommandAllocator].end())
            {
                uint64_t gpuVirtualBaseAddress = info.virtualAddressInfos[0].startAddress;
                UpdateApiObjectSizeAndAddress(timestamp, ApiObjectType::CommandAllocator, existingIdIt->second, m_allocationTracker.GetMemoryUsage(allocationInfoHandle), gpuVirtualBaseAddress, callbacks);
            }
        }
    }
}

void D3D12ObjectProcessor::AddDevice(const DeviceEntry& deviceEntry, ApiObjectCallbacks* callbacks)
{
    UINT64 deviceId;
    ThrowFailure(callbacks->OnDeviceCreation(deviceEntry.Timestamp, &deviceEntry.Data, &deviceId));
    m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device][deviceEntry.ObjectAddress] = deviceId;

    // Process any objects that were waiting for this device id
    if (m_objectsMissingDeviceId.count(deviceEntry.ObjectAddress) > 0)
    {
        for (const auto& objectsByType : m_objectsMissingDeviceId[deviceEntry.ObjectAddress])
        {
            const ApiObjectType type = objectsByType.first;
            for (const auto& objectAddress : objectsByType.second)
            {
                assert(m_deferredEntriesByObjectAddress.count(objectAddress) == 1);
                switch (type)
                {
                case ApiObjectType::Resource:
                {
                    auto& resourceEntry = std::get<ResourceEntry>(m_deferredEntriesByObjectAddress.at(objectAddress));
                    resourceEntry.DeviceId = deviceId;

                    if (resourceEntry.IsComplete())
                    {
                        InsertD3D12Resource(resourceEntry, callbacks);
                    }
                }
                break;
                case ApiObjectType::Heap:
                {
                    auto& heapEntry = std::get<HeapEntry>(m_deferredEntriesByObjectAddress.at(objectAddress));
                    heapEntry.DeviceId = deviceId;

                    if (heapEntry.IsComplete())
                    {
                        InsertD3D12Heap(heapEntry, callbacks);
                    }
                }
                break;
                case ApiObjectType::StateObject:
                {
                    auto& entry = std::get<StateObjectEntry>(m_deferredEntriesByObjectAddress.at(objectAddress));
                    entry.DeviceId = deviceId;

                    if (entry.IsComplete())
                    {
                        InsertD3D12StateObject(entry, callbacks);
                    }
                }
                break;
                case ApiObjectType::CommandAllocator:
                {
                    auto& entry = std::get<CommandAllocatorEntry>(m_deferredEntriesByObjectAddress.at(objectAddress));
                    entry.DeviceId = deviceId;

                    if (entry.IsComplete())
                    {
                        InsertD3D12CommandAllocator(entry, callbacks);
                    }
                }
                break;
                case ApiObjectType::DescriptorHeap:
                {
                    auto& entry = std::get<DescriptorHeapEntry>(m_deferredEntriesByObjectAddress.at(objectAddress));
                    entry.DeviceId = deviceId;

                    if (entry.IsComplete())
                    {
                        InsertD3D12DescriptorHeap(entry, callbacks);
                    }
                }
                break;
                case ApiObjectType::MetaCommand:
                {
                    auto& entry = std::get<MetaCommandEntry>(m_deferredEntriesByObjectAddress.at(objectAddress));
                    entry.DeviceId = deviceId;

                    if (entry.IsComplete())
                    {
                        InsertD3D12MetaCommand(entry, callbacks);
                    }
                }
                break;
                case ApiObjectType::PipelineState:
                {
                    auto& entry = std::get<PipelineStateEntry>(m_deferredEntriesByObjectAddress.at(objectAddress));
                    entry.DeviceId = deviceId;

                    if (entry.IsComplete())
                    {
                        InsertD3D12PipelineState(entry, callbacks);
                    }
                }
                break;

                case ApiObjectType::Invalid:
                case ApiObjectType::Max:
                case ApiObjectType::Device:
                    // None of these values should exist in m_objectsMissingDeviceId
                    assert(false);
                    break;
                default:
                    // Add new cases if more object types are added
                    static_assert((uint32_t)ApiObjectType::Max == 9);
                    break;
                }
            }
        }

        (void)m_objectsMissingDeviceId.erase(deviceEntry.ObjectAddress);
    }
}

void D3D12ObjectProcessor::AddHeap(HeapEntry heapEntry, uint64_t device, ApiObjectCallbacks* callbacks)
{
    const auto deviceIt = m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].find(device);
    if (deviceIt != m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].end())
    {
        heapEntry.DeviceId = deviceIt->second;
    }
    else
    {
        m_objectsMissingDeviceId[device][ApiObjectType::Heap].push_back(heapEntry.ObjectAddress);
    }

    const auto allocationInfoIndexIt = m_objectAddressToAllocationInfoHandle.find(heapEntry.ObjectAddress);
    if (allocationInfoIndexIt != m_objectAddressToAllocationInfoHandle.end())
    {
        heapEntry.Placement.GpuVirtualAddress = GetGpuVirtualAddressFromAllocationInfoHandle(allocationInfoIndexIt->second);
    }
    else
    {
        heapEntry.Placement.GpuVirtualAddress = ResourceEntry::InvalidValue;
    }

    if (heapEntry.IsComplete())
    {
        InsertD3D12Heap(heapEntry, callbacks);
    }
    else
    {
        m_deferredEntriesByObjectAddress[heapEntry.ObjectAddress] = heapEntry;
    }

    m_heapAddressToConjoinedResource[heapEntry.ObjectAddress] = heapEntry.ConjoinedResource;
}

void D3D12ObjectProcessor::AddResource(ResourceEntry resourceEntry, uint64_t device, ApiObjectCallbacks* callbacks)
{
    const auto deviceIt = m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].find(device);
    if (deviceIt != m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].end())
    {
        resourceEntry.DeviceId = deviceIt->second;
    }
    else
    {
        m_objectsMissingDeviceId[device][ApiObjectType::Resource].push_back(resourceEntry.ObjectAddress);
    }

    D3D12_RESOURCE_DESC& desc = resourceEntry.Desc;
    ResourcePlacementInfo& placement = resourceEntry.Placement;
    switch (resourceEntry.ResourceHeapType)
    {
        // placed resource
    case ResourceHeapType::ImmutableHeap:
    {
        placement.GpuVirtualSize = resourceEntry.ResourceHeapData.PlacedSize;
        desc.Alignment = resourceEntry.ResourceHeapData.PlacedAlignment;

        placement.Placement = ResourcePlacement::Placed;

        const auto allocationInfoIndexIt = m_objectAddressToAllocationInfoHandle.find(resourceEntry.ResourceHeapData.HeapObjectAddress);
        if (allocationInfoIndexIt != m_objectAddressToAllocationInfoHandle.end())
        {
            placement.GpuVirtualAddress = GetGpuVirtualAddressFromAllocationInfoHandle(allocationInfoIndexIt->second) + resourceEntry.ResourceHeapData.ImmutableHeapOffset;
        }
        else
        {
            // We need the corresponding heap information to determine GpuVirtualBaseAddress.
            // This only happens when a ReportResource event comes in before a ReportHeap event.
            assert(std::find(
                std::begin(m_deferredPlacedResourcesByHeapObjectAddress[resourceEntry.ResourceHeapData.HeapObjectAddress]),
                std::end(m_deferredPlacedResourcesByHeapObjectAddress[resourceEntry.ResourceHeapData.HeapObjectAddress]),
                resourceEntry.ObjectAddress) == std::end(m_deferredPlacedResourcesByHeapObjectAddress[resourceEntry.ResourceHeapData.HeapObjectAddress]));
            m_deferredPlacedResourcesByHeapObjectAddress[resourceEntry.ResourceHeapData.HeapObjectAddress].push_back(resourceEntry.ObjectAddress);
        }

        break;
    }
    // committed resource
    case ResourceHeapType::ImplicitHeap:
    {
        placement.Placement = ResourcePlacement::Committed;

        // ReportHeap might be fired before ReportResource.
        // If so we can insert the resource entry into the database right now.
        const auto heapIt = m_heapDataByConjoinedResource.find(resourceEntry.ObjectAddress);
        if (heapIt != m_heapDataByConjoinedResource.end())
        {
            const auto& heapData = heapIt->second;

            placement.GpuVirtualAddress = heapData.GpuVirtualBaseAddress;
            placement.GpuVirtualSize = heapData.GpuVirtualSize;
            desc.Alignment = heapData.Alignment;

            placement.CommittedHeapProperties = heapData.Properties;
            placement.CommittedHeapFlags = heapData.Flags;

            const auto result = m_heapDataByConjoinedResource.erase(resourceEntry.ObjectAddress);
            DBG_UNREFERENCED_LOCAL_VARIABLE(result);
            assert(result == 1);
        }
        else
        {
            // The heap information will arrive in a future heap event,
            // so we have to defer the actual database insertion until we receive that event.
            placement.CommittedHeapProperties = {};
            placement.CommittedHeapFlags = D3D12_HEAP_FLAG_NONE;
        }
        break;
    }
    // reserved resource
    case ResourceHeapType::ReservedResource:
    {
        placement.Placement = ResourcePlacement::Reserved;
        placement.NumTilesForResource = resourceEntry.ResourceHeapData.NumTilesForResource;
        placement.NumPackedMips = resourceEntry.ResourceHeapData.NumPackedMips;
        placement.NumTilesForPackedMips = resourceEntry.ResourceHeapData.NumTilesForPackedMips;
        break;
    }
    case ResourceHeapType::ImplicitResource:
        // implicit resources are created by the runtime and can be safely ignored
        return;
    default:
        ThrowToolException(E_UNEXPECTED);
    }

    if (resourceEntry.IsComplete())
    {
        InsertD3D12Resource(resourceEntry, callbacks);
    }
    else
    {
        m_deferredEntriesByObjectAddress[resourceEntry.ObjectAddress] = resourceEntry;
    }
}

void D3D12ObjectProcessor::AddPipelineState(PipelineStateEntry entry, uint64_t device, ApiObjectCallbacks* callbacks)
{
    const auto deviceIt = m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].find(device);
    if (deviceIt != m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].end())
    {
        entry.DeviceId = deviceIt->second;
    }
    else
    {
        m_objectsMissingDeviceId[device][ApiObjectType::PipelineState].push_back(entry.ObjectAddress);
    }

    const auto allocationInfoIndexIt = m_objectAddressToAllocationInfoHandle.find(entry.ObjectAddress);
    if (allocationInfoIndexIt != m_objectAddressToAllocationInfoHandle.end())
    {
        entry.Size = m_allocationTracker.GetMemoryUsage(allocationInfoIndexIt->second);
    }

    if (entry.IsComplete())
    {
        InsertD3D12PipelineState(entry, callbacks);
    }
    else
    {
        m_deferredEntriesByObjectAddress[entry.ObjectAddress] = entry;
    }
}

void D3D12ObjectProcessor::AddStateObject(StateObjectEntry entry, uint64_t device, ApiObjectCallbacks* callbacks)
{
    const auto deviceIt = m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].find(device);
    if (deviceIt != m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].end())
    {
        entry.DeviceId = deviceIt->second;
    }
    else
    {
        m_objectsMissingDeviceId[device][ApiObjectType::StateObject].push_back(entry.ObjectAddress);
    }

    const auto allocationInfoIndexIt = m_objectAddressToAllocationInfoHandle.find(entry.ObjectAddress);
    if (allocationInfoIndexIt != m_objectAddressToAllocationInfoHandle.end())
    {
        entry.Size = m_allocationTracker.GetMemoryUsage(allocationInfoIndexIt->second);
    }

    if (entry.IsComplete())
    {
        InsertD3D12StateObject(entry, callbacks);
    }
    else
    {
        m_deferredEntriesByObjectAddress[entry.ObjectAddress] = entry;
    }
}

void D3D12ObjectProcessor::AddCommandAllocator(CommandAllocatorEntry entry, uint64_t device, ApiObjectCallbacks* callbacks)
{
    const auto deviceIt = m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].find(device);
    if (deviceIt != m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].end())
    {
        entry.DeviceId = deviceIt->second;
    }
    else
    {
        m_objectsMissingDeviceId[device][ApiObjectType::CommandAllocator].push_back(entry.ObjectAddress);
    }

    const auto allocationInfoIndexIt = m_objectAddressToAllocationInfoHandle.find(entry.ObjectAddress);
    if (allocationInfoIndexIt != m_objectAddressToAllocationInfoHandle.end())
    {
        entry.Size = m_allocationTracker.GetMemoryUsage(allocationInfoIndexIt->second);
    }

    if (entry.IsComplete())
    {
        InsertD3D12CommandAllocator(entry, callbacks);
    }
    else
    {
        m_deferredEntriesByObjectAddress[entry.ObjectAddress] = entry;
    }
}

void D3D12ObjectProcessor::AddDescriptorHeap(DescriptorHeapEntry entry, uint64_t device, ApiObjectCallbacks* callbacks)
{
    const auto deviceIt = m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].find(device);
    if (deviceIt != m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].end())
    {
        entry.DeviceId = deviceIt->second;
    }
    else
    {
        m_objectsMissingDeviceId[device][ApiObjectType::DescriptorHeap].push_back(entry.ObjectAddress);
    }

    const auto allocationInfoIndexIt = m_objectAddressToAllocationInfoHandle.find(entry.ObjectAddress);
    if (allocationInfoIndexIt != m_objectAddressToAllocationInfoHandle.end())
    {
        entry.Size = m_allocationTracker.GetMemoryUsage(allocationInfoIndexIt->second);
    }

    if (entry.IsComplete())
    {
        InsertD3D12DescriptorHeap(entry, callbacks);
    }
    else
    {
        m_deferredEntriesByObjectAddress[entry.ObjectAddress] = entry;
    }
}

void D3D12ObjectProcessor::AddMetaCommand(MetaCommandEntry entry, uint64_t device, ApiObjectCallbacks* callbacks)
{
    const auto deviceIt = m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].find(device);
    if (deviceIt != m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Device].end())
    {
        entry.DeviceId = deviceIt->second;
    }
    else
    {
        m_objectsMissingDeviceId[device][ApiObjectType::MetaCommand].push_back(entry.ObjectAddress);
    }

    const auto allocationInfoIndexIt = m_objectAddressToAllocationInfoHandle.find(entry.ObjectAddress);
    if (allocationInfoIndexIt != m_objectAddressToAllocationInfoHandle.end())
    {
        entry.Size = m_allocationTracker.GetMemoryUsage(allocationInfoIndexIt->second);
    }

    if (entry.IsComplete())
    {
        InsertD3D12MetaCommand(entry, callbacks);
    }
    else
    {
        m_deferredEntriesByObjectAddress[entry.ObjectAddress] = entry;
    }
}

void D3D12ObjectProcessor::UpdateName(uint64_t objectAddress, std::wstring_view name, uint64_t timestamp, ApiObjectCallbacks* callbacks)
{
    for (size_t type = (size_t)ApiObjectType::Device; type < (size_t)ApiObjectType::Max; type++)
    {
        const auto objectIt = m_apiObjectTypeAddressToIds[type].find(objectAddress);
        if (objectIt != m_apiObjectTypeAddressToIds[type].end())
        {
            callbacks->UpdateApiObjectName(timestamp, (ApiObjectType)type, objectIt->second, name.data());
            break;
        }
    }
}

void D3D12ObjectProcessor::DestroyApiObject(ApiObjectType objectType, uint64_t objectAddress, uint64_t timestamp, ApiObjectCallbacks* callbacks)
{
    assert(objectType != ApiObjectType::Invalid && objectType != ApiObjectType::Max);

    // We don't store implicit resources or heaps so we have to make sure the object exists before removing it.
    const auto objectIdIt = m_apiObjectTypeAddressToIds[(size_t)objectType].find(objectAddress);

    // If looking for a device we should always have it
    assert(objectType != ApiObjectType::Device || objectIdIt != m_apiObjectTypeAddressToIds[(size_t)objectType].end());

    if (objectIdIt != m_apiObjectTypeAddressToIds[(size_t)objectType].end())
    {
        ThrowFailure(callbacks->OnObjectDestruction(timestamp, objectType, objectIdIt->second));
        m_apiObjectTypeAddressToIds[(size_t)objectType].erase(objectAddress);
    }

    // If the object is deferred for insertion then we should clear that out
    const auto deferredEntryIt = m_deferredEntriesByObjectAddress.find(objectAddress);
    if (deferredEntryIt != m_deferredEntriesByObjectAddress.end())
    {
        // First, ensure we clear out m_deferredPlacedResourcesByHeapObjectAddress appropriately
        const auto deferredEntry = deferredEntryIt->second;
        if (const ResourceEntry* deferredResourceEntry = std::get_if<ResourceEntry>(&deferredEntry))
        {
            if (deferredResourceEntry->ResourceHeapType == ResourceHeapType::ImmutableHeap)
            {
                const auto resourceObjectAddress = deferredResourceEntry->ObjectAddress;
                const auto heapObjectAddress = deferredResourceEntry->ResourceHeapData.HeapObjectAddress;
                auto& deferredPlacedResources = m_deferredPlacedResourcesByHeapObjectAddress[heapObjectAddress];

                const auto deferredPlacedResourceIt = std::find(deferredPlacedResources.begin(), deferredPlacedResources.end(), resourceObjectAddress);
                if (deferredPlacedResourceIt != deferredPlacedResources.end())
                {
                    // Swap the entry to the back of the vector to avoid the shift
                    std::iter_swap(deferredPlacedResourceIt, &deferredPlacedResources.back());
                    deferredPlacedResources.pop_back();
                }
            }
        }
        else if (const HeapEntry* deferredHeapEntry = std::get_if<HeapEntry>(&deferredEntry))
        {
            m_deferredPlacedResourcesByHeapObjectAddress.erase(deferredHeapEntry->ObjectAddress);
        }
        
        // Remove the deferred entry
        m_deferredEntriesByObjectAddress.erase(objectAddress);
    }
}

void D3D12ObjectProcessor::ProcessDeferredEntries(ApiObjectCallbacks* callbacks)
{
    // Because the InsertD3D12* functions will erase entries from m_deferredEntriesByObjectAddress,
    // we need to keep a separate list of all the deferred object addresses to iterate over instead.
    std::vector<uint64_t> objectAddresses;
    objectAddresses.reserve(m_deferredEntriesByObjectAddress.size());
    std::transform(m_deferredEntriesByObjectAddress.cbegin(), m_deferredEntriesByObjectAddress.cend(), std::back_inserter(objectAddresses), [](const auto& kvp) { return kvp.first; });

    for (auto objectAddress : objectAddresses)
    {
        auto it = m_deferredEntriesByObjectAddress.find(objectAddress);
        if (it != m_deferredEntriesByObjectAddress.end())
        {
            if (const auto* heapEntry = std::get_if<HeapEntry>(&it->second))
            {
                // Insert all deferred heap entries (ones where a corresonding AllocationInfo never came in).
                // These entries will have an incorrect GpuVirtualBaseAddress (set to ResourceEntry::InvalidValue).

                // Since DeviceId is a foreign key we'll get an error if we try to insert an value that doesn't exist.
                // This means we will not insert any deferred entries that do not have a valid DeviceId (should be very unusual).
                if (heapEntry->DeviceId.has_value())
                {
                    InsertD3D12Heap(*heapEntry, callbacks);
                }
            }
            else if (auto* pipelineStateEntry = std::get_if<PipelineStateEntry>(&it->second))
            {
                // If the AllocationInfo event wasn't stored then there were no allocations made.
                pipelineStateEntry->Size = 0;

                if (pipelineStateEntry->DeviceId.has_value())
                {
                    InsertD3D12PipelineState(*pipelineStateEntry, callbacks);
                }
            }
        }
    }

    // Resources have a dependency on the heap data, so we process deferred heaps before deferred resources.
    // Any leftover resources here never got corresponding heap info.
    for (auto objectAddress : objectAddresses)
    {
        auto it = m_deferredEntriesByObjectAddress.find(objectAddress);
        if (it != m_deferredEntriesByObjectAddress.end())
        {
            if (const auto* resourceEntry = std::get_if<ResourceEntry>(&it->second))
            {
                // Insert any remaining deferred resource entries. These entries will not have associated heap information.
                if (resourceEntry->DeviceId.has_value())
                {
                    InsertD3D12Resource(*resourceEntry, callbacks);
                }
            }
        }
    }
}

std::vector<D3D12ObjectProcessor::ApiObjectInfo> D3D12ObjectProcessor::TryGetOwningApiObjectInfos(uint64_t vidMmAlloc, bool isAllocGlobal) const
{
    std::vector<const AllocationTracker::DeviceAllocation*> deviceAllocations;

    if (isAllocGlobal)
    {
        deviceAllocations = m_allocationTracker.TryFindDeviceAllocationsByVidMmGlobalAlloc(vidMmAlloc);
    }
    else
    {
        const auto deviceAllocation = m_allocationTracker.TryFindDeviceAllocationByVidMmAlloc(vidMmAlloc);
        if (deviceAllocation)
        {
            deviceAllocations.push_back(deviceAllocation);
        }
    }

    std::vector<D3D12ObjectProcessor::ApiObjectInfo> results;
    for (const auto deviceAllocation : deviceAllocations)
    {
        if (deviceAllocation)
        {
            // Now find the AllocationInfo
            uint64_t thunkAllocation = deviceAllocation->ThunkAllocation;
            const auto allocationInfo = m_allocationTracker.TryFindAllocationInfoByThunk(thunkAllocation);

            if (allocationInfo)
            {
                // And finally look up the object info by its address
                uint64_t objectAddress = std::get<1>(*allocationInfo).object;

                if (const auto objectInfo = TryGetOwningApiObjectInfoByObjectAddress(objectAddress); objectInfo.has_value())
                {
                    results.push_back(*objectInfo);
                }
            }
        }
    }

    // If the allocation isn't global we should never have more than one entry.
    assert(results.size() <= 1 || isAllocGlobal);

    return results;
}

std::optional<D3D12ObjectProcessor::ApiObjectInfo> D3D12ObjectProcessor::TryGetOwningApiObjectInfoByObjectAddress(uint64_t objectAddress) const
{
    // If objectAddress is for an implied heap we want the committed resource.
    if (m_heapAddressToConjoinedResource.count(objectAddress) > 0
        && m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Resource].count(m_heapAddressToConjoinedResource.at(objectAddress)) > 0)
    {
        objectAddress = m_heapAddressToConjoinedResource.at(objectAddress);

        auto idIt = m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Resource].find(objectAddress);
        if (idIt != m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Resource].end())
        {
            return D3D12ObjectProcessor::ApiObjectInfo({ idIt->second, ApiObjectType::Resource });
        }
    }
    else {
        // Otherwise the owning object is itself. Look through all the different object types to find it.
        for (uint32_t apiObjectType = 0; apiObjectType < (uint32_t)ApiObjectType::Max; apiObjectType++)
        {
            auto idIt = m_apiObjectTypeAddressToIds[apiObjectType].find(objectAddress);
            if (idIt != m_apiObjectTypeAddressToIds[apiObjectType].end())
            {
                return D3D12ObjectProcessor::ApiObjectInfo({ idIt->second, (ApiObjectType)apiObjectType });
            }
        }
    }

    return std::nullopt;
}

// Fetches the first virtual address from the allocation info at the given index (in m_allocationInfos)
uint64_t D3D12ObjectProcessor::GetGpuVirtualAddressFromAllocationInfoHandle(AllocationTracker::Handle handle)
{
    return m_allocationTracker.GetAllocationInfo(handle).virtualAddressInfos[0].startAddress;
}

void D3D12ObjectProcessor::InsertD3D12Heap(const HeapEntry& entry, ApiObjectCallbacks* callbacks)
{
    // For placed resources it's possible ReportResource happens before ReportHeap.
    // If that happens we defer inserting the resource entry until now.
    const auto placedResourceIt = m_deferredPlacedResourcesByHeapObjectAddress.find(entry.ObjectAddress);
    if (placedResourceIt != m_deferredPlacedResourcesByHeapObjectAddress.end())
    {
        for (const auto& resource : placedResourceIt->second)
        {
            assert(m_deferredEntriesByObjectAddress.count(resource) == 1);
            auto& resourceEntry = std::get<ResourceEntry>(m_deferredEntriesByObjectAddress[resource]);

            assert(resourceEntry.DeviceId == entry.DeviceId);   // sanity check the heap and resource are on the same device

            resourceEntry.Placement.GpuVirtualAddress = entry.Placement.GpuVirtualAddress + resourceEntry.ResourceHeapData.ImmutableHeapOffset;

            InsertD3D12Resource(resourceEntry, callbacks);
        }

        const auto result = m_deferredPlacedResourcesByHeapObjectAddress.erase(entry.ObjectAddress);
        DBG_UNREFERENCED_LOCAL_VARIABLE(result);
        assert(result == 1);
    }

    // For committed resources the actual allocation size won't be known until the heap is created.
    // Look up the deferred committed resource entry and then insert it.
    const auto conjoinedResourceIt = m_deferredEntriesByObjectAddress.find(entry.ConjoinedResource);
    if (conjoinedResourceIt != m_deferredEntriesByObjectAddress.end())
    {
        auto& resourceEntry = std::get<ResourceEntry>(conjoinedResourceIt->second);
        assert(resourceEntry.DeviceId == entry.DeviceId);   // sanity check the heap and resource are on the same device

        auto& resourcePlacement = resourceEntry.Placement;
        resourcePlacement.GpuVirtualAddress = entry.Placement.GpuVirtualAddress;
        resourcePlacement.GpuVirtualSize = entry.Placement.GpuVirtualSize;
        resourceEntry.Desc.Alignment = entry.Desc.Alignment;
        resourcePlacement.CommittedHeapProperties = entry.Desc.Properties;
        resourcePlacement.CommittedHeapFlags = entry.Desc.Flags;

        InsertD3D12Resource(resourceEntry, callbacks);

        // Don't store implied heaps in the objects table in order to match Xbox behavior.
    }
    else
    {
        // For committed resources ReportHeap can happen before ReportResource. In this case we need to store
        // the heap information for when ReportResource happens.
        // Unfortunately we can't tell whether this is an implied heap or not (immutable heaps can have a non-zero conjoinedResource too),
        // so we have to store everything.
        // We don't need to ensure this is a new insertion because if the heap is implied then it will map to exactly one resource.
        m_heapDataByConjoinedResource[entry.ConjoinedResource] = ConjoinedHeapData{
            entry.Placement.GpuVirtualAddress,
            entry.Placement.GpuVirtualSize,
            entry.Desc.Alignment,
            entry.Desc.Properties,
            entry.Desc.Flags
        };

        UINT64 objectId;
        ThrowFailure(callbacks->OnHeapCreation(entry.Timestamp, entry.DeviceId.value(), entry.ProcessId, entry.ThreadId, &entry.Desc, &entry.Placement, &objectId));

        m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Heap][entry.ObjectAddress] = objectId;
        this->OnApiObjectInserted({ ApiObjectType::Heap, objectId, entry.ThreadId });
    }

    (void)m_deferredEntriesByObjectAddress.erase(entry.ObjectAddress);
}

void D3D12ObjectProcessor::InsertD3D12Resource(const ResourceEntry& entry, ApiObjectCallbacks* callbacks)
{
    UINT64 objectId = 0;
    const auto& placement = entry.Placement;
    switch (placement.Placement)
    {
    case ResourcePlacement::Committed:
    {
        ObjectPlacementInfo objectPlacement{ placement.GpuVirtualAddress, placement.GpuVirtualSize };
        ThrowFailure(callbacks->OnCommittedResourceCreation(entry.Timestamp, entry.DeviceId.value(), entry.ProcessId, entry.ThreadId, &entry.Desc, &objectPlacement, &placement.CommittedHeapProperties, placement.CommittedHeapFlags, &objectId));
        break;
    }
    case ResourcePlacement::Placed:
    {
        ObjectPlacementInfo objectPlacement{ placement.GpuVirtualAddress, placement.GpuVirtualSize };
        ThrowFailure(callbacks->OnPlacedResourceCreation(entry.Timestamp, entry.DeviceId.value(), entry.ProcessId, entry.ThreadId, &entry.Desc, &objectPlacement, &objectId));
        break;
    }
    case ResourcePlacement::Reserved:
    {
        ReservedResourceInfo reserved{ placement.NumTilesForResource, placement.NumPackedMips, placement.NumTilesForPackedMips };
        ThrowFailure(callbacks->OnReservedResourceCreation(entry.Timestamp, entry.DeviceId.value(), entry.ProcessId, entry.ThreadId, &entry.Desc, &reserved, &objectId));
        break;
    }
    }

    m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::Resource][entry.ObjectAddress] = objectId;
    this->OnApiObjectInserted({ ApiObjectType::Resource, objectId, entry.ThreadId });

    (void)m_deferredEntriesByObjectAddress.erase(entry.ObjectAddress);
}

void D3D12ObjectProcessor::InsertD3D12PipelineState(const PipelineStateEntry& entry, ApiObjectCallbacks* callbacks)
{
    UINT64 objectId;
    ObjectPlacementInfo placement{ entry.GpuVirtualBaseAddress, entry.Size };
    ThrowFailure(callbacks->OnPipelineStateCreation(entry.Timestamp, entry.DeviceId.value(), entry.ProcessId, entry.ThreadId, &placement, &objectId));

    m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::PipelineState][entry.ObjectAddress] = objectId;
    this->OnApiObjectInserted({ ApiObjectType::PipelineState, objectId, entry.ThreadId });

    (void)m_deferredEntriesByObjectAddress.erase(entry.ObjectAddress);
}

void D3D12ObjectProcessor::InsertD3D12StateObject(const StateObjectEntry& entry, ApiObjectCallbacks* callbacks)
{
    UINT64 objectId;
    ObjectPlacementInfo placement{ entry.GpuVirtualBaseAddress, entry.Size };
    ThrowFailure(callbacks->OnStateObjectCreation(entry.Timestamp, entry.DeviceId.value(), entry.ProcessId, entry.ThreadId, &placement, &objectId));

    m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::StateObject][entry.ObjectAddress] = objectId;
    this->OnApiObjectInserted({ ApiObjectType::StateObject, objectId, entry.ThreadId });

    (void)m_deferredEntriesByObjectAddress.erase(entry.ObjectAddress);
}

void D3D12ObjectProcessor::InsertD3D12CommandAllocator(const CommandAllocatorEntry& entry, ApiObjectCallbacks* callbacks)
{
    UINT64 objectId;
    ObjectPlacementInfo placement{ entry.GpuVirtualBaseAddress, entry.Size };
    ThrowFailure(callbacks->OnCommandAllocatorCreation(entry.Timestamp, entry.DeviceId.value(), entry.ProcessId, entry.ThreadId, static_cast<D3D12_COMMAND_LIST_TYPE>(entry.CommandListType), &placement, &objectId));

    m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::CommandAllocator][entry.ObjectAddress] = objectId;
    this->OnApiObjectInserted({ ApiObjectType::CommandAllocator, objectId, entry.ThreadId });

    (void)m_deferredEntriesByObjectAddress.erase(entry.ObjectAddress);
}

void D3D12ObjectProcessor::InsertD3D12DescriptorHeap(const DescriptorHeapEntry& entry, ApiObjectCallbacks* callbacks)
{
    UINT64 objectId;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(entry.DescriptorHeapType);
    desc.NumDescriptors = entry.NumDescriptors;
    desc.Flags = static_cast<D3D12_DESCRIPTOR_HEAP_FLAGS>(entry.DescriptorHeapFlags);
    desc.NodeMask = entry.NodeMask;
    ObjectPlacementInfo placement{ entry.GpuVirtualBaseAddress, entry.Size };
    ThrowFailure(callbacks->OnDescriptorHeapCreation(entry.Timestamp, entry.DeviceId.value(), entry.ProcessId, entry.ThreadId, &desc, &placement, &objectId));

    m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::DescriptorHeap][entry.ObjectAddress] = objectId;
    this->OnApiObjectInserted({ ApiObjectType::DescriptorHeap, objectId, entry.ThreadId });

    (void)m_deferredEntriesByObjectAddress.erase(entry.ObjectAddress);
}

void D3D12ObjectProcessor::InsertD3D12MetaCommand(const MetaCommandEntry& entry, ApiObjectCallbacks* callbacks)
{
    UINT64 objectId;
    ObjectPlacementInfo placement{ entry.GpuVirtualBaseAddress, entry.Size };
    ThrowFailure(callbacks->OnMetaCommandCreation(entry.Timestamp, entry.DeviceId.value(), entry.ProcessId, entry.ThreadId, entry.CommandId, &placement, &objectId));

    m_apiObjectTypeAddressToIds[(size_t)ApiObjectType::MetaCommand][entry.ObjectAddress] = objectId;
    this->OnApiObjectInserted({ ApiObjectType::MetaCommand, objectId, entry.ThreadId });

    (void)m_deferredEntriesByObjectAddress.erase(entry.ObjectAddress);
}

void D3D12ObjectProcessor::UpdateApiObjectSizeAndAddress(uint64_t timestamp, ApiObjectType objectType, UINT64 id, uint64_t memoryUsage, uint64_t baseAddress, ApiObjectCallbacks* callbacks) const
{
    ThrowFailure(callbacks->UpdateApiObjectSizeAndAddress(timestamp, objectType, id, memoryUsage, baseAddress));
}
