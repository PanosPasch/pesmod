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
#include "vk_present.h"
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

    int g_failures = 0;

    void check(bool ok, const char* what)
    {
        printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++g_failures;
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
            "  --skintest         check the skinning kernel, exit\n"
            "  --resendtest       check the resend loop (no GPU), exit\n"
            "  --replaytest       check record/replay (no GPU), exit\n"
            "  --record <file>    tee the scene stream to a file\n"
            "  --replay <file>    render a recording instead of the game\n"
            "  --skip <n>         drain n frames without rendering them\n"
            "  --shaders <dir>    directory holding the compiled .spv files\n"
            "                     (default: shaders)\n"
            "  --trace-height <n> traced image height; the width follows the\n"
            "                     game aspect ratio (default 720, 0 = off)\n"
            "  --save-frame <n>   write the nth traced frame to disk\n"
            "  --texture-budget <n>  texture uploads per frame (default 8)\n"
            "  --save-every <n>   write every nth traced frame, numbered\n"
            "  --save-path <file> where to write it (default traced_frame.png;\n"
            "                     a .ppm extension writes a PPM instead)\n"
            "  --debug-view <n>   write one shading term instead of the image:\n"
            "                     1 instance, 2 albedo, 3 normal,\n"
            "                     4 sky occlusion, 5 shadow, 6 record flags\n"
            "  --dectest          check the texture decoders (needs no device)\n"
            "  --ao <n>           sky occlusion rays per hit (default 8;\n"
            "                     0 reproduces the game's own flat sky term)\n"
            "  --ao-reach <units> how far they look, in the game's units\n"
            "                     (default 150, about a metre and a half)\n"
            "  --headless         trace without opening a window (for tests\n"
            "                     and for saving frames over a recording)\n"
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



    // Records a stream and replays it, checking the replay reconstructs the
    // same scene.
    //
    // This proves the harness before anything is diagnosed with it. A replay
    // that quietly differed from the recording would send every later
    // investigation down the wrong path, which is worse than having no
    // harness at all.
    //
    // Needs no GPU.
    int RunReplayTest()
    {
        const char* kSection = "Local\\PESMod.SceneStream.ReplayTest";
        const char* kFile    = "pesmod_replaytest.bin";
        printf("-------- record and replay --------\n\n");

        SceneIPC::SharedRing producer;
        if (!producer.CreateAsProducer(kSection, 1ull * 1024ull * 1024ull))
        {
            printf("FAIL: could not create the test section\n");
            return 1;
        }

        // Two geometries and a texture, so the recording covers a message
        // with a payload as well as the fixed-size ones.
        const uint64_t kGeoA = 0xC0DE000000000001ull;
        const uint64_t kGeoB = 0xC0DE000000000002ull;
        const uint64_t kTex  = 0xC0DE000000000003ull;

        for (int k = 0; k < 2; ++k)
        {
            struct V { float px, py, pz; };
            V verts[3] = { {0,0,0}, {1,0,0}, {0,1,0} };

            SceneIPC::GeometryDesc gd{};
            gd.geometryId   = k ? kGeoB : kGeoA;
            gd.vertexKind   = SceneIPC::kVertexPreLit;
            gd.vertexStride = sizeof(V);
            gd.vertexCount  = 3;
            gd.uvOffset     = SceneIPC::kNoVertexAttribute;
            gd.normalOffset = SceneIPC::kNoVertexAttribute;
            gd.colorOffset  = SceneIPC::kNoVertexAttribute;
            producer.TryWrite(SceneIPC::kMsgGeometry, &gd, sizeof(gd),
                              verts, sizeof(verts), false);
        }
        {
            SceneIPC::TextureDesc td{};
            td.textureId    = kTex;
            td.format       = SceneIPC::kTexBGRA8;
            td.width        = 2;
            td.height       = 2;
            td.mipLevels    = 1;
            td.payloadBytes = 2 * 2 * 4;

            std::vector<uint8_t> px(td.payloadBytes, 0x7F);
            producer.TryWrite(SceneIPC::kMsgTexture, &td, sizeof(td),
                              px.data(), td.payloadBytes, false);
        }

        SceneIPC::FrameBegin fb{};
        fb.frameIndex    = 11;
        fb.renderWidth   = 640;
        fb.renderHeight  = 480;
        fb.instanceCount = 3;
        producer.TryWrite(SceneIPC::kMsgFrameBegin, &fb, sizeof(fb), nullptr, 0, true);

        const uint64_t ids[3] = { kGeoA, kGeoB, kGeoA };
        for (int i = 0; i < 3; ++i)
        {
            SceneIPC::InstanceDesc inst{};
            inst.geometryId    = ids[i];
            inst.baseTextureId = kTex;
            producer.TryWrite(SceneIPC::kMsgInstance, &inst, sizeof(inst),
                              nullptr, 0, true);
        }

        SceneIPC::FrameEnd fe{};
        fe.frameIndex    = 11;
        fe.instanceCount = 3;
        producer.TryWrite(SceneIPC::kMsgFrameEnd, &fe, sizeof(fe), nullptr, 0, true);

        // ── Record ───────────────────────────────────────────────────────
        uint64_t liveBytes = 0, liveFrameIndex = 0;
        size_t   liveGeo = 0, liveTex = 0, liveInstances = 0;
        {
            Host::SceneReceiver rx;
            if (!rx.Attach(kSection)) { printf("FAIL: attach\n"); return 1; }
            check(rx.StartRecording(kFile), "recording opened");
            rx.Poll();

            liveGeo        = rx.GeometryCount();
            liveTex        = rx.TextureCount();
            liveInstances  = rx.CurrentFrame().instances.size();
            liveFrameIndex = rx.CurrentFrame().begin.frameIndex;
            liveBytes      = rx.RecordedBytes();

            // Closed explicitly so the file is complete and readable below.
            rx.StopRecording();
        }
        check(liveBytes > 0, "the recording has bytes in it");

        // ── Replay ───────────────────────────────────────────────────────
        Host::SceneReceiver replay;
        if (!replay.StartReplay(kFile))
        {
            printf("FAIL: could not reopen the recording\n");
            return 1;
        }

        // A replay stops at each frame boundary, so one Poll is one frame.
        const bool gotFrame = replay.Poll();

        check(gotFrame, "the replay produced a frame");
        check(replay.CurrentFrame().begin.frameIndex == liveFrameIndex,
              "same frame index");
        check(replay.CurrentFrame().instances.size() == liveInstances,
              "same instance count");
        check(replay.GeometryCount() == liveGeo, "same geometry resident");
        check(replay.TextureCount()  == liveTex, "same textures resident");
        check(replay.FindGeometry(kGeoA) && replay.FindGeometry(kGeoB),
              "both geometries came back by id");

        const Host::Texture* t = replay.FindTexture(kTex);
        check(t && t->pixels.size() == 16 && t->pixels[0] == 0x7F,
              "the texture payload survived the round trip");
        check(replay.Stats().malformedMessages == 0,
              "nothing in the recording was malformed");

        remove(kFile);
        printf("\n%s\n", g_failures == 0 ? "ALL CHECKS PASSED"
                                         : "FAILURES PRESENT");
        return g_failures == 0 ? 0 : 1;
    }

    // The consumer -> producer resend loop, end to end in one process.
    //
    // This is the invariant that failed twice: the producer decided whether
    // to re-send from its own record of what it had sent, the consumer
    // evicted on its own schedule, and nothing reconciled them. A frame
    // measured live had 278 of 992 instances referencing geometry the
    // consumer had dropped and the producer would never send again.
    //
    // Needs no GPU, so it runs before the device is created.
    // ── Texture decoding ────────────────────────────────────────────────────
    //
    // Needs no device and no game: the decoders are pure functions, and this is
    // the only way they get exercised at all. Every recording captured so far
    // contains textures in exactly one format - B8G8R8A8 - so a replay cannot
    // tell a correct DXT decoder from one that returns nothing, and the live
    // game is where the other formats turn up.
    //
    // The blocks below are written out by hand with their answers derived from
    // the format, not from running this code and writing down what it said.
    int RunDecodeTest()
    {
        printf("PESMod texture decode self-test\n\n");

        auto texel = [](const uint8_t* px, uint32_t w, uint32_t x, uint32_t y)
        {
            return px + ((size_t)y * w + x) * 4;
        };
        auto isBgra = [](const uint8_t* p, int b, int g, int r, int a)
        {
            return p[0] == b && p[1] == g && p[2] == r && p[3] == a;
        };

        // ── The peel must resume inside one decal step ──────────────────────
    //
    // The builder puts a blended decal one step in front of what it
    // decorates; the ray generation, having composited the decal, resumes
    // just past it. If that resume reaches a whole step, it lands past the
    // base surface too and the base is never composited at all.
    //
    // That is not a subtle degradation: on the pitch it skipped the grass
    // and left a near-black wear overlay at 5% alpha over whatever lay
    // beyond, which rendered as flat blue-grey. It went unnoticed through a
    // replay because the step a decal lands on depends on how many blended
    // draws precede it, and letting the game's non-occluding draws into the
    // scene changed those counts.
    //
    // Checked across the distances this game actually uses, and at both
    // ends of what float32 can resolve there.
    {
        const float kStepFloor = 1.0e-4f;      // the shader's absolute floor
        bool insideAStep = true, clearsPrecision = true;
        for (double d = 1.0; d <= 40000.0; d *= 1.7)
        {
            const double step   = d * Host::kDecalBias;
            const double resume = (step * Host::kResumeFraction > kStepFloor)
                                ? step * Host::kResumeFraction : kStepFloor;
            if (resume >= step && step > kStepFloor) insideAStep = false;

            // And far enough out to not re-hit the same surface: float32
            // spacing at d is about d * 2^-23.
            if (resume < d * 1.2e-7) clearsPrecision = false;
        }
        check(insideAStep,
              "the peel resumes inside one decal step, so a decal cannot "
              "hide the surface it decorates");
        check(clearsPrecision,
              "and still clears float32 spacing, so a surface cannot re-hit "
              "itself");
        printf("  decal step %.1e of the distance, peel resumes at %.0f%% of "
               "one step\n",
               Host::kDecalBias, 100.0 * Host::kResumeFraction);
    }

    // ── Sizes ───────────────────────────────────────────────────────────
        // A 4x4 DXT1 image is one block of 8 bytes, DXT3 and DXT5 one of 16.
        // A 5x5 needs 2x2 blocks, because a block is 4x4 and does not divide.
        check(Host::TextureSourceMipBytes(SceneIPC::kTexDXT1, 4, 4, 0) == 8,
              "DXT1 4x4 is one 8-byte block");
        check(Host::TextureSourceMipBytes(SceneIPC::kTexDXT5, 4, 4, 0) == 16,
              "DXT5 4x4 is one 16-byte block");
        check(Host::TextureSourceMipBytes(SceneIPC::kTexDXT1, 5, 5, 0) == 32,
              "DXT1 5x5 rounds up to four blocks");
        check(Host::TextureSourceMipBytes(SceneIPC::kTexL8, 8, 8, 1) == 16,
              "L8 mip 1 of 8x8 is 4x4 bytes");
        check(Host::TextureSourceMipBytes(SceneIPC::kTexBGRA4444, 4, 4, 0) == 32,
              "4444 is two bytes a texel");

        uint8_t out[4 * 4 * 4];

        // ── DXT1, four-colour mode ──────────────────────────────────────────
        // c0 = 0xF800 is pure red and c1 = 0x001F pure blue; c0 > c1 selects the
        // opaque four-colour mode. Indices run two bits per texel from the
        // low end of a little-endian word, so 0x00 gives index 0 across the top
        // row: red. The third row is index 2, which is (2*c0 + c1)/3.
        {
            const uint8_t block[8] = {
                0x00, 0xF8,   // c0: red
                0x1F, 0x00,   // c1: blue
                0x00,         // row 0: all index 0
                0x55,         // row 1: all index 1
                0xAA,         // row 2: all index 2
                0xFF          // row 3: all index 3
            };
            memset(out, 0, sizeof(out));
            check(Host::DecodeTextureMip(SceneIPC::kTexDXT1, block, sizeof(block),
                                         4, 4, out),
                  "DXT1 block decoded");
            check(isBgra(texel(out, 4, 0, 0), 0, 0, 255, 255),
                  "DXT1 index 0 is the first endpoint");
            check(isBgra(texel(out, 4, 3, 1), 255, 0, 0, 255),
                  "DXT1 index 1 is the second endpoint");

            // (2*255 + 0)/3 = 170 red, (2*0 + 255)/3 = 85 blue.
            const uint8_t* p = texel(out, 4, 0, 2);
            check(p[2] == 170 && p[0] == 85 && p[1] == 0 && p[3] == 255,
                  "DXT1 index 2 is two thirds of the way to the first endpoint");
            check(texel(out, 4, 0, 3)[3] == 255,
                  "DXT1 four-colour mode has no transparent index");
        }

        // ── DXT1, three-colour mode ─────────────────────────────────────────
        // The endpoints swapped, so c0 < c1 and index 3 becomes transparent.
        // This is the whole reason the mode bit exists, and getting it backwards
        // makes a third of every alpha-cut texture opaque black.
        {
            const uint8_t block[8] = {
                0x1F, 0x00,   // c0: blue
                0x00, 0xF8,   // c1: red
                0x00, 0x00, 0x00, 0xFF   // row 3 all index 3
            };
            memset(out, 0xCD, sizeof(out));
            check(Host::DecodeTextureMip(SceneIPC::kTexDXT1, block, sizeof(block),
                                         4, 4, out),
                  "DXT1 three-colour block decoded");
            check(texel(out, 4, 0, 0)[3] == 255,
                  "DXT1 three-colour index 0 stays opaque");
            check(isBgra(texel(out, 4, 2, 3), 0, 0, 0, 0),
                  "DXT1 three-colour index 3 is transparent");
        }

        // ── DXT3 ────────────────────────────────────────────────────────────
        // Four-bit alpha, two texels a byte, low nibble first. 0x0F then 0xF0
        // gives 15, 0, 0, 15 across the first two bytes - that is alpha 255 at
        // texel 0, 0 at texels 1 and 2, and 255 at texel 3.
        //
        // The colour half is deliberately in c0 < c1 order: DXT3 has its own
        // alpha, so its colour endpoints are always four-colour whatever their
        // order. Reading them the DXT1 way would make index 3 transparent and
        // throw the block's real alpha away.
        {
            uint8_t block[16] = { 0 };
            block[0] = 0x0F;   // texel 0 -> 15, texel 1 -> 0
            block[1] = 0xF0;   // texel 2 -> 0,  texel 3 -> 15
            block[8]  = 0x1F; block[9]  = 0x00;   // c0: blue
            block[10] = 0x00; block[11] = 0xF8;   // c1: red
            block[15] = 0xFF;                      // row 3: all index 3

            memset(out, 0, sizeof(out));
            check(Host::DecodeTextureMip(SceneIPC::kTexDXT3, block, sizeof(block),
                                         4, 4, out),
                  "DXT3 block decoded");
            check(texel(out, 4, 0, 0)[3] == 255 && texel(out, 4, 1, 0)[3] == 0 &&
                  texel(out, 4, 2, 0)[3] == 0   && texel(out, 4, 3, 0)[3] == 255,
                  "DXT3 alpha nibbles land on the right texels");
            check(texel(out, 4, 0, 3)[3] == 0,
                  "DXT3 index 3 takes its alpha from the alpha block, not the "
                  "colour mode");

            // Index 3 in four-colour mode is (c0 + 2*c1)/3: mostly red.
            const uint8_t* p = texel(out, 4, 0, 3);
            check(p[2] == 170 && p[0] == 85,
                  "DXT3 colour endpoints are always four-colour");
        }

        // ── DXT5 ────────────────────────────────────────────────────────────
        // a0 = 255, a1 = 0 and a0 > a1 selects the eight-value ramp:
        // index 0 is 255, index 1 is 0, and index 2 is (6*255 + 1*0)/7 = 218.
        {
            uint8_t block[16] = { 0 };
            block[0] = 255;    // a0
            block[1] = 0;      // a1
            // Sixteen 3-bit indices over six bytes. 0b...010'001'000 = 0x88, 0x00
            // puts index 0 at texel 0, index 1 at texel 1, index 2 at texel 2.
            block[2] = 0x88;
            block[3] = 0x00;
            block[8]  = 0x00; block[9]  = 0xF8;   // c0: red
            block[10] = 0x1F; block[11] = 0x00;   // c1: blue

            memset(out, 0, sizeof(out));
            check(Host::DecodeTextureMip(SceneIPC::kTexDXT5, block, sizeof(block),
                                         4, 4, out),
                  "DXT5 block decoded");
            check(texel(out, 4, 0, 0)[3] == 255, "DXT5 alpha index 0 is a0");
            check(texel(out, 4, 1, 0)[3] == 0,   "DXT5 alpha index 1 is a1");
            check(texel(out, 4, 2, 0)[3] == 218,
                  "DXT5 alpha index 2 is one seventh of the way from a0 to a1");
        }

        // ── The uncompressed formats ────────────────────────────────────────
        {
            // R5G6B5: 0xFFFF is white, and bit replication has to reach 255
            // exactly rather than 248 or 252.
            const uint8_t px565[8] = { 0xFF, 0xFF,  0x00, 0x00,
                                       0x00, 0xF8,  0x1F, 0x00 };
            memset(out, 0, sizeof(out));
            check(Host::DecodeTextureMip(SceneIPC::kTexBGR565, px565, sizeof(px565),
                                         2, 2, out),
                  "565 decoded");
            check(isBgra(out + 0, 255, 255, 255, 255),
                  "565 all bits set is exactly white, not 248");
            check(isBgra(out + 4, 0, 0, 0, 255), "565 zero is opaque black");
            check(isBgra(out + 8, 0, 0, 255, 255), "565 red channel is the top bits");
            check(isBgra(out + 12, 255, 0, 0, 255), "565 blue channel is the low bits");
        }
        {
            // A1R5G5B5: the top bit is alpha, all or nothing.
            const uint8_t px1555[4] = { 0xFF, 0xFF,  0xFF, 0x7F };
            memset(out, 0, sizeof(out));
            check(Host::DecodeTextureMip(SceneIPC::kTexBGRA5551, px1555,
                                         sizeof(px1555), 2, 1, out),
                  "5551 decoded");
            check(isBgra(out + 0, 255, 255, 255, 255), "5551 top bit set is opaque");
            check(out[7] == 0, "5551 top bit clear is transparent");
        }
        {
            // A4R4G4B4: every nibble expands by 17, so 0xF is 255 and 0x8 is 136.
            const uint8_t px4444[4] = { 0xFF, 0xFF,  0x88, 0x88 };
            memset(out, 0, sizeof(out));
            check(Host::DecodeTextureMip(SceneIPC::kTexBGRA4444, px4444,
                                         sizeof(px4444), 2, 1, out),
                  "4444 decoded");
            check(isBgra(out + 0, 255, 255, 255, 255), "4444 all nibbles set is white");
            check(isBgra(out + 4, 136, 136, 136, 136), "4444 mid nibble is 136");
        }
        {
            const uint8_t l8[2] = { 0, 200 };
            memset(out, 0, sizeof(out));
            check(Host::DecodeTextureMip(SceneIPC::kTexL8, l8, sizeof(l8), 2, 1, out),
                  "L8 decoded");
            check(isBgra(out + 0, 0, 0, 0, 255) && isBgra(out + 4, 200, 200, 200, 255),
                  "L8 is grey and opaque");

            // A8L8 is luminance in the low byte and alpha in the high one, which
            // is the opposite way round from how the name reads.
            const uint8_t a8l8[4] = { 200, 64,  10, 255 };
            memset(out, 0, sizeof(out));
            check(Host::DecodeTextureMip(SceneIPC::kTexA8L8, a8l8, sizeof(a8l8),
                                         2, 1, out),
                  "A8L8 decoded");
            check(isBgra(out + 0, 200, 200, 200, 64) &&
                  isBgra(out + 4, 10, 10, 10, 255),
                  "A8L8 puts luminance in the low byte and alpha in the high one");
        }

        // ── A payload that lies about its size ──────────────────────────────
        // The producer is another process; a short payload must be refused
        // rather than read past.
        {
            const uint8_t truncated[4] = { 0, 0, 0, 0 };
            check(!Host::DecodeTextureMip(SceneIPC::kTexDXT1, truncated,
                                          sizeof(truncated), 4, 4, out),
                  "a payload shorter than its own description is refused");
            check(!Host::DecodeTextureMip(SceneIPC::kTexUnknown, truncated,
                                          sizeof(truncated), 1, 1, out),
                  "an unknown format is refused rather than guessed at");
        }

        printf("\n%s\n", g_failures ? "FAILURES PRESENT" : "ALL CHECKS PASSED");
        return g_failures ? 1 : 0;
    }

    int RunResendTest()
    {
        const char* kSection = "Local\\PESMod.SceneStream.ResendTest";
        printf("-------- resend loop --------\n\n");

        SceneIPC::SharedRing producer;
        if (!producer.CreateAsProducer(kSection, 1ull * 1024ull * 1024ull))
        {
            printf("FAIL: could not create the test section\n");
            return 1;
        }

        const uint64_t kSent    = 0xAAAA000000000001ull;
        const uint64_t kMissing = 0xBBBB000000000002ull;

        // One geometry actually sent...
        {
            struct V { float px, py, pz; };
            V verts[3] = { {0,0,0}, {1,0,0}, {0,1,0} };

            SceneIPC::GeometryDesc gd{};
            gd.geometryId   = kSent;
            gd.vertexKind   = SceneIPC::kVertexPreLit;
            gd.vertexStride = sizeof(V);
            gd.vertexCount  = 3;
            gd.uvOffset     = SceneIPC::kNoVertexAttribute;
            gd.normalOffset = SceneIPC::kNoVertexAttribute;
            gd.colorOffset  = SceneIPC::kNoVertexAttribute;
            producer.TryWrite(SceneIPC::kMsgGeometry, &gd, sizeof(gd),
                              verts, sizeof(verts), false);
        }

        // ...and a frame that draws it twice plus one geometry never sent,
        // twice as well, so the duplicate collapsing is covered too.
        SceneIPC::FrameBegin fb{};
        fb.frameIndex    = 7;
        fb.renderWidth   = 640;
        fb.renderHeight  = 480;
        fb.instanceCount = 4;
        producer.TryWrite(SceneIPC::kMsgFrameBegin, &fb, sizeof(fb), nullptr, 0, true);

        const uint64_t ids[4] = { kSent, kMissing, kSent, kMissing };
        for (int i = 0; i < 4; ++i)
        {
            SceneIPC::InstanceDesc inst{};
            inst.geometryId = ids[i];
            producer.TryWrite(SceneIPC::kMsgInstance, &inst, sizeof(inst),
                              nullptr, 0, true);
        }

        SceneIPC::FrameEnd fe{};
        fe.frameIndex    = 7;
        fe.instanceCount = 4;
        producer.TryWrite(SceneIPC::kMsgFrameEnd, &fe, sizeof(fe), nullptr, 0, true);

        Host::SceneReceiver rx;
        if (!rx.Attach(kSection)) { printf("FAIL: could not attach\n"); return 1; }
        rx.Poll();

        check(rx.CurrentFrame().instances.size() == 4, "4 instances decoded");
        check(rx.FindGeometry(kSent) != nullptr,   "the sent geometry is resident");
        check(rx.FindGeometry(kMissing) == nullptr, "the other one is not");

        // Two instances referenced it, but the request is for the id, once.
        check(rx.Stats().resendRequested == 1,
              "the consumer asked for the missing geometry, and only once "
              "despite two instances wanting it");

        uint64_t wanted[SceneIPC::kMaxResendRequests];
        const uint32_t n = producer.TakeResendRequests(
            wanted, SceneIPC::kMaxResendRequests);
        check(n == 1 && wanted[0] == kMissing,
              "the producer received exactly that id");

        // Taking clears the table, so a request is served once and anything
        // still needed is asked for again next frame.
        const uint32_t again = producer.TakeResendRequests(
            wanted, SceneIPC::kMaxResendRequests);
        check(again == 0, "the table is empty once taken");

        printf("\n%s\n", g_failures == 0 ? "ALL CHECKS PASSED"
                                         : "FAILURES PRESENT");
        return g_failures == 0 ? 0 : 1;
    }

    // Checks the skinning kernel against arithmetic done by hand.
    //
    // Everything else about skinning is visible in the output; whether the
    // palette is applied *correctly* is not - a plausible-looking player in
    // the wrong pose reads the same as a right one. The values here are
    // shaped like the game's: c57.z is 765, three times 255, so a bone index
    // byte of n selects palette row 3n, exactly as `mul r10, v2, c57.z`
    // followed by `m4x3 r11, v0, c0` does.
    int RunSkinTest()
    {
        printf("-------- skinning --------\n\n");

        // A 40-byte vertex, matching the layout the game's players use:
        // position, weights, indices, normal, uv.
        struct SkinVertex
        {
            float   px, py, pz;
            uint8_t weights[4];   // BGRA in memory; x,y,z,w = R,G,B,A
            uint8_t indices[4];
            float   nx, ny, nz;
            float   u, v;
        };
        static_assert(sizeof(SkinVertex) == 40, "test vertex layout");

        Host::Geometry geo;
        memset(&geo.desc, 0, sizeof(geo.desc));
        geo.desc.vertexKind       = SceneIPC::kVertexLit;
        geo.desc.vertexStride     = sizeof(SkinVertex);
        geo.desc.vertexCount      = 2;
        geo.desc.normalOffset     = 20;
        geo.desc.uvOffset         = 32;
        geo.desc.colorOffset      = SceneIPC::kNoVertexAttribute;
        geo.desc.boneCount        = 2;
        geo.desc.boneWeightOffset = 12;
        geo.desc.boneIndexOffset  = 16;

        SkinVertex src[2];
        memset(src, 0, sizeof(src));

        // Vertex 0: entirely bone 0.
        src[0].px = 1.0f; src[0].py = 2.0f; src[0].pz = 3.0f;
        src[0].nx = 0.0f; src[0].ny = 1.0f; src[0].nz = 0.0f;
        src[0].u  = 0.25f; src[0].v = 0.75f;
        src[0].indices[2] = 0;    // x = R = byte 2 -> bone 0
        src[0].indices[1] = 1;    // y = G = byte 1 -> bone 1
        src[0].weights[2] = 255;  // full weight on the first influence
        src[0].weights[1] = 0;

        // Vertex 1: half of each bone.
        src[1] = src[0];
        src[1].weights[2] = 128;
        src[1].weights[1] = 128;

        geo.vertices.assign((const uint8_t*)src,
                            (const uint8_t*)src + sizeof(src));

        // Bone 0 at rows 0..2 translates by (10, 20, 30); bone 1 at rows 3..5
        // scales by 2. Rows are the m4x3 dot products, so row r is the r-th
        // component of the result.
        std::vector<float> palette(6 * 4, 0.0f);
        palette[0*4+0] = 1.0f; palette[0*4+3] = 10.0f;   // x' = x + 10
        palette[1*4+1] = 1.0f; palette[1*4+3] = 20.0f;   // y' = y + 20
        palette[2*4+2] = 1.0f; palette[2*4+3] = 30.0f;   // z' = z + 30
        palette[3*4+0] = 2.0f;
        palette[4*4+1] = 2.0f;
        palette[5*4+2] = 2.0f;

        std::vector<uint8_t> dst(sizeof(src));
        Host::SkinVertices(geo, palette, 765.0f, dst.data());
        const SkinVertex* out = (const SkinVertex*)dst.data();

        // Not called `near`: windows.h defines that as a macro.
        auto close = [](float a, float b) { return fabs(a - b) < 1e-3f; };

        // Bone 0 only: (1,2,3) + (10,20,30).
        check(close(out[0].px, 11.0f) && close(out[0].py, 22.0f) &&
              close(out[0].pz, 33.0f),
              "a single-bone vertex takes that bone's transform");

        // The normal gets the same rows without translation.
        check(close(out[0].nx, 0.0f) && close(out[0].ny, 1.0f) &&
              close(out[0].nz, 0.0f),
              "the normal is rotated but not translated");

        // Half of each: 0.502*(11,22,33) + 0.502*(2,4,6). The weights are
        // bytes over 255, so 128 is 0.50196, not 0.5 - and the shader does
        // not renormalise, so neither does this.
        const float w = 128.0f / 255.0f;
        check(close(out[1].px, w * 11.0f + w * 2.0f) &&
              close(out[1].py, w * 22.0f + w * 4.0f) &&
              close(out[1].pz, w * 33.0f + w * 6.0f),
              "a two-bone vertex blends both, unnormalised as the shader does");

        // Everything the pose does not touch has to survive intact.
        check(out[1].u == src[1].u && out[1].v == src[1].v,
              "texture coordinates pass through untouched");
        check(out[1].indices[2] == src[1].indices[2] &&
              out[1].weights[2] == src[1].weights[2],
              "bone data passes through untouched");

        printf("\n%s\n", g_failures == 0 ? "ALL CHECKS PASSED"
                                         : "FAILURES PRESENT");
        return g_failures == 0 ? 0 : 1;
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

            // The normals differ at every vertex, and deliberately disagree
            // with the geometry: the first quad lies in the z = 0 plane, so
            // its triangles' own normals point along z while these point
            // mostly along y. That is what makes the shading check below
            // able to tell the two apart. All of them keep a positive y so
            // the mesh still faces the light and stays lit.
            LitVertex verts[6] = {
                {  0,  0,  0,  0.00f,  1.00f,  0.00f, 0,0 },
                {  1,  0,  0, -0.71f, -0.71f,  0.00f, 1,0 },
                {  1,  1,  0,  0.00f, -0.71f,  0.71f, 1,1 },
                {  0,  1,  0, -0.71f,  0.00f,  0.71f, 0,1 },
                {  0,  0,  1,  0.71f, -0.71f,  0.00f, 0,0 },
                {  1,  0,  1,  0.00f,  0.71f,  0.71f, 1,0 },
            };
            uint16_t idx[12] = { 0,1,2,  0,2,3,  0,1,4,  1,5,4 };

            SceneIPC::GeometryDesc gd{};
            gd.geometryId   = 0x1111111100000001ull;
            gd.vertexKind   = SceneIPC::kVertexLit;
            gd.vertexStride = sizeof(LitVertex);
            gd.vertexCount  = 6;
            gd.indexCount   = 12;
            gd.indexStride  = 2;
            // The layout the producer would have decoded from the game.
            gd.normalOffset = 12;
            gd.uvOffset     = 24;
            gd.colorOffset  = SceneIPC::kNoVertexAttribute;
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
            // The layout the producer would have decoded from the game.
            gd.normalOffset = 12;
            gd.uvOffset     = 24;
            gd.colorOffset  = SceneIPC::kNoVertexAttribute;
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
            // Every UV is the same point, so the outcome does not depend on
            // where in the triangle the ray landed. The centre rather than a
            // corner: this texture has one opaque texel in a corner, and
            // bilinear filtering with wrap addressing bleeds it across the
            // seam into any UV within half a texel of an edge.
            LitVertex verts[6] = {
                { 0,0,0, 0,0,1, 0.5f,0.5f }, { 4,0,0, 0,0,1, 0.5f,0.5f },
                { 4,4,0, 0,0,1, 0.5f,0.5f }, { 0,4,0, 0,0,1, 0.5f,0.5f },
                { 0,0,1, 0,0,1, 0.5f,0.5f }, { 4,0,1, 0,0,1, 0.5f,0.5f },
            };
            uint16_t idx[12] = { 0,1,2,  0,2,3,  0,1,4,  1,5,4 };

            SceneIPC::GeometryDesc gd{};
            gd.geometryId   = 0x3333333300000004ull;
            gd.vertexKind   = SceneIPC::kVertexLit;
            gd.vertexStride = sizeof(LitVertex);
            gd.vertexCount  = 6;
            gd.indexCount   = 12;
            gd.indexStride  = 2;
            // The layout the producer would have decoded from the game.
            gd.normalOffset = 12;
            gd.uvOffset     = 24;
            gd.colorOffset  = SceneIPC::kNoVertexAttribute;
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
            // One opaque texel, in the corner the sheet never samples.
            // Without it the whole channel is empty, and the cache now reads
            // that as "this format carries no alpha" and forces it opaque -
            // correctly, because that is what the game's own textures mean by
            // it. A texture that is transparent *somewhere* is the realistic
            // case and the one worth testing.
            texels[(td.payloadBytes - 4) + 3] = 255;
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

        // A skinned mesh, drawn twice below. Two instances of one skinned
        // geometry cannot share a structure - each has its own pose - and
        // when they did, both queued a build into the same destination in one
        // command buffer. That is undefined, and it lost the device.
        const uint64_t kSkinnedGeometryId = 0x4444444400000006ull;
        {
            struct SkinVertex
            {
                float   px, py, pz;
                uint8_t weights[4];
                uint8_t indices[4];
                float   nx, ny, nz;
                float   u, v;
            };
            SkinVertex verts[6];
            memset(verts, 0, sizeof(verts));
            const float xs[6] = { 0, 1, 1, 0, 0, 1 };
            const float ys[6] = { 0, 0, 1, 1, 0, 0 };
            const float zs[6] = { 0, 0, 0, 0, 1, 1 };
            for (int k = 0; k < 6; ++k)
            {
                verts[k].px = xs[k]; verts[k].py = ys[k]; verts[k].pz = zs[k];
                verts[k].ny = 1.0f;
                verts[k].weights[2] = 255;   // all weight on the first bone
                verts[k].indices[2] = 0;
            }
            uint16_t idx[12] = { 0,1,2,  0,2,3,  0,1,4,  1,5,4 };

            SceneIPC::GeometryDesc gd{};
            gd.geometryId       = kSkinnedGeometryId;
            gd.vertexKind       = SceneIPC::kVertexLit;
            gd.vertexStride     = sizeof(SkinVertex);
            gd.vertexCount      = 6;
            gd.indexCount       = 12;
            gd.indexStride      = 2;
            gd.contentHash      = 0x51C0DE01u;
            gd.normalOffset     = 20;
            gd.uvOffset         = 32;
            gd.colorOffset      = SceneIPC::kNoVertexAttribute;
            gd.boneCount        = 1;
            gd.boneWeightOffset = 12;
            gd.boneIndexOffset  = 16;

            std::vector<uint8_t> payload(sizeof(verts) + sizeof(idx));
            memcpy(payload.data(), verts, sizeof(verts));
            memcpy(payload.data() + sizeof(verts), idx, sizeof(idx));
            producer.TryWrite(SceneIPC::kMsgGeometry, &gd, sizeof(gd),
                              payload.data(), (uint32_t)payload.size(), false);
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
        fb.instanceCount = 6;
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

        // Two instances of the one skinned geometry, posed far apart and well
        // off screen so the pixel checks below are unaffected. What matters
        // here is that they get a structure each.
        for (int k = 0; k < 2; ++k)
        {
            Host::Math::Mat4 world = Host::Math::Identity();

            SceneIPC::InstanceDesc inst{};
            inst.geometryId    = kSkinnedGeometryId;
            inst.clipTransform = Host::Math::Multiply(world, trueVp);
            inst.baseColorFactor[0] = inst.baseColorFactor[1] =
            inst.baseColorFactor[2] = inst.baseColorFactor[3] = 1.0f;
            inst.paletteRegisters = 3;
            inst.boneIndexScale   = 765.0f;

            // Bone 0 as a pure translation, different for each instance.
            float palette[12] = {0};
            palette[0] = 1.0f; palette[3]  = 1000.0f * (k + 1);
            palette[5] = 1.0f; palette[7]  = 0.0f;
            palette[10] = 1.0f; palette[11] = 0.0f;

            producer.TryWrite(SceneIPC::kMsgInstance, &inst, sizeof(inst),
                              palette, sizeof(palette), true);
        }

        SceneIPC::FrameEnd fe{};
        fe.frameIndex    = 1;
        fe.instanceCount = 6;
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

        check(f.instances.size() == 6,      "6 instances decoded");
        check(rx.GeometryCount() == 4,      "4 geometries resident");
        check(st.geometryUnresolved == 0,   "all instances resolved their geometry");
        check(st.transformsRejected == 0,   "all world transforms affine");
        check(st.persistentBlas == 4,
              "two static meshes plus one structure per skinned instance");
        check(st.skinnedRebuilds == 2,
              "both skinned instances were posed and rebuilt");
        check(st.duplicateBuildsDropped == 0,
              "no two build jobs targeted the same structure");
        check(st.spriteInstances == 2,      "both quads folded into the sprite batch");
        check(st.spriteTriangles == 4,      "sprite batch holds 4 triangles");
        check(st.tlasInstances == 5,        "TLAS = 4 meshes + 1 merged sprite instance");
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
            ++g_failures;
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
            const float scl[4] = { 1.0f, 1.0f, 1.0f, 1.0f };   // c69
            memcpy(u.lightingScale, scl, sizeof(scl));
            u.params[0] = 1000.0f;   // shadow ray length
            u.params[1] = 1.0f;      // exposure
            u.decal[0]  = Host::kDecalBias;
            u.decal[1]  = Host::kResumeFraction;

            if (!tracer.Trace(accel.Tlas(), u, &textures,
                              &accel.InstanceRecords(),
                              &accel.SpriteTriangles()))
            {
                printf("  [FAIL] trace: %s\n", tracer.LastError().c_str());
                ++g_failures;
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

                            // A tolerance, not equality: compositing a
                            // surface of negligible alpha shifts a pixel by
                            // a single quantisation step, which is correct
                            // behaviour and should not read as coverage.
                            if (abs((int)px[i]   - (int)bg[0]) <= 2 &&
                                abs((int)px[i+1] - (int)bg[1]) <= 2 &&
                                abs((int)px[i+2] - (int)bg[2]) <= 2) continue;
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

                    // ── Are the mesh's own normals being used? ────────
                    //
                    // Inside one triangle the texture is a single colour and
                    // the surface is flat, so the only thing that can vary
                    // from pixel to pixel is the normal. A normal taken from
                    // the triangle is constant across it and gives
                    // byte-identical neighbours; the mesh's own normals
                    // differ at every vertex and give a gradient.
                    //
                    // Only textured pixels are counted, because the merged
                    // sprite batch has no normals by design and covers most
                    // of the frame.
                    size_t litPairs = 0, gradientPairs = 0;
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x + 1 < w; ++x)
                        {
                            const size_t i = ((size_t)y * w + x) * 3;
                            const size_t j = i + 3;

                            bool bothTextured = true;
                            for (const size_t k : { i, j })
                            {
                                const double r = pow(px[k + 0] / 255.0, 2.2);
                                const double b = pow(px[k + 2] / 255.0, 2.2);
                                const double ratio = (r > 1e-6) ? b / r : 0.0;
                                if (!(ratio > want * 0.85 && ratio < want * 1.15))
                                    bothTextured = false;
                            }
                            if (!bothTextured) continue;
                            ++litPairs;

                            // A gradient, not an edge: a large jump is a
                            // silhouette or a different instance, and only a
                            // shading ramp is evidence about normals.
                            int biggest = 0;
                            for (int c = 0; c < 3; ++c)
                                biggest = std::max(biggest,
                                    abs((int)px[i + c] - (int)px[j + c]));
                            if (biggest >= 1 && biggest <= 24) ++gradientPairs;
                        }

                    // Measured: 14% with the mesh's normals, and exactly
                    // 0 of 2204 with them switched off - not "close to
                    // zero", but every neighbouring pair byte-identical,
                    // which is what a constant normal over a flat triangle
                    // of one colour has to produce. The threshold sits well
                    // clear of both.
                    check(litPairs > 500 && gradientPairs * 20 > litPairs,
                          "shading follows the mesh's own normals, not the "
                          "triangle's");
                    printf("  %zu of %zu neighbouring textured pixels are on "
                           "a shading gradient (%.0f%%)\n",
                           gradientPairs, litPairs,
                           litPairs ? 100.0 * gradientPairs / litPairs : 0.0);

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

                    // ── Sky occlusion ────────────────────────────────────
                    //
                    // Traced twice, because the thing worth asserting is a
                    // relation between the two images rather than any
                    // absolute value: occlusion can only ever take light
                    // away. A brighter pixel means the hemisphere basis is
                    // wrong, or the visibility is inverted, or the rays are
                    // being fired into the surface - none of which an
                    // absolute threshold would catch, and all of which this
                    // does.
                    Host::SceneUniforms ao = u;
                    ao.params[2] = 8.0f;
                    ao.params[3] = 2.0f;   // the test scene is a unit cube

                    if (!tracer.Trace(accel.Tlas(), ao, &textures,
                                      &accel.InstanceRecords(),
                                      &accel.SpriteTriangles()))
                    {
                        check(false, "the occluded trace completed");
                    }
                    else if (!tracer.SaveImage("astest_ao.ppm"))
                    {
                        check(false, "occluded image written");
                    }
                    else
                    {
                        FILE* aoImg = nullptr;
                        fopen_s(&aoImg, "astest_ao.ppm", "rb");
                        int aw = 0, ah = 0, amax = 0;
                        std::vector<uint8_t> apx;
                        if (aoImg)
                        {
                            fscanf_s(aoImg, "P6 %d %d %d", &aw, &ah, &amax);
                            fgetc(aoImg);
                            apx.resize((size_t)aw * ah * 3);
                            fread(apx.data(), 1, apx.size(), aoImg);
                            fclose(aoImg);
                        }

                        check(aw == w && ah == h && apx.size() == px.size(),
                              "the occluded image is the same size");

                        if (apx.size() == px.size())
                        {
                            size_t darker = 0, brighter = 0;
                            for (size_t i = 0; i < px.size(); ++i)
                            {
                                // One level of slack for the gamma encode
                                // rounding differently either side of a
                                // value that did not really change.
                                if ((int)apx[i] + 1 < (int)px[i]) ++darker;
                                if ((int)apx[i] > (int)px[i] + 1) ++brighter;
                            }
                            check(brighter == 0,
                                  "sky occlusion only ever removes light");
                            // Against what the scene actually covers, not
                            // the frame: the test geometry is 3.5% of it.
                            // Measured, 734 of the 6,882 covered channels
                            // darken - about a tenth. The same check with
                            // the ray offset sized for the game's units
                            // rather than the scene's gave 6, because every
                            // ray then started outside the geometry, so the
                            // threshold has to sit between those and does.
                            check(darker * 20 > differing * 3,
                                  "sky occlusion actually darkened the scene");
                            printf("  occlusion darkened %zu of %zu channels, "
                                   "brightened %zu\n",
                                   darker, px.size(), brighter);
                        }
                    }
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
        printf("%s\n", g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
        return g_failures == 0 ? 0 : 1;
    }

    void ReportAccel(const Host::AccelStats& st)
    {
        printf("    AS: %u BLAS (%u rebuilt) | %u sprites -> %u tris merged | "
               "TLAS %u inst | rejected %u | overlay %u (sky %u) | %.2f ms\n",
               st.persistentBlas, st.blasBuiltThisFrame,
               st.spriteInstances, st.spriteTriangles,
               st.tlasInstances, st.transformsRejected, st.nonOccluding,
               st.skyDraws, st.buildMilliseconds);
        if (st.duplicateBuildsDropped)
            printf("    WARNING: %u build jobs dropped for targeting a "
                   "structure another job in the same batch already had\n",
                   st.duplicateBuildsDropped);
        if (st.skinnedRebuilds)
            printf("    skinned: %u structures rebuilt for a new pose\n",
                   st.skinnedRebuilds);
        if (st.blasResized)
            printf("    resized: %u structures recreated because the "
                   "driver asked for more room than they were made with\n",
                   st.blasResized);
    }

    void ReportSummary(const Host::SceneReceiver& rx,
                       const Host::TextureCache& textures)
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

        // Always, not only when non-zero. A texture the host never managed
        // to make resident renders as white, and on screen that looks the
        // same whatever the reason - so the reasons are printed even when
        // there are none, and a run with a missing texture says so itself.
        const Host::TextureStats& ts = textures.Stats();
        printf("  textures resident    %u of %u slots, %.1f MB\n",
               ts.resident, textures.Capacity(),
               ts.bytesResident / (1024.0 * 1024.0));
        printf("  textures not shown   %u undecodable, %u past capacity\n",
               ts.skippedFormat, ts.skippedFull);
        printf("  textures adjusted    %u X8 forced opaque, "
               "%u fully transparent\n",
               ts.opaqueForced, ts.fullyTransparent);
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
    bool        skinTest = false;
    bool        resendTest = false;
    bool        replayTest = false;
    bool        decodeTest = false;
    bool        validation = true;
    // The traced image is the output now, so a window is the default.
    // Batch work over a recording still wants no window at all.
    bool        headless = false;

    // Sky occlusion. Eight rather than four: stratified, eight measured
    // cleaner than the surface's own texture grain (7.7 against 8.0 with
    // occlusion off) while four was still visibly speckled at 8.5 - and
    // eight costs 0.6 ms more than four, not double, because stratified rays
    // stay coherent.
    uint32_t    aoSamples = 8;
    uint32_t    debugView = 0;
    uint32_t    debugSkip = 0;
    float       aoReach   = 150.0f;   // ~1.5 m, at 100 units to the metre
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
    const char* recordPath = nullptr;
    const char* replayPath = nullptr;
    // Frames to drain without building or tracing. A recording usually
    // opens on menus, and skipping to the part being investigated keeps
    // the edit-run-look loop to a few seconds.
    uint64_t    skipFrames = 0;
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
        else if (!strcmp(argv[i], "--skintest")) skinTest = true;
        else if (!strcmp(argv[i], "--resendtest")) resendTest = true;
        else if (!strcmp(argv[i], "--replaytest")) replayTest = true;
        else if (!strcmp(argv[i], "--dectest")) decodeTest = true;
        else if (!strcmp(argv[i], "--debug-view") && i + 1 < argc)
            debugView = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--debug-skip") && i + 1 < argc)
            debugSkip = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) recordPath = argv[++i];
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replayPath = argv[++i];
        else if (!strcmp(argv[i], "--skip") && i + 1 < argc)
            skipFrames = strtoull(argv[++i], nullptr, 10);
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
        else if (!strcmp(argv[i], "--ao") && i + 1 < argc)
            aoSamples = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--ao-reach") && i + 1 < argc)
            aoReach = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--no-validation")) validation = false;
        else if (!strcmp(argv[i], "--help")) { PrintUsage(); return 0; }
        else { printf("unknown argument: %s\n\n", argv[i]); PrintUsage(); return 2; }
    }

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // Needs no device, so it runs before one is created.
    if (decodeTest) return RunDecodeTest();
    if (resendTest) return RunResendTest();
    if (replayTest) return RunReplayTest();

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
    if (skinTest)  return RunSkinTest();
    if (asTest)    return RunAccelSelfTest(gpu, shaderDir);

    if (replayPath) printf("\nreplaying '%s'\n", replayPath);
    else            printf("\nattaching to '%s'\n", section);

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

    // The window cannot be sized until the first frame says what aspect
    // ratio the game is rendering at, so it comes up with the tracer.
    Host::Presenter presenter;
    bool presenterUp = false;

    Host::SceneReceiver rx;

    // A recording drives the identical pipeline with no game running,
    // which is the only way to test a fix against the frame that broke.
    if (replayPath)
    {
        if (!rx.StartReplay(replayPath))
        {
            printf("cannot open recording: %s\n", replayPath);
            return 1;
        }
        printf("replaying.\n\n");
    }
    else
    {
        // The game may not be running yet, and may be restarted while the
        // host stays up, so attaching is a retry loop rather than a one-shot.
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

        if (recordPath && !rx.StartRecording(recordPath))
            printf("WARNING: cannot write recording to %s\n", recordPath);
    }

    uint64_t reported = 0, skipped = 0;
    while (!g_quit)
    {
        // Before the poll, and unconditionally: a window that is not
        // pumped while the host waits for a producer stops responding,
        // which Windows greys out and reports to the user as a hang.
        if (presenterUp)
        {
            presenter.PumpMessages();
            if (!presenter.IsOpen()) break;   // closing the window quits
        }

        if (!rx.Poll())
        {
            // A replay that produced no frame has reached the end of
            // the file; nothing more is coming.
            if (rx.IsReplaying()) break;
            Sleep(1);
            continue;
        }

        // Textures upload while skipping too. Draining the messages alone
        // is not enough: the receiver would hold them but the GPU cache
        // would still be empty, and the first rendered frame would sample
        // white for everything - which looks exactly like a texture bug.
        if (skipped < skipFrames)
        {
            ++skipped;
            textures.Sync(rx, textureBudget);
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
            {
                printf("ray tracer ready: %ux%u (game renders %ux%u)\n",
                       w, traceHeight, srcW, srcH);

                // Same size as the traced image, so the first frame is
                // shown 1:1 and any later stretch is the user resizing.
                if (!headless)
                {
                    if (presenter.Init(&gpu, w, traceHeight,
                                       "PESMod - ray traced"))
                    {
                        presenterUp = true;
                        printf("window open; close it to stop the host\n");
                    }
                    else
                    {
                        // Not fatal. Tracing and saving frames still
                        // work, and the reason is more useful than a
                        // dead process.
                        printf("WARNING: no window - %s\n",
                               presenter.LastError().c_str());
                    }
                }
                printf("\n");
            }
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
            memcpy(u.lightingScale,  f.lighting.lightingScale,   sizeof(u.lightingScale));
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
                u.lightingScale[0] = u.lightingScale[1] = u.lightingScale[2] = 1.0f;
            }
            u.params[0] = 20000.0f;   // shadow ray length, in the game's units
            u.params[1] = 1.0f;
            u.params[2] = (float)aoSamples;
            u.params[3] = aoReach;
            u.decal[0]  = Host::kDecalBias;
            u.decal[1]  = Host::kResumeFraction;
            u.debug[0]  = (float)debugView;
            u.debug[1]  = (float)debugSkip;

            if (tracer.Trace(accel.Tlas(), u, &textures,
                             &accel.InstanceRecords(),
                             &accel.SpriteTriangles()))
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
                // Onto the screen. The blit reads the traced image in
                // VK_IMAGE_LAYOUT_GENERAL and puts it back that way, so
                // the next Trace finds it as it left it.
                if (presenterUp && !presenter.Present(tracer.OutputImage(),
                                                      tracer.Width(),
                                                      tracer.Height()))
                {
                    // A lost or out-of-date swapchain is rebuilt inside
                    // Present, so reaching here means something the
                    // presenter could not handle. Say it once and carry
                    // on tracing rather than tearing the host down.
                    printf("    present failed: %s\n",
                           presenter.LastError().c_str());
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
            if (ts.uploadedThisFrame || ts.skippedFormat || ts.skippedFull ||
                ts.fullyTransparent)
                printf("    tex: %u resident (+%u this frame), %.1f MB | "
                       "X8 forced opaque %u | undecodable %u | past capacity %u"
                       " | all-transparent %u\n",
                       ts.resident, ts.uploadedThisFrame,
                       ts.bytesResident / (1024.0 * 1024.0),
                       ts.opaqueForced, ts.skippedFormat, ts.skippedFull,
                       ts.fullyTransparent);
        }
        ++reported;
        if (maxFrames && reported >= maxFrames) break;
    }

    ReportSummary(rx, textures);

    // Explicit, and in reverse order of creation: the presenter holds a
    // swapchain whose images the tracer's last blit may still be reading,
    // the tracer holds descriptors naming the builder TLAS, and both draw
    // memory from the allocator.
    presenter.Shutdown();
    tracer.Shutdown();
    textures.Shutdown();
    accel.Shutdown();
    alloc.Shutdown();
    return 0;
}
