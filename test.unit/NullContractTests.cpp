// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <memory>

// MarkerOp.h (pulled in transitively) expands the D3D12_MARKER_API_* constants
// from the D3D12 ETW manifest headers, so those must be included first.
#include <external/d3d/d3d12.h>
#include <D3D12Events.h>

#include <DxEtwLibrary/DxEtwEventHandler.h>

using namespace DirectX::Etw;

#include "NoOpCallbacks.h"

namespace
{
    // Identifies which constructor argument to pass as null. The eight callbacks
    // and the timestamp converter are required; the diagnostics sink and the
    // runtime-failure callbacks are optional.
    enum class NullSlot
    {
        None,
        ApiObject,
        PixCounter,
        Residency,
        PixEvent,
        Monitor,
        GpuTimings,
        DxgkObject,
        PipelineState,
        DiagnosticsSink,
        RuntimeFailure,
        TimestampConverter,
    };

    std::unique_ptr<DxEtwEventHandler> MakeHandlerWithNull(NullSlot slot)
    {
        Options options{};
        return std::make_unique<DxEtwEventHandler>(
            options,
            true,
            slot == NullSlot::ApiObject ? nullptr : std::make_unique<NoOpApiObjectCallbacks>(),
            slot == NullSlot::PixCounter ? nullptr : std::make_unique<NoOpPixCounterCallbacks>(),
            slot == NullSlot::Residency ? nullptr : std::make_unique<NoOpResidencyEventCallbacks>(),
            slot == NullSlot::PixEvent ? nullptr : std::make_unique<NoOpPixEventCallbacks>(),
            slot == NullSlot::Monitor ? nullptr : std::make_unique<NoOpMonitorEventCallbacks>(),
            slot == NullSlot::GpuTimings ? nullptr : std::make_unique<NoOpGpuTimingsCallbacks>(),
            slot == NullSlot::DxgkObject ? nullptr : std::make_unique<NoOpDxgkObjectCallbacks>(),
            slot == NullSlot::PipelineState ? nullptr : std::make_unique<NoOpPipelineStateEventCallbacks>(),
            slot == NullSlot::DiagnosticsSink ? nullptr : std::make_unique<NullDiagnosticsSink>(),
            slot == NullSlot::RuntimeFailure ? nullptr : std::make_unique<NoOpRuntimeFailureCallbacks>(),
            slot == NullSlot::TimestampConverter ? nullptr : std::make_unique<NoOpTimestampConverter>(),
            1234u);
    }
}

TEST(CallbackNullContract, AllCallbacksPresentSucceeds)
{
    EXPECT_NO_THROW({
        auto handler = MakeHandlerWithNull(NullSlot::None);
        EXPECT_NE(handler, nullptr);
    });
}

// The diagnostics sink is the one optional argument: a null sink is fine and the
// processor substitutes a no-op.
TEST(CallbackNullContract, NullDiagnosticsSinkIsAllowed)
{
    EXPECT_NO_THROW(MakeHandlerWithNull(NullSlot::DiagnosticsSink));
}

// The runtime-failure callbacks are also optional: a null is fine and means
// journal-entry events are simply ignored.
TEST(CallbackNullContract, NullRuntimeFailureCallbacksIsAllowed)
{
    EXPECT_NO_THROW(MakeHandlerWithNull(NullSlot::RuntimeFailure));
}

// Each required argument being null must be rejected at construction. (The thrown
// exception type differs by UsePixUtility build mode, so assert only that it
// throws.)
TEST(CallbackNullContract, NullApiObjectCallbacksThrows)        { EXPECT_ANY_THROW(MakeHandlerWithNull(NullSlot::ApiObject)); }
TEST(CallbackNullContract, NullPixCounterCallbacksThrows)       { EXPECT_ANY_THROW(MakeHandlerWithNull(NullSlot::PixCounter)); }
TEST(CallbackNullContract, NullResidencyEventCallbacksThrows)   { EXPECT_ANY_THROW(MakeHandlerWithNull(NullSlot::Residency)); }
TEST(CallbackNullContract, NullPixEventCallbacksThrows)         { EXPECT_ANY_THROW(MakeHandlerWithNull(NullSlot::PixEvent)); }
TEST(CallbackNullContract, NullMonitorEventCallbacksThrows)     { EXPECT_ANY_THROW(MakeHandlerWithNull(NullSlot::Monitor)); }
TEST(CallbackNullContract, NullGpuTimingsCallbacksThrows)       { EXPECT_ANY_THROW(MakeHandlerWithNull(NullSlot::GpuTimings)); }
TEST(CallbackNullContract, NullDxgkObjectCallbacksThrows)       { EXPECT_ANY_THROW(MakeHandlerWithNull(NullSlot::DxgkObject)); }
TEST(CallbackNullContract, NullPipelineStateEventCallbacksThrows){ EXPECT_ANY_THROW(MakeHandlerWithNull(NullSlot::PipelineState)); }
TEST(CallbackNullContract, NullTimestampConverterThrows)        { EXPECT_ANY_THROW(MakeHandlerWithNull(NullSlot::TimestampConverter)); }
