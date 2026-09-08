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

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
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
            "  --no-validation    disable Vulkan validation layers\n"
            "  --help\n",
            SceneIPC::kDefaultSectionName);
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
    int RunAccelSelfTest(Host::VulkanDevice& gpu)
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

        // Identity view and projection, so the recovered world transform is
        // the clip transform itself and the affinity check must pass.
        SceneIPC::FrameBegin fb{};
        fb.frameIndex   = 1;
        fb.renderWidth  = 1920;
        fb.renderHeight = 1080;
        fb.view         = Host::Math::Identity();
        fb.projection   = Host::Math::Identity();
        fb.instanceCount = 3;
        producer.TryWrite(SceneIPC::kMsgFrameBegin, &fb, sizeof(fb), nullptr, 0, true);

        // One mesh instance, and two sprites that must collapse into one.
        const uint64_t ids[3] = { 0x1111111100000001ull,
                                  0x2222222200000002ull,
                                  0x2222222200000002ull };
        for (int i = 0; i < 3; ++i)
        {
            SceneIPC::InstanceDesc inst{};
            inst.geometryId    = ids[i];
            inst.clipTransform = Host::Math::Identity();
            inst.clipTransform.m[12] = (float)i * 3.0f;   // offset each one
            inst.baseColorFactor[0] = inst.baseColorFactor[1] =
            inst.baseColorFactor[2] = inst.baseColorFactor[3] = 1.0f;
            producer.TryWrite(SceneIPC::kMsgInstance, &inst, sizeof(inst),
                              nullptr, 0, true);
        }

        SceneIPC::FrameEnd fe{};
        fe.frameIndex    = 1;
        fe.instanceCount = 3;
        producer.TryWrite(SceneIPC::kMsgFrameEnd, &fe, sizeof(fe), nullptr, 0, true);

        Host::SceneReceiver rx;
        if (!rx.Attach(kSection)) { printf("FAIL: consumer could not attach\n"); return 1; }
        rx.Poll();

        const Host::Frame& f = rx.CurrentFrame();
        printf("  scene decoded: %zu instances, %zu geometries\n",
               f.instances.size(), rx.GeometryCount());

        Host::GpuAllocator alloc;
        Host::AccelBuilder accel;
        if (!alloc.Init(&gpu) || !accel.Init(&gpu, &alloc))
        {
            printf("FAIL: %s\n", accel.LastError().c_str());
            return 1;
        }

        if (!accel.BuildFrame(rx))
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

        check(f.instances.size() == 3,      "3 instances decoded");
        check(rx.GeometryCount() == 2,      "2 geometries resident");
        check(st.geometryUnresolved == 0,   "all instances resolved their geometry");
        check(st.transformsRejected == 0,   "all world transforms affine");
        check(st.persistentBlas == 1,       "only the 4-triangle mesh got its own BLAS");
        check(st.spriteInstances == 2,      "both quads folded into the sprite batch");
        check(st.spriteTriangles == 4,      "sprite batch holds 4 triangles");
        check(st.tlasInstances == 2,        "TLAS = 1 mesh + 1 merged sprite instance");
        check(accel.Tlas() != VK_NULL_HANDLE, "TLAS handle created");

        printf("  build took %.2f ms, BLAS storage %.1f KB\n",
               st.buildMilliseconds, st.blasBytes / 1024.0);
        printf("  allocator: %u blocks, %u live buffers, %.1f MB in use\n",
               alloc.BlockCount(), alloc.LiveAllocations(),
               alloc.BytesInUse() / (1024.0 * 1024.0));

        accel.Shutdown();
        alloc.Shutdown();
        printf("%s\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
        return failures == 0 ? 0 : 1;
    }

    void ReportAccel(const Host::AccelStats& st)
    {
        printf("    AS: %u BLAS (%u rebuilt) | %u sprites -> %u tris merged | "
               "TLAS %u inst | rejected %u | %.2f ms\n",
               st.persistentBlas, st.blasBuiltThisFrame,
               st.spriteInstances, st.spriteTriangles,
               st.tlasInstances, st.transformsRejected,
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
    if (asTest)    return RunAccelSelfTest(gpu);

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

        if (!accel.BuildFrame(rx))
        {
            printf("FATAL: acceleration structure build failed: %s\n",
                   accel.LastError().c_str());
            break;
        }

        if (!quiet) { ReportFrame(rx); ReportAccel(accel.Stats()); }
        ++reported;
        if (maxFrames && reported >= maxFrames) break;
    }

    ReportSummary(rx);
    return 0;
}
