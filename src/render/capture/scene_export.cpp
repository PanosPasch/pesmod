// scene_export.cpp
#include "scene_export.h"
#include "resource_registry.h"
#include "../d3d8/d3d8_util.h"
#include "../ipc/shared_ring.h"
#include "../../utils/logger.h"

#include <windows.h>
#include <cstring>
#include <unordered_map>
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

    LightingDesc g_lastLighting;
    bool         g_lightingSent = false;

    uint32_t g_instancesThisFrame = 0;
    uint32_t g_droppedThisFrame   = 0;
    uint64_t g_frameIndex         = 0;

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
        uint32_t distinctIds;
        uint32_t firstStartIndex,  lastStartIndex;
        uint32_t firstMinIndex,    lastMinIndex;
        uint32_t firstBaseVertex,  lastBaseVertex;
        uint32_t firstStartVertex, lastStartVertex;
        uint32_t firstContentHash, lastContentHash;
        uint64_t lastFullId;
    };
    std::unordered_map<uint64_t, ChurnRecord> g_churn;
    const uint32_t kChurnReportInterval = 900;   // ~15s at 60fps

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

    // Records one draw against a key that deliberately excludes every buffer
    // offset, so a key with many distinct ids proves the offsets are what is
    // churning — and the first/last samples say which ones moved.
    void RecordChurn(uint32_t vbId, uint32_t ibId, const DrawCallInfo& info,
                     uint32_t vertexCount, uint32_t baseVertex,
                     uint64_t fullId, uint32_t contentHash)
    {
        uint64_t reduced = 0xcbf29ce484222325ull;
        reduced = Mix64(reduced, vbId);
        reduced = Mix64(reduced, ibId);
        reduced = Mix64(reduced, info.primitiveCount);
        reduced = Mix64(reduced, vertexCount);

        ChurnRecord& r = g_churn[reduced];
        if (r.distinctIds == 0)
        {
            r.firstStartIndex  = info.startIndex;
            r.firstMinIndex    = info.minIndex;
            r.firstBaseVertex  = baseVertex;
            r.firstStartVertex = info.startVertex;
            r.firstContentHash = contentHash;
            r.distinctIds      = 1;
            r.lastFullId       = fullId;
        }
        else if (fullId != r.lastFullId)
        {
            ++r.distinctIds;
            r.lastFullId = fullId;
        }
        r.lastStartIndex  = info.startIndex;
        r.lastMinIndex    = info.minIndex;
        r.lastBaseVertex  = baseVertex;
        r.lastStartVertex = info.startVertex;
        r.lastContentHash = contentHash;
    }

    void ReportChurn()
    {
        // Only the worst offenders matter; a mesh with a stable id has 1.
        const ChurnRecord* worst[3] = { nullptr, nullptr, nullptr };
        uint32_t stableKeys = 0, churningKeys = 0;
        for (auto it = g_churn.begin(); it != g_churn.end(); ++it)
        {
            const ChurnRecord& r = it->second;
            if (r.distinctIds <= 1) { ++stableKeys; continue; }
            ++churningKeys;
            for (int s = 0; s < 3; ++s)
                if (!worst[s] || r.distinctIds > worst[s]->distinctIds)
                {
                    for (int m = 2; m > s; --m) worst[m] = worst[m - 1];
                    worst[s] = &r;
                    break;
                }
        }

        Logger::Log("[Export] geometry id churn: %u stable keys, %u churning "
                    "(cache holds %u ids)", stableKeys, churningKeys,
                    (uint32_t)g_sentGeometry.size());
        for (int s = 0; s < 3 && worst[s]; ++s)
        {
            const ChurnRecord& r = *worst[s];
            Logger::Log("  #%d: %u ids for one mesh | startIndex %u->%u | "
                        "minIndex %u->%u | baseVertex %u->%u | "
                        "startVertex %u->%u | contentHash %08X->%08X",
                        s + 1, r.distinctIds,
                        r.firstStartIndex,  r.lastStartIndex,
                        r.firstMinIndex,    r.lastMinIndex,
                        r.firstBaseVertex,  r.lastBaseVertex,
                        r.firstStartVertex, r.lastStartVertex,
                        r.firstContentHash, r.lastContentHash);
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

    // Copies c58..c61 out of the shadowed constant file. `m4x4 oPos, v0, c58`
    // performs one dp4 per row, so the four registers are the matrix rows in
    // the order a row-vector multiply expects.
    void ReadClipTransform(const DeviceState& state, Matrix4x4& out)
    {
        const uint32_t kWvpBaseRegister = 58;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                out.m[row * 4 + col] = state.vsConstants[kWvpBaseRegister + row][col];
    }

    void MatrixFromD3D(const D3DMATRIX& src, Matrix4x4& out)
    {
        memcpy(out.m, &src._11, sizeof(float) * 16);
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

    if (g_stats.framesSent && (g_stats.framesSent % kChurnReportInterval) == 0)
        ReportChurn();
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

    uint32_t indexStride = 2, indexCount = 0;
    g_indexScratch.clear();
    if (info.indexed)
    {
        indexCount = D3D8Util::PrimitiveVertexCount(info.primitiveType,
                                                    info.primitiveCount);
        D3DINDEXBUFFER_DESC ibDesc;
        if (SUCCEEDED(((IDirect3DIndexBuffer8*)state.indexBuffer)->GetDesc(&ibDesc)))
            indexStride = (ibDesc.Format == D3DFMT_INDEX32) ? 4u : 2u;

        if (!ReadIndexRange((IDirect3DIndexBuffer8*)state.indexBuffer,
                            info.startIndex * indexStride,
                            indexCount * indexStride,
                            g_indexScratch, indexStride))
        {
            ++g_stats.drawsSkipped;
            return;
        }
    }

    // ── Upload the slice if it is new or has changed ─────────────────────
    // Hashing the vertex range every frame is what catches re-skinned
    // players: the game does CPU skinning, so their vertices are rewritten
    // in place and the buffer id alone would look unchanged.
    const uint32_t contentHash = HashBytes(g_vertexScratch.data(),
                                           g_vertexScratch.size());

    RecordChurn(vbInfo->id, ibInfo ? ibInfo->id : 0u, info, vertexCount,
                state.baseVertexIndex, geometryId, contentHash);

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

    ReadClipTransform(state, inst.clipTransform);

    // A fixed-function draw genuinely is transformed by SetTransform, so its
    // world matrix is known outright and needs no factorisation by the host.
    if (state.VertexShaderIsFvf() && state.vertexShader != 0)
    {
        MatrixFromD3D(state.world, inst.worldTransform);
        inst.flags |= kInstanceWorldValid;
    }

    // c72 is the global tint the vertex shaders multiply their output by.
    memcpy(inst.baseColorFactor, state.vsConstants[72], sizeof(float) * 4);

    if (state.renderState[D3DRS_ALPHABLENDENABLE]) inst.flags |= kInstanceAlphaBlend;
    if (state.renderState[D3DRS_ALPHATESTENABLE])  inst.flags |= kInstanceAlphaTest;
    if (state.renderState[D3DRS_CULLMODE] == 1)    inst.flags |= kInstanceTwoSided;
    if (kind == kVertexPreLit)                     inst.flags |= kInstancePreLit;

    if (g_ring.TryWrite(kMsgInstance, &inst, sizeof(inst), nullptr, 0, true))
        ++g_instancesThisFrame, ++g_stats.instancesSent;
    else
        ++g_droppedThisFrame;
}

}} // namespace Capture::SceneExport
