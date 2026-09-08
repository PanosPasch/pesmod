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

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

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
        uint32_t resolved = 0, missing = 0;
        for (size_t i = 0; i < f.instances.size(); ++i)
        {
            const Host::Geometry* g = rx.FindGeometry(f.instances[i].geometryId);
            if (!g) { ++missing; continue; }
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
            printf("    WARNING: %u instance(s) reference geometry the host "
                   "has not received - producer may be dropping messages\n",
                   missing);
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

    printf("\nattaching to '%s'\n", section);

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
        if (retention) rx.EvictUnused(retention);

        if (!quiet) ReportFrame(rx);
        ++reported;
        if (maxFrames && reported >= maxFrames) break;
    }

    ReportSummary(rx);
    return 0;
}
