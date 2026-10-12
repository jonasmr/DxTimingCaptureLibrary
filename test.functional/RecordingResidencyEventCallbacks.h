// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <algorithm>
#include <atomic>
#include <span>
#include <vector>

#include <DxTimingCaptureLibrary/EtwCallbacks.h>

using namespace DirectX::Etw;

// A test double for ResidencyEventCallbacks that records every callback the library makes.
// Tests inspect the vectors after SimpleEtwSession::End() has joined the consumer thread, so
// no locking is needed (see RecordingApiObjectCallbacks). While the session is running, only
// the atomic counter may be read.
class RecordingResidencyEventCallbacks : public ResidencyEventCallbacks
{
public:
    std::atomic<size_t> LocalSegmentGroupChangeCount = 0;

    std::vector<ResidencyOperation> Operations;
    std::vector<DemotedAllocation> Demotions;
    std::vector<AllocationMigration> Migrations;
    std::vector<AllocationSegmentGroupChange> SegmentGroupChanges;

    HRESULT OnResidencyOperation(const ResidencyOperation* data) override
    {
        Operations.push_back(*data);
        return S_OK;
    }

    HRESULT OnDemotedAllocations(const DemotedAllocation* data, UINT32 count) override
    {
        Demotions.insert(Demotions.end(), data, data + count);
        return S_OK;
    }

    HRESULT OnAllocationMigrations(const AllocationMigration* data, UINT32 count) override
    {
        Migrations.insert(Migrations.end(), data, data + count);
        return S_OK;
    }

    HRESULT OnAllocationSegmentGroupChanges(const AllocationSegmentGroupChange* data, UINT32 count) override
    {
        SegmentGroupChanges.insert(SegmentGroupChanges.end(), data, data + count);
        LocalSegmentGroupChangeCount += std::ranges::count(
            std::span(data, count), MemorySegmentGroup::Local, &AllocationSegmentGroupChange::SegmentGroup);
        return S_OK;
    }
};
