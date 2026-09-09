// vk_accel.h
//
// Acceleration structure management: bottom-level structures for the scene's
// meshes, and a top-level structure rebuilt every frame from the instances.
//
// ── Why this is not one BLAS per draw ────────────────────────────────────
//
// The obvious mapping — one geometry, one BLAS — does not survive contact
// with this game. Measured over a 2350-frame session, 82% of geometry ids
// were used in exactly one draw ever, and the worst offenders were 4-vertex
// quads drawn hundreds of times per frame from a shared index buffer:
// sprites for crowd, shadows and grass. One BLAS per quad would mean tens of
// thousands of acceleration structures, nearly all of them rebuilt once and
// then discarded.
//
// So geometry is split by size:
//
//   • Small geometry (at or below kSpriteTriangleLimit) is transformed to
//     world space on the CPU and merged into a single BLAS rebuilt each
//     frame. Thousands of sprites become one structure and one TLAS instance.
//
//   • Everything larger keeps a persistent BLAS keyed by geometry id, rebuilt
//     only when its content hash changes. That is what makes CPU-skinned
//     players affordable: their topology is stable, only the vertices move.
//
// ── Transforms ───────────────────────────────────────────────────────────
//
// A TLAS instance needs an affine object-to-world matrix, but the game's
// shader draws only ever produce a combined world-view-projection (constants
// c58..c61). The world matrix is recovered as WVP * inverse(VP), using the
// view-projection shared by every draw in the frame. Because that inversion
// is only as good as the VP it is given, every recovered matrix is checked
// for affinity and the failures are counted rather than quietly used.
#pragma once

#include "scene_receiver.h"
#include "scene_math.h"
#include "vk_alloc.h"
#include "vk_textures.h"
#include "vk_device.h"

#include <unordered_map>
#include <vector>

namespace Host
{
    // Applies the game's matrix palette to one mesh, writing world-relative
    // positions and normals over the bone-local ones. `dst` must have room
    // for desc.vertexCount * desc.vertexStride bytes.
    //
    // Exposed for the self-test: this mirrors the game's vertex shader
    // arithmetic exactly, and the one thing that cannot be checked by looking
    // at the output is whether it does so correctly.
    void SkinVertices(const Geometry& geo, const std::vector<float>& palette,
                      float indexScale, uint8_t* dst);

    // InstanceRecord::flags, mirrored in shaders/common.glsl.
    enum RecordFlags : uint32_t
    {
        // The surface composites over what is behind it rather than replacing
        // it, so the ray has to continue past it and its alpha is meaningful.
        kRecordBlended = 1u << 0
    };

    // A draw at or below this many triangles is treated as a sprite and
    // merged rather than given its own acceleration structure. Two triangles
    // covers the quads that dominate the stream; the limit is deliberately
    // low so that real meshes are never swept into the merged batch.
    static const uint32_t kSpriteTriangleLimit = 2;

    struct AccelStats
    {
        uint32_t persistentBlas;        // live, keyed by geometry id
        uint32_t blasBuiltThisFrame;    // rebuilt because content changed
        uint32_t spriteInstances;       // draws folded into the merged BLAS
        uint32_t spriteTriangles;
        uint32_t tlasInstances;
        uint32_t transformsRejected;    // recovered world matrix not affine
        uint32_t vpCandidatesTried;
        uint32_t vpBestScore;           // instances yielding an affine world
        uint32_t vpSampleSize;
        const char* vpSource;           // where the winning VP came from
        uint32_t geometryUnresolved;    // instance referenced missing geometry
        uint32_t nonOccluding;          // skipped: drawn with depth writes off
        uint32_t skinnedRebuilds;       // structures rebuilt because the pose moved

        // Structures thrown away and recreated because the driver asked
        // for more room than the one already there. Expected to be small
        // and occasional; a large number every frame would mean the reuse
        // test is not reusing anything.
        uint32_t blasResized;

        // Build jobs dropped because another job in the same batch already
        // targeted that structure. Building one destination twice in a single
        // command is undefined and takes the device with it, so this must
        // stay zero; it is counted rather than assumed.
        uint32_t duplicateBuildsDropped;
        uint64_t blasBytes;
        uint64_t scratchBytes;
        double   buildMilliseconds;
    };

    class AccelBuilder
    {
    public:
        AccelBuilder();
        ~AccelBuilder();

        bool Init(VulkanDevice* device, GpuAllocator* allocator);
        void Shutdown();

        // Rebuilds the acceleration structures for one frame of scene data.
        // Returns false only on an unrecoverable Vulkan error; a frame with
        // no usable geometry succeeds with an empty TLAS.
        // `textures` may be null, in which case every record points at the
        // white slot and the scene renders untextured.
        bool BuildFrame(const SceneReceiver& scene,
                        const TextureCache* textures = nullptr);

        VkAccelerationStructureKHR Tlas() const { return m_tlas.handle; }
        const AccelStats& Stats() const { return m_stats; }

        // The inverse of the view-projection the resolver settled on. The ray
        // generation camera must come from this and not from SetTransform,
        // for the same reason the world transforms do.
        bool HasViewProj() const { return m_haveVpHint; }
        const Math::Mat4& InverseViewProj() const { return m_lastInverseVp; }
        const std::string& LastError() const { return m_lastError; }

        // Per-TLAS-instance data for the hit shader, in the same order as
        // the TLAS instances themselves. Empty until BuildFrame runs.
        const GpuBuffer& InstanceRecords() const { return m_instanceRecords; }
        uint32_t InstanceRecordCount() const { return m_instanceRecordCount; }

        // Drops persistent structures whose geometry is no longer resident.
        void PruneOrphans(const SceneReceiver& scene);

    public:
        // What the hit shader needs to shade a surface it did not know it
        // would hit.
        //
        // A rasteriser binds a texture and a vertex layout before each draw.
        // A ray tracer cannot: the surface is only known once the ray lands.
        // So every instance publishes where its data is and what it is made
        // of, and the hit shader looks that up through
        // gl_InstanceCustomIndexEXT.
        //
        // Mirrors InstanceRecord in shaders/common.glsl. Laid out so std430
        // and C++ agree without padding surprises; asserted below.
        struct InstanceRecord
        {
            uint64_t vertexAddress;
            uint64_t indexAddress;
            uint32_t vertexStride;
            uint32_t uvOffset;      // bytes into a vertex, to its float2 UV
            uint32_t indexStride;   // 2 or 4; 0 means non-indexed
            uint32_t textureSlot;
            float    baseColor[4];

            // Which sampler pairs with that texture. Addressing belongs to
            // the draw, not the image, so it travels with the instance.
            uint32_t samplerIndex;

            // kRecord* below. Whether a surface composites or replaces what
            // is behind it is a property of the draw, and the hit shader
            // cannot ask the instance flags directly.
            uint32_t flags;
            uint32_t _pad0, _pad1;
        };

        static_assert(sizeof(InstanceRecord) == 64,
                      "InstanceRecord must match its std430 layout in "
                      "shaders/common.glsl");
        static_assert(offsetof(InstanceRecord, baseColor)    == 32,
                      "vec4 is 16-byte aligned in std430");
        static_assert(offsetof(InstanceRecord, samplerIndex) == 48,
                      "InstanceRecord must match shaders/common.glsl");
        static_assert(offsetof(InstanceRecord, flags)        == 52,
                      "InstanceRecord must match shaders/common.glsl");

    private:
        struct Accel
        {
            VkAccelerationStructureKHR handle;
            VkDeviceAddress            address;
            GpuBuffer                  storage;

            // The size the *structure* was created with, which is not
            // the size of the buffer holding it: the buffer is
            // deliberately over-allocated so a growing scene does not
            // reallocate every frame. A build must never target a
            // structure smaller than the size the driver asks for, so
            // this is what the reuse test has to compare against.
            VkDeviceSize               size;

            Accel() : handle(VK_NULL_HANDLE), address(0), size(0) {}
        };

        // Keyed by geometry id for static meshes. A skinned mesh drawn more
        // than once in a frame needs one structure per *instance*, because
        // each carries its own pose - so its key mixes in which occurrence it
        // is, and `geometryId` records where it came from for pruning.
        struct MeshBlas
        {
            Accel     accel;
            GpuBuffer vertices;
            GpuBuffer indices;
            uint64_t  geometryId;
            uint32_t  contentHash;
            uint32_t  triangleCount;
            uint64_t  lastUsedFrame;

            MeshBlas() : geometryId(0), contentHash(0), triangleCount(0)
                       , lastUsedFrame(0) {}
        };

        // One structure build, prepared but not yet recorded.
        //
        // Builds are collected first and recorded together, because a submit
        // and a queue wait per structure costs far more than the builds
        // themselves — measured at roughly 7 ms of overhead each, which a
        // scene of a few hundred meshes cannot afford. `geometry` is stored
        // inline because `build.pGeometries` points at it and must stay valid
        // until the command buffer is recorded.
        struct PendingBuild
        {
            VkAccelerationStructureGeometryKHR          geometry;
            VkAccelerationStructureBuildGeometryInfoKHR build;
            VkAccelerationStructureBuildRangeInfoKHR    range;
            VkDeviceSize                                scratchSize;
            VkDeviceSize                                scratchOffset;
        };

        // `palette` is the instance's bone pose, or null for a rigid mesh.
        // Skinning happens during the copy into the BLAS buffer, since that
        // pass has to touch every vertex anyway.
        bool PrepareMeshBlas(const Geometry& geo, MeshBlas& out, PendingBuild& job,
                             const std::vector<float>* palette = nullptr,
                             float indexScale = 0.0f);
        bool PrepareSpriteBlas(const std::vector<float>& positions, PendingBuild& job);
        bool PrepareTlas(const std::vector<VkAccelerationStructureInstanceKHR>& instances,
                         PendingBuild& job);

        // Assigns scratch, records every prepared build into a single command
        // buffer, and submits once.
        bool RecordAndSubmit(std::vector<PendingBuild>& blasJobs,
                             PendingBuild* tlasJob);

        bool EnsureScratch(VkDeviceSize bytes);
        void DestroyAccel(Accel& accel);

        // Recovers object-to-world from the frame's shared view-projection.
        // Works out which matrix the shaders actually used as the combined
        // view-projection.
        //
        // The obvious source - view * projection from SetTransform - is wrong
        // here, because the 3D draws are shader-driven and ignore SetTransform
        // entirely. Feeding it to the factorisation rejected 798 of 850
        // instances on a real frame.
        //
        // Instead the VP is identified from the data. Any object drawn with an
        // identity world matrix has clipTransform == VP exactly, and static
        // scene geometry is usually authored that way, so each distinct
        // clipTransform is scored by how many instances it turns into an
        // affine world matrix. The best scorer wins.
        //
        // Note the factorisation is only determined up to an affine change of
        // basis: several candidates can yield affine worlds, differing by a
        // fixed transform. That is harmless as long as the camera used for ray
        // generation is derived from the same VP, which it is - what matters
        // is that the scene is internally consistent, not that the basis
        // matches the game's own.
        bool ResolveViewProjection(const Frame& frame, Math::Mat4& outInverse);

        bool RecoverWorld(const SceneIPC::InstanceDesc& inst,
                          const Math::Mat4& inverseViewProj,
                          bool haveInverse, Math::Mat4& outWorld) const;

        VulkanDevice*  m_device;
        GpuAllocator*  m_alloc;
        VkCommandPool  m_commandPool;

        std::unordered_map<uint64_t, MeshBlas> m_meshBlas;

        Accel      m_spriteBlas;
        GpuBuffer  m_spriteVertices;
        VkDeviceSize m_spriteCapacityBytes;

        Accel      m_tlas;
        GpuBuffer  m_tlasInstances;
        VkDeviceSize m_tlasCapacityBytes;

        GpuBuffer    m_instanceRecords;
        VkDeviceSize m_instanceRecordCapacity;
        uint32_t     m_instanceRecordCount;

        GpuBuffer    m_scratch;
        VkDeviceSize m_scratchCapacity;

        // The VP identified last frame. The camera moves every frame, but the
        // object whose world matrix is identity does not, so last frame's
        // answer is the best first guess for this one.
        Math::Mat4  m_vpHint;
        Math::Mat4  m_lastInverseVp;

        // Recovered from the winning view-projection. Used to bias coplanar
        // decals toward the viewer by draw order; see kDecalBias.
        struct Vec3 { float x, y, z; };
        Vec3        m_cameraPos;
        bool        m_haveCameraPos;
        bool        m_haveVpHint;

        AccelStats  m_stats;
        std::string m_lastError;
        uint64_t    m_frameCounter;
    };
}
