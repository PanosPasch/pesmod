// frame_capture.h
//
// The analysis half of the interception layer. Two things happen here:
//
//  1. Running statistics, every frame, cheap. Draw counts split by 2D vs 3D,
//     triangle totals, the set of vertex formats in use, state-change churn.
//     This is what answers "what does this engine actually do per frame".
//
//  2. A deep single-frame dump, on demand (hotkey). Every draw call in the
//     frame is written out with its full pipeline state, and every vertex
//     buffer, index buffer and texture it referenced is dumped to disk. This
//     is the ground truth the Vulkan scene builder will be written against.
//
// The 2D/3D split is the load-bearing classification: a draw whose FVF is
// D3DFVF_XYZRHW is pre-transformed screen space (HUD, menus, the fade quad at
// FUN_0087be70) and stays on the game's own path, while everything else is
// real world-space geometry the ray tracer takes ownership of.
#pragma once

#include "../d3d8/d3d8_min.h"
#include "state_tracker.h"
#include <cstdint>

namespace Capture
{
    // Describes one draw call as it was issued, independent of pipeline state.
    struct DrawCallInfo
    {
        const char*      apiName;         // "DrawIndexedPrimitive", ...
        D3DPRIMITIVETYPE primitiveType;
        uint32_t         primitiveCount;

        bool             indexed;
        bool             userPointer;     // the *UP variants

        // Indexed draws
        uint32_t         minIndex;
        uint32_t         numVertices;
        uint32_t         startIndex;

        // Non-indexed draws
        uint32_t         startVertex;

        // User-pointer draws carry their data inline rather than in a buffer.
        const void*      upVertexData;
        uint32_t         upVertexStride;
        const void*      upIndexData;
        D3DFORMAT        upIndexFormat;
    };

    struct FrameStats
    {
        uint32_t frameIndex;

        uint32_t drawCalls;
        uint32_t drawCalls2D;            // XYZRHW — overlay / HUD
        uint32_t drawCalls3D;            // world-space geometry
        uint32_t drawCallsUnknownFvf;    // FVF we could not decode

        uint32_t triangles;
        uint32_t triangles2D;
        uint32_t triangles3D;

        uint32_t setRenderStateCalls;
        uint32_t setTextureStageStateCalls;
        uint32_t setTextureCalls;
        uint32_t setTransformCalls;
        uint32_t redundantRenderStateCalls;   // value already current

        uint32_t clears;
        uint32_t beginScenes;

        uint32_t distinctFvfCount;
        uint32_t distinctTextureCount;

        // Extents of the *object origins* (world-matrix translations) of all
        // world-space draws this frame. Not a true geometry bounding box —
        // that would mean locking every vertex buffer on every draw — but it
        // is free to compute and establishes the coordinate-system scale the
        // ray tracer has to work in.
        bool     worldBoundsValid;
        float    worldMin[3];
        float    worldMax[3];
    };

    namespace Frame
    {
        // `outputDir` is created if needed; captures land in subfolders.
        bool Init(const char* outputDir);
        void Shutdown();

        // Frame lifecycle, driven from the proxy device.
        void BeginFrame();
        void EndFrame(IDirect3DDevice8* realDevice);

        // Request a full dump of the next frame. Safe to call from anywhere.
        void ArmSingleFrame();

        // True while the current frame is being fully dumped.
        bool IsCapturingFrame();

        uint32_t CurrentFrameIndex();

        // ── Recording entry points, called from the proxy device ─────────
        void OnDraw(IDirect3DDevice8* realDevice, const DeviceState& state,
                    const DrawCallInfo& info);
        void OnClear(uint32_t flags, D3DCOLOR color, float z);
        void OnBeginScene();
        void OnSetRenderState(uint32_t state, uint32_t value, bool redundant);
        void OnSetTextureStageState();
        void OnSetTexture(IDirect3DBaseTexture8* texture);
        void OnSetTransform(uint32_t state);

        // The most recent completed frame's statistics.
        const FrameStats& LastFrameStats();

        // Averaged statistics since Init, for the summary report.
        void WriteSessionReport(const char* path);
    }
}
