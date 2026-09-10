// scene_export.h
//
// The producer half of the renderer: turns the intercepted D3D8 draw stream
// into the scene messages the 64-bit host consumes.
//
// This runs on the game's render thread, inside every draw call, so the rule
// throughout is that it must never block and never allocate on the hot path.
// Under back-pressure it drops frame traffic and carries on; the game's frame
// rate is not allowed to depend on whether the host is keeping up.
//
// ── What gets sent ───────────────────────────────────────────────────────
//
// Geometry is keyed by a hash of the *draw's slice* of its buffers, not by
// the buffer itself: many draws share one vertex buffer and differ only by
// index range, and each of those slices is a separate mesh as far as a BLAS
// is concerned. A slice is uploaded on first use and re-uploaded only when
// its contents change, which is what makes animated players work — the game
// does CPU skinning, so their vertices are rewritten every frame.
//
// ── What is deliberately *not* decided here ──────────────────────────────
//
// The object-to-world transform. The game's 3D draws are shader-driven and
// their only transform is the combined world-view-projection matrix at
// constants c58..c61 (docs/RENDERER.md §4.3), so a world matrix does not
// exist anywhere in the draw. Rather than guess at a factorisation on the
// render thread, the exporter sends the raw WVP and lets the host divide out
// the shared camera.
#pragma once

#include "../d3d8/d3d8_min.h"
#include "../ipc/scene_protocol.h"
#include "state_tracker.h"
#include "frame_capture.h"

#include <cstdint>

namespace Capture
{
    namespace SceneExport
    {
        // Opens the shared section as producer. Returns false if the section
        // cannot be created, in which case export stays off and the rest of
        // the mod is unaffected.
        bool Init(const char* sectionName, uint64_t ringBytes);
        void Shutdown();
        bool IsActive();

        // Frame lifecycle, driven from the proxy device's Present.
        void BeginFrame(uint64_t frameIndex, uint32_t width, uint32_t height);
        void EndFrame();

        // The game cleared the render target, so everything sent for this
        // frame so far is not part of the picture.
        //
        // pes6.exe renders its player shadows first, from the light's point
        // of view, copies the result into a texture with CopyRects and then
        // clears and draws the visible frame. Those draws reach OnWorldDraw
        // looking exactly like world geometry - there is no SetRenderTarget
        // anywhere in the binary to tell them apart - and on a measured
        // match frame they are 133 of 832 world draws, not one of which
        // shares the camera's view-projection.
        //
        // Only a full-target clear counts. A depth-only clear says nothing
        // about the colour already drawn, and a rectangle clear does not
        // discard the whole frame.
        void OnClearTarget();

        // Called for every draw the capture layer classifies as world-space.
        // `realDevice` is used to read back buffer contents when a geometry
        // slice needs uploading.
        void OnWorldDraw(IDirect3DDevice8* realDevice, const DeviceState& state,
                         const DrawCallInfo& info);

        // Publishes the lighting rig read out of the vertex shader constants.
        // Sent only when it changes, so the host carries it across frames.
        void UpdateLighting(const DeviceState& state);

        struct ExportStats
        {
            uint64_t framesSent;
            uint64_t instancesSent;
            uint64_t geometryUploads;
            uint64_t geometryBytes;
            uint64_t textureUploads;
            uint64_t textureBytes;
            uint64_t drawsSkipped;      // could not read geometry back
            uint64_t writeFailures;     // ring full

            // Textures the game has that the host never gets, and why.
            // Every one of these is a surface that renders untextured, so
            // they are counted rather than left to be noticed in a
            // screenshot: a texture missing for one of these reasons looks
            // exactly like a texture missing for any other.
            uint64_t texturesUnknownFormat;  // palettised and the like
            uint64_t texturesCopiedBack;     // unlockable, read via CopyRects
            uint64_t texturesUnreadable;     // not lockable and not copyable
            uint64_t texturesRetried;        // a later attempt after a failure

            // Textures re-sent because the content behind the pointer
            // changed. Two things cause it: the game updating a texture in
            // place, and - the reason this check exists - the game releasing
            // a texture and Direct3D handing the same address to a new one.
            //
            // Nothing hooks Release, so the resource registry cannot know a
            // texture died; it finds the stale pointer and reuses its id. If
            // "already sent" is then taken at face value, the new texture's
            // pixels are never transmitted and every draw using it samples
            // the dead one. That is a pitch drawn with a kit atlas and a goal
            // net drawn with crowd, and it only appears once a session has
            // run long enough for an address to be recycled.
            uint64_t texturesContentChanged;
        };
        const ExportStats& Stats();
    }
}
