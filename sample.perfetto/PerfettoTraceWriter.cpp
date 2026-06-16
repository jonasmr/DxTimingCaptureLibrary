// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <perfetto.h>

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "PerfettoCategories.h"
#include "PerfettoTraceWriter.h"

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

namespace
{
    std::string Narrow(const wchar_t* w)
    {
        if (w == nullptr || *w == L'\0')
            return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1)
            return {};
        std::string s(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
        return s;
    }
}

struct PerfettoTraceWriter::Impl
{
    std::string outputPath;
    bool captureAll = false; // when false, only GPU Queue work + API Markers
    std::unique_ptr<perfetto::TracingSession> session;

    uint64_t nextId = 1;

    // Map every event's nanosecond timestamp onto the SDK's clock domain. The
    // first event seen anchors "now"; the rest are placed relative to it. This
    // keeps the trace internally consistent regardless of the source clock epoch.
    bool baseSet = false;
    uint64_t sdkBaseNs = 0;
    int64_t firstEventNs = 0;
    uint64_t maxPft = 0;

    // Friendly names that may arrive before the track is first emitted.
    std::unordered_map<uint64_t, std::string> hwQueueName;   // hwQueueId -> name
    std::unordered_map<uint64_t, std::string> markerName;    // apiMarkerId -> name

    struct ApiQueue { int64_t beginNs; std::string type; std::string name; };
    std::unordered_map<uint64_t, ApiQueue> apiQueues;

    struct Object { int64_t beginNs; std::string typeLabel; std::string name; };
    std::unordered_map<uint64_t, Object> objects;

    uint64_t ToPft(int64_t eventNs)
    {
        if (!baseSet)
        {
            baseSet = true;
            firstEventNs = eventNs;
            sdkBaseNs = static_cast<uint64_t>(perfetto::base::GetBootTimeNs().count());
        }
        int64_t t = static_cast<int64_t>(sdkBaseNs) + (eventNs - firstEventNs);
        uint64_t pft = t < 0 ? 0 : static_cast<uint64_t>(t);
        if (pft > maxPft)
            maxPft = pft;
        return pft;
    }

    static perfetto::NamedTrack Track(const std::string& label, uint64_t id)
    {
        return perfetto::NamedTrack(perfetto::DynamicString{ label.c_str() }, id, perfetto::Track());
    }

    // flowId != 0 attaches a flow id to the event; events sharing a flow id are
    // connected by an arrow in the UI (in timestamp order). Used to link an API
    // marker to the GPU work it corresponds to.
    void Slice(uint64_t trackId, const std::string& trackLabel, const std::string& sliceLabel, uint64_t begin, uint64_t end, uint64_t flowId = 0)
    {
        auto track = Track(trackLabel, trackId);
        if (flowId)
            TRACE_EVENT_BEGIN("dxetw", perfetto::DynamicString{ sliceLabel.c_str() }, track, begin, perfetto::Flow::ProcessScoped(flowId));
        else
            TRACE_EVENT_BEGIN("dxetw", perfetto::DynamicString{ sliceLabel.c_str() }, track, begin);
        TRACE_EVENT_END("dxetw", track, end);
    }

    void Instant(uint64_t trackId, const std::string& trackLabel, const std::string& name, uint64_t ts, uint64_t flowId = 0)
    {
        auto track = Track(trackLabel, trackId);
        if (flowId)
            TRACE_EVENT_INSTANT("dxetw", perfetto::DynamicString{ name.c_str() }, track, ts, perfetto::Flow::ProcessScoped(flowId));
        else
            TRACE_EVENT_INSTANT("dxetw", perfetto::DynamicString{ name.c_str() }, track, ts);
    }
};

PerfettoTraceWriter::PerfettoTraceWriter(std::string outputPath, bool captureAll)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->outputPath = std::move(outputPath);
    m_impl->captureAll = captureAll;

    perfetto::TracingInitArgs args;
    args.backends = perfetto::kInProcessBackend;
    perfetto::Tracing::Initialize(args);
    perfetto::TrackEvent::Register();

    perfetto::TraceConfig cfg;
    cfg.add_buffers()->set_size_kb(64 * 1024); // 64 MB ring buffer
    // Track events intern their event/track names into per-sequence incremental
    // state. With a ring buffer, once the capture exceeds the buffer size the old
    // packets that defined those interned names get overwritten, and later packets
    // referencing them are dropped wholesale (reported as
    // traced_buf_incremental_sequences_dropped). Periodically clearing and
    // re-emitting incremental state gives the reader a re-sync point after a wrap.
    // https://perfetto.dev/docs/concepts/buffers#incremental-state-in-trace-packets
    cfg.mutable_incremental_state_config()->set_clear_period_ms(5000);
    auto* ds = cfg.add_data_sources()->mutable_config();
    ds->set_name("track_event");
    perfetto::protos::gen::TrackEventConfig te;
    te.add_enabled_categories("dxetw");
    // Our events carry their own (out-of-order) timestamps from ETW. Absolute
    // timestamps let the trace processor sort them; the default incremental
    // (delta) encoding would abort on a timestamp that moves backwards.
    te.set_disable_incremental_timestamps(true);
    ds->set_track_event_config_raw(te.SerializeAsString());

    m_impl->session = perfetto::Tracing::NewTrace();
    m_impl->session->Setup(cfg);
    m_impl->session->StartBlocking();
}

PerfettoTraceWriter::~PerfettoTraceWriter() = default;

uint64_t PerfettoTraceWriter::NextId()
{
    return m_impl->nextId++;
}

void PerfettoTraceWriter::NameHwQueue(uint64_t hwQueueId, const wchar_t* name)
{
    std::string n = Narrow(name);
    if (!n.empty())
        m_impl->hwQueueName[hwQueueId] = std::move(n);
}

void PerfettoTraceWriter::EmitGpuWork(uint64_t hwQueueId, uint64_t apiMarkerId, int64_t beginNs, int64_t endNs)
{
    auto itName = m_impl->hwQueueName.find(hwQueueId);
    std::string trackLabel = itName != m_impl->hwQueueName.end()
        ? "GPU Queue: " + itName->second
        : "GPU Queue #" + std::to_string(hwQueueId);

    std::string sliceLabel = "GPU work";
    auto itMarker = m_impl->markerName.find(apiMarkerId);
    if (itMarker != m_impl->markerName.end() && !itMarker->second.empty())
        sliceLabel = itMarker->second;

    // Link this GPU work to its API marker with a flow arrow (apiMarkerId is the
    // same id minted in StoreApiMarker / RegisterApiMarker). 0 means no marker.
    m_impl->Slice(hwQueueId, trackLabel, sliceLabel, m_impl->ToPft(beginNs), m_impl->ToPft(endNs), apiMarkerId);
}

void PerfettoTraceWriter::RegisterApiMarker(uint64_t apiMarkerId, const wchar_t* name, int64_t ts)
{
    std::string n = Narrow(name);
    m_impl->markerName[apiMarkerId] = n;
    // Same flowId as the matching GPU work, so the UI draws an arrow between them.
    m_impl->Instant(/*trackId*/ 1, "API Markers", n.empty() ? "marker" : n, m_impl->ToPft(ts), apiMarkerId);
}

void PerfettoTraceWriter::DefineApiQueue(uint64_t apiQueueId, int64_t beginNs, const wchar_t* type)
{
    if (!m_impl->captureAll) return;
    m_impl->apiQueues[apiQueueId] = Impl::ApiQueue{ beginNs, Narrow(type), {} };
}

void PerfettoTraceWriter::NameApiQueue(uint64_t apiQueueId, const wchar_t* name)
{
    auto it = m_impl->apiQueues.find(apiQueueId);
    if (it != m_impl->apiQueues.end())
        it->second.name = Narrow(name);
}

void PerfettoTraceWriter::EndApiQueue(uint64_t apiQueueId, int64_t endNs)
{
    auto it = m_impl->apiQueues.find(apiQueueId);
    if (it == m_impl->apiQueues.end())
        return;

    const Impl::ApiQueue& q = it->second;
    std::string label = !q.name.empty() ? q.name
        : (!q.type.empty() ? q.type + " Queue" : "Command Queue #" + std::to_string(apiQueueId));

    m_impl->Slice(apiQueueId, label, label, m_impl->ToPft(q.beginNs), m_impl->ToPft(endNs));
    m_impl->apiQueues.erase(it);
}

void PerfettoTraceWriter::ObjectCreated(uint64_t objectId, const char* typeTrack, int64_t ts, std::wstring label)
{
    (void)typeTrack;
    if (!m_impl->captureAll) return;
    m_impl->objects[objectId] = Impl::Object{ ts, Narrow(label.c_str()), {} };
}

void PerfettoTraceWriter::ObjectNamed(uint64_t objectId, const wchar_t* name)
{
    auto it = m_impl->objects.find(objectId);
    if (it != m_impl->objects.end())
        it->second.name = Narrow(name);
}

void PerfettoTraceWriter::ObjectDestroyed(uint64_t objectId, int64_t ts)
{
    auto it = m_impl->objects.find(objectId);
    if (it == m_impl->objects.end())
        return;

    const Impl::Object& o = it->second;
    std::string label = o.name.empty() ? o.typeLabel : o.typeLabel + ": " + o.name;
    m_impl->Slice(objectId, label, label, m_impl->ToPft(o.beginNs), m_impl->ToPft(ts));
    m_impl->objects.erase(it);
}

void PerfettoTraceWriter::EmitPsoCompile(uint32_t threadId, int64_t startNs, int64_t endNs)
{
    if (!m_impl->captureAll) return;
    std::string label = "PSO Compiles (thread " + std::to_string(threadId) + ")";
    // Thread id keys the track so concurrent compiles land on separate tracks.
    m_impl->Slice(0x50000000ull | threadId, label, "PSO compile", m_impl->ToPft(startNs), m_impl->ToPft(endNs));
}

void PerfettoTraceWriter::Finish()
{
    // Close any slices that never saw their end event: queues still open and
    // objects never destroyed, ended at the last timestamp we observed.
    uint64_t end = m_impl->maxPft;
    for (auto& [id, q] : m_impl->apiQueues)
    {
        std::string label = !q.name.empty() ? q.name
            : (!q.type.empty() ? q.type + " Queue" : "Command Queue #" + std::to_string(id));
        m_impl->Slice(id, label, label, m_impl->ToPft(q.beginNs), end);
    }
    for (auto& [id, o] : m_impl->objects)
    {
        std::string label = o.name.empty() ? o.typeLabel : o.typeLabel + ": " + o.name;
        m_impl->Slice(id, label, label, m_impl->ToPft(o.beginNs), end);
    }
    m_impl->apiQueues.clear();
    m_impl->objects.clear();

    perfetto::TrackEvent::Flush();
    m_impl->session->StopBlocking();
    std::vector<char> data = m_impl->session->ReadTraceBlocking();

    std::ofstream out(m_impl->outputPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out)
    {
        std::fprintf(stderr, "Failed to open %s for writing\n", m_impl->outputPath.c_str());
        return;
    }
    if (!data.empty())
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    std::printf("Wrote %zu bytes to %s\n", data.size(), m_impl->outputPath.c_str());
}
