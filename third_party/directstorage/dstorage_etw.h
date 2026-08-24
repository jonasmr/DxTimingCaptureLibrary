// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <evntprov.h>
#include <evntrace.h>
#include <stdint.h>

#include <string_view>

namespace DirectStorage
{
    constexpr GUID DStorageEtwProvider = /* {FB58BDF6-5E40-48E5-AFF5-4FC9AB7A9ABE} */
        {0xFB58BDF6, 0x5E40, 0x48E5, 0xAF, 0xF5, 0x4F, 0xC9, 0xAB, 0x7A, 0x9A, 0xBE};

#pragma pack(push, 1)

    using EndingWideString = std::wstring_view;
    using EndingString = std::string_view;

    constexpr inline EVENT_DESCRIPTOR MakeDescriptor(uint16_t Id, uint8_t Level = TRACE_LEVEL_VERBOSE)
    {
        EVENT_DESCRIPTOR d{};
        d.Id = Id;
        d.Version = 1;
        d.Level = Level;

        return d;
    }

    struct FileEventData
    {
        uint64_t File;
    };

    struct OpenFileEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(0);

        std::tuple<FileEventData, EndingWideString> Data;
    };

    struct CloseFileEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(1);

        FileEventData Data;
    };

    struct CreateQueueEventData
    {
        uint64_t Queue;
        uint8_t SourceType;
        uint16_t Capacity;
        int8_t Priority;
        uint64_t Device;
    };

    struct CreateQueueEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(2);

        std::tuple<CreateQueueEventData, EndingString> Data;
    };

    struct CloseQueueEventData
    {
        uint64_t Queue;
    };

    struct CloseQueueEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(3);

        CloseQueueEventData Data;
    };

    struct EnqueueRequestEventData
    {
        uint64_t Request;
        uint64_t Queue;
        uint32_t SourceSize;
        uint32_t UncompressedSize;
        uint8_t CompressionFormat;
        uint8_t SourceType;
        uint8_t DestinationType;
        uint64_t File;
        uint64_t Offset;
    };

    // Extends the EnqueeueRequestEventData with additional fields
    struct EnqueueRequestEventExtendedData
    {
        uint8_t TransformType;
    };

    struct EnqueueRequestEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(4, 2 /* Version 2 */);

        std::tuple<EnqueueRequestEventData, EndingString, EnqueueRequestEventExtendedData> Data;
    };

    struct RequestStartedEventData
    {
        uint64_t Request;
        uint64_t Queue;
    };

    struct RequestStartedEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(5);

        RequestStartedEventData Data;
    };

    struct RequestCompletedEventData
    {
        uint64_t Request;
        int32_t Result;
        uint64_t Queue;
    };

    struct RequestCompletedEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(6);

        RequestCompletedEventData Data;
    };

    struct CreateStatusArrayEventData
    {
        uint64_t StatusArray;
        uint32_t Capacity;
    };

    struct CreateStatusArrayEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(7);

        std::tuple<CreateStatusArrayEventData, EndingString> Data;
    };

    struct EnqueueStatusEventData
    {
        uint64_t StatusArray;
        uint32_t Index;
        uint64_t Queue;
    };

    struct EnqueueStatusEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(8);

        EnqueueStatusEventData Data;
    };

    struct StatusCompletedEventData
    {
        uint64_t StatusArray;
        uint32_t Index;
        int32_t Result;
        uint64_t Queue;
    };

    struct StatusCompletedEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(9);

        StatusCompletedEventData Data;
    };

    struct SignalEventData
    {
        uint64_t Fence;
        uint64_t Value;
        uint64_t Queue;
    };

    struct EnqueueSignalEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(10);

        SignalEventData Data;
    };

    struct SignalCompletedEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(11);

        SignalEventData Data;
    };

    struct SubmitEventData
    {
        uint64_t Queue;
        uint8_t IsAutoSubmit;
    };

    struct SubmitEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(12);

        SubmitEventData Data;
    };

    struct FileReadEventData
    {
        uint64_t File;
        uint64_t Offset;
        uint64_t Request;
        uint32_t SourceSize;
        uint32_t UncompressedSize;
        uint8_t CompressionFormat;
    };

    struct StartFileReadEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(13);

        FileReadEventData Data;
    };

    struct FileReadCompletedEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(14);

        FileReadEventData Data;
    };

    struct RequestEventData
    {
        uint64_t Request;
        uint32_t SourceSize;
        uint32_t UncompressedSize;
        uint8_t CompressionFormat;
        uint8_t DestinationType;
    };

    struct StartCpuProcessStageEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(15);

        RequestEventData Data;
    };

    struct CpuProcessStageCompletedEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(16);

        RequestEventData Data;
    };

    struct StartGpuProcessStageEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(17);

        RequestEventData Data;
    };

    struct GpuProcessStageCompletedEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(18);

        RequestEventData Data;
    };

    struct GpuCopyStageEventData
    {
        uint64_t Request;
        uint32_t UncompressedSize;
        uint8_t CompressionFormat;
        uint8_t DestinationType;
    };

    struct StartGpuCopyStageEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(19);

        GpuCopyStageEventData Data;
    };

    struct GpuCopyStageCompletedEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(20);

        GpuCopyStageEventData Data;
    };

    struct ActivitySummaryEventData
    {
        bool CopyQueueEnabled;
        bool ComputeQueueEnabled;
        uint32_t NumSubmitThreads;
        bool ForceMappingLayer;
        bool DisableBypassIO;
        bool DisableTelemetry;
        uint64_t TotalBytesRead;
    };

    struct ActivitySummaryEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(21, TRACE_LEVEL_INFORMATION);

        ActivitySummaryEventData Data;
    };

    struct SetEventData
    {
        uint64_t Handle;
        uint64_t Queue;
    };

    struct EnqueueSetEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(22);

        SetEventData Data;
    };

    struct SetEventCompletedEvent
    {
        static inline constexpr EVENT_DESCRIPTOR Desc = MakeDescriptor(23);

        SetEventData Data;
    };

#pragma pack(pop)
} // namespace DirectStorage
