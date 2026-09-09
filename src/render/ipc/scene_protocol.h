// scene_protocol.h
//
// The wire format between the 32-bit game process and the 64-bit render host.
//
// ── Why this file is unusual ─────────────────────────────────────────────
//
// Ray tracing cannot run inside pes6.exe: the game is 32-bit and NVIDIA's
// 32-bit Vulkan ICD exposes no acceleration-structure or ray-pipeline
// extensions (see docs/RENDERER.md §1). So the scene has to cross a process
// boundary *and* a bitness boundary.
//
// This header is compiled into both sides. Every structure here is therefore
// written to have byte-identical layout under x86 and x64:
//
//   • no pointers, ever — a pointer is 4 bytes on one side and 8 on the other
//   • no size_t, long, bool, enum-without-underlying-type, or any type whose
//     width or packing differs between the two ABIs
//   • fixed-width integers only, each field naturally aligned
//   • explicit padding, never implicit
//   • every struct's size and every field's offset asserted at compile time,
//     so a layout mistake is a build error on whichever side disagrees
//
// The asserts are the load-bearing part. They fire in both builds, so a
// change that is fine in x64 but shifts an offset in x86 cannot compile.
//
// ── Transport shape ──────────────────────────────────────────────────────
//
// A single reliable byte ring in shared memory carrying a tagged message
// stream. Two classes of message share it:
//
//   • resources (geometry, textures) — sent once, keyed by id, cached by the
//     host. These must never be dropped or the host renders garbage.
//   • per-frame scene (camera, instances) — small, and safe to drop if the
//     host falls behind, because the next frame supersedes it.
//
// The producer therefore drops only frame traffic under back-pressure, never
// resource traffic. See shared_ring.h.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace SceneIPC
{
    // 'PSCN' — bumped whenever any structure below changes shape.
    static const uint32_t kSceneMagic   = 0x4E435350u;
    static const uint32_t kSceneVersion = 2u;

    // Default shared mapping size. This is address space in the *32-bit*
    // process, which only has ~2 GB of it, so the default is deliberately
    // modest; the host drains continuously so it does not need to be large.
    static const uint64_t kDefaultRingBytes = 64ull * 1024ull * 1024ull;

    // Message tags. Explicit underlying type: a bare enum's width is not
    // guaranteed to match across compilers or bitness.
    enum MessageType : uint32_t
    {
        kMsgNone            = 0,
        kMsgFrameBegin      = 1,
        kMsgFrameEnd        = 2,
        kMsgInstance        = 3,
        kMsgGeometry        = 4,   // vertex + index payload follows
        kMsgTexture         = 5,   // pixel payload follows
        kMsgLighting        = 6,
        kMsgShutdown        = 7
    };

    // Every message begins with this. `byteLength` covers the header plus its
    // payload, so a consumer that does not understand a tag can still skip it.
    struct MessageHeader
    {
        uint32_t type;         // MessageType
        uint32_t byteLength;   // total, including this header, 8-byte aligned
    };

    // ── Geometry ─────────────────────────────────────────────────────────
    // The two vertex layouts the game actually uses, from docs/RENDERER.md.
    // Both are 24 or 32 bytes with position first, which is all the BLAS
    // builder needs; the rest is material input.
    enum VertexKind : uint32_t
    {
        kVertexUnknown  = 0,
        kVertexPreLit   = 1,   // 24B: float3 pos, D3DCOLOR diffuse, float2 uv
        kVertexLit      = 2    // 32B: float3 pos, float3 normal, float2 uv
    };

    struct GeometryDesc
    {
        uint64_t geometryId;      // stable key; host caches by this
        uint32_t vertexKind;      // VertexKind
        uint32_t vertexStride;
        uint32_t vertexCount;
        uint32_t indexCount;      // 0 = non-indexed
        uint32_t indexStride;     // 2 (the game is 16-bit) or 4
        uint32_t contentHash;     // cheap change detector for reused ids
        // Payload follows: vertexCount*vertexStride bytes, then
        // indexCount*indexStride bytes, each padded to 8 bytes.
    };

    // ── Textures ─────────────────────────────────────────────────────────
    enum TextureFormat : uint32_t
    {
        kTexUnknown  = 0,
        kTexBGRA8    = 1,   // D3DFMT_A8R8G8B8 / X8R8G8B8
        kTexBGR565   = 2,
        kTexBGRA5551 = 3,
        kTexBGRA4444 = 4,
        kTexDXT1     = 5,
        kTexDXT3     = 6,
        kTexDXT5     = 7,
        kTexL8       = 8,
        kTexA8L8     = 9
    };

    struct TextureDesc
    {
        uint64_t textureId;
        uint32_t format;          // TextureFormat
        uint32_t width;
        uint32_t height;
        uint32_t mipLevels;
        uint32_t payloadBytes;    // all mip levels, tightly packed
        uint32_t _pad0;
        // Payload follows, padded to 8 bytes.
    };

    // ── Per-frame scene ──────────────────────────────────────────────────
    // Row-major 4x4, applied as row-vector * matrix. This is the form after
    // the producer has transposed the game's shader constants, which hold the
    // columns rather than the rows — see scene_conventions.h.
    struct Matrix4x4
    {
        float m[16];
    };

    struct FrameBegin
    {
        uint64_t  frameIndex;
        uint64_t  producerTimeNs;
        Matrix4x4 view;             // reconstructed camera
        Matrix4x4 projection;
        uint32_t  renderWidth;
        uint32_t  renderHeight;
        uint32_t  instanceCount;    // advisory; kMsgFrameEnd is authoritative
        uint32_t  _pad0;
    };

    // Material parameters as the game's shaders express them. Named for what
    // they mean rather than for the register they came from, but the mapping
    // is recorded so a capture can be traced back:
    //   baseColorFactor  <- vs constant c72 (global tint)
    //   flags            <- alpha blend / alpha test / two-sided render states
    struct InstanceDesc
    {
        uint64_t  geometryId;
        uint64_t  baseTextureId;    // 0 = untextured
        uint64_t  normalTextureId;  // 0 = none

        // What the game actually hands us. The 3D scene is drawn by vs.1.1
        // shaders whose only transform is `m4x4 oPos, v0, c58` — a single
        // *combined* world-view-projection matrix. There is no separate world
        // matrix anywhere in the shader draws, so this is the ground truth.
        Matrix4x4 clipTransform;

        // Object -> world, which is what a TLAS instance needs. The producer
        // can only fill this for the handful of fixed-function draws where
        // SetTransform is genuinely in play; for shader draws the host derives
        // it as clipTransform * inverse(viewProjection), since the camera is
        // shared by every draw in a frame. Valid only with kInstanceWorldValid.
        Matrix4x4 worldTransform;

        float     baseColorFactor[4];
        uint32_t  flags;            // InstanceFlags
        uint32_t  _pad0;
    };

    enum InstanceFlags : uint32_t
    {
        kInstanceNone         = 0,
        kInstanceAlphaBlend   = 1u << 0,
        kInstanceAlphaTest    = 1u << 1,
        kInstanceTwoSided     = 1u << 2,
        kInstancePreLit       = 1u << 3,   // bake vertex colour, do not relight
        kInstanceNoShadow     = 1u << 4,
        kInstanceWorldValid   = 1u << 5,   // worldTransform is filled in

        // Drawn with D3DRS_ZWRITEENABLE off — the game's own statement that
        // this geometry must not occlude anything.
        //
        // A rasteriser honours that by draw order and depth writes. A ray
        // tracer has no equivalent: whatever is nearest along the ray wins.
        // In this game that difference is the whole image, because the sky is
        // a small dome ~75 units from the camera while the stadium is
        // thousands away, so every primary ray hits the sky first and the
        // frame comes out a flat wall.
        //
        // On a measured match frame this covers 37 of 405 world draws and 242
        // of 21,441 triangles: the sky dome plus two-triangle overlay
        // sprites. Nothing that should be traced.
        kInstanceNoDepthWrite = 1u << 6
    };

    // The game's own lighting rig, read from vertex shader constants rather
    // than invented — see docs/RENDERER.md §4.3.
    struct LightingDesc
    {
        float directionalDir[4];    // c95, unit length (w unused)
        float directionalColor[4];  // c94
        float hemisphereAxis[4];    // c93
        float skyColor[4];          // c92
        float groundColor[4];       // c91
        float ambient[4];           // c68
        float specularColor[4];     // c70, w = exponent
        float specularHalfDir[4];   // c63, unit length
    };

    struct FrameEnd
    {
        uint64_t frameIndex;
        uint32_t instanceCount;     // instances actually sent
        uint32_t droppedInstances;  // dropped due to back-pressure
    };

    // ── ABI locks ────────────────────────────────────────────────────────
    // These must hold identically in the 32-bit and 64-bit builds. If a field
    // is reordered or a type widened, whichever side disagrees fails to
    // compile rather than silently misreading the stream at runtime.
    #define SCENEIPC_ASSERT_LAYOUT(type, expectedSize)                        \
        static_assert(sizeof(type) == (expectedSize),                         \
                      #type " changed size - 32/64-bit ABI would diverge")

    SCENEIPC_ASSERT_LAYOUT(MessageHeader,  8);
    SCENEIPC_ASSERT_LAYOUT(GeometryDesc,  32);
    SCENEIPC_ASSERT_LAYOUT(TextureDesc,   32);
    SCENEIPC_ASSERT_LAYOUT(Matrix4x4,     64);
    SCENEIPC_ASSERT_LAYOUT(FrameBegin,   160);
    SCENEIPC_ASSERT_LAYOUT(InstanceDesc, 176);
    SCENEIPC_ASSERT_LAYOUT(LightingDesc, 128);
    SCENEIPC_ASSERT_LAYOUT(FrameEnd,      16);

    // Alignment must match too: a struct can be the right size and still sit
    // at a different offset inside the ring if its alignment differs.
    static_assert(alignof(MessageHeader) == 4,  "MessageHeader alignment");
    static_assert(alignof(GeometryDesc)  == 8,  "GeometryDesc alignment");
    static_assert(alignof(TextureDesc)   == 8,  "TextureDesc alignment");
    static_assert(alignof(FrameBegin)    == 8,  "FrameBegin alignment");
    static_assert(alignof(InstanceDesc)  == 8,  "InstanceDesc alignment");

    // Spot-check the offsets that a bitness mistake would most plausibly
    // shift — the ones following 64-bit members.
    static_assert(offsetof(GeometryDesc, vertexKind)   == 8,  "GeometryDesc layout");
    static_assert(offsetof(TextureDesc,  format)       == 8,  "TextureDesc layout");
    static_assert(offsetof(FrameBegin,   view)         == 16, "FrameBegin layout");
    static_assert(offsetof(FrameBegin,   projection)   == 80, "FrameBegin layout");
    static_assert(offsetof(InstanceDesc, clipTransform)  == 24,  "InstanceDesc layout");
    static_assert(offsetof(InstanceDesc, worldTransform) == 88,  "InstanceDesc layout");
    static_assert(offsetof(InstanceDesc, flags)          == 168, "InstanceDesc layout");

    #undef SCENEIPC_ASSERT_LAYOUT

    // Messages are kept 8-byte aligned so every struct above lands naturally
    // aligned wherever it sits in the ring.
    inline uint32_t AlignMessage(uint32_t bytes)
    {
        return (bytes + 7u) & ~7u;
    }
}
