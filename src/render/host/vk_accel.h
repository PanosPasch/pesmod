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
        kRecordBlended = 1u << 0,

        // This record covers the merged sprite batch, whose triangles
        // came from many draws. Its material is per triangle, in the
        // SpriteTriangle table, not in the record.
        kRecordSpriteBatch = 1u << 1,

        // Shade with the texture alone: no normal, no shadow ray, no
        // lighting rig. The sky is a pre-lit texture on a box, and running
        // the stadium's directional and hemisphere terms over it would light
        // a thing that is already the light.
        kRecordUnlit = 1u << 2,

        // Take coverage from the vertex colour's alpha as well as the
        // texture's. Set only where the texture has no alpha of its own, so
        // the game cannot be reading coverage from it. See
        // TextureCache::TextureCarriesAlpha.
        kRecordVertexAlpha = 1u << 3
    };

    // ── Ray masks ────────────────────────────────────────────────────────
    //
    // "Depth writes off" is the game saying a draw must not occlude anything.
    // Dropping those draws entirely was the first answer to that, and it cost
    // the stadium surround, the athletics track and the projected shadows -
    // sixty-eight draws a frame, most of which belong in the picture and only
    // one of which is actually the sky.
    //
    // A mask says it properly: which rays can see an instance at all. A
    // non-occluding overlay is visible to the eye and invisible to a shadow
    // ray, which is exactly what a surface that writes no depth means. The
    // sky is visible to neither, and is looked up on its own.
    enum RayMask : uint32_t
    {
        // Ordinary geometry: seen by the eye, and casts shadows.
        kMaskSolid = 0x01,

        // Drawn with depth writes off: seen by the eye, casts no shadow.
        kMaskNonOccluding = 0x02,

        // The sky. Its box encloses the camera and every other surface, so a
        // ray that could see it would see nothing else; only the miss path
        // looks it up.
        kMaskSky = 0x04,

        // What a primary ray traces against, and what a shadow ray does.
        kMaskPrimary = kMaskSolid | kMaskNonOccluding,
        kMaskShadow  = kMaskSolid
    };

    // ── Coplanar decals ─────────────────────────────────────────────────
    //
    // Per draw-order step, as a fraction of the distance to the camera. The
    // builder nudges each blended instance toward the viewer by this much
    // times its position in the frame, so the game's draw order becomes
    // depth order and coplanar layers stop tying.
    //
    // This is public, and reaches the shaders through SceneUniforms, because
    // the ray generation's peel has to resume *inside* one step of it. Those
    // two numbers were once chosen independently and came out the same
    // order, which meant a decal's own base surface always fell within the
    // epsilon that stepped past it: the pitch's grass was skipped and the
    // pitch rendered as the near-black wear overlay over nothing.
    const float kDecalBias = 1.0e-5f;

    // How far into one decal step the peel resumes. Comfortably less than a
    // whole step, so a base surface one step behind its decal is still
    // ahead of tmin - and still tens of times the float spacing at the
    // distances this scene uses, so a surface cannot re-hit itself.
    const float kResumeFraction = 0.25f;

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
        uint32_t nonOccluding;          // drawn with depth writes off
        uint32_t skyDraws;              // of those, the ones enclosing the camera
        uint32_t skinnedRebuilds;       // structures rebuilt because the pose moved

        // Structures thrown away and recreated because the driver asked
        // for more room than the one already there. Expected to be small
        // and occasional; a large number every frame would mean the reuse
        // test is not reusing anything.
        uint32_t blasResized;

        // Blended instances taking coverage from the vertex colour.
        uint32_t vertexAlphaDraws;

        // Instances carrying the game's own affine texture transform - the
        // advertising hoardings and anything else windowing an atlas. Zero
        // here with hoardings on screen means the producer is not sending
        // it, which is a different fault from the host not applying it.
        uint32_t uvTransformedDraws;

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

        // One entry per triangle of the merged sprite batch, in the same
        // order the triangles were merged, so gl_PrimitiveID indexes it.
        const GpuBuffer& SpriteTriangles() const { return m_spriteTriangles; }
        uint32_t SpriteTriangleCount() const { return m_spriteTriangleCount; }

        // Drops persistent structures whose geometry is no longer resident.
        void PruneOrphans(const SceneReceiver& scene);

        // True while a frame has produced a camera position, which the sky
        // test and the decal bias both need.
        bool HaveCameraPos() const { return m_haveCameraPos; }

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

            // Bytes to the float3 normal, or kNoVertexAttribute. Only the
            // game's lit layouts have one; its 24-byte pre-lit layout
            // carries a baked colour instead, and those surfaces stay
            // faceted.
            uint32_t normalOffset;

            // Bytes to the D3DCOLOR diffuse, or kNoVertexAttribute.
            //
            // Only its alpha is used. The game modulates texture by vertex
            // colour, so the coverage of a blended surface is the product of
            // both - and taking it from the texture alone drew a sky glow
            // whose vertex alpha runs 0 to 89 as a solid white sheet across
            // the stadium roof. Its RGB is deliberately left alone: on this
            // layout it carries the game's own baked lighting, and folding
            // that in would light the scene twice.
            uint32_t colorOffset;

            // The draw's affine texture-coordinate transform, or the
            // identity. Two float4 rows rather than six floats so that
            // std430 and C++ agree without a padding argument:
            //
            //     u' = uv.x * uvTransform0[0] + uv.y * uvTransform0[2]
            //                                 + uvTransform1[0]
            //     v' = uv.x * uvTransform0[1] + uv.y * uvTransform0[3]
            //                                 + uvTransform1[1]
            //
            // The game's own `mad oT0.xy, v2.y, c76, (v2.x*c75.xy+c75.zw)`,
            // which windows an atlas. Ignoring it drew every advertising
            // hoarding as the whole sheet of adverts at once.
            float    uvTransform0[4];
            float    uvTransform1[4];
        };

        static_assert(sizeof(InstanceRecord) == 96,
                      "InstanceRecord must match its std430 layout in "
                      "shaders/common.glsl");
        static_assert(offsetof(InstanceRecord, baseColor)    == 32,
                      "vec4 is 16-byte aligned in std430");
        static_assert(offsetof(InstanceRecord, samplerIndex) == 48,
                      "InstanceRecord must match shaders/common.glsl");
        static_assert(offsetof(InstanceRecord, flags)        == 52,
                      "InstanceRecord must match shaders/common.glsl");
        static_assert(offsetof(InstanceRecord, normalOffset) == 56,
                      "InstanceRecord must match shaders/common.glsl");
        static_assert(offsetof(InstanceRecord, colorOffset)   == 60,
                      "InstanceRecord must match shaders/common.glsl");
        static_assert(offsetof(InstanceRecord, uvTransform0) == 64,
                      "vec4 is 16-byte aligned in std430");
        static_assert(offsetof(InstanceRecord, uvTransform1) == 80,
                      "vec4 is 16-byte aligned in std430");

        // ── Materials for the merged sprite batch ────────────────────
        //
        // A sprite draw is two triangles, and the scene has around a
        // hundred of them a frame: advertising hoardings, stand panels,
        // projected shadows. Giving each its own structure costs far
        // more than tracing it, so they are baked into world space and
        // merged - which throws away the one thing a rasteriser gets for
        // free, namely which draw a triangle came from.
        //
        // This is that identity, put back. The merged batch is
        // non-indexed, three vertices per triangle, so gl_PrimitiveID
        // indexes this table directly and no indirection is needed.
        struct SpriteTriangle
        {
            float    uv[6];         // uv0.xy, uv1.xy, uv2.xy
            uint32_t textureSlot;
            uint32_t samplerIndex;
            float    baseColor[4];
            uint32_t flags;         // kRecordBlended
            uint32_t _pad0, _pad1, _pad2;
        };

        static_assert(sizeof(SpriteTriangle) == 64,
                      "SpriteTriangle must match shaders/common.glsl");
        static_assert(offsetof(SpriteTriangle, textureSlot)  == 24,
                      "SpriteTriangle must match shaders/common.glsl");
        static_assert(offsetof(SpriteTriangle, baseColor)    == 32,
                      "SpriteTriangle must match shaders/common.glsl");
        static_assert(offsetof(SpriteTriangle, flags)        == 48,
                      "SpriteTriangle must match shaders/common.glsl");

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

        // An object-space bounding box, cached per geometry because it takes
        // a pass over the vertices and the sky is drawn every frame from the
        // same mesh.
        struct Bounds
        {
            float    lo[3];
            float    hi[3];
            uint32_t contentHash;
        };

        // Whether this draw is the game's sky.
        //
        // Two shapes have to be recognised, and a single test does not catch
        // both. Some stadiums draw a box around the camera; others draw a
        // cap overhead plus a ring around the horizon, and that ring's lower
        // edge can sit slightly above the camera - thirty-two units, in one
        // measured frame - so a containment test misses it entirely and the
        // sky ends up lit like a wall.
        //
        // So: the draw's world bounds either contain the camera outright, or
        // lie wholly overhead while surrounding it horizontally. "Overhead"
        // is along the game's own up axis, taken from the hemisphere axis it
        // publishes in c93 rather than assumed.
        //
        // Both halves are needed. Wholly overhead alone would sweep up a
        // floodlight glow; surrounding alone would sweep up the pitch, which
        // surrounds the camera horizontally and is the last thing that should
        // stop being traced.
        bool LooksLikeSky(const Geometry& geo, const Math::Mat4& world,
                          const float upAxis[3]);

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
        std::unordered_map<uint64_t, Bounds>   m_objectBounds;

        Accel      m_spriteBlas;
        GpuBuffer  m_spriteVertices;
        VkDeviceSize m_spriteCapacityBytes;

        Accel      m_tlas;
        GpuBuffer  m_tlasInstances;
        VkDeviceSize m_tlasCapacityBytes;

        GpuBuffer    m_instanceRecords;
        VkDeviceSize m_instanceRecordCapacity;
        uint32_t     m_instanceRecordCount;

        GpuBuffer    m_spriteTriangles;
        VkDeviceSize m_spriteTriangleCapacity;
        uint32_t     m_spriteTriangleCount;

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
