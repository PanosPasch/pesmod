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
#include "vk_device.h"

#include <unordered_map>
#include <vector>

namespace Host
{
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
        bool BuildFrame(const SceneReceiver& scene);

        VkAccelerationStructureKHR Tlas() const { return m_tlas.handle; }
        const AccelStats& Stats() const { return m_stats; }
        const std::string& LastError() const { return m_lastError; }

        // Drops persistent structures whose geometry is no longer resident.
        void PruneOrphans(const SceneReceiver& scene);

    private:
        struct Accel
        {
            VkAccelerationStructureKHR handle;
            VkDeviceAddress            address;
            GpuBuffer                  storage;

            Accel() : handle(VK_NULL_HANDLE), address(0) {}
        };

        struct MeshBlas
        {
            Accel     accel;
            GpuBuffer vertices;
            GpuBuffer indices;
            uint32_t  contentHash;
            uint32_t  triangleCount;
            uint64_t  lastUsedFrame;
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

        bool PrepareMeshBlas(const Geometry& geo, MeshBlas& out, PendingBuild& job);
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

        GpuBuffer    m_scratch;
        VkDeviceSize m_scratchCapacity;

        // The VP identified last frame. The camera moves every frame, but the
        // object whose world matrix is identity does not, so last frame's
        // answer is the best first guess for this one.
        Math::Mat4  m_vpHint;
        bool        m_haveVpHint;

        AccelStats  m_stats;
        std::string m_lastError;
        uint64_t    m_frameCounter;
    };
}
