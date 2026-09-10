// scene_receiver.cpp
#include "scene_receiver.h"

#include <algorithm>
#include <cstring>

using namespace SceneIPC;

namespace Host
{

SceneReceiver::SceneReceiver()
    : m_residentBytes(0), m_recording(nullptr), m_replay(nullptr)
    , m_recordedBytes(0)
{
    memset(&m_frame.begin,    0, sizeof(m_frame.begin));
    memset(&m_frame.lighting, 0, sizeof(m_frame.lighting));
    m_frame.complete      = false;
    m_frame.lightingValid = false;
    m_building = m_frame;
    memset(&m_stats, 0, sizeof(m_stats));

    // Sized for the largest single message we expect: a texture payload.
    // TryRead reports the required size rather than truncating, so this only
    // affects how often the buffer has to grow.
    m_scratch.resize(4u * 1024u * 1024u);
}

SceneReceiver::~SceneReceiver()
{
    StopRecording();
    if (m_replay) { fclose(m_replay); m_replay = nullptr; }
}

void SceneReceiver::StopRecording()
{
    if (!m_recording) return;
    fclose(m_recording);
    m_recording = nullptr;
}

bool SceneReceiver::Attach(const char* sectionName)
{
    return m_ring.OpenAsConsumer(sectionName);
}

void SceneReceiver::Detach()
{
    m_ring.Close();
}

uint64_t SceneReceiver::ProducerFramesDropped() const
{
    const RingHeader* h = m_ring.Header();
    return h ? h->framesDropped : 0;
}

uint64_t SceneReceiver::ProducerBytesDropped() const
{
    const RingHeader* h = m_ring.Header();
    return h ? h->bytesDropped : 0;
}

const Geometry* SceneReceiver::FindGeometry(uint64_t id) const
{
    auto it = m_geometry.find(id);
    return (it == m_geometry.end()) ? nullptr : &it->second;
}

const Texture* SceneReceiver::FindTexture(uint64_t id) const
{
    auto it = m_textures.find(id);
    return (it == m_textures.end()) ? nullptr : &it->second;
}

void SceneReceiver::CollectDirtyTextures(std::vector<uint64_t>& out,
                                         uint32_t max) const
{
    out.clear();
    for (auto it = m_textures.begin(); it != m_textures.end(); ++it)
    {
        if (!it->second.dirty || it->second.pixels.empty()) continue;
        out.push_back(it->first);
        if (out.size() >= max) break;
    }
}

void SceneReceiver::MarkTextureClean(uint64_t textureId)
{
    auto it = m_textures.find(textureId);
    if (it != m_textures.end()) it->second.dirty = false;
}

void SceneReceiver::MarkTextureDirty(uint64_t textureId)
{
    auto it = m_textures.find(textureId);
    if (it != m_textures.end()) it->second.dirty = true;
}

void SceneReceiver::HandleMessage(const uint8_t* msg, uint32_t bytes)
{
    const MessageHeader* mh = (const MessageHeader*)msg;
    const uint8_t* body     = msg + sizeof(MessageHeader);
    const uint32_t bodyBytes = bytes - (uint32_t)sizeof(MessageHeader);

    // Every case below re-checks that the body is big enough for the struct
    // it claims to be. The stream comes from another process, so a truncated
    // or hostile length must not become an out-of-bounds read.
    switch (mh->type)
    {
    case kMsgGeometry:
    {
        if (bodyBytes < sizeof(GeometryDesc)) { ++m_stats.malformedMessages; return; }
        const GeometryDesc* d = (const GeometryDesc*)body;

        const uint64_t vbBytes = (uint64_t)d->vertexCount * d->vertexStride;
        const uint64_t ibBytes = (uint64_t)d->indexCount  * d->indexStride;
        if (sizeof(GeometryDesc) + vbBytes + ibBytes > bodyBytes)
        {
            ++m_stats.malformedMessages;
            return;
        }

        // Morph deltas follow the indices. A producer that blended in
        // process declared a count and sent no deltas, so a payload with no
        // room for them means the vertices are already blended - not a
        // malformed message. That is what keeps those recordings replayable.
        uint64_t morphFloats = 0;
        uint32_t morphTargets = d->morphTargets;
        if (morphTargets)
        {
            const uint64_t want = (uint64_t)morphTargets * d->vertexCount * 12;
            if (sizeof(GeometryDesc) + vbBytes + ibBytes + want > bodyBytes)
            {
                morphTargets = 0;
                ++m_stats.geometryPreBlended;
            }
            else
            {
                morphFloats = want / 4;
            }
        }

        Geometry& g = m_geometry[d->geometryId];
        m_residentBytes -= (g.vertices.size() + g.indices.size() +
                            g.morphDeltas.size() * sizeof(float));

        g.desc = *d;
        g.desc.morphTargets = morphTargets;
        const uint8_t* p = body + sizeof(GeometryDesc);
        g.vertices.assign(p, p + vbBytes);
        g.indices.assign(p + vbBytes, p + vbBytes + ibBytes);
        g.morphDeltas.resize((size_t)morphFloats);
        if (morphFloats)
            memcpy(g.morphDeltas.data(), p + vbBytes + ibBytes,
                   (size_t)morphFloats * 4);
        g.dirty = true;

        m_residentBytes += (g.vertices.size() + g.indices.size() +
                            g.morphDeltas.size() * sizeof(float));
        m_everReceived.insert(d->geometryId);
        ++m_stats.geometryUploads;
        break;
    }

    case kMsgTexture:
    {
        if (bodyBytes < sizeof(TextureDesc)) { ++m_stats.malformedMessages; return; }
        const TextureDesc* d = (const TextureDesc*)body;
        if (sizeof(TextureDesc) + (uint64_t)d->payloadBytes > bodyBytes)
        {
            ++m_stats.malformedMessages;
            return;
        }

        Texture& t = m_textures[d->textureId];
        m_residentBytes -= t.pixels.size();

        t.desc = *d;
        const uint8_t* p = body + sizeof(TextureDesc);
        t.pixels.assign(p, p + d->payloadBytes);
        t.dirty = true;

        m_residentBytes += t.pixels.size();
        ++m_stats.textureUploads;
        break;
    }

    case kMsgLighting:
    {
        if (bodyBytes < sizeof(LightingDesc)) { ++m_stats.malformedMessages; return; }
        memcpy(&m_building.lighting, body, sizeof(LightingDesc));
        m_building.lightingValid = true;
        break;
    }

    case kMsgFrameBegin:
    {
        if (bodyBytes < sizeof(FrameBegin)) { ++m_stats.malformedMessages; return; }
        // Carry lighting across the frame boundary: the game sends it only
        // when it changes, so an unchanged frame would otherwise go dark.
        const LightingDesc carriedLighting = m_building.lighting;
        const bool         carriedValid    = m_building.lightingValid;

        m_building.instances.clear();
        m_building.palettes.clear();
        m_building.uvTransforms.clear();
        m_building.morphWeights.clear();
        memcpy(&m_building.begin, body, sizeof(FrameBegin));
        m_building.lighting      = carriedLighting;
        m_building.lightingValid = carriedValid;
        m_building.complete      = false;

        if (m_building.begin.instanceCount)
            m_building.instances.reserve(m_building.begin.instanceCount);
        break;
    }

    case kMsgInstance:
    {
        if (bodyBytes < sizeof(InstanceDesc)) { ++m_stats.malformedMessages; return; }
        InstanceDesc inst;
        memcpy(&inst, body, sizeof(inst));

        // A skinned instance carries its bone palette as the payload. It is
        // kept in a parallel array rather than inside InstanceDesc because
        // its length varies and the wire structures are fixed-size by design.
        const uint32_t rows = inst.paletteRegisters;
        const size_t   want = (size_t)rows * 16;
        if (rows && bodyBytes < sizeof(InstanceDesc) + want)
        {
            ++m_stats.malformedMessages;
            inst.paletteRegisters = 0;   // render it unskinned rather than wrong
        }

        m_building.palettes.push_back(std::vector<float>());
        if (inst.paletteRegisters)
        {
            std::vector<float>& p = m_building.palettes.back();
            p.resize(rows * 4);
            memcpy(p.data(), body + sizeof(InstanceDesc), want);
        }

        // The texture transform follows the palette, so a stream recorded
        // before this existed reads exactly as it always did: the flag is
        // clear and the identity goes in.
        std::array<float, 6> uv = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
        if (inst.flags & kInstanceUvTransform)
        {
            const size_t at = sizeof(InstanceDesc) + want;
            if (bodyBytes < at + kUvTransformBytes)
            {
                ++m_stats.malformedMessages;
            }
            else
            {
                // Two float4 rows, A then B. A.xy is the u basis and A.zw
                // the offset; B.xy is the v basis.
                float a[4], b[4];
                memcpy(a, body + at,      sizeof(a));
                memcpy(b, body + at + 16, sizeof(b));
                uv[0] = a[0]; uv[1] = a[1];    // uv.x contribution
                uv[2] = b[0]; uv[3] = b[1];    // uv.y contribution
                uv[4] = a[2]; uv[5] = a[3];    // constant offset
            }
        }
        m_building.uvTransforms.push_back(uv);

        // The morph weights come after the texture transform, so where they
        // start depends on whether that flag is set.
        std::array<float, kMaxMorphTargets> mw;
        mw.fill(0.0f);
        if (inst.flags & kInstanceMorphWeights)
        {
            size_t at = sizeof(InstanceDesc) + want;
            if (inst.flags & kInstanceUvTransform) at += kUvTransformBytes;
            if (bodyBytes < at + kMorphWeightBytes) ++m_stats.malformedMessages;
            else memcpy(mw.data(), body + at, kMorphWeightBytes);
        }
        m_building.morphWeights.push_back(mw);

        m_building.instances.push_back(inst);
        break;
    }

    case kMsgFrameReset:
    {
        // The game cleared the colour target, so everything sent for this
        // frame so far was an offscreen pass that has already been copied
        // away and overwritten. Dropping it here is what a rasteriser does
        // for free; a ray tracer has to be told.
        ++m_stats.framesReset;
        m_stats.instancesVoided += m_building.instances.size();
        m_building.instances.clear();
        m_building.palettes.clear();
        m_building.uvTransforms.clear();
        m_building.morphWeights.clear();
        break;
    }

    case kMsgFrameEnd:
    {
        if (bodyBytes < sizeof(FrameEnd)) { ++m_stats.malformedMessages; return; }
        const FrameEnd* fe = (const FrameEnd*)body;

        // A frame whose end does not match its beginning means the stream was
        // truncated mid-frame — most likely the producer dropped messages
        // under back-pressure. Publishing it would render a half-built scene.
        if (fe->frameIndex != m_building.begin.frameIndex)
        {
            ++m_stats.malformedMessages;
            return;
        }

        m_building.complete = true;
        m_frame = m_building;

        // Touch what this frame used, and ask for what it needed and did
        // not have. The second half is why the producer's own record of what
        // it has sent cannot be trusted alone - see RingHeader's resend table.
        m_resendScratch.clear();
        for (size_t i = 0; i < m_frame.instances.size(); ++i)
        {
            const InstanceDesc& in = m_frame.instances[i];
            auto g = m_geometry.find(in.geometryId);
            if (g != m_geometry.end())
                g->second.lastUsedFrame = m_frame.begin.frameIndex;
            else
                m_resendScratch.push_back(in.geometryId);

            auto t = m_textures.find(in.baseTextureId);
            if (t != m_textures.end()) t->second.lastUsedFrame = m_frame.begin.frameIndex;
        }

        if (!m_resendScratch.empty())
        {
            // Many instances share a geometry, so the raw list repeats
            // heavily; the table has 256 slots and asking for one id ten
            // times would waste nine of them.
            std::sort(m_resendScratch.begin(), m_resendScratch.end());
            m_resendScratch.erase(
                std::unique(m_resendScratch.begin(), m_resendScratch.end()),
                m_resendScratch.end());

            m_stats.resendRequested += m_resendScratch.size();
            m_ring.RequestResend(m_resendScratch.data(),
                                 (uint32_t)m_resendScratch.size());
        }

        ++m_stats.framesCompleted;
        break;
    }

    case kMsgShutdown:
        break;

    default:
        ++m_stats.malformedMessages;
        break;
    }
}

uint32_t SceneReceiver::EvictUnused(uint64_t retentionFrames,
                                   uint64_t budgetBytes)
{
    // Nothing to do until the cache is actually costing something. See the
    // header: evicting on age alone dropped geometry the game was still
    // using and made it pop.
    if (m_residentBytes <= budgetBytes) return 0;

    const uint64_t now = m_frame.begin.frameIndex;
    if (now < retentionFrames) return 0;   // not enough history yet
    const uint64_t cutoff = now - retentionFrames;

    // Oldest first, so the least likely to be wanted goes first. Gathering
    // candidates rather than erasing in place because the order matters and
    // an unordered_map has none.
    struct Candidate { uint64_t age; uint64_t id; bool isTexture; size_t bytes; };
    std::vector<Candidate> candidates;
    candidates.reserve(m_geometry.size() + m_textures.size());

    for (auto it = m_geometry.begin(); it != m_geometry.end(); ++it)
    {
        if (it->second.lastUsedFrame >= cutoff) continue;
        candidates.push_back({ it->second.lastUsedFrame, it->first, false,
                               it->second.vertices.size() + it->second.indices.size() +
                               it->second.morphDeltas.size() * sizeof(float) });
    }
    for (auto it = m_textures.begin(); it != m_textures.end(); ++it)
    {
        if (it->second.lastUsedFrame >= cutoff) continue;
        candidates.push_back({ it->second.lastUsedFrame, it->first, true,
                               it->second.pixels.size() });
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.age < b.age; });

    uint32_t dropped = 0;
    for (size_t i = 0; i < candidates.size() && m_residentBytes > budgetBytes; ++i)
    {
        const Candidate& c = candidates[i];
        if (c.isTexture)
        {
            auto it = m_textures.find(c.id);
            if (it == m_textures.end()) continue;
            m_residentBytes -= it->second.pixels.size();
            m_textures.erase(it);
            ++m_stats.texturesEvicted;
        }
        else
        {
            auto it = m_geometry.find(c.id);
            if (it == m_geometry.end()) continue;
            m_residentBytes -= (it->second.vertices.size() + it->second.indices.size() +
                                it->second.morphDeltas.size() * sizeof(float));
            m_geometry.erase(it);
            ++m_stats.geometryEvicted;
        }
        ++dropped;
    }

    return dropped;
}

bool SceneReceiver::StartRecording(const char* path)
{
    if (m_recording) fclose(m_recording);
    m_recording = nullptr;
    fopen_s(&m_recording, path, "wb");
    return m_recording != nullptr;
}

bool SceneReceiver::StartReplay(const char* path)
{
    if (m_replay) fclose(m_replay);
    m_replay = nullptr;
    fopen_s(&m_replay, path, "rb");
    return m_replay != nullptr;
}

// One message from the file, into the scratch buffer. The header carries the
// total length, so the file needs no framing of its own.
bool SceneReceiver::ReadReplayMessage(uint32_t& outBytes)
{
    if (!m_replay) return false;

    MessageHeader mh;
    if (fread(&mh, 1, sizeof(mh), m_replay) != sizeof(mh)) return false;
    if (mh.byteLength < sizeof(mh) || mh.byteLength > (1u << 28))
    {
        ++m_stats.malformedMessages;
        return false;
    }

    if (m_scratch.size() < mh.byteLength) m_scratch.resize(mh.byteLength + 4096);
    memcpy(m_scratch.data(), &mh, sizeof(mh));

    const uint32_t rest = mh.byteLength - (uint32_t)sizeof(mh);
    if (rest && fread(m_scratch.data() + sizeof(mh), 1, rest, m_replay) != rest)
        return false;

    outBytes = mh.byteLength;
    return true;
}

bool SceneReceiver::Poll(uint32_t maxMessages)
{
    if (!m_ring.IsOpen() && !m_replay) return false;

    const uint64_t framesBefore = m_stats.framesCompleted;

    for (uint32_t i = 0; i < maxMessages; ++i)
    {
        uint32_t msgBytes = 0, needed = 0;

        if (m_replay)
        {
            if (!ReadReplayMessage(msgBytes)) break;   // end of recording
        }
        else if (!m_ring.TryRead(m_scratch.data(), (uint32_t)m_scratch.size(),
                                 &msgBytes, &needed))
        {
            if (needed)
            {
                // Grow and retry rather than dropping: a message this large
                // is a texture, and skipping it would leave the scene without
                // a material it references.
                m_scratch.resize(needed + 4096);
                --i;
                continue;
            }
            break;   // ring empty
        }

        if (m_recording)
        {
            fwrite(m_scratch.data(), 1, msgBytes, m_recording);
            m_recordedBytes += msgBytes;
        }

        ++m_stats.messagesRead;
        m_stats.bytesReceived += msgBytes;
        HandleMessage(m_scratch.data(), msgBytes);

        // A replay stops at each frame boundary so the caller renders every
        // recorded frame rather than racing to the end of the file.
        if (m_replay && m_stats.framesCompleted != framesBefore) break;
    }

    return m_stats.framesCompleted != framesBefore;
}

} // namespace Host
