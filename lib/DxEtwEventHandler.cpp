// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <PixEventDecoder.h>

#include <DxEtwLibrary/D3D12EtwEventStructs.h>
#include <DxEtwLibrary/EventTypes.h>
#include <DxEtwLibrary/GpuTimingData.h>

#include "D3D12ObjectProcessor.h"

#include "Identifiers.h"

#include "AdapterTracker.h"
#include "MonitorTracker.h"

#include <DxEtwLibrary/DxEtwEventHandler.h>

#include "PerProcessData.h"

#include "DxEtwProcessor.h"

#ifndef UsePixUtility
// <evntrace.h> declares EventTraceConfigGuid but only defines it when INITGUID
// is active before that header is first included. The library pulls in
// <evntrace.h> (via D3D12Events.h) ahead of <initguid.h>, so the GUID is left
// undefined. In a standalone build, define it here so the library is
// self-contained; in-tree (UsePixUtility) PIX's PixUtility supplies it. Value
// from the Windows SDK <evntrace.h>: {01853a65-418f-4f36-aefc-dc0f1d2fd235}.
EXTERN_C __declspec(selectany) const GUID EventTraceConfigGuid =
    { 0x01853a65, 0x418f, 0x4f36, { 0xae, 0xfc, 0xdc, 0x0f, 0x1d, 0x2f, 0xd2, 0x35 } };
#endif

DxEtwEventHandler::DxEtwEventHandler(
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
    : m_processor(std::make_unique<DxEtwProcessor>(options, gpuOnlyEvents, std::move(apiObjectCallbacks), std::move(pixCounterCallbacks), std::move(residencyEventCallbacks), std::move(pixEventCallbacks), std::move(monitorEventCallbacks), std::move(gpuTimingsCallbacks), std::move(dxgkObjectCallbacks), std::move(pipelineStateEventCallbacks), std::move(diagnosticsSink), std::move(runtimeFailureCallbacks), std::move(timestampConverter), targetProcessId))
    , m_dispatcher(std::make_unique<EtwDispatcher<DxEtwProcessor>>(m_processor.get()))
    , m_options(options)
    , m_targetProcessId(targetProcessId)
{
}

DxEtwEventHandler::~DxEtwEventHandler() = default;

void DxEtwEventHandler::HandleEventRecord(PEVENT_RECORD record)
{
    if (record->EventHeader.ProviderId == PixEtw::PIX_ETW_PROVIDER_WINDOWS ||
        record->EventHeader.ProviderId == Direct3D12EtwProviderGuid ||
        record->EventHeader.ProviderId == DxgkControlGuid ||
        record->EventHeader.ProviderId == EventTraceConfigGuid)
    {
        // Dispatch decodes a raw, possibly-truncated or malformed ETW payload and
        // throws on a bad payload; a callback that returns a failure HRESULT also
        // throws. Both propagate to the caller by design - the consumer owns the
        // ProcessTrace loop and must wrap this call in try/catch (so nothing
        // unwinds through ETW's C frames) and decide what a failure means. A
        // partial event surfaces nothing to the callbacks before it throws. See
        // the sample's OnEvent and the functional test's SimpleEtwSession.
        m_dispatcher->Dispatch(record);
    }
    return;
}

void DxEtwEventHandler::OnDataComplete()
{
    m_processor->OnComplete();
}

void DxEtwEventHandler::ReportTraceStatistics(const EVENT_TRACE_LOGFILE& logfile)
{
    m_processor->ReportTraceStatistics(logfile);
}

DirectX::Etw::LostEventStatistics DxEtwEventHandler::GetLostEventStatistics() const
{
    return m_processor->GetLostEventStatistics();
}
