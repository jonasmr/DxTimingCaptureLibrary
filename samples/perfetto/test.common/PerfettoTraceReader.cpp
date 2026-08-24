// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <perfetto.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

#include "PerfettoTraceReader.h"

namespace DxEtwLibraryTest
{
    namespace
    {
        namespace pbzero = perfetto::protos::pbzero;

        std::vector<uint8_t> ReadFileBytes(const std::string& path)
        {
            std::ifstream file(path, std::ios::in | std::ios::binary | std::ios::ate);
            if (!file)
            {
                throw std::runtime_error("Could not open trace file: " + path);
            }

            const std::streamoff sizeInBytes = file.tellg();
            file.seekg(0, std::ios::beg);

            std::vector<uint8_t> bytes(static_cast<size_t>(sizeInBytes));
            if (!bytes.empty())
            {
                file.read(reinterpret_cast<char*>(bytes.data()), sizeInBytes);
            }
            return bytes;
        }

        std::string ToString(::protozero::ConstChars chars)
        {
            return std::string(chars.data, chars.size);
        }
    }

    bool ParsedTrace::HasTrackNameContaining(std::string_view fragment) const
    {
        return CountTrackNamesContaining(fragment) != 0;
    }

    size_t ParsedTrace::CountTrackNamesContaining(std::string_view fragment) const
    {
        std::vector<std::string_view> names;
        for (const Track& track : Tracks)
        {
            if (track.Name.find(fragment) != std::string::npos)
            {
                names.push_back(track.Name);
            }
        }
        std::ranges::sort(names);
        return static_cast<size_t>(std::ranges::unique(names).begin() - names.begin());
    }

    size_t ParsedTrace::CountTracksContaining(std::string_view fragment) const
    {
        std::vector<uint64_t> mergeKeys;
        for (const Track& track : Tracks)
        {
            if (track.Name.find(fragment) != std::string::npos)
            {
                mergeKeys.push_back(track.MergeKey);
            }
        }
        std::ranges::sort(mergeKeys);
        return static_cast<size_t>(std::ranges::unique(mergeKeys).begin() - mergeKeys.begin());
    }

    std::vector<TraceSlice> ParsedTrace::SlicesOnTracksContaining(std::string_view fragment) const
    {
        std::vector<TraceSlice> matching;
        std::ranges::copy_if(
            Slices,
            std::back_inserter(matching),
            [fragment](const TraceSlice& slice) { return slice.TrackName.find(fragment) != std::string::npos; });
        return matching;
    }

    ParsedTrace ReadPerfettoTrace(const std::string& path)
    {
        const std::vector<uint8_t> bytes = ReadFileBytes(path);

        // Resolved in a second pass: a slice can be written before the descriptor
        // naming its track, and interned names arrive in their own packets.
        std::unordered_map<uint64_t, std::string> trackNameByUuid;
        // Lanes of one queue carry the same merge key; a track without one is its own.
        std::unordered_map<uint64_t, uint64_t> mergeKeyByUuid;
        std::unordered_map<uint64_t, std::string> sliceNameByInternedId;
        struct RawSlice { uint64_t TrackUuid; std::string Name; uint64_t NameInternedId; };
        std::vector<RawSlice> rawSlices;

        // A trace file is a sequence of Trace.packet (field 1) submessages.
        ::protozero::ProtoDecoder traceDecoder(bytes.data(), bytes.size());
        for (auto field = traceDecoder.ReadField(); field.valid(); field = traceDecoder.ReadField())
        {
            if (field.id() != 1)
            {
                continue;
            }

            pbzero::TracePacket::Decoder packet(field.as_bytes());

            if (packet.has_track_descriptor())
            {
                pbzero::TrackDescriptor::Decoder track(packet.track_descriptor());
                if (track.has_name())
                {
                    trackNameByUuid[track.uuid()] = ToString(track.name());
                }
                else if (track.has_static_name())
                {
                    trackNameByUuid[track.uuid()] = ToString(track.static_name());
                }
                mergeKeyByUuid[track.uuid()] =
                    track.has_sibling_merge_key_int() ? track.sibling_merge_key_int() : track.uuid();
            }

            if (packet.has_interned_data())
            {
                pbzero::InternedData::Decoder interned(packet.interned_data());
                for (auto it = interned.event_names(); it; ++it)
                {
                    pbzero::EventName::Decoder eventName(*it);
                    sliceNameByInternedId[eventName.iid()] = ToString(eventName.name());
                }
            }

            if (packet.has_track_event())
            {
                pbzero::TrackEvent::Decoder trackEvent(packet.track_event());
                const auto type = trackEvent.type();
                const bool startsASlice =
                    type == pbzero::TrackEvent::TYPE_SLICE_BEGIN || type == pbzero::TrackEvent::TYPE_INSTANT;
                if (startsASlice)
                {
                    rawSlices.push_back({
                        trackEvent.track_uuid(),
                        trackEvent.has_name() ? ToString(trackEvent.name()) : std::string(),
                        trackEvent.has_name_iid() ? trackEvent.name_iid() : 0 });
                }
            }
        }

        ParsedTrace parsed;
        std::vector<std::pair<std::string, uint64_t>> distinctTracks;
        for (const auto& [uuid, name] : trackNameByUuid)
        {
            distinctTracks.emplace_back(name, mergeKeyByUuid[uuid]);
        }
        std::ranges::sort(distinctTracks);
        distinctTracks.erase(std::ranges::unique(distinctTracks).begin(), distinctTracks.end());
        for (auto& [name, mergeKey] : distinctTracks)
        {
            parsed.Tracks.push_back({ std::move(name), mergeKey });
        }

        for (const RawSlice& raw : rawSlices)
        {
            std::string name = raw.Name;
            if (name.empty() && raw.NameInternedId != 0)
            {
                auto interned = sliceNameByInternedId.find(raw.NameInternedId);
                if (interned != sliceNameByInternedId.end())
                {
                    name = interned->second;
                }
            }

            auto track = trackNameByUuid.find(raw.TrackUuid);
            parsed.Slices.push_back({ track == trackNameByUuid.end() ? std::string() : track->second, std::move(name) });
        }

        return parsed;
    }
}
