// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <memory>
#include <string>

// PerfettoTraceWriter owns the in-process Perfetto tracing session and turns the
// DxEtwLibrary callbacks into Perfetto track events. The Perfetto SDK header is
// large and is kept out of this header (PIMPL) so only PerfettoTraceWriter.cpp
// pays its compile cost; the callback classes below just forward to it.
class PerfettoTraceWriter
{
public:
    // outputPath: where the .perfetto-trace file is written on Finish().
    explicit PerfettoTraceWriter(std::string outputPath);
    ~PerfettoTraceWriter();

    // Mints a unique, non-zero id for the library's _Out_ id parameters. A single
    // counter keeps every id distinct across all callback categories, which is what
    // the library requires (it feeds ids from one callback into another).
    uint64_t NextId();

    // GPU command queues + GPU work spans.
    void NameHwQueue(uint64_t hwQueueId, const wchar_t* name);
    void EmitGpuWork(uint64_t hwQueueId, uint64_t apiMarkerId, int64_t beginNs, int64_t endNs);
    void RegisterApiMarker(uint64_t apiMarkerId, const wchar_t* name, int64_t ts);

    // API command queues (lifetime slices).
    void DefineApiQueue(uint64_t apiQueueId, int64_t beginNs, const wchar_t* type);
    void NameApiQueue(uint64_t apiQueueId, const wchar_t* name);
    void EndApiQueue(uint64_t apiQueueId, int64_t endNs);

    // D3D12 object lifetimes (create -> destroy slices, one track per type).
    void ObjectCreated(uint64_t objectId, const char* typeTrack, int64_t ts, std::wstring label);
    void ObjectNamed(uint64_t objectId, const wchar_t* name);
    void ObjectDestroyed(uint64_t objectId, int64_t ts);

    // Pipeline-state-object compilation spans (per-thread track to keep the
    // possibly-concurrent compiles from overlapping on a single track).
    void EmitPsoCompile(uint32_t threadId, int64_t startNs, int64_t endNs);

    // Close any still-open slices, stop tracing, and write the trace file.
    void Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
