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
    static const uint32_t kSceneVersion = 6u;

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
        kMsgShutdown        = 7,

        // Everything sent for this frame so far is not part of the picture.
        //
        // The game renders more than one pass per frame into the same back
        // buffer. Player shadows are drawn first, from the light's point of
        // view, and then copied into a texture with CopyRects
        // (pes6.exe FUN_00873350: GetSurfaceLevel(0) then device->CopyRects)
        // before the target is cleared and the visible frame begins.
        //
        // Nothing distinguishes those draws at the draw call - same device,
        // same back buffer, no SetRenderTarget anywhere in the binary - so
        // they arrive looking exactly like world geometry, and on a measured
        // match frame that is 133 of 832 world draws, none of which share the
        // camera's view-projection. Placed by the host's factorisation they
        // land wherever the light-space matrix sends them: floating fragments
        // that swing around when the replay camera moves.
        //
        // The clear is what separates the passes, and it is the game's own
        // statement that the pixels so far are gone. This carries it.
        kMsgFrameReset      = 8
    };

    // Every message begins with this. `byteLength` covers the header plus its
    // payload, so a consumer that does not understand a tag can still skip it.
    struct MessageHeader
    {
        uint32_t type;         // MessageType
        uint32_t byteLength;   // total, including this header, 8-byte aligned
    };

    // ── Geometry ─────────────────────────────────────────────────────────
    // Whether the game supplied a normal, which decides whether the renderer
    // can shade the surface or has to derive a geometric normal.
    //
    // This used to be inferred from the vertex stride, on the belief that the
    // game had two layouts of 24 and 32 bytes. It has at least five - 24, 32,
    // 36 and 40 bytes plus multi-stream variants - and the 40-byte one is the
    // most common of all, 5,944 of 11,207 world draws across every capture.
    // Inferring anything from the stride dropped it entirely, which is why
    // players appeared as a floating head and a hand.
    enum VertexKind : uint32_t
    {
        kVertexUnknown  = 0,
        kVertexPreLit   = 1,   // no normal; a baked vertex colour instead
        kVertexLit      = 2    // carries a per-vertex normal
    };

    // Offsets are byte positions within a vertex, or this when absent.
    static const uint32_t kNoVertexAttribute = 0xFFFFFFFFu;

    struct GeometryDesc
    {
        uint64_t geometryId;      // stable key; host caches by this
        uint32_t vertexKind;      // VertexKind
        uint32_t vertexStride;
        uint32_t vertexCount;
        uint32_t indexCount;      // 0 = non-indexed
        uint32_t indexStride;     // 2 (the game is 16-bit) or 4
        uint32_t contentHash;     // cheap change detector for reused ids

        // Where the attributes actually are, decoded from the game's own
        // vertex declaration rather than guessed from the stride. Position is
        // always at offset 0 - that is what the acceleration structure build
        // requires - but everything after it moves between layouts.
        uint32_t uvOffset;        // float2, or kNoVertexAttribute
        uint32_t normalOffset;    // float3, or kNoVertexAttribute
        uint32_t colorOffset;     // D3DCOLOR, or kNoVertexAttribute

        // ── Skinning ─────────────────────────────────────────────────────
        // The game skins players on the GPU with a matrix palette: the
        // vertex buffer holds bone-local positions, and the shader blends
        // `boneCount` matrices indexed per vertex before applying c58.
        //
        //     mul   r10, v2, c57.z        ; index register = colour * scale
        //     mov   a0.x, r10.x
        //     m4x3  r11, v0, c0           ; c[a0.x + 0..2] is one bone
        //     mul   r11.xyz, r11.xyzz, v1.x   ; weighted by v1
        //
        // So these positions are meaningless until the palette is applied.
        // Transformed by c58 alone they scatter across the pitch, which is
        // exactly what happened when this layout was first accepted.
        //
        // The palette rides on the instance, not here: the vertices are
        // static and cached once, while the pose changes every frame.
        uint32_t boneCount;       // matrices blended per vertex; 0 = not skinned
        uint32_t boneIndexOffset; // D3DCOLOR, or kNoVertexAttribute
        uint32_t boneWeightOffset;// D3DCOLOR, or kNoVertexAttribute (1 bone)

        // ── Morph targets ────────────────────────────────────────────────
        // How many delta streams the game's shader blended into the
        // positions below:
        //
        //     mov   r11,      v0
        //     mad   r11.xyz,  v3, c81.x, r11      ; stream 1
        //     mad   r11.xyz,  v4, c81.y, r11      ; stream 2
        //     ...
        //     m4x4  oPos,     r11, c58
        //
        // This is how the game shapes a player's build and moves their face.
        // The deltas live in streams 1..6, which the exporter used to ignore
        // entirely - it reads stream 0 and nothing else - so every player
        // rendered in the shader's neutral pose.
        //
        // The blend happens in the producer and the positions above are the
        // result, so a consumer needs nothing from this beyond a record of
        // what was applied. It is worth reporting because it is measurable:
        // the weights are static for a build morph and change for an
        // expression, and which of those a draw is decides whether the
        // geometry re-uploads.
        //
        // Zero in anything captured before morph blending existed, which is
        // exactly what it means: no deltas applied.
        uint32_t morphTargets;
        // Payload follows: vertexCount*vertexStride bytes, then
        // indexCount*indexStride bytes, each padded to 8 bytes.
    };

    // ── Textures ─────────────────────────────────────────────────────────
    enum TextureFormat : uint32_t
    {
        kTexUnknown  = 0,
        kTexBGRA8    = 1,   // D3DFMT_A8R8G8B8
        kTexBGR565   = 2,
        kTexBGRA5551 = 3,
        kTexBGRA4444 = 4,
        kTexDXT1     = 5,
        kTexDXT3     = 6,
        kTexDXT5     = 7,
        kTexL8       = 8,
        kTexA8L8     = 9,

        // D3DFMT_X8R8G8B8. Byte-identical to kTexBGRA8, but the high
        // byte is undefined rather than alpha - typically zero. Kept
        // distinct because a consumer that alpha tests would otherwise
        // discard every such surface, which is exactly what happened to
        // the pitch.
        kTexBGRX8    = 10
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

        // The bone palette for this draw, when its geometry is skinned.
        // `paletteRegisters` float4 rows follow this message as its payload,
        // copied verbatim from the shader constant file starting at c0, so
        // bone b occupies rows 3b, 3b+1, 3b+2 exactly as `m4x3 rN, v0, c0`
        // reads them.
        uint32_t  paletteRegisters; // 0 when the geometry is not skinned
        float     boneIndexScale;   // c57.z: colour byte -> palette row

        // Stage 0's texture stage state, packed a byte at a time:
        //
        //   byte 0  D3DTSS_ADDRESSU   D3DTADDRESS_*
        //   byte 1  D3DTSS_ADDRESSV   D3DTADDRESS_*
        //   byte 2  D3DTSS_ALPHAOP    D3DTOP_*
        //   byte 3  D3DTSS_ALPHAARG1  D3DTA_* (selector bits only)
        //
        // All zero means the producer did not report any of it, and the
        // Direct3D defaults apply: WRAP addressing, and alpha taken from the
        // texture alone.
        //
        // Addressing is per draw, not per texture, and it matters: the pitch
        // grass is tiled and needs WRAP, while a projected shadow samples one
        // blob out of a mostly-empty atlas with UVs running to 5.6 and needs
        // CLAMP. Sampling that with WRAP tiles the atlas across the quad and
        // paints the player's face onto the grass five times over.
        //
        // The alpha op decides whether a blended surface's coverage is the
        // texture's alone or the texture times the vertex colour's. Guessing
        // it from the texture was measurably wrong: the pitch grass is a
        // blended draw whose texture is opaque, so a guess folded its vertex
        // alpha in and lightened the pitch from rgb(129,134,106) to
        // rgb(157,173,138) against the game's own rgb(76,95,51).
        //
        // Kept inside the existing four bytes deliberately. Growing
        // InstanceDesc would bump the wire version and make every recording
        // captured so far unreplayable, which is the only way this renderer
        // gets debugged without a game running.
        uint32_t  stageState;
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
        kInstanceNoDepthWrite = 1u << 6,

        // The instance payload carries a texture transform after the bone
        // palette: two float4 rows, A then B, applied as
        //
        //     u' = uv.x * A.x + uv.y * B.x + A.z
        //     v' = uv.x * A.y + uv.y * B.y + A.w
        //
        // which is the game's own
        //
        //     mad  rN.xy,  v2.x, c75.xyyy, c75.zwww
        //     mad  oT0.xy, v2.y, c76,      rN
        //
        // with A = c75 and B = c76. It windows an atlas: the advertising
        // hoardings scroll by animating these registers over a sheet holding
        // every advert at once. Sampling the raw UV instead draws the whole
        // sheet across the hoarding, which is what it did.
        //
        // After the palette rather than before it, so that the palette stays
        // at the offset it has always been at and a recording made before
        // this flag existed still reads correctly.
        kInstanceUvTransform  = 1u << 7,

        // Drawn with D3DRS_ZFUNC = D3DCMP_ALWAYS: the game's statement that
        // this draw goes over whatever is already there, whatever the depth
        // says. Distinct from kInstanceNoDepthWrite, which only says it must
        // not occlude what comes after.
        kInstanceDepthAlways  = 1u << 8
    };

    // The texture transform's payload, when kInstanceUvTransform is set.
    static const uint32_t kUvTransformBytes = 32;   // two float4 rows

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

        // c69. The shader scales the whole lit result by this before adding
        // ambient, so leaving it out makes every surface too bright:
        //   oD0 = ((directional + hemisphere) * c69 + c68) * c72
        float lightingScale[4];
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
    SCENEIPC_ASSERT_LAYOUT(GeometryDesc,  64);
    SCENEIPC_ASSERT_LAYOUT(TextureDesc,   32);
    SCENEIPC_ASSERT_LAYOUT(Matrix4x4,     64);
    SCENEIPC_ASSERT_LAYOUT(FrameBegin,   160);
    SCENEIPC_ASSERT_LAYOUT(InstanceDesc, 184);
    SCENEIPC_ASSERT_LAYOUT(LightingDesc, 144);
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
    static_assert(offsetof(GeometryDesc, uvOffset)     == 32, "GeometryDesc layout");
    static_assert(offsetof(GeometryDesc, normalOffset) == 36, "GeometryDesc layout");
    static_assert(offsetof(GeometryDesc, colorOffset)  == 40, "GeometryDesc layout");
    static_assert(offsetof(GeometryDesc, boneCount)    == 44, "GeometryDesc layout");
    static_assert(offsetof(TextureDesc,  format)       == 8,  "TextureDesc layout");
    static_assert(offsetof(FrameBegin,   view)         == 16, "FrameBegin layout");
    static_assert(offsetof(FrameBegin,   projection)   == 80, "FrameBegin layout");
    static_assert(offsetof(InstanceDesc, clipTransform)  == 24,  "InstanceDesc layout");
    static_assert(offsetof(InstanceDesc, worldTransform) == 88,  "InstanceDesc layout");
    static_assert(offsetof(InstanceDesc, flags)          == 168, "InstanceDesc layout");
    static_assert(offsetof(InstanceDesc, paletteRegisters) == 172, "InstanceDesc layout");

    #undef SCENEIPC_ASSERT_LAYOUT

    // Messages are kept 8-byte aligned so every struct above lands naturally
    // aligned wherever it sits in the ring.
    inline uint32_t AlignMessage(uint32_t bytes)
    {
        return (bytes + 7u) & ~7u;
    }
}
