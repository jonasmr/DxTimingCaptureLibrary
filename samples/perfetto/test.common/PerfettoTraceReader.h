// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace DxEtwLibraryTest
{
    // A slice the sample drew, with the name of the track it landed on. The writer
    // spreads overlapping spans across several lane tracks that share one name, so
    // the name - not the track uuid - is what identifies a queue to a test.
    struct TraceSlice
    {
        std::string TrackName;
        std::string Name;
    };

    struct ParsedTrace
    {
        // Distinct tracks, not lanes: the writer spreads overlapping spans across
        // several lane tracks that share a name and a merge key.
        struct Track
        {
            std::string Name;
            uint64_t MergeKey;
        };

        std::vector<Track> Tracks;
        std::vector<TraceSlice> Slices;

        bool HasTrackNameContaining(std::string_view fragment) const;

        // Distinct track names matching, ignoring how many tracks share each name.
        size_t CountTrackNamesContaining(std::string_view fragment) const;

        // Distinct tracks as the UI renders them. Lanes of one queue - and, under a
        // regression, two processes keyed on a shared engine - fold together by
        // merge key, so that, not the track name or uuid, is what counts here.
        size_t CountTracksContaining(std::string_view fragment) const;

        std::vector<TraceSlice> SlicesOnTracksContaining(std::string_view fragment) const;
    };

    // Decodes a .perfetto-trace file far enough to see which tracks exist and what
    // was drawn on them. Throws if the file cannot be read.
    ParsedTrace ReadPerfettoTrace(const std::string& path);
}
