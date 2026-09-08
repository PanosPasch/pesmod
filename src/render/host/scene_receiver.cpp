// scene_receiver.cpp
#include "scene_receiver.h"

#include <cstring>

using namespace SceneIPC;

namespace Host
{

SceneReceiver::SceneReceiver()
    : m_residentBytes(0)
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

        Geometry& g = m_geometry[d->geometryId];
        m_residentBytes -= (g.vertices.size() + g.indices.size());

        g.desc = *d;
        const uint8_t* p = body + sizeof(GeometryDesc);
        g.vertices.assign(p, p + vbBytes);
        g.indices.assign(p + vbBytes, p + vbBytes + ibBytes);
        g.dirty = true;

        m_residentBytes += (g.vertices.size() + g.indices.size());
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
        m_building.instances.push_back(inst);
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

        for (size_t i = 0; i < m_frame.instances.size(); ++i)
        {
            const InstanceDesc& in = m_frame.instances[i];
            auto g = m_geometry.find(in.geometryId);
            if (g != m_geometry.end()) g->second.lastUsedFrame = m_frame.begin.frameIndex;
            auto t = m_textures.find(in.baseTextureId);
            if (t != m_textures.end()) t->second.lastUsedFrame = m_frame.begin.frameIndex;
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

bool SceneReceiver::Poll(uint32_t maxMessages)
{
    if (!m_ring.IsOpen()) return false;

    const uint64_t framesBefore = m_stats.framesCompleted;

    for (uint32_t i = 0; i < maxMessages; ++i)
    {
        uint32_t msgBytes = 0, needed = 0;
        if (!m_ring.TryRead(m_scratch.data(), (uint32_t)m_scratch.size(),
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

        ++m_stats.messagesRead;
        m_stats.bytesReceived += msgBytes;
        HandleMessage(m_scratch.data(), msgBytes);
    }

    return m_stats.framesCompleted != framesBefore;
}

} // namespace Host
