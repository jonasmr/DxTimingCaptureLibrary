// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <vector>

// MarkerOp.h (pulled in transitively) expands the D3D12_MARKER_API_* constants
// from the D3D12 ETW manifest headers, so those must be included first.
#include <d3d12.h>
#include <D3D12Events.h>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>
#include <DxTimingCaptureLibrary/EtwDispatcher.h>

using namespace DirectX::Etw;

#include "NoOpCallbacks.h"

// Drives the event handler with the sequence of D3D12 + DXGK events VidMm emits when a heap is
// committed to system memory, promoted to video memory and evicted again, and checks that the
// resident segment group of the heap is reported each time.
namespace
{
    constexpr uint32_t TargetProcessId = 4321u;

    constexpr uint64_t D3D12Device = 0xD000;
    constexpr uint64_t D3D12Heap = 0xE000;
    constexpr uint32_t ThunkAllocation = 0x40000040;
    constexpr uint64_t DxgAdapter = 0xA000;
    constexpr uint64_t VidMmAlloc = 0xB000;
    constexpr uint64_t VidMmGlobalAlloc = 0xC000;

    // VidMm segment logging ids (segment index + 1). 0 is the system memory backing store.
    constexpr uint32_t LocalSegment = 1;
    constexpr uint32_t NonLocalSegment = 2;

    class RecordingResidencyCallbacks : public NoOpResidencyEventCallbacks
    {
    public:
        std::vector<AllocationSegmentGroupChange> GroupChanges;
        std::vector<DemotedAllocation> Demotions;

        HRESULT OnAllocationSegmentGroupChanges(const AllocationSegmentGroupChange* data, UINT32 count) override
        {
            GroupChanges.insert(GroupChanges.end(), data, data + count);
            return S_OK;
        }

        HRESULT OnDemotedAllocations(const DemotedAllocation* data, UINT32 count) override
        {
            Demotions.insert(Demotions.end(), data, data + count);
            return S_OK;
        }
    };

    class RecordingApiObjectCallbacks : public NoOpApiObjectCallbacks
    {
    public:
        UINT64 HeapId = 0;

        HRESULT OnHeapCreation(INT64 timestamp, UINT64 deviceId, UINT32 processId, UINT32 threadId, const D3D12_HEAP_DESC* desc, const ObjectPlacementInfo* placement, UINT64* objectId) override
        {
            NoOpApiObjectCallbacks::OnHeapCreation(timestamp, deviceId, processId, threadId, desc, placement, objectId);
            HeapId = *objectId;
            return S_OK;
        }
    };

    class Payload
    {
    public:
        std::vector<uint8_t> Bytes;

        template<typename TValue>
        Payload& Add(TValue value)
        {
            auto const* raw = reinterpret_cast<uint8_t const*>(&value);
            Bytes.insert(Bytes.end(), raw, raw + sizeof(value));
            return *this;
        }
    };

    class Harness
    {
    public:
        RecordingResidencyCallbacks Residency;
        RecordingApiObjectCallbacks ApiObjects;
        NoOpTimestampConverter Converter;
        std::unique_ptr<DxTimingCaptureEventHandler> Handler;
        uint64_t Timestamp = 100;

        explicit Harness(bool trackApiObjects = true)
        {
            DxTimingCaptureEventCallbacks callbacks{};
            callbacks.ResidencyEventCallbacks = &Residency;
            callbacks.ApiObjectCallbacks = &ApiObjects;

            DxTimingCaptureLibraryOptions options{};
            options.TrackApiObjects = trackApiObjects;

            Handler = DxTimingCaptureEventHandler::Create(TargetProcessId, options, callbacks, Converter);
        }

        void Dispatch(GUID const& provider, uint16_t eventId, Payload& payload, uint32_t processId)
        {
            EVENT_RECORD record{};
            record.EventHeader.ProviderId = provider;
            record.EventHeader.EventDescriptor.Id = eventId;
            record.EventHeader.Flags = EVENT_HEADER_FLAG_64_BIT_HEADER;
            record.EventHeader.ProcessId = processId;
            record.EventHeader.TimeStamp.QuadPart = static_cast<LONGLONG>(Timestamp++);
            record.UserData = payload.Bytes.data();
            record.UserDataLength = static_cast<USHORT>(payload.Bytes.size());

            Handler->HandleEventRecord(&record);
        }

        void D3D12(uint16_t eventId, Payload payload) { Dispatch(Direct3D12EtwProviderGuid, eventId, payload, TargetProcessId); }

        // DXGK residency events come from the system process.
        void Dxgk(uint16_t eventId, Payload payload) { Dispatch(DxgkControlGuid, eventId, payload, 4); }

        void CreateHeap()
        {
            D3D12(EventD3D12CreateDevice_value, Payload{}
                .Add(D3D12Device)
                .Add(uint32_t{ D3D_FEATURE_LEVEL_12_0 })
                .Add(uint32_t{ 1 })         // kmAdapter
                .Add(uint64_t{ 0xF000 })    // umAdapter
                .Add(uint64_t{ 0 })         // umAdapterVersion
                .Add(uint32_t{ 2 })         // kmDevice
                .Add(uint64_t{ 0 }));       // umDeviceVersion

            D3D12(EventD3D12AllocationInfo_value, Payload{}
                .Add(D3D12Device)
                .Add(D3D12Heap)
                .Add(uint32_t{ 1 })
                .Add(VirtualAddressInfos{ 0, 0, 0x100000, 0x200000 })
                .Add(uint32_t{ 1 })
                .Add(KMTInfos{ 0, ThunkAllocation, 0, 0x100000 }));

            D3D12(EventD3D12CreateHeap_value, Payload{}
                .Add(D3D12Device)
                .Add(D3D12Heap)
                .Add(uint64_t{ 0x100000 })  // sizeInBytes
                .Add(uint64_t{ 0x10000 })   // alignment
                .Add(uint32_t{ D3D12_HEAP_TYPE_DEFAULT })
                .Add(uint32_t{ D3D12_CPU_PAGE_PROPERTY_UNKNOWN })
                .Add(uint32_t{ D3D12_MEMORY_POOL_UNKNOWN })
                .Add(uint32_t{ 1 })         // creationNodeMask
                .Add(uint32_t{ 1 })         // visibleNodeMask
                .Add(uint32_t{ D3D12_HEAP_FLAG_NONE })
                .Add(uint64_t{ 0 })         // conjoinedResource: explicit heap
                .Add(ThunkAllocation)
                .Add(uint32_t{ 0 }));       // kmResource
        }

        void CreateKernelAllocation(uint32_t preferredSegmentId)
        {
            Dxgk(EventCreateDeviceAllocation_value, Payload{}
                .Add(uint64_t{ TargetProcessId })
                .Add(uint64_t{ 0x9000 })    // device
                .Add(DxgAdapter)
                .Add(VidMmAlloc)
                .Add(VidMmGlobalAlloc)
                .Add(uint64_t{ 0 })         // dxgResource
                .Add(uint64_t{ 0 })         // dxgSharedResource
                .Add(uint64_t{ ThunkAllocation })
                .Add(uint64_t{ 0 })         // thunkResource
                .Add(uint64_t{ 0 })         // privateRuntimeResourceHandle
                .Add(uint64_t{ 0x100000 })  // virtualAddress
                .Add(uint64_t{ 0 }));       // processAllocDetails

            Dxgk(EventCreateAdapterAllocation_value, Payload{}
                .Add(uint64_t{ TargetProcessId })
                .Add(uint64_t{ 0x9000 })    // hDevice
                .Add(DxgAdapter)
                .Add(uint32_t{ 0 })         // flags
                .Add(uint64_t{ 0x100000 })  // allocSize
                .Add(uint32_t{ 0x10000 })   // alignment
                .Add(uint32_t{ 0 })         // readSegment
                .Add(uint32_t{ 0 })         // writeSegment
                .Add(preferredSegmentId)    // preferredSegment (SegmentId0 is the low 5 bits)
                .Add(uint32_t{ 0 })         // hintedBank
                .Add(uint32_t{ 0 })         // evictionSegment
                .Add(uint32_t{ 0 })         // priority
                .Add(VidMmGlobalAlloc)
                .Add(uint64_t{ 0 })         // hDxgGlobalAlloc
                .Add(uint64_t{ 0 })         // hDxgSharedResource
                .Add(uint32_t{ 0 }).Add(uint32_t{ 0 }).Add(uint32_t{ 0 }).Add(uint32_t{ 0 }).Add(uint32_t{ 0 })
                .Add(uint32_t{ 0 }).Add(uint32_t{ 0 }).Add(uint32_t{ 0 }).Add(uint32_t{ 0 }).Add(uint32_t{ 0 })
                .Add(uint8_t{ 0 })          // backingStoreWasPinned
                .Add(uint64_t{ 0 })         // pSectionObject
                .Add(uint16_t{ 0 })         // physicalAdapterIndex
                .Add(uint8_t{ 0 }));        // pageTableOrDirection
        }

        void ReportAdapter()
        {
            Dxgk(EventDpiReportAdapter_value, Payload{}
                .Add(DxgAdapter)
                .Add(uint32_t{ 0 })         // configSpaceSize
                .Add(uint32_t{ 0 })         // chainUid
                .Add(uint32_t{ 1 })         // numberOfLinksInChain
                .Add(uint32_t{ 0 })         // leadLink
                .Add(uint32_t{ 0 })         // busType: undefined
                .Add(uint32_t{ 0 }).Add(uint32_t{ 0 }).Add(uint32_t{ 0 }).Add(uint32_t{ 0 }).Add(uint32_t{ 0 })
                .Add(uint64_t{ 0x1234 }));  // adapterLuid
        }

        void ReportSegment(uint32_t segmentId, D3DKMT_MEMORY_SEGMENT_GROUP group)
        {
            Dxgk(EventReportSegment_value, Payload{}
                .Add(segmentId)
                .Add(DxgAdapter)
                .Add(uint64_t{ 0 })         // baseAddress
                .Add(uint64_t{ 0 })         // cpuTranslatedAddress
                .Add(uint64_t{ 0x40000000 })// size
                .Add(uint32_t{ 1 })         // numberOfBanks
                .Add(uint32_t{ 0 })         // flags
                .Add(uint64_t{ 0x40000000 })// commitLimit
                .Add(uint64_t{ 0 })         // systemMemoryEndAddress
                .Add(static_cast<uint8_t>(group)));
        }

        void ReportSegments()
        {
            ReportSegment(LocalSegment, D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL);
            ReportSegment(NonLocalSegment, D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL);
        }

        void SysmemCommit(uint32_t segmentId)
        {
            Dxgk(EventPagingOpSysmemCommit_value, Payload{}
                .Add(DxgAdapter)
                .Add(VidMmGlobalAlloc)
                .Add(segmentId));
        }

        void Transfer(DXGK_MEMORY_TRANSFER_DIRECTION direction, uint32_t sourceSegment, uint32_t destinationSegment, uint64_t allocationOffset = 0)
        {
            Dxgk(EventPagingOpVirtualTransfer_value, Payload{}
                .Add(DxgAdapter)
                .Add(uint64_t{ 0x7000 })    // hDmaBuffer
                .Add(uint32_t{ 0 })         // continueNextBuffer
                .Add(VidMmGlobalAlloc)
                .Add(allocationOffset)
                .Add(uint64_t{ 0x10000 })   // transferSize
                .Add(sourceSegment)
                .Add(destinationSegment)
                .Add(uint64_t{ 0 })         // sourceVirtualAddress
                .Add(uint64_t{ 0 })         // destinationVirtualAddress
                .Add(uint64_t{ 0 })         // sourcePageTable
                .Add(static_cast<uint32_t>(direction))
                .Add(uint32_t{ 0 })         // transferFlags
                .Add(uint64_t{ 0 })         // destinationPageTable
                .Add(uint64_t{ 0 })         // sourceSegmentOffset
                .Add(uint64_t{ 0 }));       // destinationSegmentOffset
        }

        void Discard(uint32_t segmentId)
        {
            Dxgk(EventPagingOpDiscard_value, Payload{}
                .Add(DxgAdapter)
                .Add(uint64_t{ 0x7000 })    // hDmaBuffer
                .Add(uint32_t{ 0 })         // continueNextBuffer
                .Add(VidMmGlobalAlloc)
                .Add(uint32_t{ 0 })         // flags
                .Add(segmentId)
                .Add(uint64_t{ 0 }));       // segmentOffset
        }

        std::vector<MemorySegmentGroup> ReportedGroups() const
        {
            std::vector<MemorySegmentGroup> groups;
            for (const auto& change : Residency.GroupChanges)
            {
                EXPECT_EQ(ApiObjects.HeapId, change.ObjectId);
                EXPECT_EQ(ApiObjectType::Heap, change.ObjectType);
                groups.push_back(change.SegmentGroup);
            }
            return groups;
        }
    };
}

TEST(ResidencySegmentGroupTests, TransfersBetweenVideoAndSystemMemoryAreReported)
{
    Harness h;
    h.CreateHeap();
    ASSERT_NE(0u, h.ApiObjects.HeapId);
    h.CreateKernelAllocation(0);
    h.ReportAdapter();
    h.ReportSegments();

    // Created in system memory because video memory is overcommitted...
    h.SysmemCommit(NonLocalSegment);
    // ...promoted to video memory once there's room...
    h.Transfer(DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL, NonLocalSegment, LocalSegment);
    // ...and evicted to its backing store again.
    h.Transfer(DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM, LocalSegment, 0);

    const std::vector<MemorySegmentGroup> expected{ MemorySegmentGroup::NonLocal, MemorySegmentGroup::Local, MemorySegmentGroup::NonLocal };
    EXPECT_EQ(expected, h.ReportedGroups());
}

TEST(ResidencySegmentGroupTests, OnlyTheFirstChunkOfATransferIsConsidered)
{
    Harness h;
    h.CreateHeap();
    h.CreateKernelAllocation(0);
    h.ReportAdapter();
    h.ReportSegments();

    h.Transfer(DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL, NonLocalSegment, LocalSegment, 0);
    h.Transfer(DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL, NonLocalSegment, LocalSegment, 0x10000);
    // A later chunk claiming a different destination must not flip the group.
    h.Transfer(DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM, LocalSegment, 0, 0x20000);

    const std::vector<MemorySegmentGroup> expected{ MemorySegmentGroup::Local };
    EXPECT_EQ(expected, h.ReportedGroups());
}

TEST(ResidencySegmentGroupTests, LocalToLocalTransferToBackingStoreIsIgnored)
{
    Harness h;
    h.CreateHeap();
    h.CreateKernelAllocation(0);
    h.ReportAdapter();
    h.ReportSegments();

    h.Transfer(DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL, NonLocalSegment, LocalSegment);
    h.Transfer(DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL, LocalSegment, 0);

    const std::vector<MemorySegmentGroup> expected{ MemorySegmentGroup::Local };
    EXPECT_EQ(expected, h.ReportedGroups());
}

// Segments can be reported before the adapter itself (e.g. in rundown); they must still be usable.
TEST(ResidencySegmentGroupTests, SegmentsReportedBeforeTheAdapterAreUsed)
{
    Harness h;
    h.CreateHeap();
    h.CreateKernelAllocation(0);
    h.ReportSegments();
    h.ReportAdapter();

    h.Transfer(DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL, NonLocalSegment, LocalSegment);

    const std::vector<MemorySegmentGroup> expected{ MemorySegmentGroup::Local };
    EXPECT_EQ(expected, h.ReportedGroups());
}

TEST(ResidencySegmentGroupTests, TransferOutOfThePreferredSegmentIsReportedAsDemotion)
{
    Harness h;
    h.CreateHeap();
    h.CreateKernelAllocation(LocalSegment);
    h.ReportAdapter();
    h.ReportSegments();

    h.Transfer(DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL, NonLocalSegment, LocalSegment);
    EXPECT_TRUE(h.Residency.Demotions.empty());

    // Demoted to an aperture segment (as opposed to evicted to the backing store).
    h.Transfer(DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM, LocalSegment, NonLocalSegment);

    ASSERT_EQ(1u, h.Residency.Demotions.size());
    EXPECT_EQ(h.ApiObjects.HeapId, h.Residency.Demotions[0].ObjectId);

    const std::vector<MemorySegmentGroup> expected{ MemorySegmentGroup::Local, MemorySegmentGroup::NonLocal };
    EXPECT_EQ(expected, h.ReportedGroups());
}

TEST(ResidencySegmentGroupTests, NothingIsReportedWithoutTrackApiObjects)
{
    Harness h(false);
    h.CreateHeap();
    h.CreateKernelAllocation(0);
    h.ReportAdapter();
    h.ReportSegments();
    h.Transfer(DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL, NonLocalSegment, LocalSegment);
    h.Discard(LocalSegment);
    h.Handler->OnDataComplete();

    EXPECT_TRUE(h.Residency.GroupChanges.empty());
    EXPECT_TRUE(h.Residency.Demotions.empty());
}

// Allocations whose contents needn't be preserved are evicted with a discard rather than a transfer.
TEST(ResidencySegmentGroupTests, DiscardFromVideoMemoryIsReportedAsEviction)
{
    Harness h;
    h.CreateHeap();
    h.CreateKernelAllocation(0);
    h.ReportAdapter();
    h.ReportSegments();

    h.Transfer(DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL, NonLocalSegment, LocalSegment);
    h.Discard(LocalSegment);
    // Being paged back in afterwards must be reported too.
    h.Transfer(DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL, 0, LocalSegment);

    const std::vector<MemorySegmentGroup> expected{ MemorySegmentGroup::Local, MemorySegmentGroup::NonLocal, MemorySegmentGroup::Local };
    EXPECT_EQ(expected, h.ReportedGroups());
}

// Without a NonLocal segment VidMm budgets the system memory segment as Local, so evicting to the backing
// store doesn't change the group.
TEST(ResidencySegmentGroupTests, EvictionOnAdapterWithoutNonLocalSegmentStaysLocal)
{
    Harness h;
    h.CreateHeap();
    h.CreateKernelAllocation(0);
    h.ReportAdapter();
    h.ReportSegment(LocalSegment, D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL);

    h.Transfer(DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL, 0, LocalSegment);
    h.Transfer(DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM, LocalSegment, 0);
    h.Transfer(DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL, 0, LocalSegment);
    h.Discard(LocalSegment);

    const std::vector<MemorySegmentGroup> expected{ MemorySegmentGroup::Local };
    EXPECT_EQ(expected, h.ReportedGroups());
}
