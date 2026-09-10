// scene_export.cpp
#include "scene_export.h"
#include "shader_analysis.h"
#include "resource_registry.h"
#include "../d3d8/d3d8_util.h"
#include "../ipc/shared_ring.h"
#include "../ipc/scene_conventions.h"
#include "../../utils/logger.h"

#include <windows.h>
#include <cstring>
#include <unordered_map>
#include <set>
#include <vector>

using namespace SceneIPC;

namespace
{
    using namespace Capture;

    SceneIPC::SharedRing g_ring;
    bool                 g_active = false;
    SceneExport::ExportStats g_stats;

    // Scratch buffers, allocated once and reused. This code runs inside the
    // game's draw calls, so a per-draw allocation would show up as a stutter.
    std::vector<uint8_t> g_vertexScratch;
    std::vector<uint8_t> g_indexScratch;

    // One morph delta stream at a time; reused across streams and draws so
    // the hot path stays free of allocation once it is warm.
    std::vector<uint8_t> g_morphScratch;

    // Geometry slices already uploaded, and the content hash they were
    // uploaded with. A slice whose hash changes has been re-skinned by the
    // game and must be sent again.
    std::unordered_map<uint64_t, uint32_t> g_sentGeometry;
    // Texture id -> how it went. A texture that failed once used to be
    // recorded here and never looked at again, which is wrong for exactly
    // the textures that matter most: the game composites its pitch into a
    // render target during the match load, so an attempt made before that
    // finishes finds nothing readable and the pitch then stays untextured
    // for the whole session.
    enum TextureState : uint32_t
    {
        kTexStateSent = 1,        // the host has it; nothing more to do
        kTexStateHopeless = 2,    // a format or a surface we cannot read at all
        kTexStateRetry = 3        // failed, but worth another attempt
    };

    struct SentTexture
    {
        uint32_t state;
        uint32_t attempts;
        uint64_t lastAttemptFrame;

        // What was actually sent, so that "already sent" can be checked
        // rather than assumed. Geometry has always worked this way - it
        // re-sends whenever its content hash changes - and textures were the
        // one resource that never looked again.
        uint32_t fingerprint;
        uint32_t width, height;
        uint32_t format;
        uint64_t lastCheckFrame;
    };
    std::unordered_map<uint64_t, SentTexture> g_sentTextures;

    // How long to wait before trying a failed texture again, and how many
    // times. Spread out because the cost is a lock and a copy of a whole
    // surface, and bounded because a texture that has failed a dozen times
    // over several seconds is not going to start working.
    const uint64_t kTextureRetryFrames   = 30;
    const uint32_t kTextureRetryAttempts = 12;

    // How often a texture already sent is read back and checked against what
    // was sent. Staggered by id, so with a few hundred textures resident this
    // is a couple of surface reads a frame rather than a burst.
    const uint64_t kTextureRecheckFrames = 240;

    // A hash of what was sent. Sparse on purpose: enough samples to separate
    // two different textures of the same size, few enough to be free next to
    // the surface read that produced the bytes.
    uint32_t FingerprintTexture(uint32_t width, uint32_t height, uint32_t format,
                                const std::vector<uint8_t>& pixels)
    {
        uint32_t h = 2166136261u;
        auto mix = [&h](uint32_t v)
        {
            h ^= v;
            h *= 16777619u;
        };
        mix(width); mix(height); mix(format); mix((uint32_t)pixels.size());

        const size_t n = pixels.size();
        const size_t step = (n > 4096) ? (n / 1024) : 1;
        for (size_t i = 0; i < n; i += step) mix(pixels[i]);
        return h;
    }

    // Last frame each geometry id was actually drawn.
    //
    // This exists because the producer's "already sent" set and the host's
    // resident cache are two caches with no invalidation channel between
    // them. When the host evicted a geometry the producer still believed was
    // sent, the producer never re-sent it and the host was left with
    // instances referencing geometry it did not have.
    //
    // The fix is an ordering invariant rather than a new message: the
    // producer forgets an id after kGeometryRetentionFrames, and the host
    // keeps entries for strictly longer (its --retain default is 3x this).
    // Anything the producer still believes cached was therefore used
    // recently enough that the host cannot yet have dropped it.
    std::unordered_map<uint64_t, uint64_t> g_geometryLastUsed;
    // Deliberately far below the host's own retention (900 frames by
    // default). A live run still showed ~35 instances per frame referencing
    // geometry the host had evicted and the producer still believed sent, so
    // the margin is widened rather than trusted: anything undrawn for a
    // second is forgotten here and re-sent on next use.
    const uint64_t kGeometryRetentionFrames = 60;

    LightingDesc g_lastLighting;
    bool         g_lightingSent = false;

    void PruneSentGeometry(uint64_t frameIndex)
    {
        if (frameIndex < kGeometryRetentionFrames) return;
        const uint64_t cutoff = frameIndex - kGeometryRetentionFrames;

        for (auto it = g_geometryLastUsed.begin(); it != g_geometryLastUsed.end(); )
        {
            if (it->second < cutoff)
            {
                g_sentGeometry.erase(it->first);
                it = g_geometryLastUsed.erase(it);
            }
            else ++it;
        }
    }

    uint32_t g_instancesThisFrame = 0;
    uint32_t g_droppedThisFrame   = 0;
    uint64_t g_frameIndex         = 0;

    // Clip transforms whose last column is nowhere near unit length. A
    // transposition puts thousands of units there; see
    // ClipTransformLooksSane. Cumulative, and logged once, because a
    // convention error is systematic and does not need reporting per draw.
    uint64_t g_insaneClipTransforms = 0;
    bool     g_warnedInsaneClip     = false;

    // Triangles dropped because an index pointed outside the vertex
    // slice that was sent. Should be zero: minIndex/numVertices define
    // the slice precisely. Counted rather than assumed.
    uint64_t g_indicesOutOfSlice = 0;
    bool     g_warnedOutOfSlice  = false;

    // Why draws were dropped. drawsSkipped alone hid the fact that the most
    // common vertex layout in the game was being rejected outright, so the
    // reasons are counted separately now.
    // Geometry ids the host asked to have sent again. A steady non-zero
    // rate means the host keeps losing geometry, which is worth knowing
    // even though the mechanism now recovers on its own.
    uint64_t g_resendHonoured = 0;

    // ── What the shaders do that the exporter used to drop ───────────────
    // Counted so a live run says whether these paths are actually being
    // taken, rather than leaving it to be judged from a screenshot.
    uint64_t g_morphDraws          = 0;   // draws with deltas blended in
    uint64_t g_morphStreamsMissing = 0;   // a delta stream was not bound
    uint64_t g_uvTransformDraws    = 0;   // draws carrying a texture transform
    uint64_t g_uvUnrecognised      = 0;   // oT0 written by something unmodelled
    uint64_t g_programUndecoded    = 0;   // bytecode the walk could not size

    // Instances thrown away because the game cleared the target after
    // sending them: an offscreen pass, not part of the picture.
    uint64_t g_frameResets      = 0;
    uint64_t g_instancesVoided  = 0;

    uint64_t g_skippedLayout        = 0;   // no position at 0, or too small
    uint64_t g_skippedNoDeclaration = 0;   // shader created before the hook

    // ── Geometry id churn diagnostic ─────────────────────────────────────
    // The host's geometry cache was observed growing without bound (~8-15 new
    // ids per frame forever) while the instance count stayed flat, which means
    // some logical mesh is being handed a fresh id every frame. Since one id
    // is meant to become one BLAS, that has to be understood before the
    // acceleration structures are built.
    //
    // Rather than guess which component of the id is churning, this groups
    // draws by a *reduced* key that ignores all the buffer offsets. If one
    // reduced key maps to many full ids, the offsets are the culprit and this
    // records which ones actually moved.
    struct ChurnRecord
    {
        // Distinct ids, not transitions. The first version of this counted
        // how often the id *changed*, which for a key covering N draws per
        // frame just reports N x frames and says nothing about churn.
        std::set<uint64_t> distinctIds;
        bool               overflowed;

        // A short history of consecutive samples for the worst key. If the
        // same object keeps its offsets frame to frame the values repeat on
        // a cycle; if the offsets drift, they do not. That distinction is
        // the whole question.
        struct Sample
        {
            uint64_t frame;
            uint64_t id;
            uint32_t startIndex, minIndex, vertexCount;
        };
        Sample   samples[16];
        uint32_t sampleCount;
    };

    std::unordered_map<uint64_t, ChurnRecord> g_churn;

    // Per-id use counts. An id drawn in exactly one frame ever was never
    // reused, which is the signature of an identity derived from buffer
    // placement rather than from the mesh itself.
    std::unordered_map<uint64_t, uint32_t> g_idFrameCount;

    // c0..c56 is the addressable palette: c57 holds the index scale and
    // c58 the view-projection, so the bones cannot reach past it. Three
    // registers per bone, so this is 18 bones with a row to spare.
    const uint32_t kPaletteRegisters = 57;

    const uint32_t kMaxTrackedIdsPerKey  = 4096;
    const uint32_t kChurnReportInterval  = 900;   // ~15s at 60fps

    // ── Hashing ──────────────────────────────────────────────────────────
    inline uint64_t Mix64(uint64_t h, uint64_t v)
    {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h;
    }

    uint32_t HashBytes(const void* data, size_t bytes)
    {
        // FNV-1a. Cheap enough to run over a draw's vertex range every frame,
        // which is what detects re-skinned geometry.
        const uint8_t* p = (const uint8_t*)data;
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < bytes; ++i)
        {
            h ^= p[i];
            h *= 16777619u;
        }
        return h;
    }

    // A geometry id identifies a *slice* of buffers, not a buffer: many draws
    // share one vertex buffer and differ only by index range, and each such
    // slice is its own mesh for BLAS purposes.
    uint64_t MakeGeometryId(uint32_t vbId, uint32_t ibId, const DrawCallInfo& info,
                            uint32_t baseVertex, uint32_t stride)
    {
        uint64_t h = 0xcbf29ce484222325ull;
        h = Mix64(h, vbId);
        h = Mix64(h, ibId);
        h = Mix64(h, info.primitiveType);
        h = Mix64(h, info.primitiveCount);
        h = Mix64(h, info.indexed ? info.startIndex : info.startVertex);
        h = Mix64(h, info.indexed ? info.minIndex : 0u);
        h = Mix64(h, info.indexed ? info.numVertices : 0u);
        h = Mix64(h, baseVertex);
        h = Mix64(h, stride);
        return h ? h : 1ull;   // never hand out 0; the host treats it as "none"
    }

    // Groups draws by a key that deliberately excludes every buffer offset.
    // A key holding many *distinct* ids means the offsets are what varies.
    void RecordChurn(uint32_t vbId, uint32_t ibId, const DrawCallInfo& info,
                     uint32_t vertexCount, uint64_t fullId, uint64_t frameIndex)
    {
        uint64_t reduced = 0xcbf29ce484222325ull;
        reduced = Mix64(reduced, vbId);
        reduced = Mix64(reduced, ibId);
        reduced = Mix64(reduced, info.primitiveCount);
        reduced = Mix64(reduced, vertexCount);

        ChurnRecord& r = g_churn[reduced];
        if (!r.overflowed)
        {
            if (r.distinctIds.size() < kMaxTrackedIdsPerKey)
                r.distinctIds.insert(fullId);
            else
                r.overflowed = true;
        }

        // Keep the most recent samples, oldest shifted out.
        const uint32_t kSamples = 16;
        if (r.sampleCount < kSamples)
        {
            ChurnRecord::Sample& sm = r.samples[r.sampleCount++];
            sm.frame = frameIndex; sm.id = fullId;
            sm.startIndex = info.startIndex; sm.minIndex = info.minIndex;
            sm.vertexCount = vertexCount;
        }
        else
        {
            for (uint32_t i = 1; i < kSamples; ++i) r.samples[i - 1] = r.samples[i];
            ChurnRecord::Sample& sm = r.samples[kSamples - 1];
            sm.frame = frameIndex; sm.id = fullId;
            sm.startIndex = info.startIndex; sm.minIndex = info.minIndex;
            sm.vertexCount = vertexCount;
        }

        ++g_idFrameCount[fullId];
    }

    void ReportChurn()
    {
        const ChurnRecord* worst = nullptr;
        size_t worstCount = 0;
        uint32_t stableKeys = 0, churningKeys = 0;

        for (auto it = g_churn.begin(); it != g_churn.end(); ++it)
        {
            const ChurnRecord& r = it->second;
            const size_t n = r.distinctIds.size();
            if (n <= 1) { ++stableKeys; continue; }
            ++churningKeys;
            if (!worst || n > worstCount) { worst = &r; worstCount = n; }
        }

        // An id drawn in exactly one frame ever was never reused, which is
        // what an identity derived from buffer placement produces.
        uint64_t onceOnly = 0;
        for (auto it = g_idFrameCount.begin(); it != g_idFrameCount.end(); ++it)
            if (it->second <= 1) ++onceOnly;

        Logger::Log("[Export] churn: %u stable keys, %u multi-id keys | "
                    "%llu ids total, %llu used in only one draw ever (%.1f%%) | "
                    "sent-cache %u",
                    stableKeys, churningKeys,
                    (unsigned long long)g_idFrameCount.size(),
                    (unsigned long long)onceOnly,
                    g_idFrameCount.empty() ? 0.0
                        : (100.0 * onceOnly / g_idFrameCount.size()),
                    (uint32_t)g_sentGeometry.size());

        if (worst)
        {
            Logger::Log("  worst key: %llu distinct ids%s. Recent draws "
                        "(frame / startIndex / minIndex / vtx):",
                        (unsigned long long)worstCount,
                        worst->overflowed ? " (capped)" : "");
            for (uint32_t i = 0; i < worst->sampleCount; ++i)
            {
                const ChurnRecord::Sample& sm = worst->samples[i];
                Logger::Log("    f%-8llu si=%-6u mi=%-6u vtx=%-5u id=%016llX",
                            (unsigned long long)sm.frame, sm.startIndex,
                            sm.minIndex, sm.vertexCount,
                            (unsigned long long)sm.id);
            }
        }
    }

    uint32_t TranslateTextureFormat(D3DFORMAT fmt)
    {
        switch (fmt)
        {
        case D3DFMT_A8R8G8B8: return kTexBGRA8;
        // Not the same thing: X8 has no alpha, only an undefined byte where
        // alpha would be. Reporting it as BGRA8 hands the consumer garbage
        // coverage.
        case D3DFMT_X8R8G8B8: return kTexBGRX8;
        case D3DFMT_R5G6B5:   return kTexBGR565;
        case D3DFMT_A1R5G5B5:
        case D3DFMT_X1R5G5B5: return kTexBGRA5551;
        case D3DFMT_A4R4G4B4: return kTexBGRA4444;
        case D3DFMT_DXT1:     return kTexDXT1;
        case D3DFMT_DXT3:     return kTexDXT3;
        case D3DFMT_DXT5:     return kTexDXT5;
        case D3DFMT_L8:       return kTexL8;
        case D3DFMT_A8L8:     return kTexA8L8;
        default:              return kTexUnknown;
        }
    }

    // ── Skinning detection ───────────────────────────────────────────────
    //
    // The game skins players with a matrix palette in the vertex shader:
    //
    //     mul   r10, v2, c57.z        ; palette row = index colour * scale
    //     mov   a0.x, r10.x
    //     m4x3  r11, v0, c0           ; c[a0.x + 0..2] is one bone
    //     mul   r11.xyz, r11.xyzz, v1.x
    //     ...                          ; repeated per influence
    //     m4x4  oPos, r11, c58
    //
    // So `m4x3` against a constant is the signature, and the number of them
    // is the number of influences. The unskinned shaders transform straight
    // through `m4x4 oPos, v0, c58` and contain no m4x3 at all.
    //
    // Scanning the bytecode rather than pattern-matching the vertex
    // declaration matters: a 24-byte pre-lit vertex also carries a D3DCOLOR,
    // and treating that diffuse colour as a bone index would wreck geometry
    // that is currently correct.
    const uint32_t kSm1OpcodeMask   = 0x0000FFFFu;
    const uint32_t kSm1OpM4x3       = 21u;
    const uint32_t kSm1OpEnd        = 0x0000FFFFu;
    const uint32_t kSm1RegTypeShift = 28u;
    const uint32_t kSm1RegTypeConst = 2u;

    // What the shader does to a vertex, decoded from its bytecode. This
    // used to be a single loose scan counting m4x3 instructions, which was
    // enough for skinning and blind to everything else the shaders do -
    // the texture transform behind the advertising hoardings and the morph
    // targets behind every player's build and expression.
    void DescribeProgram(const std::vector<uint32_t>& fn,
                         ShaderAnalysis::VertexShaderProgram& out)
    {
        ShaderAnalysis::Analyse(fn, out);
    }

    // The two D3DCOLOR inputs a skinning declaration carries. The higher
    // register is the palette index and the lower, when present, the weights
    // - true of both the 36-byte (one influence, no weights) and 40-byte
    // (two or three influences) layouts, and confirmed against the shaders
    // that consume them.
    void FindSkinAttributes(const D3D8Util::VertexDeclLayout& decl,
                            uint32_t& outIndexOffset, uint32_t& outWeightOffset)
    {
        outIndexOffset  = kNoVertexAttribute;
        outWeightOffset = kNoVertexAttribute;

        uint32_t bestReg = 0, prevReg = 0;
        for (uint32_t i = 0; i < decl.elementCount; ++i)
        {
            const D3D8Util::VertexDeclElement& e = decl.elements[i];
            if (e.stream != 0 || e.type != D3D8Util::kVsdtD3DColor) continue;

            if (outIndexOffset == kNoVertexAttribute || e.reg > bestReg)
            {
                outWeightOffset = outIndexOffset;
                prevReg         = bestReg;
                outIndexOffset  = e.offset;
                bestReg         = e.reg;
            }
            else if (outWeightOffset == kNoVertexAttribute || e.reg > prevReg)
            {
                outWeightOffset = e.offset;
                prevReg         = e.reg;
            }
        }
    }

    // ── Vertex layout ────────────────────────────────────────────────────
    //
    // Where each attribute sits inside a vertex, read from the game's own
    // declaration rather than inferred from the stride.
    //
    // This used to be a two-line guess: stride 24 meant pos/colour/uv, stride
    // 32 meant pos/normal/uv, anything else was rejected. The game has at
    // least five layouts — 24, 32, 36 and 40 bytes, plus multi-stream
    // variants — and the 40-byte one is the most common of all, 5,944 of
    // 11,207 world draws across every capture. Rejecting it dropped every
    // player body while their heads and hands, which use other layouts, kept
    // rendering.
    //
    // Position is required at offset 0, because that is what an acceleration
    // structure build reads and it is true of every layout the game uses.
    // Everything after it moves.
    struct VertexLayout
    {
        uint32_t kind;           // VertexKind
        uint32_t uvOffset;       // kNoVertexAttribute when absent
        uint32_t normalOffset;
        uint32_t colorOffset;
        uint32_t boneCount;      // 0 when the shader does not skin
        uint32_t boneIndexOffset;
        uint32_t boneWeightOffset;
        bool     usable;         // position at offset 0, stride large enough

        // ── What the vertex shader does beyond reading stream 0 ──────────
        // Both of these are per-shader facts that no amount of looking at
        // the vertex buffer can reveal.
        ShaderAnalysis::VertexShaderProgram program;

        // Where each morph delta stream lives, resolved from the shader's
        // input register through the declaration. Parallel to
        // program.morph[]; kNoVertexAttribute for a register the declaration
        // does not bind, which should not happen and is skipped if it does.
        uint32_t morphStream[ShaderAnalysis::kMaxMorphTargets];
        uint32_t morphOffset[ShaderAnalysis::kMaxMorphTargets];
    };

    void ClearSkin(VertexLayout& l)
    {
        l.boneCount        = 0;
        l.boneIndexOffset  = kNoVertexAttribute;
        l.boneWeightOffset = kNoVertexAttribute;
        memset(&l.program, 0, sizeof(l.program));
        for (uint32_t i = 0; i < ShaderAnalysis::kMaxMorphTargets; ++i)
        {
            l.morphStream[i] = kNoVertexAttribute;
            l.morphOffset[i] = kNoVertexAttribute;
        }
    }

    VertexLayout DescribeFvfLayout(uint32_t fvf, uint32_t stride)
    {
        VertexLayout out;
        out.kind         = kVertexPreLit;
        out.uvOffset     = kNoVertexAttribute;
        out.normalOffset = kNoVertexAttribute;
        out.colorOffset  = kNoVertexAttribute;
        out.usable       = false;

        ClearSkin(out);

        D3D8Util::FvfLayout fl;
        if (!D3D8Util::FvfDecode(fvf, fl)) return out;
        if (fl.posOffset != 0 || fl.positionIsTransformed) return out;

        if (fl.normalOffset >= 0)
        {
            out.normalOffset = (uint32_t)fl.normalOffset;
            out.kind         = kVertexLit;
        }
        if (fl.diffuseOffset >= 0) out.colorOffset = (uint32_t)fl.diffuseOffset;

        // Only a 2-float set is a texture coordinate this renderer can use.
        for (int i = 0; i < fl.texCoordCount; ++i)
        {
            if (fl.texCoordFloats[i] == 2 && fl.texCoordOffset[i] >= 0)
            {
                out.uvOffset = (uint32_t)fl.texCoordOffset[i];
                break;
            }
        }

        out.usable = (stride >= 12);
        return out;
    }

    // A declaration feeding a real vertex shader binds plain input registers
    // with no semantics, so the attributes have to be identified structurally:
    // the float3 at offset 0 is the position, a later float3 is a normal, and
    // a float2 is a texture coordinate. That holds for every layout this game
    // uses, and the alternative — trusting D3DVSDE_* register numbers — is
    // meaningless without a fixed-function pipeline behind them.
    VertexLayout DescribeDeclLayout(const Registry::VertexShaderInfo& vs,
                                    uint32_t stride)
    {
        VertexLayout out;
        out.kind         = kVertexPreLit;
        out.uvOffset     = kNoVertexAttribute;
        out.normalOffset = kNoVertexAttribute;
        out.colorOffset  = kNoVertexAttribute;
        out.usable       = false;

        ClearSkin(out);
        const D3D8Util::VertexDeclLayout& decl = vs.layout;
        if (!decl.valid) return out;

        // A skinned vertex's D3DCOLOR fields are bone data, not a diffuse
        // colour, so this has to be settled before the loop below claims one
        // as colorOffset.
        DescribeProgram(vs.function, out.program);
        out.boneCount = out.program.paletteTransforms;
        if (out.boneCount)
            FindSkinAttributes(decl, out.boneIndexOffset, out.boneWeightOffset);

        // A morph delta is bound to an input register; the declaration says
        // which stream and offset that register reads from. Resolved here
        // rather than assumed to be "stream N for register N+2", because the
        // mapping is the declaration's to make.
        for (uint32_t m = 0; m < out.program.morphCount; ++m)
        {
            const uint32_t reg = out.program.morph[m].inputRegister;
            for (uint32_t i = 0; i < decl.elementCount; ++i)
            {
                const D3D8Util::VertexDeclElement& e = decl.elements[i];
                if (e.reg != reg || e.type != D3D8Util::kVsdtFloat3) continue;
                out.morphStream[m] = e.stream;
                out.morphOffset[m] = e.offset;
                break;
            }
        }

        bool havePosition = false;
        for (uint32_t i = 0; i < decl.elementCount; ++i)
        {
            const D3D8Util::VertexDeclElement& e = decl.elements[i];

            // Stream 0 only. The multi-stream layouts put extra float3 data
            // in stream 1, which is not a normal for stream 0's vertices and
            // would be read at the wrong stride if treated as one.
            if (e.stream != 0) continue;

            if (e.type == D3D8Util::kVsdtFloat3 && e.offset == 0)
            {
                havePosition = true;
                continue;
            }
            if (e.type == D3D8Util::kVsdtFloat3 &&
                out.normalOffset == kNoVertexAttribute)
            {
                out.normalOffset = e.offset;
                out.kind         = kVertexLit;
            }
            else if (e.type == D3D8Util::kVsdtFloat2 &&
                     out.uvOffset == kNoVertexAttribute)
            {
                out.uvOffset = e.offset;
            }
            else if (e.type == D3D8Util::kVsdtD3DColor &&
                     out.colorOffset == kNoVertexAttribute &&
                     e.offset != out.boneIndexOffset &&
                     e.offset != out.boneWeightOffset)
            {
                out.colorOffset = e.offset;
            }
        }

        out.usable = havePosition && stride >= 12;
        return out;
    }

    // The interpretation of c58..c61 lives in scene_conventions.h, shared
    // with the host so its self-test can assert it; this is inside the game
    // and has no test harness of its own.
    void ReadClipTransform(const DeviceState& state, Matrix4x4& out)
    {
        ClipTransformFromConstants(state.vsConstants, kClipTransformRegister,
                                   out);
    }

    // out = a * b, row-vector convention: a point transforms as v * a * b.
    void Multiply(const Matrix4x4& a, const Matrix4x4& b, Matrix4x4& out)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
            {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k) s += a.m[r * 4 + k] * b.m[k * 4 + c];
                out.m[r * 4 + c] = s;
            }
    }

    void MatrixFromD3D(const D3DMATRIX& src, Matrix4x4& out)
    {
        memcpy(out.m, &src._11, sizeof(float) * 16);
    }

    // ── Topology ─────────────────────────────────────────────────────────
    //
    // An acceleration structure has exactly one triangle topology: a list.
    // Direct3D 8 has three, and this game overwhelmingly uses the one that is
    // not a list — 403 of a match frame's 405 world draws are strips.
    //
    // Sending a strip's indices verbatim and letting the consumer group them
    // in threes builds triangles out of vertices that were never adjacent:
    // a 513-triangle strip becomes 171 arbitrary ones spanning the whole
    // mesh. That is what produced the long slivers fanning across the first
    // traced frames.
    //
    // Indices are also rebased here. Only the slice a draw touches is sent
    // (`firstVertex` onwards), while the index buffer holds absolute indices,
    // so they have to be moved into the slice's numbering by the side that
    // knows both numbers — this one.
    struct TriangleListBuilder
    {
        std::vector<uint8_t>& out;
        uint32_t              firstVertex;
        uint32_t              vertexCount;
        uint32_t              stride;       // 2 or 4, chosen for the slice
        uint32_t              emitted;
        uint32_t              rejected;     // out of slice, counted not hidden

        TriangleListBuilder(std::vector<uint8_t>& dst, uint32_t first,
                            uint32_t count)
            : out(dst), firstVertex(first), vertexCount(count)
            , stride(count > 0xFFFFu ? 4u : 2u), emitted(0), rejected(0)
        {
            out.clear();
        }

        void Add(uint32_t a, uint32_t b, uint32_t c)
        {
            // Strips stitch separate runs together with degenerate triangles.
            // They contribute nothing and would only bloat the structure.
            if (a == b || b == c || a == c) return;

            if (a < firstVertex || b < firstVertex || c < firstVertex)
            {
                ++rejected;
                return;
            }
            a -= firstVertex; b -= firstVertex; c -= firstVertex;
            if (a >= vertexCount || b >= vertexCount || c >= vertexCount)
            {
                ++rejected;
                return;
            }

            const uint32_t tri[3] = { a, b, c };
            const size_t at = out.size();
            out.resize(at + 3 * stride);
            for (int i = 0; i < 3; ++i)
            {
                if (stride == 2)
                {
                    uint16_t v = (uint16_t)tri[i];
                    memcpy(&out[at + i * 2], &v, 2);
                }
                else
                {
                    memcpy(&out[at + i * 4], &tri[i], 4);
                }
            }
            ++emitted;
        }
    };

    // Reads index `i` of the draw, in the index buffer's own numbering.
    inline uint32_t IndexAt(const uint8_t* raw, uint32_t rawStride, uint32_t i)
    {
        if (rawStride == 4)
        {
            uint32_t v; memcpy(&v, raw + i * 4, 4); return v;
        }
        uint16_t v; memcpy(&v, raw + i * 2, 2); return v;
    }

    // Expands a draw into a rebased triangle list. `raw` may be null for a
    // non-indexed draw, whose vertices were read consecutively from
    // `startVertex` and so are already in order.
    // The builder already knows the slice, so it is not repeated here.
    void BuildTriangleList(const DrawCallInfo& info, const uint8_t* raw,
                           uint32_t rawStride, uint32_t baseVertexIndex,
                           TriangleListBuilder& b)
    {
        const uint32_t verts =
            D3D8Util::PrimitiveVertexCount(info.primitiveType,
                                           info.primitiveCount);
        if (verts < 3) return;

        // One accessor for both cases keeps the topology logic single-copy.
        // A non-indexed draw's nth vertex is startVertex + n; an indexed
        // draw's is baseVertexIndex + the index buffer's nth entry.
        struct Fetch
        {
            const uint8_t* raw; uint32_t rawStride, base;
            uint32_t operator()(uint32_t n) const
            {
                return raw ? base + IndexAt(raw, rawStride, n) : base + n;
            }
        } at = { raw, rawStride, raw ? baseVertexIndex : info.startVertex };

        switch (info.primitiveType)
        {
        case D3DPT_TRIANGLELIST:
            for (uint32_t t = 0; t < info.primitiveCount; ++t)
                b.Add(at(t * 3), at(t * 3 + 1), at(t * 3 + 2));
            break;

        case D3DPT_TRIANGLESTRIP:
            // Winding alternates. Ray tracing only cares when culling is on,
            // but preserving it costs nothing and keeps that option open.
            for (uint32_t t = 0; t < info.primitiveCount; ++t)
            {
                if (t & 1) b.Add(at(t + 1), at(t), at(t + 2));
                else       b.Add(at(t),     at(t + 1), at(t + 2));
            }
            break;

        case D3DPT_TRIANGLEFAN:
            for (uint32_t t = 0; t < info.primitiveCount; ++t)
                b.Add(at(0), at(t + 1), at(t + 2));
            break;

        default:
            break;   // points and lines are not surfaces; nothing to trace
        }
    }

    // ── Buffer readback ──────────────────────────────────────────────────
    bool ReadVertexRange(IDirect3DVertexBuffer8* vb, uint32_t byteOffset,
                         uint32_t byteCount, std::vector<uint8_t>& out)
    {
        if (!vb || byteCount == 0) return false;

        D3DVERTEXBUFFER_DESC desc;
        if (FAILED(vb->GetDesc(&desc)))                     return false;
        if ((uint64_t)byteOffset + byteCount > desc.Size)   return false;

        BYTE* p = nullptr;
        if (FAILED(vb->Lock(byteOffset, byteCount, &p, D3DLOCK_READONLY)) || !p)
            return false;

        out.assign(p, p + byteCount);
        vb->Unlock();
        return true;
    }

    bool ReadIndexRange(IDirect3DIndexBuffer8* ib, uint32_t byteOffset,
                        uint32_t byteCount, std::vector<uint8_t>& out,
                        uint32_t& outStride)
    {
        if (!ib || byteCount == 0) return false;

        D3DINDEXBUFFER_DESC desc;
        if (FAILED(ib->GetDesc(&desc)))                     return false;
        outStride = (desc.Format == D3DFMT_INDEX32) ? 4u : 2u;
        if ((uint64_t)byteOffset + byteCount > desc.Size)   return false;

        BYTE* p = nullptr;
        if (FAILED(ib->Lock(byteOffset, byteCount, &p, D3DLOCK_READONLY)) || !p)
            return false;

        out.assign(p, p + byteCount);
        ib->Unlock();
        return true;
    }

    // Copies the rows of a locked surface out, tightly packed.
    //
    // The lock's pitch is not the row length: the driver pads rows, and for a
    // block-compressed format a "row" is a row of 4x4 blocks rather than of
    // pixels. Both have to be right or the image arrives sheared.
    void PackLockedRows(const D3DLOCKED_RECT& lr, uint32_t fmt,
                        const D3DSURFACE_DESC& level0,
                        std::vector<uint8_t>& pixels)
    {
        const uint32_t rowBytes = D3D8Util::SurfaceBytes(level0.Format,
                                                         level0.Width, 1);
        const uint32_t rows = (fmt == kTexDXT1 || fmt == kTexDXT3 ||
                               fmt == kTexDXT5)
                            ? (level0.Height + 3) / 4
                            : level0.Height;

        pixels.resize((size_t)rowBytes * rows);
        for (uint32_t y = 0; y < rows; ++y)
            memcpy(pixels.data() + (size_t)y * rowBytes,
                   (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch, rowBytes);
    }

    // Reads level 0 of a texture, whichever way works.
    //
    // A texture in D3DPOOL_MANAGED or SYSTEMMEM locks directly. One in
    // D3DPOOL_DEFAULT - which is where a render target lives, and where this
    // game puts the pitch it composites at match load - refuses to lock, and
    // has to be copied into a surface that can be. That is what CopyRects is
    // for, and skipping it was costing every runtime-generated texture in
    // the game.
    bool ReadTextureLevel0(IDirect3DDevice8* device,
                           IDirect3DTexture8* tex2d, uint32_t fmt,
                           const D3DSURFACE_DESC& level0,
                           std::vector<uint8_t>& pixels, bool& viaCopy)
    {
        viaCopy = false;

        D3DLOCKED_RECT lr;
        if (SUCCEEDED(tex2d->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
        {
            PackLockedRows(lr, fmt, level0, pixels);
            tex2d->UnlockRect(0);
            return true;
        }

        if (!device) return false;

        IDirect3DSurface8* src = nullptr;
        if (FAILED(tex2d->GetSurfaceLevel(0, &src)) || !src) return false;

        // Same size and same format: CopyRects will not convert, and asking
        // it to is how this silently produces nothing.
        IDirect3DSurface8* dst = nullptr;
        bool ok = false;
        if (SUCCEEDED(device->CreateImageSurface(level0.Width, level0.Height,
                                                 level0.Format, &dst)) && dst)
        {
            if (SUCCEEDED(device->CopyRects(src, nullptr, 0, dst, nullptr)) &&
                SUCCEEDED(dst->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
            {
                PackLockedRows(lr, fmt, level0, pixels);
                dst->UnlockRect();
                ok = true;
                viaCopy = true;
            }
            dst->Release();
        }
        src->Release();
        return ok;
    }

    // Uploads a texture the first time an instance references it.
    void EnsureTextureSent(IDirect3DDevice8* device,
                           IDirect3DBaseTexture8* texture, uint64_t& outId)
    {
        outId = 0;
        if (!texture) return;

        ResourceInfo* info = Registry::Find(texture);
        if (!info) return;
        outId = info->id;

        SentTexture& record = g_sentTextures[info->id];
        if (record.state == kTexStateHopeless) return;

        if (record.state == kTexStateSent)
        {
            // Is the texture behind this pointer still the one that was sent?
            //
            // The registry keys on the pointer and nothing hooks Release, so
            // a released texture whose address Direct3D recycles arrives here
            // wearing the dead one's id. Taking "sent" at face value is what
            // painted the pitch with a kit atlas.
            IDirect3DTexture8* live = (IDirect3DTexture8*)texture;
            D3DSURFACE_DESC now;
            if (FAILED(live->GetLevelDesc(0, &now))) return;

            // Size or format differing settles it without reading anything.
            const bool shapeChanged = now.Width  != record.width  ||
                                      now.Height != record.height ||
                                      (uint32_t)now.Format != record.format;

            if (!shapeChanged)
            {
                // Otherwise the content has to be looked at, which costs a
                // surface read - so only every so often, and staggered by id
                // so the cost is spread rather than spiking.
                if (g_stats.framesSent - record.lastCheckFrame <
                    kTextureRecheckFrames)
                    return;
                record.lastCheckFrame = g_stats.framesSent;

                const uint32_t fmt = TranslateTextureFormat(info->format);
                std::vector<uint8_t> current;
                bool viaCopy = false;
                if (!ReadTextureLevel0(device, live, fmt, now, current, viaCopy))
                    return;   // unreadable this time; keep what was sent

                if (FingerprintTexture(now.Width, now.Height,
                                       (uint32_t)now.Format, current) ==
                    record.fingerprint)
                    return;
            }

            // Changed. Fall through and send it again under the same id: from
            // the game's point of view there is one texture at this pointer,
            // and the host replaces the pixels it holds for that id.
            ++g_stats.texturesContentChanged;
            record.state    = 0;
            record.attempts = 0;
        }

        if (record.attempts)
        {
            // A retry, spread out in time and bounded in number.
            if (record.attempts >= kTextureRetryAttempts)
            {
                record.state = kTexStateHopeless;
                ++g_stats.texturesUnreadable;
                return;
            }
            if (g_stats.framesSent - record.lastAttemptFrame <
                kTextureRetryFrames)
                return;
            ++g_stats.texturesRetried;
        }
        ++record.attempts;
        record.lastAttemptFrame = g_stats.framesSent;

        const uint32_t fmt = TranslateTextureFormat(info->format);
        if (fmt == kTexUnknown)
        {
            // Palettised and other exotic formats need the palette to be
            // meaningful. Nothing about that will change, so this one is
            // genuinely finished rather than worth retrying.
            record.state = kTexStateHopeless;
            ++g_stats.texturesUnknownFormat;
            return;
        }

        IDirect3DTexture8* tex2d = (IDirect3DTexture8*)texture;
        D3DSURFACE_DESC level0;
        if (FAILED(tex2d->GetLevelDesc(0, &level0)))
        {
            record.state = kTexStateHopeless;
            ++g_stats.texturesUnreadable;
            return;
        }

        std::vector<uint8_t> pixels;
        bool viaCopy = false;
        if (!ReadTextureLevel0(device, tex2d, fmt, level0, pixels, viaCopy))
        {
            // Worth another go: a render target that is not populated yet
            // can become readable a moment later, and the pitch is exactly
            // that case.
            record.state = kTexStateRetry;
            return;
        }
        if (viaCopy) ++g_stats.texturesCopiedBack;

        TextureDesc td;
        memset(&td, 0, sizeof(td));
        td.textureId    = info->id;
        td.format       = fmt;
        td.width        = level0.Width;
        td.height       = level0.Height;
        td.mipLevels    = 1;              // level 0 only for now
        td.payloadBytes = (uint32_t)pixels.size();

        if (g_ring.TryWrite(kMsgTexture, &td, sizeof(td), pixels.data(),
                            td.payloadBytes, false))
        {
            record.state       = kTexStateSent;
            record.width       = level0.Width;
            record.height      = level0.Height;
            record.format      = (uint32_t)level0.Format;
            record.fingerprint = FingerprintTexture(level0.Width, level0.Height,
                                                    (uint32_t)level0.Format,
                                                    pixels);
            record.lastCheckFrame = g_stats.framesSent;
            ++g_stats.textureUploads;
            g_stats.textureBytes += td.payloadBytes;
        }
        else
        {
            // A resource message that does not fit is not dropped silently —
            // it is retried next frame, because the host cannot render an
            // instance whose texture never arrived.
            ++g_stats.writeFailures;
        }
    }
}

namespace Capture { namespace SceneExport {

bool Init(const char* sectionName, uint64_t ringBytes)
{
    memset(&g_stats, 0, sizeof(g_stats));
    g_sentGeometry.clear();
    g_sentTextures.clear();
    g_lightingSent = false;

    if (!g_ring.CreateAsProducer(sectionName, ringBytes))
    {
        Logger::Log("[Export] Could not create scene section '%s' (err=%lu); "
                    "export disabled.", sectionName, GetLastError());
        g_active = false;
        return false;
    }

    g_vertexScratch.reserve(1u << 20);
    g_indexScratch.reserve(1u << 18);
    g_active = true;
    Logger::Log("[Export] Scene stream open on '%s' (%llu MB ring).",
                sectionName, (unsigned long long)(ringBytes / (1024 * 1024)));
    return true;
}

void Shutdown()
{
    if (!g_active) return;
    g_ring.TryWrite(kMsgShutdown, nullptr, 0, nullptr, 0, false);
    Logger::Log("[Export] Scene stream closing: %llu frames, %llu instances, "
                "%llu geometry uploads (%.1f MB), %llu textures (%.1f MB), "
                "%llu draws skipped, %llu write failures. Textures not sent: "
                "%llu unknown format, %llu unreadable (%llu needed a copy, "
                "%llu retries). Textures re-sent after their content changed "
                "under the same pointer: %llu. Resources recreated at an "
                "address already on record: %u.",
                (unsigned long long)g_stats.framesSent,
                (unsigned long long)g_stats.instancesSent,
                (unsigned long long)g_stats.geometryUploads,
                g_stats.geometryBytes / (1024.0 * 1024.0),
                (unsigned long long)g_stats.textureUploads,
                g_stats.textureBytes / (1024.0 * 1024.0),
                (unsigned long long)g_stats.drawsSkipped,
                (unsigned long long)g_stats.writeFailures,
                (unsigned long long)g_stats.texturesUnknownFormat,
                (unsigned long long)g_stats.texturesUnreadable,
                (unsigned long long)g_stats.texturesCopiedBack,
                (unsigned long long)g_stats.texturesRetried,
                (unsigned long long)g_stats.texturesContentChanged,
                Registry::RecreatedAtSameAddress());

    // What the shader bytecode turned out to be doing. Reported separately
    // because each of these was invisible until it was counted: the whole
    // reason players stood in a neutral pose and every hoarding showed its
    // entire advert sheet is that nothing here was ever measured.
    Logger::Log("[Export] Shader-driven vertex work: %llu draws with morph "
                "targets blended (%llu delta streams unavailable), %llu draws "
                "carrying a texture transform, %llu with an oT0 this does not "
                "model, %llu shaders that would not decode.",
                (unsigned long long)g_morphDraws,
                (unsigned long long)g_morphStreamsMissing,
                (unsigned long long)g_uvTransformDraws,
                (unsigned long long)g_uvUnrecognised,
                (unsigned long long)g_programUndecoded);

    Logger::Log("[Export] Offscreen passes discarded: %llu mid-frame target "
                "clears voided %llu instances (%.1f per frame) that the game "
                "drew and then threw away.",
                (unsigned long long)g_frameResets,
                (unsigned long long)g_instancesVoided,
                g_stats.framesSent
                    ? (double)g_instancesVoided / (double)g_stats.framesSent
                    : 0.0);
    g_ring.Close();
    g_active = false;
}

bool IsActive() { return g_active; }
const ExportStats& Stats() { return g_stats; }

void BeginFrame(uint64_t frameIndex, uint32_t width, uint32_t height)
{
    if (!g_active) return;

    g_frameIndex         = frameIndex;
    g_instancesThisFrame = 0;
    g_droppedThisFrame   = 0;

    // ── Honour the host's resend requests ────────────────────────────────
    // Anything the host was asked to draw but did not have is forgotten
    // here, so the next draw that uses it sends it again. This is the only
    // thing that makes the two caches agree; the retention windows on either
    // side count different clocks and cannot (see RingHeader's resend
    // table). Done before any draw this frame, so a request is served
    // immediately rather than a frame late.
    {
        uint64_t wanted[SceneIPC::kMaxResendRequests];
        const uint32_t n = g_ring.TakeResendRequests(wanted,
                                                     SceneIPC::kMaxResendRequests);
        for (uint32_t i = 0; i < n; ++i)
        {
            g_sentGeometry.erase(wanted[i]);
            g_geometryLastUsed.erase(wanted[i]);
        }
        g_resendHonoured += n;
    }

    FrameBegin fb;
    memset(&fb, 0, sizeof(fb));
    fb.frameIndex   = frameIndex;
    fb.renderWidth  = width;
    fb.renderHeight = height;

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    fb.producerTimeNs = (uint64_t)((now.QuadPart * 1000000000ull) / freq.QuadPart);

    // The view/projection here come from SetTransform, which the shader draws
    // ignore. They are sent anyway as the host's starting guess at the shared
    // camera; the authoritative per-draw transform is the WVP in each
    // instance. Where the two disagree, the instances win.
    if (!g_ring.TryWrite(kMsgFrameBegin, &fb, sizeof(fb), nullptr, 0, true))
        ++g_stats.writeFailures;
}

void OnClearTarget()
{
    if (!g_active) return;

    // Nothing to void, and nothing worth a message: a clear before any draw
    // is the ordinary start of a pass.
    if (g_instancesThisFrame == 0) return;

    // The first few, in full. This rule decides which draws are in the
    // picture at all, so if it is ever wrong the failure is a blank frame -
    // and a blank frame with no explanation is the worst thing to debug.
    // Five lines in the log turn that into a diagnosis: a reset firing at
    // the very end of a frame, voiding everything, says so plainly.
    if (g_frameResets < 5)
        Logger::Log("[Export] Frame %llu: full target clear after %u "
                    "instances - discarding them as an offscreen pass.",
                    (unsigned long long)g_frameIndex, g_instancesThisFrame);

    ++g_frameResets;
    g_instancesVoided += g_instancesThisFrame;
    g_instancesThisFrame = 0;

    // Sent as a resource message, not as frame traffic. Frame traffic is
    // droppable by design, and a dropped reset is worse than a dropped
    // frame: the instances it was meant to void have already arrived, so
    // losing it leaves an offscreen pass in the picture.
    if (!g_ring.TryWrite(kMsgFrameReset, nullptr, 0, nullptr, 0, false))
        ++g_stats.writeFailures;
}

void EndFrame()
{
    if (!g_active) return;

    FrameEnd fe;
    memset(&fe, 0, sizeof(fe));
    fe.frameIndex       = g_frameIndex;
    fe.instanceCount    = g_instancesThisFrame;
    fe.droppedInstances = g_droppedThisFrame;

    if (g_ring.TryWrite(kMsgFrameEnd, &fe, sizeof(fe), nullptr, 0, true))
        ++g_stats.framesSent;
    else
        ++g_stats.writeFailures;

    PruneSentGeometry(g_frameIndex);

    if (g_stats.framesSent && (g_stats.framesSent % kChurnReportInterval) == 0)
        ReportChurn();

    // Once is enough: if the c58 convention is wrong it is wrong for every
    // draw of every frame, and the point of the message is to name the cause
    // rather than to count the symptoms.
    if (g_insaneClipTransforms && !g_warnedInsaneClip)
    {
        g_warnedInsaneClip = true;
        Logger::Log("[Export] %llu clip transforms have a non-unit last "
            "column. The usual cause is reading c58..c61 as rows; they are "
            "columns (see ReadClipTransform). Geometry will collapse toward "
            "the screen centre.",
            (unsigned long long)g_insaneClipTransforms);
    }

    if (g_stats.framesSent && (g_stats.framesSent % kChurnReportInterval) == 0 &&
        (g_skippedLayout || g_skippedNoDeclaration))
    {
        Logger::Log("[Export] resend requests honoured: %llu",
            (unsigned long long)g_resendHonoured);
        Logger::Log("[Export] draws skipped: %llu unusable vertex layout, "
            "%llu with a declaration created before the hook",
            (unsigned long long)g_skippedLayout,
            (unsigned long long)g_skippedNoDeclaration);
    }

    if (g_indicesOutOfSlice && !g_warnedOutOfSlice)
    {
        g_warnedOutOfSlice = true;
        Logger::Log("[Export] %llu triangles dropped for referencing vertices "
            "outside the slice that was sent. minIndex/numVertices are "
            "supposed to bound exactly what a draw touches, so this means "
            "either the game lied about them or the slice is being computed "
            "wrongly.",
            (unsigned long long)g_indicesOutOfSlice);
    }
}

void UpdateLighting(const DeviceState& state)
{
    if (!g_active) return;

    // Register assignments come from disassembling the game's own vertex
    // shaders; see docs/RENDERER.md §4.3.
    LightingDesc L;
    memset(&L, 0, sizeof(L));
    memcpy(L.directionalDir,   state.vsConstants[95], sizeof(float) * 4);
    memcpy(L.directionalColor, state.vsConstants[94], sizeof(float) * 4);
    memcpy(L.hemisphereAxis,   state.vsConstants[93], sizeof(float) * 4);
    memcpy(L.skyColor,         state.vsConstants[92], sizeof(float) * 4);
    memcpy(L.groundColor,      state.vsConstants[91], sizeof(float) * 4);
    memcpy(L.ambient,          state.vsConstants[68], sizeof(float) * 4);
    memcpy(L.specularColor,    state.vsConstants[70], sizeof(float) * 4);
    memcpy(L.specularHalfDir,  state.vsConstants[63], sizeof(float) * 4);
    memcpy(L.lightingScale,    state.vsConstants[69], sizeof(float) * 4);

    // Only resend on change: the rig is stable for long stretches and the
    // host carries the last value across frames.
    if (g_lightingSent && memcmp(&L, &g_lastLighting, sizeof(L)) == 0) return;

    if (g_ring.TryWrite(kMsgLighting, &L, sizeof(L), nullptr, 0, false))
    {
        g_lastLighting = L;
        g_lightingSent = true;
    }
}

void OnWorldDraw(IDirect3DDevice8* realDevice, const DeviceState& state,
                 const DrawCallInfo& info)
{
    (void)realDevice;
    if (!g_active) return;

    // User-pointer draws carry their vertices inline and are always 2D in
    // this game; nothing world-space arrives that way.
    if (info.userPointer) { ++g_stats.drawsSkipped; return; }

    const uint32_t stride = state.stream[0].stride;

    // Ask the game what its vertices look like rather than guessing from the
    // stride; see DescribeDeclLayout for what that guess cost.
    VertexLayout layout;
    if (state.VertexShaderIsFvf())
    {
        layout = DescribeFvfLayout(state.vertexShader, stride);
    }
    else
    {
        const Registry::VertexShaderInfo* vs =
            Registry::FindVertexShader(state.vertexShader);
        if (!vs) { ++g_stats.drawsSkipped; ++g_skippedNoDeclaration; return; }
        layout = DescribeDeclLayout(*vs, stride);
    }

    if (!layout.usable) { ++g_stats.drawsSkipped; ++g_skippedLayout; return; }
    const uint32_t kind = layout.kind;

    ResourceInfo* vbInfo = Registry::Find(state.stream[0].buffer);
    if (!vbInfo) { ++g_stats.drawsSkipped; return; }

    ResourceInfo* ibInfo = info.indexed ? Registry::Find(state.indexBuffer)
                                        : nullptr;
    if (info.indexed && !ibInfo) { ++g_stats.drawsSkipped; return; }

    uint64_t geometryId =
        MakeGeometryId(vbInfo->id, ibInfo ? ibInfo->id : 0u, info,
                       state.baseVertexIndex, stride);

    // A morphed draw's positions are stream 0 blended with other streams, so
    // two draws sharing stream 0 and differing only in their delta buffers
    // are different meshes. Folded in only when the shader morphs, which
    // leaves every other draw's id byte-identical to what it has always
    // been - including in recordings made before this existed.
    for (uint32_t m = 0; m < layout.program.morphCount; ++m)
    {
        const uint32_t si = layout.morphStream[m];
        if (si == kNoVertexAttribute || si >= D3D8_MAX_STREAMS) continue;
        const ResourceInfo* mi = Registry::Find(state.stream[si].buffer);
        geometryId = Mix64(geometryId, mi ? mi->id : 0u);
    }
    if (!geometryId) geometryId = 1ull;

    // ── Read back exactly the slice this draw uses ───────────────────────
    uint32_t vertexCount = 0, firstVertex = 0;
    if (info.indexed)
    {
        firstVertex = state.baseVertexIndex + info.minIndex;
        vertexCount = info.numVertices;
    }
    else
    {
        firstVertex = info.startVertex;
        vertexCount = D3D8Util::PrimitiveVertexCount(info.primitiveType,
                                                     info.primitiveCount);
    }
    if (vertexCount == 0) { ++g_stats.drawsSkipped; return; }

    if (!ReadVertexRange((IDirect3DVertexBuffer8*)state.stream[0].buffer,
                         firstVertex * stride, vertexCount * stride,
                         g_vertexScratch))
    {
        ++g_stats.drawsSkipped;
        return;
    }

    // ── Morph targets ────────────────────────────────────────────────────
    //
    // The shader adds weighted deltas from streams 1..6 to stream 0's
    // position before transforming it:
    //
    //     mov   r11,      v0
    //     mad   r11.xyz,  v3, c81.x, r11
    //     ...
    //     m4x4  oPos,     r11, c58
    //
    // Applied here rather than sent for the host to apply, because the
    // weights are the game's own and almost always static - a build morph
    // is set once per player and never changes - so the blended positions
    // hash identically frame to frame and upload exactly once. Sending the
    // deltas separately would cost a payload per stream and buy nothing
    // except for the draws whose weights genuinely animate, which are the
    // small ones.
    //
    // Measured on the capture set: 110 morph draws in a match frame,
    // sharing two distinct weight sets between them, unchanged across
    // frames a hundred apart.
    uint32_t morphApplied = 0;
    if (layout.program.morphCount && stride >= 12)
    {
        for (uint32_t m = 0; m < layout.program.morphCount; ++m)
        {
            const uint32_t streamIndex = layout.morphStream[m];
            if (streamIndex == kNoVertexAttribute ||
                streamIndex >= D3D8_MAX_STREAMS)
            {
                ++g_morphStreamsMissing;
                continue;
            }

            const StreamBinding& src = state.stream[streamIndex];
            if (!src.buffer || src.stride < layout.morphOffset[m] + 12)
            {
                ++g_morphStreamsMissing;
                continue;
            }

            const ShaderAnalysis::MorphTarget& t = layout.program.morph[m];
            if (t.weightRegister >= kMaxVsConstants) continue;
            const float weight =
                state.vsConstants[t.weightRegister][t.weightComponent];
            if (weight == 0.0f) continue;   // contributes nothing; skip the read

            if (!ReadVertexRange(src.buffer, firstVertex * src.stride,
                                 vertexCount * src.stride, g_morphScratch))
            {
                ++g_morphStreamsMissing;
                continue;
            }

            uint8_t* dst = g_vertexScratch.data();
            const uint8_t* delta = g_morphScratch.data() + layout.morphOffset[m];
            for (uint32_t v = 0; v < vertexCount; ++v)
            {
                float pos[3], d[3];
                memcpy(pos, dst + (size_t)v * stride, sizeof(pos));
                memcpy(d,   delta + (size_t)v * src.stride, sizeof(d));
                pos[0] += d[0] * weight;
                pos[1] += d[1] * weight;
                pos[2] += d[2] * weight;
                memcpy(dst + (size_t)v * stride, pos, sizeof(pos));
            }
            ++morphApplied;
        }
        if (morphApplied) ++g_morphDraws;
    }

    // Read the draw's own indices, then expand whatever topology it used
    // into the triangle list an acceleration structure requires. Every draw
    // ends up indexed, including the non-indexed ones: it costs a little
    // bandwidth and leaves the consumer with exactly one case to handle.
    std::vector<uint8_t> rawIndices;
    uint32_t rawStride = 2;
    if (info.indexed)
    {
        const uint32_t rawCount =
            D3D8Util::PrimitiveVertexCount(info.primitiveType,
                                           info.primitiveCount);
        D3DINDEXBUFFER_DESC ibDesc;
        if (SUCCEEDED(((IDirect3DIndexBuffer8*)state.indexBuffer)->GetDesc(&ibDesc)))
            rawStride = (ibDesc.Format == D3DFMT_INDEX32) ? 4u : 2u;

        if (!ReadIndexRange((IDirect3DIndexBuffer8*)state.indexBuffer,
                            info.startIndex * rawStride,
                            rawCount * rawStride, rawIndices, rawStride))
        {
            ++g_stats.drawsSkipped;
            return;
        }
    }

    TriangleListBuilder builder(g_indexScratch, firstVertex, vertexCount);
    BuildTriangleList(info, info.indexed ? rawIndices.data() : nullptr,
                      rawStride, state.baseVertexIndex, builder);

    if (builder.emitted == 0) { ++g_stats.drawsSkipped; return; }
    g_indicesOutOfSlice += builder.rejected;

    const uint32_t indexStride = builder.stride;
    const uint32_t indexCount  = builder.emitted * 3;

    // ── Upload the slice if it is new or has changed ─────────────────────
    // Hashing the vertex range every frame is what catches re-skinned
    // players: the game does CPU skinning, so their vertices are rewritten
    // in place and the buffer id alone would look unchanged.
    const uint32_t contentHash = HashBytes(g_vertexScratch.data(),
                                           g_vertexScratch.size());

    RecordChurn(vbInfo->id, ibInfo ? ibInfo->id : 0u, info, vertexCount,
                geometryId, g_frameIndex);

    auto sent = g_sentGeometry.find(geometryId);
    if (sent == g_sentGeometry.end() || sent->second != contentHash)
    {
        GeometryDesc gd;
        memset(&gd, 0, sizeof(gd));
        gd.geometryId   = geometryId;
        gd.vertexKind   = kind;
        gd.vertexStride = stride;
        gd.vertexCount  = vertexCount;
        gd.indexCount   = indexCount;
        gd.indexStride  = indexStride;
        gd.contentHash  = contentHash;
        gd.uvOffset     = layout.uvOffset;
        gd.normalOffset = layout.normalOffset;
        gd.colorOffset  = layout.colorOffset;
        gd.boneCount        = layout.boneCount;
        gd.boneIndexOffset  = layout.boneIndexOffset;
        gd.boneWeightOffset = layout.boneWeightOffset;
        gd.morphTargets     = morphApplied;

        // Vertices and indices go as one payload, in that order, matching
        // how GeometryDesc documents the layout.
        std::vector<uint8_t>& payload = g_vertexScratch;
        const size_t vbBytes = payload.size();
        payload.insert(payload.end(), g_indexScratch.begin(), g_indexScratch.end());

        if (g_ring.TryWrite(kMsgGeometry, &gd, sizeof(gd), payload.data(),
                            (uint32_t)payload.size(), false))
        {
            g_sentGeometry[geometryId] = contentHash;
            ++g_stats.geometryUploads;
            g_stats.geometryBytes += payload.size();
        }
        else
        {
            // Retry next frame rather than emitting an instance whose
            // geometry the host does not have.
            ++g_stats.writeFailures;
            payload.resize(vbBytes);
            return;
        }
        payload.resize(vbBytes);
    }

    // Touch it whether or not it was re-uploaded: retention is about when a
    // geometry was last *drawn*, not when it last changed.
    g_geometryLastUsed[geometryId] = g_frameIndex;

    // ── Materials ────────────────────────────────────────────────────────
    uint64_t baseTextureId = 0, normalTextureId = 0;
    EnsureTextureSent(realDevice, state.texture[0], baseTextureId);
    EnsureTextureSent(realDevice, state.texture[1], normalTextureId);

    // ── The instance ─────────────────────────────────────────────────────
    InstanceDesc inst;
    memset(&inst, 0, sizeof(inst));
    inst.geometryId      = geometryId;
    inst.baseTextureId   = baseTextureId;
    inst.normalTextureId = normalTextureId;

    // A fixed-function draw genuinely is transformed by SetTransform, so its
    // world matrix is known outright and needs no factorisation by the host —
    // and its clip transform comes from the same place. It must NOT come from
    // c58: a fixed-function draw does not run a vertex shader, so those
    // registers still hold whatever the last shader draw left behind. Feeding
    // that to the host's view-projection resolver offers it candidates that
    // belong to no draw at all.
    if (state.VertexShaderIsFvf() && state.vertexShader != 0)
    {
        MatrixFromD3D(state.world, inst.worldTransform);
        inst.flags |= kInstanceWorldValid;

        Matrix4x4 v, p, vp;
        MatrixFromD3D(state.view, v);
        MatrixFromD3D(state.projection, p);
        Multiply(v, p, vp);
        Multiply(inst.worldTransform, vp, inst.clipTransform);
    }
    else
    {
        ReadClipTransform(state, inst.clipTransform);

        // A transposed read is the failure mode this check exists for; see
        // ClipTransformLooksSane. Counted rather than dropped, because a
        // genuinely odd matrix is data about the game, not a reason to
        // discard the draw.
        if (!ClipTransformLooksSane(inst.clipTransform)) ++g_insaneClipTransforms;
    }

    // c72 is the global tint the vertex shaders multiply their output by.
    memcpy(inst.baseColorFactor, state.vsConstants[72], sizeof(float) * 4);

    // ── Bone palette ─────────────────────────────────────────────────────
    // Sent with the instance rather than with the geometry: the bone-local
    // vertices never change and are cached once, while the pose changes
    // every frame. That keeps animated players off the geometry path, which
    // would otherwise re-upload 26,000 vertices per frame.
    //
    // The whole addressable range goes, not just the bones this draw touches,
    // because which rows it indexes is per-vertex data we would have to scan
    // to find out - and the range is under a kilobyte.
    std::vector<uint8_t> palette;
    if (layout.boneCount)
    {
        const uint32_t rows = kPaletteRegisters;
        palette.resize(rows * 16);
        memcpy(palette.data(), state.vsConstants[0], rows * 16);

        inst.paletteRegisters = rows;
        inst.boneIndexScale   = state.vsConstants[57][2];   // c57.z
    }

    // ── The texture transform ────────────────────────────────────────────
    // Evaluated here because only the producer knows which constants this
    // particular shader reads: c75 is the u basis and offset in one shader
    // and an entirely unrelated fade plane (`dp4 oD0.w, v0, c75`) in
    // another, so the registers mean nothing without the bytecode that uses
    // them. Appended after the palette so the palette keeps its offset.
    if (layout.program.uvIsTransformed &&
        layout.program.uvRegisterA < kMaxVsConstants &&
        layout.program.uvRegisterB < kMaxVsConstants)
    {
        const float* a = state.vsConstants[layout.program.uvRegisterA];
        const float* b = state.vsConstants[layout.program.uvRegisterB];

        const size_t at = palette.size();
        palette.resize(at + kUvTransformBytes);
        memcpy(palette.data() + at,      a, sizeof(float) * 4);
        memcpy(palette.data() + at + 16, b, sizeof(float) * 4);

        inst.flags |= kInstanceUvTransform;
        ++g_uvTransformDraws;
    }
    else if (layout.program.uvUnrecognised)
    {
        ++g_uvUnrecognised;
    }
    if (!layout.program.decoded && !state.VertexShaderIsFvf())
        ++g_programUndecoded;

    // Stage 0 is the base map. See InstanceDesc::stageState for the packing
    // and for why it stays inside four bytes.
    inst.stageState =
        (state.stageState[0][D3DTSS_ADDRESSU]  & 0xFFu) |
        ((state.stageState[0][D3DTSS_ADDRESSV] & 0xFFu) << 8) |
        ((state.stageState[0][D3DTSS_ALPHAOP]  & 0xFFu) << 16) |
        ((state.stageState[0][D3DTSS_ALPHAARG1] & 0xFFu) << 24);

    if (state.renderState[D3DRS_ALPHABLENDENABLE]) inst.flags |= kInstanceAlphaBlend;
    if (!state.renderState[D3DRS_ZWRITEENABLE])    inst.flags |= kInstanceNoDepthWrite;
    if (state.renderState[D3DRS_ALPHATESTENABLE])  inst.flags |= kInstanceAlphaTest;
    if (state.renderState[D3DRS_CULLMODE] == 1)    inst.flags |= kInstanceTwoSided;

    // D3DCMP_ALWAYS is the game saying this draw goes over what is already
    // there whatever the depth buffer holds - a stronger statement than
    // "does not write depth", and the only thing that orders two exactly
    // coplanar surfaces in a rasteriser. 150 of 893 world draws on a
    // measured frame.
    if (state.renderState[D3DRS_ZFUNC] == D3DCMP_ALWAYS)
        inst.flags |= kInstanceDepthAlways;
    if (kind == kVertexPreLit)                     inst.flags |= kInstancePreLit;

    if (g_ring.TryWrite(kMsgInstance, &inst, sizeof(inst),
                        palette.empty() ? nullptr : palette.data(),
                        (uint32_t)palette.size(), true))
        ++g_instancesThisFrame, ++g_stats.instancesSent;
    else
        ++g_droppedThisFrame;
}

}} // namespace Capture::SceneExport
