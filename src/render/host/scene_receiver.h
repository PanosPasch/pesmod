// scene_receiver.h
//
// Consumes the scene message stream from the game and maintains the host's
// view of the world: a cache of geometry and textures keyed by id, plus the
// current frame's instance list.
//
// This is deliberately independent of Vulkan. The renderer asks the receiver
// what the scene *is*; how it gets turned into acceleration structures is the
// renderer's business. Keeping the split here means the transport and scene
// bookkeeping are testable without a GPU, which matters because the producer
// side can only be exercised by running the game.
#pragma once

#include "../ipc/shared_ring.h"

#include <stdint.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Host
{
    // A mesh as received. `vertices` and `indices` are raw bytes in the
    // game's own layout — the renderer reinterprets them according to
    // `desc.vertexKind` rather than the receiver converting eagerly, since
    // the GPU upload path wants to choose its own destination format.
    struct Geometry
    {
        SceneIPC::GeometryDesc desc;
        std::vector<uint8_t>   vertices;
        std::vector<uint8_t>   indices;
        uint64_t               lastUsedFrame;
        bool                   dirty;      // needs (re)upload / BLAS rebuild
    };

    struct Texture
    {
        SceneIPC::TextureDesc desc;
        std::vector<uint8_t>  pixels;
        uint64_t              lastUsedFrame;
        bool                  dirty;
    };

    // One frame's worth of scene, rebuilt every frame.
    struct Frame
    {
        SceneIPC::FrameBegin                begin;
        SceneIPC::LightingDesc              lighting;
        std::vector<SceneIPC::InstanceDesc> instances;

        // One entry per instance, parallel to `instances`. Empty for an
        // unskinned draw. Not inside InstanceDesc because its length
        // varies and every wire structure is fixed-size by design.
        std::vector<std::vector<float> >    palettes;
        bool                                complete;   // saw kMsgFrameEnd
        bool                                lightingValid;
    };

    struct ReceiverStats
    {
        uint64_t messagesRead;
        uint64_t framesCompleted;
        uint64_t geometryUploads;
        uint64_t textureUploads;
        uint64_t bytesReceived;
        uint64_t malformedMessages;
        uint64_t geometryEvicted;
        uint64_t texturesEvicted;
    };

    class SceneReceiver
    {
    public:
        SceneReceiver();

        // Attaches to the shared section. Returns false if the game side is
        // not running yet; callers are expected to retry.
        bool Attach(const char* sectionName);
        void Detach();
        bool IsAttached() const { return m_ring.IsOpen(); }

        // Drains up to `maxMessages` messages. Returns true when at least one
        // frame was completed, meaning `CurrentFrame()` is worth rendering.
        //
        // Bounded rather than draining fully so a host that has fallen behind
        // still presents rather than spinning forever inside the queue.
        bool Poll(uint32_t maxMessages = 4096);

        const Frame& CurrentFrame() const { return m_frame; }

        const Geometry* FindGeometry(uint64_t id) const;
        const Texture*  FindTexture(uint64_t id) const;

        // Drops cached resources not referenced for `retentionFrames`.
        //
        // This is not just a memory concern. One geometry is meant to become
        // one BLAS, so an unbounded cache means an unbounded number of
        // acceleration structures — the cache has been observed growing by
        // 8-15 entries per frame indefinitely while the instance count stayed
        // flat. Eviction bounds that; whether the underlying id churn is
        // itself a bug is a separate question the producer diagnoses.
        //
        // Returns the number of entries dropped.
        uint32_t EvictUnused(uint64_t retentionFrames);

        size_t GeometryCount() const { return m_geometry.size(); }
        size_t TextureCount()  const { return m_textures.size(); }
        uint64_t ResidentBytes() const { return m_residentBytes; }

        const ReceiverStats& Stats() const { return m_stats; }

        // True if this geometry id was received at some point, even if it has
        // since been evicted. An unresolved instance whose id was never
        // received means the producer never sent it; one whose id *was*
        // received means the caches fell out of sync. Those are different
        // bugs and the distinction is not otherwise visible.
        bool WasEverReceived(uint64_t geometryId) const
        {
            return m_everReceived.count(geometryId) != 0;
        }

        // Producer-side counters, read straight out of the shared header.
        // A non-zero drop count means the host is not keeping up.
        uint64_t ProducerFramesDropped() const;
        uint64_t ProducerBytesDropped() const;

        // Textures that have arrived or changed and are not yet on the GPU.
        // Collecting and clearing are separate calls so a failed upload
        // leaves the texture dirty and it is retried, rather than being
        // dropped because it was marked clean optimistically.
        void CollectDirtyTextures(std::vector<uint64_t>& out, uint32_t max) const;
        void MarkTextureClean(uint64_t textureId);

    private:
        void HandleMessage(const uint8_t* msg, uint32_t bytes);

        SceneIPC::SharedRing m_ring;
        std::vector<uint8_t> m_scratch;

        std::unordered_map<uint64_t, Geometry> m_geometry;
        std::unordered_map<uint64_t, Texture>  m_textures;

        std::unordered_set<uint64_t> m_everReceived;

        Frame          m_frame;      // most recently completed frame
        Frame          m_building;   // frame currently being assembled
        ReceiverStats  m_stats;
        uint64_t       m_residentBytes;
    };
}
