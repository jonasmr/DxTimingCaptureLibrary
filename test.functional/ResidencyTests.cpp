// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <algorithm>
#include <set>
#include <vector>

#include "EtwSession.h"
#include "RecordingApiObjectCallbacks.h"
#include "RecordingResidencyEventCallbacks.h"

using namespace DirectX::Etw;

namespace
{
    constexpr UINT64 TestHeapSize = 256ull << 20;

    ComPtr<ID3D12Heap> CreateBufferHeap(ID3D12Device* device)
    {
        D3D12_HEAP_DESC desc{};
        desc.SizeInBytes = TestHeapSize;
        desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        ComPtr<ID3D12Heap> heap;
        ThrowFailure(device->CreateHeap(&desc, IID_PPV_ARGS(&heap)));
        return heap;
    }

    ComPtr<ID3D12Resource> CreateBufferInHeap(ID3D12Device* device, ID3D12Heap* heap)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = TestHeapSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        ThrowFailure(device->CreatePlacedResource(heap, 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer)));
        return buffer;
    }

    // Overcommits video memory so VidMm commits the overflow heaps to system memory, frees
    // the heaps that landed in video memory, then keeps the GPU using the overflow heaps
    // until VidMm promotes at least one of them back to video memory (or the timeout hits).
    // Returns a skip reason if the adapter can't run this scenario.
    const char* RunOvercommitAndPromote(const RecordingResidencyEventCallbacks* residency)
    {
        ComPtr<ID3D12Device> device;
        ThrowFailure(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

        D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
        ThrowFailure(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture)));
        if (architecture.UMA)
            return "UMA adapters have no separate video memory to promote into";

        ComPtr<IDXGIFactory4> factory;
        ThrowFailure(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter3> adapter;
        ThrowFailure(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));

        DXGI_ADAPTER_DESC1 adapterDesc{};
        ThrowFailure(adapter->GetDesc1(&adapterDesc));
        if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            return "software adapters don't page between memory segment groups";

        DXGI_QUERY_VIDEO_MEMORY_INFO memoryInfo{};
        ThrowFailure(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryInfo));

        // Allocate past the budget so the last heaps can't fit in video memory.
        std::vector<ComPtr<ID3D12Heap>> heaps;
        for (UINT64 total = 0; total < memoryInfo.Budget + (1ull << 30); total += TestHeapSize)
            heaps.push_back(CreateBufferHeap(device.Get()));

        // Free the first half, which VidMm placed in video memory, to make room for promotion.
        heaps.erase(heaps.begin(), heaps.begin() + heaps.size() / 2);

        std::vector<ComPtr<ID3D12Resource>> buffers;
        for (auto& heap : heaps)
            buffers.push_back(CreateBufferInHeap(device.Get(), heap.Get()));

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        ComPtr<ID3D12CommandQueue> queue;
        ThrowFailure(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator;
        ThrowFailure(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> commandList;
        ThrowFailure(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)));
        for (size_t i = 0; i + 1 < buffers.size(); ++i)
            commandList->CopyBufferRegion(buffers[i + 1].Get(), 0, buffers[i].Get(), TestHeapSize / 2, 4096);
        ThrowFailure(commandList->Close());

        ComPtr<ID3D12Fence> fence;
        ThrowFailure(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        UINT64 fenceValue = 0;

        // VidMm only promotes allocations the GPU is actively using. The library processes
        // events with some latency, so keep the GPU busy until a promotion is reported.
        const ULONGLONG start = GetTickCount64();
        while (residency->LocalSegmentGroupChangeCount == 0 && GetTickCount64() - start < 30000)
        {
            ID3D12CommandList* lists[] = { commandList.Get() };
            queue->ExecuteCommandLists(1, lists);
            ThrowFailure(queue->Signal(fence.Get(), ++fenceValue));
            ThrowFailure(fence->SetEventOnCompletion(fenceValue, nullptr));
            Sleep(100);
        }

        return nullptr;
    }
}

// Regression test: VidMm reports moves between system and video memory with
// PagingOpVirtualTransfer. The library must turn those into OnAllocationSegmentGroupChanges,
// otherwise objects that start in system memory under memory pressure look like they stay
// there forever even after VidMm promotes them.
TEST(Residency, PromotionFromSystemMemoryReportsSegmentGroupChange)
{
    auto apiCallbacks = std::make_unique<RecordingApiObjectCallbacks>();
    auto* api = apiCallbacks.get();
    auto residencyCallbacks = std::make_unique<RecordingResidencyEventCallbacks>();
    auto* residency = residencyCallbacks.get();

    SimpleEtwSession session;
    session.SetApiObjectCallbacks(std::move(apiCallbacks));
    session.SetResidencyEventCallbacks(std::move(residencyCallbacks));
    session.Begin();
    const char* skipReason = RunOvercommitAndPromote(residency);
    session.End();
    session.RethrowConsumerException();

    if (skipReason)
        GTEST_SKIP() << skipReason;

    std::set<UINT64> heapIds;
    std::set<UINT64> heapsStartedInSystemMemory;
    for (auto& heap : api->Heaps)
    {
        if (heap.Desc.SizeInBytes != TestHeapSize)
            continue;
        heapIds.insert(heap.ObjectId);
        if (heap.Placement.ResidentSegmentGroup == MemorySegmentGroup::NonLocal)
            heapsStartedInSystemMemory.insert(heap.ObjectId);
    }

    if (heapsStartedInSystemMemory.empty())
        GTEST_SKIP() << "VidMm didn't place any overcommitted heap in system memory";

    // Heaps VidMm paged into video memory, according to the PageIn residency operations
    // (which the library reported even before the fix).
    std::set<UINT64> promotedHeaps;
    for (auto& operation : residency->Operations)
    {
        if (operation.OperationType == ResidencyOperationType::PageIn && heapsStartedInSystemMemory.contains(operation.ObjectId))
            promotedHeaps.insert(operation.ObjectId);
    }

    if (promotedHeaps.empty())
        GTEST_SKIP() << "VidMm didn't promote any heap from system memory within the timeout";

    for (UINT64 heapId : promotedHeaps)
    {
        auto change = std::ranges::find_if(residency->SegmentGroupChanges, [&](const AllocationSegmentGroupChange& c)
        {
            return c.ObjectId == heapId && c.SegmentGroup == MemorySegmentGroup::Local;
        });
        const bool reported = change != residency->SegmentGroupChanges.end();
        ASSERT_TRUE(reported) << "heap " << heapId << " was paged in but no segment group change was reported";
        EXPECT_EQ(ApiObjectType::Heap, change->ObjectType);
        EXPECT_NE(0, change->Timestamp);
    }

    for (auto& change : residency->SegmentGroupChanges)
    {
        if (change.ObjectType == ApiObjectType::Heap)
            EXPECT_TRUE(heapIds.contains(change.ObjectId)) << "segment group change for unknown heap " << change.ObjectId;
    }
}
