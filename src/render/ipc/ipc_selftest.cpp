// ipc_selftest.cpp
//
// Verifies that the scene transport is byte-compatible across the 32/64-bit
// boundary. This matters more than a normal unit test: the producer is a
// 32-bit DLL inside the game and the consumer is a 64-bit exe, so a layout
// mistake would not be a crash but silently wrong geometry.
//
// Build it both ways and run:
//
//     ipc_selftest64.exe consume     (starts, waits for the producer)
//     ipc_selftest32.exe produce
//
// The consumer checks every field against the same expected values the
// producer wrote, including 64-bit ids with their high dword set — the
// specific thing that would be truncated if a field were declared as a
// pointer or a bare `long` somewhere.
//
//     ipc_selftest.exe layout        prints sizes/offsets for eyeball diffing
#include "shared_ring.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace SceneIPC;

namespace
{
    const char* kTestSection = "Local\\PESMod.SceneStream.SelfTest";

    // Deliberately has its high dword set: a 64-bit id passed through
    // anything 32-bit-sized would come back truncated.
    const uint64_t kTestGeometryId = 0xDEADBEEF12345678ull;
    const uint64_t kTestTextureId  = 0xFEEDFACE87654321ull;
    const uint32_t kTestVertexCount = 1234;
    const uint32_t kTestIndexCount  = 4321;
    const uint64_t kTestFrameIndex  = 0x00000001FFFFFFFFull;

    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++g_failures;
    }

    void PrintLayout()
    {
        printf("pointer size = %d bytes (%s)\n\n",
               (int)sizeof(void*), sizeof(void*) == 4 ? "32-BIT" : "64-BIT");
        printf("%-16s %6s  %s\n", "struct", "size", "key offsets");
        printf("%-16s %6zu\n", "MessageHeader", sizeof(MessageHeader));
        printf("%-16s %6zu  vertexKind=%zu vertexCount=%zu\n", "GeometryDesc",
               sizeof(GeometryDesc), offsetof(GeometryDesc, vertexKind),
               offsetof(GeometryDesc, vertexCount));
        printf("%-16s %6zu  format=%zu payloadBytes=%zu\n", "TextureDesc",
               sizeof(TextureDesc), offsetof(TextureDesc, format),
               offsetof(TextureDesc, payloadBytes));
        printf("%-16s %6zu  view=%zu projection=%zu\n", "FrameBegin",
               sizeof(FrameBegin), offsetof(FrameBegin, view),
               offsetof(FrameBegin, projection));
        printf("%-16s %6zu  worldTransform=%zu flags=%zu\n", "InstanceDesc",
               sizeof(InstanceDesc), offsetof(InstanceDesc, worldTransform),
               offsetof(InstanceDesc, flags));
        printf("%-16s %6zu\n", "LightingDesc", sizeof(LightingDesc));
        printf("%-16s %6zu  writeCursor=%zu readCursor=%zu\n", "RingHeader",
               sizeof(RingHeader), offsetof(RingHeader, writeCursor),
               offsetof(RingHeader, readCursor));
    }

    int RunProducer()
    {
        SharedRing ring;
        if (!ring.CreateAsProducer(kTestSection, 4ull * 1024ull * 1024ull))
        {
            printf("producer: CreateAsProducer failed (err=%lu)\n", GetLastError());
            return 1;
        }
        printf("producer: ring created (%d-bit), writing test stream\n",
               (int)sizeof(void*) * 8);

        // Geometry, with a payload whose bytes are a known function of index.
        GeometryDesc geo;
        memset(&geo, 0, sizeof(geo));
        geo.geometryId   = kTestGeometryId;
        geo.vertexKind   = kVertexLit;
        geo.vertexStride = 32;
        geo.vertexCount  = kTestVertexCount;
        geo.indexCount   = kTestIndexCount;
        geo.indexStride  = 2;
        geo.contentHash  = 0xA5A5A5A5u;

        const uint32_t vbBytes = geo.vertexCount * geo.vertexStride;
        const uint32_t ibBytes = geo.indexCount * geo.indexStride;
        uint8_t* payload = (uint8_t*)malloc(vbBytes + ibBytes);
        for (uint32_t i = 0; i < vbBytes + ibBytes; ++i)
            payload[i] = (uint8_t)((i * 31u + 7u) & 0xFF);

        if (!ring.TryWrite(kMsgGeometry, &geo, sizeof(geo), payload,
                           vbBytes + ibBytes, false))
            printf("producer: geometry write FAILED\n");

        // Texture.
        TextureDesc tex;
        memset(&tex, 0, sizeof(tex));
        tex.textureId    = kTestTextureId;
        tex.format       = kTexBGRA8;
        tex.width        = 128;
        tex.height       = 64;
        tex.mipLevels    = 1;
        tex.payloadBytes = 128 * 64 * 4;
        uint8_t* pixels = (uint8_t*)malloc(tex.payloadBytes);
        for (uint32_t i = 0; i < tex.payloadBytes; ++i)
            pixels[i] = (uint8_t)(i & 0xFF);
        if (!ring.TryWrite(kMsgTexture, &tex, sizeof(tex), pixels,
                           tex.payloadBytes, false))
            printf("producer: texture write FAILED\n");

        // Lighting, using the real constants read from the game's shaders.
        LightingDesc light;
        memset(&light, 0, sizeof(light));
        const float dir[4] = { -0.4968f, -0.7360f, 0.4600f, 1.0f };
        memcpy(light.directionalDir, dir, sizeof(dir));
        const float col[4] = { 0.97f, 0.97f, 0.97f, 0.0f };
        memcpy(light.directionalColor, col, sizeof(col));
        ring.TryWrite(kMsgLighting, &light, sizeof(light), nullptr, 0, false);

        // Frame with one instance.
        FrameBegin fb;
        memset(&fb, 0, sizeof(fb));
        fb.frameIndex    = kTestFrameIndex;
        fb.renderWidth   = 2560;
        fb.renderHeight  = 1600;
        fb.instanceCount = 1;
        for (int i = 0; i < 16; ++i) fb.view.m[i]       = (float)i;
        for (int i = 0; i < 16; ++i) fb.projection.m[i] = (float)(100 + i);
        ring.TryWrite(kMsgFrameBegin, &fb, sizeof(fb), nullptr, 0, true);

        InstanceDesc inst;
        memset(&inst, 0, sizeof(inst));
        inst.geometryId    = kTestGeometryId;
        inst.baseTextureId = kTestTextureId;
        inst.flags         = kInstanceAlphaTest | kInstanceTwoSided;
        for (int i = 0; i < 16; ++i) inst.worldTransform.m[i] = (float)(i * 2);
        inst.baseColorFactor[0] = 1.0f; inst.baseColorFactor[3] = 0.5f;
        ring.TryWrite(kMsgInstance, &inst, sizeof(inst), nullptr, 0, true);

        FrameEnd fe;
        memset(&fe, 0, sizeof(fe));
        fe.frameIndex    = kTestFrameIndex;
        fe.instanceCount = 1;
        ring.TryWrite(kMsgFrameEnd, &fe, sizeof(fe), nullptr, 0, true);

        free(payload);
        free(pixels);

        printf("producer: wrote %llu bytes; waiting for consumer to drain\n",
               (unsigned long long)ring.PendingBytes());
        for (int i = 0; i < 200 && ring.PendingBytes() > 0; ++i)
            Sleep(25);
        printf("producer: %llu bytes left, exiting\n",
               (unsigned long long)ring.PendingBytes());
        return 0;
    }

    int RunConsumer()
    {
        SharedRing ring;
        printf("consumer (%d-bit): waiting for producer\n",
               (int)sizeof(void*) * 8);
        for (int i = 0; i < 400 && !ring.IsOpen(); ++i)
        {
            if (ring.OpenAsConsumer(kTestSection)) break;
            Sleep(25);
        }
        if (!ring.IsOpen())
        {
            printf("consumer: never saw the section\n");
            return 1;
        }
        printf("consumer: attached\n\n");

        const uint32_t bufBytes = 8u * 1024u * 1024u;
        uint8_t* buf = (uint8_t*)malloc(bufBytes);
        int seen = 0;
        const int expected = 6;   // geometry, texture, lighting,
                                  // frameBegin, instance, frameEnd

        for (int spins = 0; spins < 400 && seen < expected; )
        {
            uint32_t msgBytes = 0, needed = 0;
            if (!ring.TryRead(buf, bufBytes, &msgBytes, &needed))
            {
                if (needed) { printf("consumer: message too big (%u)\n", needed); break; }
                Sleep(25); ++spins; continue;
            }

            const MessageHeader* mh = (const MessageHeader*)buf;
            const uint8_t* body = buf + sizeof(MessageHeader);
            ++seen;

            switch (mh->type)
            {
            case kMsgGeometry:
            {
                const GeometryDesc* g = (const GeometryDesc*)body;
                printf("kMsgGeometry:\n");
                Check(g->geometryId == kTestGeometryId,
                      "64-bit geometryId survived (high dword intact)");
                Check(g->vertexCount == kTestVertexCount, "vertexCount");
                Check(g->indexCount == kTestIndexCount, "indexCount");
                Check(g->vertexKind == kVertexLit, "vertexKind");
                Check(g->contentHash == 0xA5A5A5A5u, "contentHash");
                const uint8_t* p = body + sizeof(GeometryDesc);
                const uint32_t n = g->vertexCount * g->vertexStride +
                                   g->indexCount * g->indexStride;
                bool payloadOk = true;
                for (uint32_t i = 0; i < n && payloadOk; ++i)
                    payloadOk = (p[i] == (uint8_t)((i * 31u + 7u) & 0xFF));
                Check(payloadOk, "vertex+index payload byte-exact");
                break;
            }
            case kMsgTexture:
            {
                const TextureDesc* t = (const TextureDesc*)body;
                printf("kMsgTexture:\n");
                Check(t->textureId == kTestTextureId, "64-bit textureId survived");
                Check(t->width == 128 && t->height == 64, "dimensions");
                Check(t->payloadBytes == 128u * 64u * 4u, "payloadBytes");
                const uint8_t* p = body + sizeof(TextureDesc);
                bool ok = true;
                for (uint32_t i = 0; i < t->payloadBytes && ok; ++i)
                    ok = (p[i] == (uint8_t)(i & 0xFF));
                Check(ok, "pixel payload byte-exact");
                break;
            }
            case kMsgLighting:
            {
                const LightingDesc* l = (const LightingDesc*)body;
                printf("kMsgLighting:\n");
                Check(l->directionalDir[0] > -0.4969f && l->directionalDir[0] < -0.4967f,
                      "float survived (light dir x == -0.4968)");
                Check(l->directionalColor[1] > 0.969f && l->directionalColor[1] < 0.971f,
                      "light colour g == 0.97");
                break;
            }
            case kMsgFrameBegin:
            {
                const FrameBegin* f = (const FrameBegin*)body;
                printf("kMsgFrameBegin:\n");
                Check(f->frameIndex == kTestFrameIndex,
                      "64-bit frameIndex survived");
                Check(f->renderWidth == 2560 && f->renderHeight == 1600,
                      "render size");
                bool m = true;
                for (int i = 0; i < 16; ++i) if (f->view.m[i] != (float)i) m = false;
                Check(m, "view matrix intact");
                m = true;
                for (int i = 0; i < 16; ++i)
                    if (f->projection.m[i] != (float)(100 + i)) m = false;
                Check(m, "projection matrix intact (offset 80 not shifted)");
                break;
            }
            case kMsgInstance:
            {
                const InstanceDesc* in = (const InstanceDesc*)body;
                printf("kMsgInstance:\n");
                Check(in->geometryId == kTestGeometryId, "instance geometryId");
                Check(in->baseTextureId == kTestTextureId, "instance textureId");
                Check(in->flags == (uint32_t)(kInstanceAlphaTest | kInstanceTwoSided),
                      "flags at offset 104 not shifted");
                bool m = true;
                for (int i = 0; i < 16; ++i)
                    if (in->worldTransform.m[i] != (float)(i * 2)) m = false;
                Check(m, "world transform intact");
                break;
            }
            case kMsgFrameEnd:
            {
                const FrameEnd* f = (const FrameEnd*)body;
                printf("kMsgFrameEnd:\n");
                Check(f->frameIndex == kTestFrameIndex, "frameIndex");
                Check(f->instanceCount == 1, "instanceCount");
                break;
            }
            default:
                printf("unexpected message type %u\n", mh->type);
                ++g_failures;
                break;
            }
        }

        free(buf);
        printf("\nmessages received: %d of %d expected\n", seen, expected);
        if (seen != expected) ++g_failures;
        printf("%s\n", g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
        return g_failures == 0 ? 0 : 1;
    }
}

int main(int argc, char** argv)
{
    const char* mode = (argc > 1) ? argv[1] : "layout";
    if (!strcmp(mode, "produce")) return RunProducer();
    if (!strcmp(mode, "consume")) return RunConsumer();
    PrintLayout();
    return 0;
}
