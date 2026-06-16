// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

// Diagnostics seam for DxEtwLibrary.
//
// The library reports recoverable problems it encounters while parsing and
// correlating ETW events (an unmatched GPU EndEvent, a backwards clock
// calibration, lost ETW events, ...) through a consumer-supplied DiagnosticsSink
// rather than writing to stdout/stderr. An embeddable library must not own the
// process's console, so the consumer decides where these messages go (a log
// file, an in-app overlay, a no-op, ...).
//
// The sink is optional: a consumer that passes no sink gets the NullDiagnosticsSink
// below, which discards everything. The library never calls a null sink.

#include <cstdint>
#include <string_view>

namespace DirectX::Etw
{
    enum class DiagnosticSeverity
    {
        // A problem the library worked around; data may be slightly incomplete.
        Warning,
        // A problem that means some data is missing or could not be trusted.
        Error,
    };

    // Identifies the specific condition so a consumer can react programmatically
    // (count occurrences, surface lost-event totals, suppress known-noisy cases)
    // without having to parse the human-readable message.
    enum class DiagnosticCode
    {
        // A GPU EndEvent arrived with no matching BeginEvent on the command list.
        GpuEndEventWithoutBeginEvent,
        // A command list execution had no matching D3D12 command queue, so some
        // GPU timing data is missing.
        CommandListExecutionMissingQueue,
        // A computed end-of-pipe timestamp preceded its top-of-pipe timestamp.
        TimestampEndBeforeStart,
        // A clock calibration reported a CPU tick earlier than the previous one.
        ClockCalibrationCpuWentBackwards,
        // A clock calibration reported a GPU tick earlier than the previous one.
        ClockCalibrationGpuWentBackwards,
        // A pipeline-state compilation start event was superseded before its API
        // object id arrived, so the in-flight compilation was dropped.
        PipelineStateCompilationStartWithoutObjectId,
        // A pipeline-state compilation end event arrived with no matching start.
        PipelineStateCompilationEndWithoutStart,
        // The ETW session reported that event records were lost (buffer overrun).
        EventsLost,
        // The ETW session reported that whole buffers were lost.
        BuffersLost,
    };

    class DiagnosticsSink
    {
    public:
        virtual ~DiagnosticsSink() = default;

        // Called when the library encounters a reportable condition. The message
        // is a complete, human-readable description; it is owned by the library
        // and only valid for the duration of the call (copy it to retain it).
        virtual void OnDiagnostic(
            DiagnosticSeverity severity,
            DiagnosticCode code,
            std::wstring_view message) = 0;
    };

    // Running totals of ETW data loss observed for a trace, as last reported to
    // the handler via ReportTraceStatistics. Both counts are cumulative for the
    // session, so a non-zero value means some captured data is missing.
    struct LostEventStatistics
    {
        uint64_t EventsLost = 0;
        uint64_t BuffersLost = 0;
    };

    // Default sink used when a consumer supplies none. Discards every diagnostic.
    class NullDiagnosticsSink final : public DiagnosticsSink
    {
    public:
        void OnDiagnostic(DiagnosticSeverity, DiagnosticCode, std::wstring_view) override
        {
        }
    };
}
