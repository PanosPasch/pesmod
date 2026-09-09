// main.cpp — PESMod render host (64-bit)
//
// The half of the renderer that can actually ray trace. It attaches to the
// scene stream published by the 32-bit ASI inside pes6.exe, maintains a
// GPU-side copy of the scene, and renders it.
//
// This exists as a separate process because NVIDIA's 32-bit Vulkan ICD does
// not expose the ray tracing extensions at all, so RT is unreachable from
// inside the game. See docs/RENDERER.md §1 for the measurements.
//
// At this stage the host attaches, decodes and reports the scene. The Vulkan
// device and ray tracing pipeline land on top of this; the transport and
// scene bookkeeping are deliberately proven first, since they are the only
// parts that depend on the game running.
#include "scene_receiver.h"
#include "vk_device.h"
#include "vk_alloc.h"
#include "vk_accel.h"
#include "vk_raytracer.h"
#include "vk_textures.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

namespace
{
    volatile bool g_quit = false;

    BOOL WINAPI ConsoleHandler(DWORD type)
    {
        if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT ||
            type == CTRL_BREAK_EVENT)
        {
            g_quit = true;
            return TRUE;
        }
        return FALSE;
    }

    void PrintUsage()
    {
        printf(
            "PESMod render host (64-bit)\n"
            "\n"
            "  --section <name>   shared section to attach to\n"
            "                     (default: %s)\n"
            "  --frames <n>       exit after n completed frames (0 = run until Ctrl-C)\n"
            "  --retain <frames>  drop cached geometry unused this long (default 900;\n"
            "                     must exceed the producer's 300-frame retention)\n"
            "  --quiet            only print the summary\n"
            "  --probe            create the Vulkan RT device, report, exit\n"
            "  --astest           trace a synthetic scene, self-check, exit\n"
            "  --shaders <dir>    directory holding the compiled .spv files\n"
            "                     (default: shaders)\n"
            "  --trace-height <n> traced image height; the width follows the\n"
            "                     game aspect ratio (default 720, 0 = off)\n"
            "  --save-frame <n>   write the nth traced frame to disk\n"
            "  --texture-budget <n>  texture uploads per frame (default 8)\n"
            "  --save-every <n>   write every nth traced frame, numbered\n"
            "  --save-path <file> where to write it (default traced_frame.png;\n"
            "                     a .ppm extension writes a PPM instead)\n"
            "  --no-validation    disable Vulkan validation layers\n"
            "  --help\n",
            SceneIPC::kDefaultSectionName);
    }

    // The compiled shaders sit next to the build directory, not next to the
    // executable, so a host launched from its own output folder would not
    // find them. Both layouts are probed before giving up, which keeps
    // --shaders for genuinely unusual locations rather than routine ones.
    std::string ResolveShaderDir()
    {
        char exePath[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string dir(exePath);
        const size_t slash = dir.find_last_of('\\');
        dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);

        const char* candidates[] = { "\\shaders", "\\..\\shaders", "\\..\\..\\shaders" };
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
        {
            const std::string probe = dir + candidates[i];
            const DWORD attrs = GetFileAttributesA((probe + "\\primary.rgen.spv").c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES) return probe;
        }
        return "shaders";   // relative to the working directory, as before
    }

    // "shot.png" + 42 -> "shot_000042.png". Zero-padded so the frames sort
    // in order in a file listing, which is the whole point of writing a
    // series of them rather than one.
    std::string NumberedPath(const char* base, uint64_t n)
    {
        std::string s(base);
        const size_t dot   = s.find_last_of('.');
        const size_t slash = s.find_last_of("\\/");
        const size_t split = (dot != std::string::npos &&
                              (slash == std::string::npos || dot > slash))
                                 ? dot : s.size();

        char suffix[32];
        sprintf_s(suffix, "_%06llu", (unsigned long long)n);
        return s.substr(0, split) + suffix + s.substr(split);
    }

    const char* VertexKindName(uint32_t k)
    {
        switch (k)
        {
        case SceneIPC::kVertexPreLit: return "pre-lit  (24B pos/colour/uv)";
        case SceneIPC::kVertexLit:    return "lit      (32B pos/normal/uv)";
        default:                      return "unknown";
        }
    }

    void ReportFrame(const Host::SceneReceiver& rx)
    {
        const Host::Frame& f = rx.CurrentFrame();

        uint64_t triangles = 0;
        uint32_t resolved = 0, missing = 0, evicted = 0, neverSent = 0;
        for (size_t i = 0; i < f.instances.size(); ++i)
        {
            const Host::Geometry* g = rx.FindGeometry(f.instances[i].geometryId);
            if (!g)
            {
                ++missing;
                // Splits a cache-coherence failure from a producer that
                // never sent the geometry in the first place.
                if (rx.WasEverReceived(f.instances[i].geometryId)) ++evicted;
                else                                              ++neverSent;
                continue;
            }
            ++resolved;
            const uint32_t idx = g->desc.indexCount ? g->desc.indexCount
                                                    : g->desc.vertexCount;
            triangles += idx / 3;
        }

        printf("frame %-8llu  instances %-4zu (resolved %u, missing %u)  "
               "tris %-7llu  geo %zu  tex %zu  resident %.1f MB\n",
               (unsigned long long)f.begin.frameIndex,
               f.instances.size(), resolved, missing,
               (unsigned long long)triangles,
               rx.GeometryCount(), rx.TextureCount(),
               rx.ResidentBytes() / (1024.0 * 1024.0));

        if (missing)
            printf("    WARNING: %u unresolved (%u evicted-then-reused, "
                   "%u never sent)\n", missing, evicted, neverSent);
    }

    // Builds a synthetic scene, pushes it through the real transport, and
    // runs the acceleration structure builder over it.
    //
    // Going through SharedRing rather than poking the receiver's internals
    // means this covers the protocol, the receiver and the builder together,
    // and it needs no running game — which matters because the game side can
    // only be exercised by actually playing a match.
    int RunAccelSelfTest(Host::VulkanDevice& gpu, const char* shaderDir)
    {
        const char* kSection = "Local\\PESMod.SceneStream.ASTest";
        printf("\n-------- acceleration structure self-test --------\n");

        SceneIPC::SharedRing producer;
        if (!producer.CreateAsProducer(kSection, 4ull * 1024ull * 1024ull))
        {
            printf("FAIL: could not create the test section\n");
            return 1;
        }

        // A 4-triangle mesh: large enough to earn its own BLAS.
        {
            struct LitVertex { float px, py, pz, nx, ny, nz, u, v; };
            LitVertex verts[6] = {
                {  0,  0,  0,  0,1,0, 0,0 }, {  1,  0,  0,  0,1,0, 1,0 },
                {  1,  1,  0,  0,1,0, 1,1 }, {  0,  1,  0,  0,1,0, 0,1 },
                {  0,  0,  1,  0,1,0, 0,0 }, {  1,  0,  1,  0,1,0, 1,0 },
            };
            uint16_t idx[12] = { 0,1,2,  0,2,3,  0,1,4,  1,5,4 };

            SceneIPC::GeometryDesc gd{};
            gd.geometryId   = 0x1111111100000001ull;
            gd.vertexKind   = SceneIPC::kVertexLit;
            gd.vertexStride = sizeof(LitVertex);
            gd.vertexCount  = 6;
            gd.indexCount   = 12;
            gd.indexStride  = 2;
            gd.contentHash  = 0xABCD1234u;

            std::vector<uint8_t> payload(sizeof(verts) + sizeof(idx));
            memcpy(payload.data(), verts, sizeof(verts));
            memcpy(payload.data() + sizeof(verts), idx, sizeof(idx));
            producer.TryWrite(SceneIPC::kMsgGeometry, &gd, sizeof(gd),
                              payload.data(), (uint32_t)payload.size(), false);
        }

        // A 2-triangle quad: exactly the sprite case that must be merged
        // rather than given a structure of its own.
        {
            struct LitVertex { float px, py, pz, nx, ny, nz, u, v; };
            LitVertex verts[4] = {
                { 0,0,0, 0,1,0, 0,0 }, { 2,0,0, 0,1,0, 1,0 },
                { 2,2,0, 0,1,0, 1,1 }, { 0,2,0, 0,1,0, 0,1 },
            };
            uint16_t idx[6] = { 0,1,2, 0,2,3 };

            SceneIPC::GeometryDesc gd{};
            gd.geometryId   = 0x2222222200000002ull;
            gd.vertexKind   = SceneIPC::kVertexLit;
            gd.vertexStride = sizeof(LitVertex);
            gd.vertexCount  = 4;
            gd.indexCount   = 6;
            gd.indexStride  = 2;
            gd.contentHash  = 0x55AA55AAu;

            std::vector<uint8_t> payload(sizeof(verts) + sizeof(idx));
            memcpy(payload.data(), verts, sizeof(verts));
            memcpy(payload.data() + sizeof(verts), idx, sizeof(idx));
            producer.TryWrite(SceneIPC::kMsgGeometry, &gd, sizeof(gd),
                              payload.data(), (uint32_t)payload.size(), false);
        }

        // A 4-triangle sheet for the alpha test. Four rather than two so it
        // earns its own BLAS: the merged sprite batch is forced opaque and
        // would never reach the any-hit shader.
        {
            struct LitVertex { float px, py, pz, nx, ny, nz, u, v; };
            LitVertex verts[6] = {
                { 0,0,0, 0,0,1, 0,0 }, { 4,0,0, 0,0,1, 1,0 },
                { 4,4,0, 0,0,1, 1,1 }, { 0,4,0, 0,0,1, 0,1 },
                { 0,0,1, 0,0,1, 0,0 }, { 4,0,1, 0,0,1, 1,0 },
            };
            uint16_t idx[12] = { 0,1,2,  0,2,3,  0,1,4,  1,5,4 };

            SceneIPC::GeometryDesc gd{};
            gd.geometryId   = 0x3333333300000004ull;
            gd.vertexKind   = SceneIPC::kVertexLit;
            gd.vertexStride = sizeof(LitVertex);
            gd.vertexCount  = 6;
            gd.indexCount   = 12;
            gd.indexStride  = 2;
            gd.contentHash  = 0x0F0F0F0Fu;

            std::vector<uint8_t> payload(sizeof(verts) + sizeof(idx));
            memcpy(payload.data(), verts, sizeof(verts));
            memcpy(payload.data() + sizeof(verts), idx, sizeof(idx));
            producer.TryWrite(SceneIPC::kMsgGeometry, &gd, sizeof(gd),
                              payload.data(), (uint32_t)payload.size(), false);
        }

        // Fully transparent, and bright red so that failing to skip it is
        // impossible to miss. This is the case that made the game's pitch
        // into noise: six blended overlays sharing a plane with the grass.
        const uint64_t kClearTextureId = 0x8888888800000005ull;
        {
            SceneIPC::TextureDesc td{};
            td.textureId    = kClearTextureId;
            td.format       = SceneIPC::kTexBGRA8;
            td.width        = 4;
            td.height       = 4;
            td.mipLevels    = 1;
            td.payloadBytes = 4 * 4 * 4;

            std::vector<uint8_t> texels(td.payloadBytes, 0);
            for (uint32_t i = 0; i < td.payloadBytes; i += 4)
            {
                texels[i + 2] = 255;   // R
                texels[i + 3] = 0;     // A - the whole point
            }
            producer.TryWrite(SceneIPC::kMsgTexture, &td, sizeof(td),
                              texels.data(), td.payloadBytes, false);
        }

        // A solid-colour texture. Solid rather than patterned because the
        // check below has to hold for whichever texel the UVs land on: if
        // sampling works at all, the surface takes this hue.
        const uint64_t kTestTextureId = 0x7777777700000003ull;
        const float    kTestRgb[3]    = { 0.20f, 0.60f, 0.90f };
        {
            SceneIPC::TextureDesc td{};
            td.textureId    = kTestTextureId;
            td.format       = SceneIPC::kTexBGRA8;
            td.width        = 4;
            td.height       = 4;
            td.mipLevels    = 1;
            td.payloadBytes = 4 * 4 * 4;

            std::vector<uint8_t> texels(td.payloadBytes);
            for (uint32_t i = 0; i < td.payloadBytes; i += 4)
            {
                texels[i + 0] = (uint8_t)(kTestRgb[2] * 255.0f + 0.5f);   // B
                texels[i + 1] = (uint8_t)(kTestRgb[1] * 255.0f + 0.5f);   // G
                texels[i + 2] = (uint8_t)(kTestRgb[0] * 255.0f + 0.5f);   // R
                texels[i + 3] = 255;
            }
            producer.TryWrite(SceneIPC::kMsgTexture, &td, sizeof(td),
                              texels.data(), td.payloadBytes, false);
        }

        // Reproduces the failure seen against the real game: the view and
        // projection reported via SetTransform are NOT the ones the shaders
        // used, so the naive factorisation rejects almost every instance.
        // Here they are left as identity while the instances are built with a
        // genuine perspective VP, which the resolver has to find on its own.
        Host::Math::Mat4 trueVp = Host::Math::Identity();
        trueVp.m[0]  = 1.3f;    // 1/(aspect*tan)
        trueVp.m[5]  = 2.4f;    // 1/tan
        trueVp.m[10] = 1.001f;
        trueVp.m[11] = 1.0f;    // the terms that make it non-affine
        trueVp.m[14] = -0.1f;
        trueVp.m[15] = 0.0f;

        SceneIPC::FrameBegin fb{};
        fb.frameIndex   = 1;
        fb.renderWidth  = 1920;
        fb.renderHeight = 1080;
        fb.view         = Host::Math::Identity();   // deliberately wrong
        fb.projection   = Host::Math::Identity();   // deliberately wrong
        fb.instanceCount = 4;
        producer.TryWrite(SceneIPC::kMsgFrameBegin, &fb, sizeof(fb), nullptr, 0, true);

        // One mesh instance, and two sprites that must collapse into one.
        const uint64_t ids[4] = { 0x1111111100000001ull,
                                  0x2222222200000002ull,
                                  0x2222222200000002ull,
                                  0x3333333300000004ull };
        for (int i = 0; i < 4; ++i)
        {
            // clip = world * VP, exactly as the game's shaders compute it.
            // Instance 0 has an identity world, so its clip transform IS the
            // VP - which is the foothold the resolver needs.
            Host::Math::Mat4 world = Host::Math::Identity();
            world.m[12] = (float)i * 3.0f;

            SceneIPC::InstanceDesc inst{};
            inst.geometryId    = ids[i];
            inst.baseTextureId = kTestTextureId;

            // The last instance is the transparent one, placed clear of the
            // others so that failing to skip it paints red on empty
            // background rather than hiding behind something.
            if (i == 3)
            {
                inst.baseTextureId = kClearTextureId;
                inst.flags |= SceneIPC::kInstanceAlphaBlend;
                // Left of the mesh and clear of the sprite quads, in the
                // upper half. The test camera scales world x,y by 0.15, so
                // this lands at ndc x -0.75..-0.15, y 0.15..0.75.
                world.m[12] = -5.0f;
                world.m[13] =  1.0f;
                inst.clipTransform = Host::Math::Multiply(world, trueVp);
            }
            inst.clipTransform = Host::Math::Multiply(world, trueVp);
            inst.baseColorFactor[0] = inst.baseColorFactor[1] =
            inst.baseColorFactor[2] = inst.baseColorFactor[3] = 1.0f;
            producer.TryWrite(SceneIPC::kMsgInstance, &inst, sizeof(inst),
                              nullptr, 0, true);
        }

        SceneIPC::FrameEnd fe{};
        fe.frameIndex    = 1;
        fe.instanceCount = 4;
        producer.TryWrite(SceneIPC::kMsgFrameEnd, &fe, sizeof(fe), nullptr, 0, true);

        Host::SceneReceiver rx;
        if (!rx.Attach(kSection)) { printf("FAIL: consumer could not attach\n"); return 1; }
        rx.Poll();

        const Host::Frame& f = rx.CurrentFrame();
        printf("  scene decoded: %zu instances, %zu geometries\n",
               f.instances.size(), rx.GeometryCount());

        Host::GpuAllocator alloc;
        Host::AccelBuilder accel;
        Host::TextureCache textures;
        if (!alloc.Init(&gpu) || !accel.Init(&gpu, &alloc))
        {
            printf("FAIL: %s\n", accel.LastError().c_str());
            return 1;
        }

        if (!textures.Init(&gpu, &alloc))
        {
            printf("FAIL: texture cache - %s\n", textures.LastError().c_str());
            return 1;
        }
        textures.Sync(rx, 64);

        if (!accel.BuildFrame(rx, &textures))
        {
            printf("FAIL: BuildFrame - %s\n", accel.LastError().c_str());
            return 1;
        }

        const Host::AccelStats& st = accel.Stats();
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
            if (!ok) ++failures;
        };

        check(f.instances.size() == 4,      "4 instances decoded");
        check(rx.GeometryCount() == 3,      "3 geometries resident");
        check(st.geometryUnresolved == 0,   "all instances resolved their geometry");
        check(st.transformsRejected == 0,   "all world transforms affine");
        check(st.persistentBlas == 2,       "both 4-triangle meshes got their own BLAS");
        check(st.spriteInstances == 2,      "both quads folded into the sprite batch");
        check(st.spriteTriangles == 4,      "sprite batch holds 4 triangles");
        check(st.tlasInstances == 3,        "TLAS = 2 meshes + 1 merged sprite instance");
        check(accel.Tlas() != VK_NULL_HANDLE, "TLAS handle created");
        check(st.vpBestScore == st.vpSampleSize,
              "resolver found a VP explaining every instance");
        printf("  VP source: %s (%u/%u affine, %u candidates)\n",
               st.vpSource ? st.vpSource : "none",
               st.vpBestScore, st.vpSampleSize, st.vpCandidatesTried);

        printf("  build took %.2f ms, BLAS storage %.1f KB\n",
               st.buildMilliseconds, st.blasBytes / 1024.0);
        printf("  allocator: %u blocks, %u live buffers, %.1f MB in use\n",
               alloc.BlockCount(), alloc.LiveAllocations(),
               alloc.BytesInUse() / (1024.0 * 1024.0));

        // ── Trace it ─────────────────────────────────────────────────────
        // Structures that build without validation errors still prove nothing
        // about whether rays actually hit them, so the scene is traced and the
        // resulting image is written out.
        Host::RayTracer tracer;
        if (!tracer.Init(&gpu, &alloc, shaderDir, 256, 256, textures.Capacity()))
        {
            printf("  [FAIL] ray tracer init: %s\n", tracer.LastError().c_str());
            ++failures;
        }
        else
        {
            Host::SceneUniforms u{};

            // The camera is built from the same VP the instances were, so the
            // recovered basis and the rays agree — which is the whole reason
            // the factorisation only needs to be self-consistent.
            Host::Math::Mat4 vp = Host::Math::Identity();
            vp.m[0] = 0.15f; vp.m[5] = 0.15f; vp.m[10] = 0.01f; vp.m[14] = 0.5f;
            Host::Math::Mat4 invVp;
            Host::Math::Inverse(vp, invVp);
            memcpy(u.invViewProj, invVp.m, sizeof(invVp.m));

            // The game's own rig, straight from the shader constants.
            const float dir[4]  = { -0.4968f, -0.7360f, 0.4600f, 0.0f };  // c95
            const float col[4]  = {  0.97f,    0.97f,   0.97f,   0.0f };  // c94
            const float axis[4] = {  0.0f,    -1.0f,    0.0f,    0.0f };  // c93
            const float sky[4]  = {  0.53f,    0.50f,   0.42f,   0.0f };  // c92
            const float gnd[4]  = {  0.22f,    0.20f,   0.18f,   0.0f };  // c91
            const float amb[4]  = {  0.10f,    0.20f,   0.20f,   0.0f };  // c68
            memcpy(u.lightDirection, dir,  sizeof(dir));
            memcpy(u.lightColor,     col,  sizeof(col));
            memcpy(u.hemisphereAxis, axis, sizeof(axis));
            memcpy(u.skyColor,       sky,  sizeof(sky));
            memcpy(u.groundColor,    gnd,  sizeof(gnd));
            memcpy(u.ambient,        amb,  sizeof(amb));
            u.params[0] = 1000.0f;   // shadow ray length
            u.params[1] = 1.0f;      // exposure

            if (!tracer.Trace(accel.Tlas(), u, &textures,
                              &accel.InstanceRecords()))
            {
                printf("  [FAIL] trace: %s\n", tracer.LastError().c_str());
                ++failures;
            }
            else
            {
                check(true, "vkCmdTraceRaysKHR completed");

                // The PPM is what the pixel checks below parse; the PNG is
                // for looking at, since nothing on Windows previews a PPM.
                check(tracer.SaveImage("astest.ppm"),
                      "traced image written to astest.ppm");
                check(tracer.SaveImage("astest.png"),
                      "traced image written to astest.png");

                // "The trace completed" is not the same as "rays hit
                // anything" — an empty TLAS or a broken camera produces a
                // perfectly clean image of nothing. So the pixels are
                // inspected: the corner must be background, and a
                // meaningful share of the frame must differ from it.
                FILE* img = nullptr;
                fopen_s(&img, "astest.ppm", "rb");
                if (!img) { check(false, "traced image readable"); }
                else
                {
                    int w = 0, h = 0, maxv = 0;
                    fscanf_s(img, "P6 %d %d %d", &w, &h, &maxv);
                    fgetc(img);   // the single whitespace before the payload

                    std::vector<uint8_t> px((size_t)w * h * 3);
                    fread(px.data(), 1, px.size(), img);
                    fclose(img);

                    const uint8_t bg[3] = { px[0], px[1], px[2] };
                    size_t differing = 0, inTopHalf = 0;
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                        {
                            const size_t i = ((size_t)y * w + x) * 3;
                            if (px[i] == bg[0] && px[i+1] == bg[1] &&
                                px[i+2] == bg[2]) continue;
                            ++differing;
                            if (y < h / 2) ++inTopHalf;
                        }

                    const double coverage = 100.0 * differing / ((double)w * h);
                    check(differing > 0, "rays actually hit geometry");
                    check(coverage < 99.0, "background is present, so the "
                                           "camera is not inside the geometry");

                    // Every vertex in the scene has y >= 0, and the test
                    // camera only scales, so all of it belongs above the
                    // horizon — that is, in the TOP half of the image, since
                    // Direct3D puts NDC y = +1 at the top while the launch
                    // index and the file both start at the top row. Get the
                    // flip wrong and the image is a perfect mirror, which no
                    // coverage count would ever notice.
                    check(differing > 0 && inTopHalf == differing,
                          "image is the right way up");

                    // Did the texture actually reach the surface?
                    //
                    // Counted per pixel rather than averaged: the merged
                    // sprite batch is deliberately untextured and covers
                    // five times the area of the textured mesh, so a mean
                    // over the whole frame sits near white either way.
                    //
                    // Lighting scales every channel by roughly the same
                    // amount, so the ratio between channels survives it:
                    // blue/red is 4.5 for this texture and 1.0 for the white
                    // fallback, which no near-miss can confuse.
                    const double want = kTestRgb[2] / kTestRgb[0];
                    size_t textured = 0;
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                        {
                            const size_t i = ((size_t)y * w + x) * 3;
                            if (px[i] == bg[0] && px[i+1] == bg[1] &&
                                px[i+2] == bg[2]) continue;
                            // Undo the gamma encode before comparing.
                            const double r = pow(px[i + 0] / 255.0, 2.2);
                            const double b = pow(px[i + 2] / 255.0, 2.2);
                            if (r < 1e-6) continue;
                            const double ratio = b / r;
                            if (ratio > want * 0.85 && ratio < want * 1.15)
                                ++textured;
                        }

                    // Nothing may be red: the transparent instance is
                    // bright red and sits on empty background, so a single
                    // red pixel means the any-hit shader failed to skip it.
                    size_t red = 0;
                    for (size_t i = 0; i < px.size(); i += 3)
                        if (px[i] > 120 && px[i+1] < 80 && px[i+2] < 80) ++red;
                    check(red == 0,
                          "the fully transparent instance was skipped, not "
                          "drawn");

                    check(textured >= 100,
                          "the textured instance sampled its own texture, "
                          "not the white fallback");
                    printf("  %zu pixels carry the texture's blue/red of %.1f\n",
                           textured, want);
                    printf("  geometry covers %.1f%% of the frame "
                           "(%.0f%% of it above the midline); "
                           "background rgb(%u,%u,%u)\n",
                           coverage,
                           differing ? 100.0 * inTopHalf / differing : 0.0,
                           bg[0], bg[1], bg[2]);
                }
                printf("  traced %ux%u in %.2f ms, SBT %llu bytes\n",
                       tracer.Stats().width, tracer.Stats().height,
                       tracer.Stats().traceMilliseconds,
                       (unsigned long long)tracer.Stats().sbtBytes);
            }
        }
        tracer.Shutdown();
        textures.Shutdown();
        accel.Shutdown();
        alloc.Shutdown();
        printf("%s\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
        return failures == 0 ? 0 : 1;
    }

    void ReportAccel(const Host::AccelStats& st)
    {
        printf("    AS: %u BLAS (%u rebuilt) | %u sprites -> %u tris merged | "
               "TLAS %u inst | rejected %u | sky/overlay %u | %.2f ms\n",
               st.persistentBlas, st.blasBuiltThisFrame,
               st.spriteInstances, st.spriteTriangles,
               st.tlasInstances, st.transformsRejected, st.nonOccluding,
               st.buildMilliseconds);
    }

    void ReportSummary(const Host::SceneReceiver& rx)
    {
        const Host::ReceiverStats& s = rx.Stats();
        const Host::Frame& f = rx.CurrentFrame();

        // ASCII rather than box-drawing: console output is frequently
        // redirected to a file or pipe, where a non-ASCII codepage turns
        // those characters into mojibake.
        printf("\n-------- session summary --------\n");
        printf("  messages read        %llu\n", (unsigned long long)s.messagesRead);
        printf("  frames completed     %llu\n", (unsigned long long)s.framesCompleted);
        printf("  bytes received       %.1f MB\n", s.bytesReceived / (1024.0*1024.0));
        printf("  geometry uploads     %llu (%zu resident)\n",
               (unsigned long long)s.geometryUploads, rx.GeometryCount());
        printf("  texture uploads      %llu (%zu resident)\n",
               (unsigned long long)s.textureUploads, rx.TextureCount());
        printf("  malformed messages   %llu\n",
               (unsigned long long)s.malformedMessages);
        printf("  producer dropped     %llu frames / %.1f MB\n",
               (unsigned long long)rx.ProducerFramesDropped(),
               rx.ProducerBytesDropped() / (1024.0*1024.0));

        if (f.lightingValid)
        {
            const SceneIPC::LightingDesc& L = f.lighting;
            printf("\n  lighting rig (from the game's own shader constants):\n");
            printf("    directional dir   %7.4f %7.4f %7.4f\n",
                   L.directionalDir[0], L.directionalDir[1], L.directionalDir[2]);
            printf("    directional col   %7.4f %7.4f %7.4f\n",
                   L.directionalColor[0], L.directionalColor[1], L.directionalColor[2]);
            printf("    hemisphere axis   %7.4f %7.4f %7.4f\n",
                   L.hemisphereAxis[0], L.hemisphereAxis[1], L.hemisphereAxis[2]);
            printf("    sky / ground      %.3f %.3f %.3f  /  %.3f %.3f %.3f\n",
                   L.skyColor[0], L.skyColor[1], L.skyColor[2],
                   L.groundColor[0], L.groundColor[1], L.groundColor[2]);
        }

        // What the acceleration structures would look like. This is the
        // handover point to the Vulkan side.
        printf("\n  acceleration structure shape for the last frame:\n");
        printf("    BLAS candidates    %zu (one per distinct geometry)\n",
               rx.GeometryCount());
        printf("    TLAS instances     %zu\n", f.instances.size());
    }
}

int main(int argc, char** argv)
{
    const char* section = SceneIPC::kDefaultSectionName;
    uint64_t    maxFrames = 0;
    // Must stay comfortably above the producer's own 300-frame retention:
    // the producer only re-sends geometry it has forgotten, so the host must
    // never drop something the producer still believes is cached.
    uint64_t    retention = 900;
    bool        quiet = false;
    bool        probeOnly = false;
    bool        asTest = false;
    bool        validation = true;
    std::string shaderDirStorage = ResolveShaderDir();
    const char* shaderDir = shaderDirStorage.c_str();
    // Width is not an option because it is not free: the recovered inverse
    // view-projection carries the game aspect ratio, so the traced image has
    // to keep it or the result comes out stretched. Only height is chosen.
    uint32_t    traceHeight = 720;
    uint64_t    saveFrame = 0;             // 1-based; 0 = never save
    uint64_t    saveEvery = 0;             // 0 = off
    // Each upload stalls on a queue wait, so a frame bringing in a hundred
    // new textures would hitch badly. The rest arrive over the following
    // frames, and their surfaces sample white until they do.
    uint32_t    textureBudget = 8;
    const char* savePath = "traced_frame.png";

    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--section") && i + 1 < argc) section = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            maxFrames = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--retain") && i + 1 < argc)
            retention = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--quiet")) quiet = true;
        else if (!strcmp(argv[i], "--probe")) probeOnly = true;
        else if (!strcmp(argv[i], "--astest")) asTest = true;
        else if (!strcmp(argv[i], "--shaders") && i + 1 < argc) shaderDir = argv[++i];
        else if (!strcmp(argv[i], "--trace-height") && i + 1 < argc)
            traceHeight = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--save-frame") && i + 1 < argc)
            saveFrame = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--save-every") && i + 1 < argc)
            saveEvery = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--texture-budget") && i + 1 < argc)
            textureBudget = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--save-path") && i + 1 < argc) savePath = argv[++i];
        else if (!strcmp(argv[i], "--no-validation")) validation = false;
        else if (!strcmp(argv[i], "--help")) { PrintUsage(); return 0; }
        else { printf("unknown argument: %s\n\n", argv[i]); PrintUsage(); return 2; }
    }

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    printf("PESMod render host (%d-bit)\n\n", (int)sizeof(void*) * 8);

    // The device comes up before the scene stream is touched. If ray tracing
    // is unavailable there is nothing this process can usefully do, and the
    // failure should be reported immediately rather than after a long wait
    // for a producer that was never the problem.
    Host::VulkanDeviceOptions vkOptions;
    vkOptions.enableValidation = validation;
    vkOptions.preferDiscrete   = true;
    vkOptions.verbose          = !quiet;

    Host::VulkanDevice gpu;
    if (!gpu.Create(vkOptions))
    {
        printf("\nFATAL: %s\n", gpu.LastError().c_str());
        return 1;
    }
    printf("\nVulkan ray tracing device ready:\n");
    gpu.PrintCapabilities();

    if (probeOnly) return 0;
    if (asTest)    return RunAccelSelfTest(gpu, shaderDir);

    printf("\nattaching to '%s'\n", section);

    // These live for the whole session; only their contents are rebuilt per
    // frame, and the persistent BLASes survive across frames by design.
    Host::GpuAllocator alloc;
    Host::AccelBuilder accel;
    if (!alloc.Init(&gpu) || !accel.Init(&gpu, &alloc))
    {
        printf("FATAL: acceleration structure init failed: %s\n",
               accel.LastError().c_str());
        return 1;
    }

    Host::TextureCache textures;
    if (!textures.Init(&gpu, &alloc))
    {
        printf("FATAL: texture cache init failed: %s\n",
               textures.LastError().c_str());
        return 1;
    }

    // Created here but initialised on the first frame: the traced image has
    // to match the game aspect ratio, which is not known until a frame
    // arrives carrying its render size.
    Host::RayTracer tracer;
    bool tracerTried = false;

    Host::SceneReceiver rx;

    // The game may not be running yet, and may be restarted while the host
    // stays up, so attaching is a retry loop rather than a one-shot.
    while (!g_quit && !rx.IsAttached())
    {
        if (rx.Attach(section)) break;
        Sleep(250);
    }
    if (!rx.IsAttached())
    {
        printf("never attached; exiting\n");
        return 1;
    }
    printf("attached.\n\n");

    uint64_t reported = 0;
    while (!g_quit)
    {
        if (!rx.Poll())
        {
            Sleep(1);
            continue;
        }

        // One geometry becomes one BLAS, so an unbounded cache is an
        // unbounded number of acceleration structures, not merely wasted RAM.
        if (retention)
        {
            rx.EvictUnused(retention);
            // Structures whose geometry has gone must go with it, or they
            // would keep GPU memory alive for meshes nothing references.
            accel.PruneOrphans(rx);
        }

        // Textures first: the builder records the slot each instance will
        // sample, so one arriving after the record is written would not be
        // seen until the next frame.
        textures.Sync(rx, textureBudget);

        if (!accel.BuildFrame(rx, &textures))
        {
            printf("FATAL: acceleration structure build failed: %s\n",
                   accel.LastError().c_str());
            break;
        }

        // ── Bring the tracer up on the first frame ───────────────────
        // Only attempted once: if the shaders are missing, retrying every
        // frame would bury the scene report under repeated errors, and the
        // host stays useful for diagnostics without a trace.
        if (traceHeight && !tracerTried)
        {
            tracerTried = true;
            const SceneIPC::FrameBegin& fb = rx.CurrentFrame().begin;
            const uint32_t srcW = fb.renderWidth  ? fb.renderWidth  : 640;
            const uint32_t srcH = fb.renderHeight ? fb.renderHeight : 480;
            const uint32_t w = (uint32_t)((double)traceHeight * srcW / srcH + 0.5);

            if (tracer.Init(&gpu, &alloc, shaderDir, w, traceHeight,
                            textures.Capacity()))
                printf("ray tracer ready: %ux%u (game renders %ux%u)\n\n",
                       w, traceHeight, srcW, srcH);
            else
                printf("WARNING: ray tracing disabled - %s\n\n",
                       tracer.LastError().c_str());
        }

        // ── Trace the frame ──────────────────────────────────────────────
        // Only once the resolver has a view-projection it trusts; without it
        // the camera and the geometry would disagree and the image would be
        // meaningless rather than merely wrong.
        if (tracer.IsReady() && accel.Tlas() != VK_NULL_HANDLE &&
            accel.HasViewProj())
        {
            const Host::Frame& f = rx.CurrentFrame();

            Host::SceneUniforms u{};
            memcpy(u.invViewProj, accel.InverseViewProj().m, sizeof(u.invViewProj));

            // The game's own rig, carried across frames by the receiver since
            // the producer only resends it on change.
            if (f.lightingValid)
            {
                memcpy(u.lightDirection, f.lighting.directionalDir,   sizeof(u.lightDirection));
                memcpy(u.lightColor,     f.lighting.directionalColor, sizeof(u.lightColor));
                memcpy(u.hemisphereAxis, f.lighting.hemisphereAxis,   sizeof(u.hemisphereAxis));
                memcpy(u.skyColor,       f.lighting.skyColor,         sizeof(u.skyColor));
                memcpy(u.groundColor,    f.lighting.groundColor,      sizeof(u.groundColor));
                memcpy(u.ambient,        f.lighting.ambient,          sizeof(u.ambient));
            }
            else
            {
                // No lit draw has run yet, so the constants hold nothing. A
                // zeroed rig would trace a black frame and read as a broken
                // tracer rather than as missing lighting, so stand in a
                // neutral overhead light until the real one arrives.
                u.lightDirection[1] = -1.0f;
                u.hemisphereAxis[1] = -1.0f;
                u.lightColor[0]  = u.lightColor[1]  = u.lightColor[2]  = 0.90f;
                u.skyColor[0]    = u.skyColor[1]    = u.skyColor[2]    = 0.45f;
                u.groundColor[0] = u.groundColor[1] = u.groundColor[2] = 0.18f;
                u.ambient[0]     = u.ambient[1]     = u.ambient[2]     = 0.10f;
            }
            u.params[0] = 20000.0f;   // shadow ray length, in the game's units
            u.params[1] = 1.0f;

            if (tracer.Trace(accel.Tlas(), u, &textures,
                             &accel.InstanceRecords()))
            {
                const uint64_t n = reported + 1;
                if ((saveFrame && n == saveFrame) ||
                    (saveEvery && n % saveEvery == 0))
                {
                    // A series gets numbered names; a single shot keeps the
                    // name it was given, so scripting around it stays simple.
                    const std::string out =
                        saveEvery ? NumberedPath(savePath, n)
                                  : std::string(savePath);

                    if (tracer.SaveImage(out.c_str()))
                        printf("    wrote traced frame to %s\n", out.c_str());
                    else
                        printf("    image save failed: %s\n",
                               tracer.LastError().c_str());
                }
                if (!quiet)
                    printf("    trace: %ux%u in %.2f ms\n",
                           tracer.Stats().width, tracer.Stats().height,
                           tracer.Stats().traceMilliseconds);
            }
            else if (!quiet)
            {
                printf("    trace failed: %s\n", tracer.LastError().c_str());
            }
        }

        if (!quiet)
        {
            ReportFrame(rx);
            ReportAccel(accel.Stats());

            // Only when there is something to say. A non-zero skippedFormat
            // or fullyTransparent means textures are arriving in a shape the
            // cache did not expect, which shows up as missing surfaces rather
            // than as an error.
            const Host::TextureStats& ts = textures.Stats();
            if (ts.uploadedThisFrame || ts.skippedFormat || ts.fullyTransparent)
                printf("    tex: %u resident (+%u this frame), %.1f MB | "
                       "X8 forced opaque %u | unsupported %u | all-transparent %u\n",
                       ts.resident, ts.uploadedThisFrame,
                       ts.bytesResident / (1024.0 * 1024.0),
                       ts.opaqueForced, ts.skippedFormat, ts.fullyTransparent);
        }
        ++reported;
        if (maxFrames && reported >= maxFrames) break;
    }

    ReportSummary(rx);

    // Explicit, and in reverse order of creation: the tracer holds descriptors
    // naming the builder TLAS, and both draw memory from the allocator.
    tracer.Shutdown();
    textures.Shutdown();
    accel.Shutdown();
    alloc.Shutdown();
    return 0;
}
