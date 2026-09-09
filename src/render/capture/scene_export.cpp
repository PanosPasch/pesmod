// scene_export.cpp
#include "scene_export.h"
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

    // Geometry slices already uploaded, and the content hash they were
    // uploaded with. A slice whose hash changes has been re-skinned by the
    // game and must be sent again.
    std::unordered_map<uint64_t, uint32_t> g_sentGeometry;
    std::unordered_map<uint64_t, uint32_t> g_sentTextures;

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
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8: return kTexBGRA8;
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

    // ── Vertex layout classification ─────────────────────────────────────
    // From docs/RENDERER.md §4.2: the game's 3D geometry is either 24 bytes
    // (position / D3DCOLOR / uv, pre-lit) or 32 bytes (position / normal /
    // uv, dynamically lit). Anything else is not something the ray tracer
    // knows how to interpret, and is reported rather than guessed at.
    uint32_t ClassifyVertexKind(uint32_t stride)
    {
        if (stride == 24) return kVertexPreLit;
        if (stride == 32) return kVertexLit;
        return kVertexUnknown;
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

    // Uploads a texture the first time an instance references it.
    void EnsureTextureSent(IDirect3DBaseTexture8* texture, uint64_t& outId)
    {
        outId = 0;
        if (!texture) return;

        ResourceInfo* info = Registry::Find(texture);
        if (!info) return;
        outId = info->id;

        if (g_sentTextures.count(info->id)) return;

        const uint32_t fmt = TranslateTextureFormat(info->format);
        if (fmt == kTexUnknown)
        {
            // Palettised and other exotic formats need the palette to be
            // meaningful; mark as sent so it is not retried every frame.
            g_sentTextures[info->id] = 0;
            return;
        }

        IDirect3DTexture8* tex2d = (IDirect3DTexture8*)texture;
        D3DSURFACE_DESC level0;
        if (FAILED(tex2d->GetLevelDesc(0, &level0)))
        {
            g_sentTextures[info->id] = 0;
            return;
        }

        D3DLOCKED_RECT lr;
        if (FAILED(tex2d->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
        {
            // Typically a D3DPOOL_DEFAULT render target. Not fatal: the host
            // renders it untextured rather than stalling.
            g_sentTextures[info->id] = 0;
            return;
        }

        const uint32_t rowBytes = D3D8Util::SurfaceBytes(level0.Format,
                                                         level0.Width, 1);
        const uint32_t rows = (fmt == kTexDXT1 || fmt == kTexDXT3 ||
                               fmt == kTexDXT5)
                            ? (level0.Height + 3) / 4
                            : level0.Height;

        std::vector<uint8_t> pixels;
        pixels.resize((size_t)rowBytes * rows);
        for (uint32_t y = 0; y < rows; ++y)
            memcpy(pixels.data() + (size_t)y * rowBytes,
                   (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch, rowBytes);
        tex2d->UnlockRect(0);

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
            g_sentTextures[info->id] = 1;
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
                "%llu draws skipped, %llu write failures.",
                (unsigned long long)g_stats.framesSent,
                (unsigned long long)g_stats.instancesSent,
                (unsigned long long)g_stats.geometryUploads,
                g_stats.geometryBytes / (1024.0 * 1024.0),
                (unsigned long long)g_stats.textureUploads,
                g_stats.textureBytes / (1024.0 * 1024.0),
                (unsigned long long)g_stats.drawsSkipped,
                (unsigned long long)g_stats.writeFailures);
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
    const uint32_t kind   = ClassifyVertexKind(stride);
    if (kind == kVertexUnknown) { ++g_stats.drawsSkipped; return; }

    ResourceInfo* vbInfo = Registry::Find(state.stream[0].buffer);
    if (!vbInfo) { ++g_stats.drawsSkipped; return; }

    ResourceInfo* ibInfo = info.indexed ? Registry::Find(state.indexBuffer)
                                        : nullptr;
    if (info.indexed && !ibInfo) { ++g_stats.drawsSkipped; return; }

    const uint64_t geometryId =
        MakeGeometryId(vbInfo->id, ibInfo ? ibInfo->id : 0u, info,
                       state.baseVertexIndex, stride);

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
    EnsureTextureSent(state.texture[0], baseTextureId);
    EnsureTextureSent(state.texture[1], normalTextureId);

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

    if (state.renderState[D3DRS_ALPHABLENDENABLE]) inst.flags |= kInstanceAlphaBlend;
    if (!state.renderState[D3DRS_ZWRITEENABLE])    inst.flags |= kInstanceNoDepthWrite;
    if (state.renderState[D3DRS_ALPHATESTENABLE])  inst.flags |= kInstanceAlphaTest;
    if (state.renderState[D3DRS_CULLMODE] == 1)    inst.flags |= kInstanceTwoSided;
    if (kind == kVertexPreLit)                     inst.flags |= kInstancePreLit;

    if (g_ring.TryWrite(kMsgInstance, &inst, sizeof(inst), nullptr, 0, true))
        ++g_instancesThisFrame, ++g_stats.instancesSent;
    else
        ++g_droppedThisFrame;
}

}} // namespace Capture::SceneExport
