// frame_capture.cpp
#include "frame_capture.h"
#include "resource_registry.h"
#include "../d3d8/d3d8_util.h"
#include "../render_config.h"
#include "../../utils/logger.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
    using namespace Capture;

    // ── Module state ─────────────────────────────────────────────────────
    std::string  g_outputDir;
    bool         g_initialised   = false;
    uint32_t     g_frameIndex    = 0;
    bool         g_armed         = false;   // dump the next frame
    bool         g_capturing     = false;   // dumping the current frame
    std::string  g_frameDir;
    FILE*        g_drawsFile     = nullptr;
    FILE*        g_vsConstFile   = nullptr;
    uint32_t     g_drawsWritten  = 0;
    bool         g_first3DSeen   = false;   // for the capture_on_first_3d trigger

    // How often the session summary is rewritten, in frames. At 60fps this is
    // roughly every 10 seconds — cheap enough not to matter, frequent enough
    // that a killed process still leaves a near-complete report.
    const uint32_t kSessionReportInterval = 600;

    // Resources referenced this frame, and whether their bytes actually made
    // it to disk. Not everything can be read back — a D3DPOOL_DEFAULT texture
    // typically refuses LockRect — and the manifest has to say so rather than
    // naming a file that was never written.
    struct DumpOutcome
    {
        bool        ok;
        const char* reason;   // null when ok
    };
    std::map<uint32_t, DumpOutcome> g_dumpedResources;

    FrameStats   g_stats;                   // frame in progress
    FrameStats   g_lastStats;               // last completed frame

    std::set<uint32_t>    g_frameFvfs;
    std::set<const void*> g_frameTextures;

    // Session aggregates for the summary report.
    struct SessionTotals
    {
        uint64_t frames;
        uint64_t draws, draws2D, draws3D, drawsUnknownFvf;
        uint64_t triangles, triangles2D, triangles3D;
        uint64_t setRenderState, redundantRenderState;
        uint64_t setTextureStageState, setTexture, setTransform;
        uint32_t maxDrawsInAFrame;
        uint32_t minDrawsInAFrame;
    };
    SessionTotals g_session;
    std::set<uint32_t>    g_sessionFvfs;

    // ── Small helpers ────────────────────────────────────────────────────
    void EnsureDirectory(const char* path)
    {
        CreateDirectoryA(path, nullptr);
    }

    // The world matrix translation. Used as a cheap stand-in for object
    // position when accumulating scene extents on non-capture frames.
    void MatrixTranslation(const D3DMATRIX& m, float out[3])
    {
        out[0] = m._41; out[1] = m._42; out[2] = m._43;
    }

    void AccumulateBounds(FrameStats& s, const float p[3])
    {
        if (!s.worldBoundsValid)
        {
            s.worldBoundsValid = true;
            for (int i = 0; i < 3; ++i) { s.worldMin[i] = s.worldMax[i] = p[i]; }
            return;
        }
        for (int i = 0; i < 3; ++i)
        {
            if (p[i] < s.worldMin[i]) s.worldMin[i] = p[i];
            if (p[i] > s.worldMax[i]) s.worldMax[i] = p[i];
        }
    }

    // ── Draw classification ──────────────────────────────────────────────
    // Three-way, not two. Treating "anything that isn't screen space" as
    // world space is wrong: a draw whose vertex format we cannot resolve is
    // unknown, and must not be fed to the ray tracer as if it were geometry.
    enum DrawSpace { kSpaceScreen, kSpaceWorld, kSpaceUnknown };

    const char* DrawSpaceName(DrawSpace s)
    {
        return s == kSpaceScreen ? "screen"
             : s == kSpaceWorld  ? "world"
                                 : "unknown";
    }

    DrawSpace ClassifyDraw(const DeviceState& st,
                           const Registry::VertexShaderInfo** outDecl)
    {
        *outDecl = nullptr;
        const uint32_t arg = st.vertexShader;

        if (D3D8Util::VertexShaderArgIsFvf(arg))
        {
            if (arg == 0)                          return kSpaceUnknown;
            if (D3D8Util::FvfIsScreenSpace(arg))   return kSpaceScreen;
            if (D3D8Util::FvfStride(arg) == 0)     return kSpaceUnknown;
            return kSpaceWorld;
        }

        // A handle from CreateVertexShader. With a declaration and no shader
        // function this is fixed-function world-space T&L; a declaration we
        // never saw created leaves the layout unknown.
        const Registry::VertexShaderInfo* info = Registry::FindVertexShader(arg);
        *outDecl = info;
        return info ? kSpaceWorld : kSpaceUnknown;
    }

    void WriteMatrixJson(FILE* f, const char* name, const D3DMATRIX& m)
    {
        fprintf(f, "\"%s\":[", name);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                fprintf(f, "%s%.6g", (r == 0 && c == 0) ? "" : ",", m.m[r][c]);
        fprintf(f, "]");
    }

    // ── DDS output ───────────────────────────────────────────────────────
    struct DdsPixelFormat
    {
        uint32_t flags, fourCC, bitCount, rMask, gMask, bMask, aMask;
    };

    const uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4;
    const uint32_t DDPF_RGB = 0x40, DDPF_LUMINANCE = 0x20000;

    bool DdsFormatFor(D3DFORMAT fmt, DdsPixelFormat& out)
    {
        memset(&out, 0, sizeof(out));
        switch (fmt)
        {
        case D3DFMT_A8R8G8B8:
            out.flags = DDPF_RGB | DDPF_ALPHAPIXELS; out.bitCount = 32;
            out.rMask = 0x00ff0000; out.gMask = 0x0000ff00;
            out.bMask = 0x000000ff; out.aMask = 0xff000000; return true;
        case D3DFMT_X8R8G8B8:
            out.flags = DDPF_RGB; out.bitCount = 32;
            out.rMask = 0x00ff0000; out.gMask = 0x0000ff00;
            out.bMask = 0x000000ff; return true;
        case D3DFMT_R8G8B8:
            out.flags = DDPF_RGB; out.bitCount = 24;
            out.rMask = 0xff0000; out.gMask = 0x00ff00; out.bMask = 0x0000ff;
            return true;
        case D3DFMT_R5G6B5:
            out.flags = DDPF_RGB; out.bitCount = 16;
            out.rMask = 0xf800; out.gMask = 0x07e0; out.bMask = 0x001f;
            return true;
        case D3DFMT_A1R5G5B5:
            out.flags = DDPF_RGB | DDPF_ALPHAPIXELS; out.bitCount = 16;
            out.rMask = 0x7c00; out.gMask = 0x03e0; out.bMask = 0x001f;
            out.aMask = 0x8000; return true;
        case D3DFMT_X1R5G5B5:
            out.flags = DDPF_RGB; out.bitCount = 16;
            out.rMask = 0x7c00; out.gMask = 0x03e0; out.bMask = 0x001f;
            return true;
        case D3DFMT_A4R4G4B4:
            out.flags = DDPF_RGB | DDPF_ALPHAPIXELS; out.bitCount = 16;
            out.rMask = 0x0f00; out.gMask = 0x00f0; out.bMask = 0x000f;
            out.aMask = 0xf000; return true;
        case D3DFMT_L8:
            out.flags = DDPF_LUMINANCE; out.bitCount = 8;
            out.rMask = 0xff; return true;
        case D3DFMT_A8L8:
            out.flags = DDPF_LUMINANCE | DDPF_ALPHAPIXELS; out.bitCount = 16;
            out.rMask = 0x00ff; out.aMask = 0xff00; return true;
        case D3DFMT_A8:
            out.flags = DDPF_ALPHA; out.bitCount = 8; out.aMask = 0xff;
            return true;
        case D3DFMT_DXT1: out.flags = DDPF_FOURCC; out.fourCC = D3DFMT_DXT1; return true;
        case D3DFMT_DXT2: out.flags = DDPF_FOURCC; out.fourCC = D3DFMT_DXT2; return true;
        case D3DFMT_DXT3: out.flags = DDPF_FOURCC; out.fourCC = D3DFMT_DXT3; return true;
        case D3DFMT_DXT4: out.flags = DDPF_FOURCC; out.fourCC = D3DFMT_DXT4; return true;
        case D3DFMT_DXT5: out.flags = DDPF_FOURCC; out.fourCC = D3DFMT_DXT5; return true;
        default:
            return false;   // P8 and friends need the palette; dumped raw
        }
    }

    void WriteDdsHeader(FILE* f, D3DFORMAT fmt, uint32_t w, uint32_t h,
                        uint32_t mipCount, const DdsPixelFormat& pf)
    {
        const bool compressed = (pf.flags & DDPF_FOURCC) != 0;

        uint32_t hdr[32];
        memset(hdr, 0, sizeof(hdr));
        hdr[0] = 124;                                   // dwSize
        hdr[1] = 0x1 | 0x2 | 0x4 | 0x1000               // CAPS|HEIGHT|WIDTH|PIXELFORMAT
               | (mipCount > 1 ? 0x20000u : 0u)         // MIPMAPCOUNT
               | (compressed ? 0x80000u : 0x8u);        // LINEARSIZE : PITCH
        hdr[2] = h;
        hdr[3] = w;
        hdr[4] = compressed ? D3D8Util::SurfaceBytes(fmt, w, h)
                            : (w * pf.bitCount + 7) / 8;
        hdr[5] = 0;                                     // depth
        hdr[6] = mipCount;
        // hdr[7..17] reserved
        hdr[18] = 32;                                   // ddpf.dwSize
        hdr[19] = pf.flags;
        hdr[20] = pf.fourCC;
        hdr[21] = pf.bitCount;
        hdr[22] = pf.rMask;
        hdr[23] = pf.gMask;
        hdr[24] = pf.bMask;
        hdr[25] = pf.aMask;
        hdr[26] = 0x1000 | (mipCount > 1 ? (0x400000u | 0x8u) : 0u);  // caps

        const uint32_t magic = 0x20534444;              // 'DDS '
        fwrite(&magic, 4, 1, f);
        fwrite(hdr, 4, 31, f);                          // 124 bytes
    }

    // ── Resource dumping ─────────────────────────────────────────────────
    bool DumpBufferBytes(const char* path, const void* data, uint32_t bytes)
    {
        FILE* f = nullptr;
        fopen_s(&f, path, "wb");
        if (!f) return false;
        if (bytes) fwrite(data, 1, bytes, f);
        fclose(f);
        return true;
    }

    bool DumpVertexBuffer(IDirect3DVertexBuffer8* vb, const char* path,
                          uint32_t& outBytes)
    {
        outBytes = 0;
        D3DVERTEXBUFFER_DESC desc;
        if (FAILED(vb->GetDesc(&desc))) return false;

        BYTE* p = nullptr;
        if (FAILED(vb->Lock(0, 0, &p, D3DLOCK_READONLY)) || !p) return false;
        const bool ok = DumpBufferBytes(path, p, desc.Size);
        vb->Unlock();
        if (ok) outBytes = desc.Size;
        return ok;
    }

    bool DumpIndexBuffer(IDirect3DIndexBuffer8* ib, const char* path,
                         uint32_t& outBytes)
    {
        outBytes = 0;
        D3DINDEXBUFFER_DESC desc;
        if (FAILED(ib->GetDesc(&desc))) return false;

        BYTE* p = nullptr;
        if (FAILED(ib->Lock(0, 0, &p, D3DLOCK_READONLY)) || !p) return false;
        const bool ok = DumpBufferBytes(path, p, desc.Size);
        ib->Unlock();
        if (ok) outBytes = desc.Size;
        return ok;
    }

    // Writes every mip level of a texture into one .dds. Returns false when
    // the format has no DDS equivalent or the surface refuses to lock — both
    // are recorded in the manifest rather than being silently swallowed.
    bool DumpTexture(IDirect3DTexture8* tex, const char* path,
                     const char*& outFailReason)
    {
        outFailReason = nullptr;

        D3DSURFACE_DESC level0;
        if (FAILED(tex->GetLevelDesc(0, &level0)))
        {
            outFailReason = "GetLevelDesc failed";
            return false;
        }

        DdsPixelFormat pf;
        if (!DdsFormatFor(level0.Format, pf))
        {
            outFailReason = "no DDS equivalent for this D3DFORMAT";
            return false;
        }

        const uint32_t mipCount = tex->GetLevelCount();

        FILE* f = nullptr;
        fopen_s(&f, path, "wb");
        if (!f) { outFailReason = "could not open output file"; return false; }

        WriteDdsHeader(f, level0.Format, level0.Width, level0.Height, mipCount, pf);

        bool ok = true;
        for (uint32_t level = 0; level < mipCount; ++level)
        {
            D3DSURFACE_DESC ld;
            if (FAILED(tex->GetLevelDesc(level, &ld))) { ok = false; break; }

            D3DLOCKED_RECT lr;
            if (FAILED(tex->LockRect(level, &lr, nullptr, D3DLOCK_READONLY)))
            {
                outFailReason = "LockRect failed (likely D3DPOOL_DEFAULT)";
                ok = false;
                break;
            }

            const bool compressed = (pf.flags & DDPF_FOURCC) != 0;
            if (compressed)
            {
                // Block-compressed levels are stored as whole 4x4 block rows.
                const uint32_t blockRows = (ld.Height + 3) / 4;
                const uint32_t rowBytes  =
                    D3D8Util::SurfaceBytes(ld.Format, ld.Width, 4);
                for (uint32_t r = 0; r < blockRows; ++r)
                    fwrite((const BYTE*)lr.pBits + (size_t)r * lr.Pitch,
                           1, rowBytes, f);
            }
            else
            {
                const uint32_t rowBytes = (ld.Width * pf.bitCount + 7) / 8;
                for (uint32_t y = 0; y < ld.Height; ++y)
                    fwrite((const BYTE*)lr.pBits + (size_t)y * lr.Pitch,
                           1, rowBytes, f);
            }

            tex->UnlockRect(level);
        }

        fclose(f);
        if (!ok) DeleteFileA(path);
        return ok;
    }

    // Dumps a resource once per capture frame, keyed by registry id.
    void DumpResourceOnce(ResourceInfo* info, void* object)
    {
        if (!info || !object) return;
        if (g_dumpedResources.count(info->id)) return;

        DumpOutcome outcome;
        outcome.ok     = false;
        outcome.reason = "unhandled resource kind";

        char path[MAX_PATH];
        switch (info->kind)
        {
        case kResourceVertexBuffer:
        {
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\buffers\\vb_%05u.bin",
                        g_frameDir.c_str(), info->id);
            uint32_t bytes = 0;
            outcome.ok = DumpVertexBuffer((IDirect3DVertexBuffer8*)object, path, bytes);
            if (!outcome.ok)
            {
                outcome.reason = "Lock failed (write-only or default pool)";
                Logger::Log("[Capture] vb %u: dump failed (usage=0x%X pool=%s)",
                            info->id, info->usage, D3D8Util::PoolName(info->pool));
            }
            break;
        }
        case kResourceIndexBuffer:
        {
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\buffers\\ib_%05u.bin",
                        g_frameDir.c_str(), info->id);
            uint32_t bytes = 0;
            outcome.ok = DumpIndexBuffer((IDirect3DIndexBuffer8*)object, path, bytes);
            if (!outcome.ok)
            {
                outcome.reason = "Lock failed (write-only or default pool)";
                Logger::Log("[Capture] ib %u: dump failed (usage=0x%X pool=%s)",
                            info->id, info->usage, D3D8Util::PoolName(info->pool));
            }
            break;
        }
        case kResourceTexture:
        {
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\textures\\tex_%05u.dds",
                        g_frameDir.c_str(), info->id);
            const char* why = nullptr;
            outcome.ok = DumpTexture((IDirect3DTexture8*)object, path, why);
            if (!outcome.ok)
            {
                outcome.reason = why ? why : "unknown";
                Logger::Log("[Capture] tex %u (%ux%u %s): not dumped - %s",
                            info->id, info->width, info->height,
                            D3D8Util::FormatName(info->format), outcome.reason);
            }
            break;
        }
        }

        if (outcome.ok) outcome.reason = nullptr;
        g_dumpedResources[info->id] = outcome;
    }

    // ── Per-draw JSON ────────────────────────────────────────────────────
    void WriteDrawJson(const DeviceState& st, const DrawCallInfo& info,
                       uint32_t vbId, uint32_t ibId, const uint32_t texIds[8],
                       uint32_t triangles, DrawSpace space,
                       const Registry::VertexShaderInfo* decl)
    {
        if (!g_drawsFile) return;

        FILE* f = g_drawsFile;
        fprintf(f, "%s\n  {", g_drawsWritten ? "," : "");
        fprintf(f, "\"i\":%u,", g_drawsWritten);
        fprintf(f, "\"api\":\"%s\",", info.apiName);
        fprintf(f, "\"prim\":\"%s\",",
                D3D8Util::PrimitiveTypeName(info.primitiveType));
        fprintf(f, "\"primCount\":%u,", info.primitiveCount);
        fprintf(f, "\"triangles\":%u,", triangles);
        fprintf(f, "\"space\":\"%s\",", DrawSpaceName(space));

        // The raw SetVertexShader argument, plus which of its two meanings it
        // carries. Reporting only the FVF loses the distinction between "FVF
        // zero" and "a declaration handle", which look identical downstream.
        const uint32_t vsArg = st.vertexShader;
        const bool isFvf = D3D8Util::VertexShaderArgIsFvf(vsArg);
        fprintf(f, "\"vs\":\"0x%X\",", vsArg);
        fprintf(f, "\"vsKind\":\"%s\",",
                isFvf ? (vsArg ? "fvf" : "none") : "declaration");

        char layoutDesc[256];
        uint32_t stride = 0;
        if (isFvf)
        {
            stride = D3D8Util::FvfStride(vsArg);
            D3D8Util::FvfDescribe(vsArg, layoutDesc, sizeof(layoutDesc));
        }
        else if (decl)
        {
            stride = decl->layout.streamStride[0];
            D3D8Util::VertexDeclDescribe(decl->layout, !decl->hasFunction,
                                         layoutDesc, sizeof(layoutDesc));
        }
        else
        {
            _snprintf_s(layoutDesc, sizeof(layoutDesc), _TRUNCATE,
                        "UNRESOLVED_HANDLE(0x%X)", vsArg);
        }
        fprintf(f, "\"fvf\":\"0x%X\",", st.Fvf());
        fprintf(f, "\"layout\":\"%s\",", layoutDesc);
        fprintf(f, "\"stride\":%u,", stride);

        // For a shader draw the constants are the transform, so every draw
        // gets its own snapshot of the register file.
        if (g_vsConstFile)
        {
            fwrite(&st.vsConstants[0][0], sizeof(float),
                   (size_t)kMaxVsConstants * 4u, g_vsConstFile);
            fprintf(f, "\"vsConstRecord\":%u,", g_drawsWritten);
        }
        fprintf(f, "\"vsConstHighWater\":%u,", st.vsConstantsHighWater);

        // A bound pixel shader overrides the texture stage states below, so
        // the "tss" block only describes the shading when this is 0.
        fprintf(f, "\"ps\":\"0x%X\",", st.pixelShader);
        fprintf(f, "\"tssAuthoritative\":%s,",
                st.pixelShader == 0 ? "true" : "false");

        fprintf(f, "\"indexed\":%s,", info.indexed ? "true" : "false");
        fprintf(f, "\"userPointer\":%s,", info.userPointer ? "true" : "false");
        if (info.indexed)
            fprintf(f, "\"minIndex\":%u,\"numVertices\":%u,\"startIndex\":%u,",
                    info.minIndex, info.numVertices, info.startIndex);
        else
            fprintf(f, "\"startVertex\":%u,", info.startVertex);

        fprintf(f, "\"vb\":%u,\"vbStride\":%u,\"ib\":%u,\"baseVertex\":%u,",
                vbId, st.stream[0].stride, ibId, st.baseVertexIndex);

        fprintf(f, "\"textures\":[");
        for (int i = 0; i < D3D8_TEXTURE_STAGES; ++i)
            fprintf(f, "%s%u", i ? "," : "", texIds[i]);
        fprintf(f, "],");

        // Transforms — the whole reason this capture exists. With no vertex
        // shaders in the game these are the true object/camera transforms.
        WriteMatrixJson(f, "world", st.world);       fprintf(f, ",");
        WriteMatrixJson(f, "view", st.view);         fprintf(f, ",");
        WriteMatrixJson(f, "proj", st.projection);   fprintf(f, ",");

        // The render states that decide how a surface actually looks.
        fprintf(f, "\"rs\":{");
        static const uint32_t kInterestingStates[] = {
            D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC,
            D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
            D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC,
            D3DRS_CULLMODE, D3DRS_LIGHTING, D3DRS_SPECULARENABLE,
            D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_AMBIENT,
            D3DRS_COLORVERTEX, D3DRS_TEXTUREFACTOR, D3DRS_COLORWRITEENABLE,
            D3DRS_VERTEXBLEND, D3DRS_SHADEMODE, D3DRS_ZBIAS
        };
        for (size_t i = 0; i < _countof(kInterestingStates); ++i)
        {
            const uint32_t s = kInterestingStates[i];
            fprintf(f, "%s\"%s\":%u", i ? "," : "",
                    D3D8Util::RenderStateName(s), st.renderState[s]);
        }
        fprintf(f, "},");

        // Stage 0 and 1 texture ops describe the fixed-function shading.
        fprintf(f, "\"tss\":[");
        for (int s = 0; s < 2; ++s)
        {
            fprintf(f, "%s{\"colorOp\":%u,\"colorArg1\":%u,\"colorArg2\":%u,"
                       "\"alphaOp\":%u,\"alphaArg1\":%u,\"alphaArg2\":%u,"
                       "\"texCoordIndex\":%u}",
                    s ? "," : "",
                    st.stageState[s][D3DTSS_COLOROP],
                    st.stageState[s][D3DTSS_COLORARG1],
                    st.stageState[s][D3DTSS_COLORARG2],
                    st.stageState[s][D3DTSS_ALPHAOP],
                    st.stageState[s][D3DTSS_ALPHAARG1],
                    st.stageState[s][D3DTSS_ALPHAARG2],
                    st.stageState[s][D3DTSS_TEXCOORDINDEX]);
        }
        fprintf(f, "],");

        fprintf(f, "\"material\":{\"diffuse\":[%.4g,%.4g,%.4g,%.4g],"
                   "\"ambient\":[%.4g,%.4g,%.4g,%.4g],"
                   "\"specular\":[%.4g,%.4g,%.4g,%.4g],"
                   "\"emissive\":[%.4g,%.4g,%.4g,%.4g],\"power\":%.4g}",
                st.material.Diffuse.r,  st.material.Diffuse.g,
                st.material.Diffuse.b,  st.material.Diffuse.a,
                st.material.Ambient.r,  st.material.Ambient.g,
                st.material.Ambient.b,  st.material.Ambient.a,
                st.material.Specular.r, st.material.Specular.g,
                st.material.Specular.b, st.material.Specular.a,
                st.material.Emissive.r, st.material.Emissive.g,
                st.material.Emissive.b, st.material.Emissive.a,
                st.material.Power);

        fprintf(f, "}");
        ++g_drawsWritten;
    }

    void OpenCaptureFrame()
    {
        char dir[MAX_PATH];
        _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\frame_%06u",
                    g_outputDir.c_str(), g_frameIndex);
        g_frameDir = dir;

        EnsureDirectory(dir);
        std::string sub = g_frameDir + "\\buffers";  EnsureDirectory(sub.c_str());
        sub = g_frameDir + "\\textures";             EnsureDirectory(sub.c_str());

        sub = g_frameDir + "\\shaders";              EnsureDirectory(sub.c_str());

        std::string drawsPath = g_frameDir + "\\draws.json";
        fopen_s(&g_drawsFile, drawsPath.c_str(), "w");
        if (g_drawsFile) fprintf(g_drawsFile, "[");
        g_drawsWritten = 0;
        g_dumpedResources.clear();

        // The vs.1.1 shaders are assembled at runtime through the statically
        // linked D3DX8 assembler, so this dump is the only place the compiled
        // bytecode exists — it cannot be recovered from the executable.
        uint32_t shadersDumped = 0;
        for (uint32_t i = 0; i < Registry::VertexShaderCount(); ++i)
        {
            const Registry::VertexShaderInfo* vs = Registry::VertexShaderAt(i);
            if (!vs || vs->function.empty()) continue;
            char shaderPath[MAX_PATH];
            _snprintf_s(shaderPath, sizeof(shaderPath), _TRUNCATE,
                        "%s\\shaders\\vs_%04X.bin", g_frameDir.c_str(),
                        vs->handle);
            if (DumpBufferBytes(shaderPath, vs->function.data(),
                                (uint32_t)(vs->function.size() * sizeof(uint32_t))))
                ++shadersDumped;
        }
        for (uint32_t i = 0; i < Registry::PixelShaderCount(); ++i)
        {
            const Registry::PixelShaderInfo* ps = Registry::PixelShaderAt(i);
            if (!ps || ps->function.empty()) continue;
            char shaderPath[MAX_PATH];
            _snprintf_s(shaderPath, sizeof(shaderPath), _TRUNCATE,
                        "%s\\shaders\\ps_%04X.bin", g_frameDir.c_str(),
                        ps->handle);
            if (DumpBufferBytes(shaderPath, ps->function.data(),
                                (uint32_t)(ps->function.size() * sizeof(uint32_t))))
                ++shadersDumped;
        }

        // One fixed-size record of the constant register file per draw,
        // appended in draw order. Fixed size keeps the file trivially
        // indexable by draw number.
        std::string constPath = g_frameDir + "\\vs_constants.bin";
        fopen_s(&g_vsConstFile, constPath.c_str(), "wb");

        Logger::Log("[Capture] Capturing frame %u to %s (%u vertex shaders "
                    "dumped)", g_frameIndex, dir, shadersDumped);
    }

    void CloseCaptureFrame(IDirect3DDevice8* realDevice)
    {
        if (g_drawsFile)
        {
            fprintf(g_drawsFile, "\n]\n");
            fclose(g_drawsFile);
            g_drawsFile = nullptr;
        }
        if (g_vsConstFile) { fclose(g_vsConstFile); g_vsConstFile = nullptr; }

        // ── manifest.json ────────────────────────────────────────────────
        std::string manifestPath = g_frameDir + "\\manifest.json";
        FILE* f = nullptr;
        fopen_s(&f, manifestPath.c_str(), "w");
        if (!f) return;

        const FrameStats& s = g_stats;
        fprintf(f, "{\n");
        fprintf(f, "  \"frameIndex\": %u,\n", s.frameIndex);
        fprintf(f, "  \"exe\": \"pes6.exe\",\n");

        D3DDEVICE_CREATION_PARAMETERS cp;
        memset(&cp, 0, sizeof(cp));
        if (realDevice && SUCCEEDED(realDevice->GetCreationParameters(&cp)))
            fprintf(f, "  \"deviceType\": \"%s\", \"behaviorFlags\": \"0x%X\",\n",
                    D3D8Util::DeviceTypeName(cp.DeviceType), cp.BehaviorFlags);

        D3DDISPLAYMODE dm;
        memset(&dm, 0, sizeof(dm));
        if (realDevice && SUCCEEDED(realDevice->GetDisplayMode(&dm)))
            fprintf(f, "  \"displayMode\": {\"width\":%u,\"height\":%u,"
                       "\"refresh\":%u,\"format\":\"%s\"},\n",
                    dm.Width, dm.Height, dm.RefreshRate,
                    D3D8Util::FormatName(dm.Format));

        fprintf(f, "  \"stats\": {\n");
        fprintf(f, "    \"drawCalls\": %u, \"drawCalls2D\": %u, \"drawCalls3D\": %u,\n",
                s.drawCalls, s.drawCalls2D, s.drawCalls3D);
        fprintf(f, "    \"triangles\": %u, \"triangles2D\": %u, \"triangles3D\": %u,\n",
                s.triangles, s.triangles2D, s.triangles3D);
        fprintf(f, "    \"setRenderState\": %u, \"redundantRenderState\": %u,\n",
                s.setRenderStateCalls, s.redundantRenderStateCalls);
        fprintf(f, "    \"setTexture\": %u, \"setTransform\": %u, \"clears\": %u,\n",
                s.setTextureCalls, s.setTransformCalls, s.clears);
        fprintf(f, "    \"distinctFvf\": %u, \"distinctTextures\": %u\n",
                s.distinctFvfCount, s.distinctTextureCount);
        fprintf(f, "  },\n");

        fprintf(f, "  \"vertexShaderConstants\": {\"file\":\"vs_constants.bin\","
                   "\"registersPerRecord\":%u,\"bytesPerRecord\":%u,"
                   "\"records\":%u,\"note\":\"one record per draw, in draw "
                   "order; float4 per register\"},\n",
                (unsigned)kMaxVsConstants, (unsigned)(kMaxVsConstants * 16),
                g_drawsWritten);

        fprintf(f, "  \"vertexShaders\": [");
        bool firstVs = true;
        for (uint32_t i = 0; i < Registry::VertexShaderCount(); ++i)
        {
            const Registry::VertexShaderInfo* vs = Registry::VertexShaderAt(i);
            if (!vs) continue;
            char d[256];
            fprintf(f, "%s\n    {\"handle\":\"0x%X\",\"hasFunction\":%s,"
                       "\"tokens\":%u,\"stride\":%u,\"layout\":\"%s\"",
                    firstVs ? "" : ",", vs->handle,
                    vs->hasFunction ? "true" : "false",
                    (unsigned)vs->function.size(),
                    vs->layout.streamStride[0],
                    D3D8Util::VertexDeclDescribe(vs->layout, !vs->hasFunction,
                                                 d, sizeof(d)));
            if (!vs->function.empty())
                fprintf(f, ",\"file\":\"shaders/vs_%04X.bin\"", vs->handle);
            fprintf(f, "}");
            firstVs = false;
        }
        fprintf(f, "\n  ],\n");

        if (s.worldBoundsValid)
            fprintf(f, "  \"objectOriginBounds\": {\"min\":[%.4f,%.4f,%.4f],"
                       "\"max\":[%.4f,%.4f,%.4f]},\n",
                    s.worldMin[0], s.worldMin[1], s.worldMin[2],
                    s.worldMax[0], s.worldMax[1], s.worldMax[2]);

        // ── resources referenced this frame ──────────────────────────────
        fprintf(f, "  \"resources\": [\n");
        bool firstRes = true;
        for (uint32_t i = 0; i < Registry::Count(); ++i)
        {
            const ResourceInfo* r = Registry::At(i);
            if (!r) continue;
            std::map<uint32_t, DumpOutcome>::const_iterator dumped =
                g_dumpedResources.find(r->id);
            if (dumped == g_dumpedResources.end()) continue;
            const DumpOutcome& outcome = dumped->second;

            fprintf(f, "%s    {\"id\":%u,\"kind\":\"%s\",\"dumped\":%s",
                    firstRes ? "" : ",\n", r->id,
                    r->kind == kResourceVertexBuffer ? "vertexBuffer" :
                    r->kind == kResourceIndexBuffer  ? "indexBuffer"  : "texture",
                    outcome.ok ? "true" : "false");
            if (!outcome.ok)
                fprintf(f, ",\"reason\":\"%s\"",
                        outcome.reason ? outcome.reason : "unknown");
            firstRes = false;

            switch (r->kind)
            {
            case kResourceVertexBuffer:
            {
                char d[128];
                fprintf(f, ",\"bytes\":%u,\"fvf\":\"0x%X\",\"fvfDesc\":\"%s\""
                           ",\"pool\":\"%s\",\"usage\":\"0x%X\""
                           ",\"writeOnlyStripped\":%s",
                        r->byteLength, r->fvf,
                        D3D8Util::FvfDescribe(r->fvf, d, sizeof(d)),
                        D3D8Util::PoolName(r->pool), r->usage,
                        r->writeOnlyStripped ? "true" : "false");
                if (outcome.ok)
                    fprintf(f, ",\"file\":\"buffers/vb_%05u.bin\"", r->id);
                break;
            }
            case kResourceIndexBuffer:
                fprintf(f, ",\"bytes\":%u,\"format\":\"%s\",\"pool\":\"%s\""
                           ",\"usage\":\"0x%X\",\"writeOnlyStripped\":%s",
                        r->byteLength, D3D8Util::FormatName(r->format),
                        D3D8Util::PoolName(r->pool), r->usage,
                        r->writeOnlyStripped ? "true" : "false");
                if (outcome.ok)
                    fprintf(f, ",\"file\":\"buffers/ib_%05u.bin\"", r->id);
                break;
            case kResourceTexture:
                fprintf(f, ",\"width\":%u,\"height\":%u,\"levels\":%u"
                           ",\"format\":\"%s\",\"pool\":\"%s\"",
                        r->width, r->height, r->levels,
                        D3D8Util::FormatName(r->format),
                        D3D8Util::PoolName(r->pool));
                if (outcome.ok)
                    fprintf(f, ",\"file\":\"textures/tex_%05u.dds\"", r->id);
                break;
            }
            fprintf(f, ",\"drawRefs\":%u}", r->drawRefCount);
        }
        fprintf(f, "\n  ]\n}\n");
        fclose(f);

        Logger::Log("[Capture] Frame %u written: %u draws, %u resources dumped.",
                    g_frameIndex, g_drawsWritten,
                    (uint32_t)g_dumpedResources.size());
    }
}

namespace Capture { namespace Frame {

bool Init(const char* outputDir)
{
    g_outputDir = outputDir ? outputDir : "pesmod_capture";
    EnsureDirectory(g_outputDir.c_str());

    memset(&g_stats, 0, sizeof(g_stats));
    memset(&g_lastStats, 0, sizeof(g_lastStats));
    memset(&g_session, 0, sizeof(g_session));
    g_session.minDrawsInAFrame = 0xFFFFFFFFu;
    g_frameIndex  = 0;
    g_initialised = true;

    Logger::Log("[Capture] Frame capture ready; output dir '%s'.",
                g_outputDir.c_str());
    return true;
}

void Shutdown()
{
    if (!g_initialised) return;
    if (g_drawsFile)   { fclose(g_drawsFile);   g_drawsFile   = nullptr; }
    if (g_vsConstFile) { fclose(g_vsConstFile); g_vsConstFile = nullptr; }
    g_initialised = false;
}

void BeginFrame()
{
    if (!g_initialised) return;

    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.frameIndex = g_frameIndex;
    g_frameFvfs.clear();
    g_frameTextures.clear();

    // Unattended trigger: capture a chosen frame with no key press. The game
    // runs fullscreen, so a hotkey is not always reachable.
    const int autoFrame = RenderConfig::CaptureAtFrame();
    if (autoFrame > 0 && g_frameIndex == (uint32_t)autoFrame)
    {
        Logger::Log("[Capture] Auto-trigger at frame %d.", autoFrame);
        g_armed = true;
    }

    if (g_armed)
    {
        g_armed     = false;
        g_capturing = true;
        OpenCaptureFrame();
    }
}

void EndFrame(IDirect3DDevice8* realDevice)
{
    if (!g_initialised) return;

    g_stats.distinctFvfCount     = (uint32_t)g_frameFvfs.size();
    g_stats.distinctTextureCount = (uint32_t)g_frameTextures.size();

    const bool didCapture = g_capturing;
    if (g_capturing)
    {
        CloseCaptureFrame(realDevice);
        g_capturing = false;
    }

    // Session aggregates. Frames with no draws at all (loading screens where
    // the game presents without drawing) are excluded from the min so the
    // reported range describes actual rendering.
    ++g_session.frames;
    g_session.draws                += g_stats.drawCalls;
    g_session.draws2D              += g_stats.drawCalls2D;
    g_session.draws3D              += g_stats.drawCalls3D;
    g_session.drawsUnknownFvf      += g_stats.drawCallsUnknownFvf;
    g_session.triangles            += g_stats.triangles;
    g_session.triangles2D          += g_stats.triangles2D;
    g_session.triangles3D          += g_stats.triangles3D;
    g_session.setRenderState       += g_stats.setRenderStateCalls;
    g_session.redundantRenderState += g_stats.redundantRenderStateCalls;
    g_session.setTextureStageState += g_stats.setTextureStageStateCalls;
    g_session.setTexture           += g_stats.setTextureCalls;
    g_session.setTransform         += g_stats.setTransformCalls;

    if (g_stats.drawCalls > g_session.maxDrawsInAFrame)
        g_session.maxDrawsInAFrame = g_stats.drawCalls;
    if (g_stats.drawCalls > 0 && g_stats.drawCalls < g_session.minDrawsInAFrame)
        g_session.minDrawsInAFrame = g_stats.drawCalls;

    g_lastStats = g_stats;
    ++g_frameIndex;

    // The game is frequently killed rather than exited cleanly (its DRM
    // wrapper ignores a polite terminate), in which case neither the device
    // Release nor ModShutdown ever runs. Flushing the summary after every
    // capture, and periodically otherwise, means a hard kill still leaves a
    // usable report rather than nothing at all.
    if (didCapture || (g_frameIndex % kSessionReportInterval) == 0)
        WriteSessionReport(RenderConfig::SessionReportPath());
}

void ArmSingleFrame()
{
    if (!g_initialised) return;
    g_armed = true;
    Logger::Log("[Capture] Armed - next frame will be fully captured.");
}

bool     IsCapturingFrame()   { return g_capturing; }
uint32_t CurrentFrameIndex()  { return g_frameIndex; }
const FrameStats& LastFrameStats() { return g_lastStats; }

void OnDraw(IDirect3DDevice8* realDevice, const DeviceState& state,
            const DrawCallInfo& info)
{
    (void)realDevice;
    if (!g_initialised) return;

    const Registry::VertexShaderInfo* decl = nullptr;
    const DrawSpace space = ClassifyDraw(state, &decl);
    const uint32_t tris =
        D3D8Util::PrimitiveTriangleCount(info.primitiveType, info.primitiveCount);

    ++g_stats.drawCalls;
    g_stats.triangles += tris;
    switch (space)
    {
    case kSpaceScreen:  ++g_stats.drawCalls2D; g_stats.triangles2D += tris; break;
    case kSpaceWorld:   ++g_stats.drawCalls3D; g_stats.triangles3D += tris; break;
    case kSpaceUnknown: ++g_stats.drawCallsUnknownFvf;                      break;
    }

    // Vertex formats are tracked by the raw SetVertexShader argument, so an
    // FVF and a declaration handle stay distinguishable in the summary.
    g_frameFvfs.insert(state.vertexShader);
    g_sessionFvfs.insert(state.vertexShader);

    // The match scene is the target of this whole exercise, and it is only
    // reachable through several menus — awkward to trigger by frame number or
    // key press. Arming on the first world-space draw catches it automatically.
    // The capture is deferred to the *next* frame because this one is already
    // partway through and its earlier draws are gone.
    if (space == kSpaceWorld && !g_first3DSeen)
    {
        g_first3DSeen = true;
        Logger::Log("[Capture] First world-space geometry at frame %u "
                    "(vs 0x%X, %s).", g_frameIndex, state.vertexShader,
                    D3D8Util::VertexShaderArgIsFvf(state.vertexShader)
                        ? "FVF" : "declaration");
        if (RenderConfig::CaptureOnFirst3D() && !g_capturing)
        {
            Logger::Log("[Capture] Arming capture for the next frame.");
            g_armed = true;
        }
    }

    // Scene extents, approximated by object origins. Exact per-vertex bounds
    // would mean locking every vertex buffer on every draw, which is far too
    // expensive outside a capture; the world-matrix translation is free and
    // still establishes the coordinate-system scale.
    if (space == kSpaceWorld)
    {
        float origin[3];
        MatrixTranslation(state.world, origin);
        AccumulateBounds(g_stats, origin);
    }

    if (!g_capturing) return;

    // ── Capture path: resolve bindings and dump what they point at ───────
    uint32_t vbId = 0, ibId = 0, texIds[D3D8_TEXTURE_STAGES] = { 0 };

    if (!info.userPointer)
    {
        if (ResourceInfo* vb = Registry::FindForDraw(state.stream[0].buffer))
        {
            vbId = vb->id;
            DumpResourceOnce(vb, state.stream[0].buffer);
        }
        if (info.indexed)
        {
            if (ResourceInfo* ib = Registry::FindForDraw(state.indexBuffer))
            {
                ibId = ib->id;
                DumpResourceOnce(ib, state.indexBuffer);
            }
        }
    }

    for (int s = 0; s < D3D8_TEXTURE_STAGES; ++s)
    {
        if (!state.texture[s]) continue;
        if (ResourceInfo* t = Registry::FindForDraw(state.texture[s]))
        {
            texIds[s] = t->id;
            DumpResourceOnce(t, state.texture[s]);
        }
    }

    // User-pointer draws carry their vertices inline; they are written to
    // their own per-draw file since there is no resource to reference.
    if (info.userPointer && info.upVertexData && info.upVertexStride)
    {
        const uint32_t verts =
            D3D8Util::PrimitiveVertexCount(info.primitiveType, info.primitiveCount);
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\buffers\\up_%05u.bin",
                    g_frameDir.c_str(), g_drawsWritten);
        DumpBufferBytes(path, info.upVertexData, verts * info.upVertexStride);
    }

    WriteDrawJson(state, info, vbId, ibId, texIds, tris, space, decl);
}

void OnClear(uint32_t flags, D3DCOLOR color, float z)
{
    (void)flags; (void)color; (void)z;
    if (g_initialised) ++g_stats.clears;
}

void OnBeginScene()
{
    if (g_initialised) ++g_stats.beginScenes;
}

void OnSetRenderState(uint32_t state, uint32_t value, bool redundant)
{
    (void)state; (void)value;
    if (!g_initialised) return;
    ++g_stats.setRenderStateCalls;
    if (redundant) ++g_stats.redundantRenderStateCalls;
}

void OnSetTextureStageState()
{
    if (g_initialised) ++g_stats.setTextureStageStateCalls;
}

void OnSetTexture(IDirect3DBaseTexture8* texture)
{
    if (!g_initialised) return;
    ++g_stats.setTextureCalls;
    if (texture) g_frameTextures.insert(texture);
}

void OnSetTransform(uint32_t state)
{
    (void)state;
    if (g_initialised) ++g_stats.setTransformCalls;
}

void WriteSessionReport(const char* path)
{
    if (!g_initialised || g_session.frames == 0) return;

    FILE* f = nullptr;
    fopen_s(&f, path, "w");
    if (!f) return;

    const double frames = (double)g_session.frames;
    const double avgDraws = g_session.draws / frames;

    fprintf(f, "# pes6.exe render session summary\n\n");
    fprintf(f, "Captured by PESMod's D3D8 interception layer.\n\n");
    fprintf(f, "Snapshot as of frame %u — this file is rewritten periodically "
               "and after every capture, so it reflects the session up to that "
               "point rather than only a clean exit.\n\n", g_frameIndex);
    fprintf(f, "| Metric | Total | Per frame |\n");
    fprintf(f, "|---|---:|---:|\n");
    fprintf(f, "| Frames | %llu | - |\n", (unsigned long long)g_session.frames);
    fprintf(f, "| Draw calls | %llu | %.1f |\n",
            (unsigned long long)g_session.draws, avgDraws);
    fprintf(f, "| - world space (3D) | %llu | %.1f |\n",
            (unsigned long long)g_session.draws3D, g_session.draws3D / frames);
    fprintf(f, "| - screen space (2D) | %llu | %.1f |\n",
            (unsigned long long)g_session.draws2D, g_session.draws2D / frames);
    fprintf(f, "| - undecodable FVF | %llu | %.2f |\n",
            (unsigned long long)g_session.drawsUnknownFvf,
            g_session.drawsUnknownFvf / frames);
    fprintf(f, "| Triangles | %llu | %.0f |\n",
            (unsigned long long)g_session.triangles, g_session.triangles / frames);
    fprintf(f, "| - world space | %llu | %.0f |\n",
            (unsigned long long)g_session.triangles3D,
            g_session.triangles3D / frames);
    fprintf(f, "| - screen space | %llu | %.0f |\n",
            (unsigned long long)g_session.triangles2D,
            g_session.triangles2D / frames);
    fprintf(f, "| SetRenderState | %llu | %.1f |\n",
            (unsigned long long)g_session.setRenderState,
            g_session.setRenderState / frames);
    fprintf(f, "| - redundant | %llu | %.1f |\n",
            (unsigned long long)g_session.redundantRenderState,
            g_session.redundantRenderState / frames);
    fprintf(f, "| SetTextureStageState | %llu | %.1f |\n",
            (unsigned long long)g_session.setTextureStageState,
            g_session.setTextureStageState / frames);
    fprintf(f, "| SetTexture | %llu | %.1f |\n",
            (unsigned long long)g_session.setTexture,
            g_session.setTexture / frames);
    fprintf(f, "| SetTransform | %llu | %.1f |\n",
            (unsigned long long)g_session.setTransform,
            g_session.setTransform / frames);
    fprintf(f, "\nDraw calls per frame ranged %u..%u.\n",
            g_session.minDrawsInAFrame == 0xFFFFFFFFu ? 0u
                                                      : g_session.minDrawsInAFrame,
            g_session.maxDrawsInAFrame);

    // ── Vertex formats ───────────────────────────────────────────────────
    // Both meanings of the SetVertexShader argument appear here: FVF codes
    // and handles created from a vertex declaration.
    fprintf(f, "\n## Vertex formats seen (%u distinct)\n\n",
            (uint32_t)g_sessionFvfs.size());
    fprintf(f, "| SetVertexShader arg | Kind | Layout | Stride | Space |\n");
    fprintf(f, "|---|---|---|---:|---|\n");
    for (std::set<uint32_t>::const_iterator it = g_sessionFvfs.begin();
         it != g_sessionFvfs.end(); ++it)
    {
        const uint32_t arg = *it;
        char d[256];

        if (D3D8Util::VertexShaderArgIsFvf(arg))
        {
            if (arg == 0)
            {
                fprintf(f, "| `0x0` | none | *(no vertex format bound)* | - "
                           "| unknown |\n");
                continue;
            }
            fprintf(f, "| `0x%X` | FVF | %s | %u | %s |\n", arg,
                    D3D8Util::FvfDescribe(arg, d, sizeof(d)),
                    D3D8Util::FvfStride(arg),
                    D3D8Util::FvfIsScreenSpace(arg) ? "screen (2D)"
                                                    : "world (3D)");
        }
        else
        {
            const Registry::VertexShaderInfo* info =
                Registry::FindVertexShader(arg);
            if (info)
                fprintf(f, "| `0x%X` | declaration%s | %s | %u | world (3D) |\n",
                        arg, info->hasFunction ? " + shader" : "",
                        D3D8Util::VertexDeclDescribe(info->layout,
                                                     !info->hasFunction,
                                                     d, sizeof(d)),
                        info->layout.streamStride[0]);
            else
                fprintf(f, "| `0x%X` | declaration | *(created before hook)* "
                           "| - | unknown |\n", arg);
        }
    }

    // ── Vertex declarations ──────────────────────────────────────────────
    if (Registry::VertexShaderCount())
    {
        fprintf(f, "\n## Vertex declarations (%u)\n\n",
                Registry::VertexShaderCount());
        fprintf(f, "| Handle | Has shader function | Elements | Stride |\n");
        fprintf(f, "|---|---|---|---:|\n");
        for (uint32_t i = 0; i < Registry::VertexShaderCount(); ++i)
        {
            const Registry::VertexShaderInfo* info = Registry::VertexShaderAt(i);
            if (!info) continue;
            char d[256];
            fprintf(f, "| `0x%X` | %s | %s | %u |\n", info->handle,
                    info->hasFunction ? "yes" : "no (fixed function)",
                    D3D8Util::VertexDeclDescribe(info->layout,
                                                 !info->hasFunction,
                                                 d, sizeof(d)),
                    info->layout.streamStride[0]);
        }
    }

    // ── Resources ────────────────────────────────────────────────────────
    const Registry::Totals t = Registry::GetTotals();
    fprintf(f, "\n## Resources created\n\n");
    fprintf(f, "| Kind | Count | Bytes |\n|---|---:|---:|\n");
    fprintf(f, "| Vertex buffers | %u | %llu |\n", t.vertexBuffers,
            (unsigned long long)t.vertexBufferBytes);
    fprintf(f, "| Index buffers | %u | %llu |\n", t.indexBuffers,
            (unsigned long long)t.indexBufferBytes);
    fprintf(f, "| Textures (level 0) | %u | %llu |\n", t.textures,
            (unsigned long long)t.textureBytes);
    if (t.writeOnlyStrippedCount)
        fprintf(f, "\n%u buffers had `D3DUSAGE_WRITEONLY` stripped so their "
                   "contents could be read back. This only happens while "
                   "capture is enabled.\n", t.writeOnlyStrippedCount);

    fclose(f);
    Logger::Log("[Capture] Session report written to %s", path);
}

}} // namespace Capture::Frame
