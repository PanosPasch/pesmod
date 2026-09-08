// state_tracker.h
//
// A CPU-side shadow of the D3D8 fixed-function pipeline state.
//
// A draw call on its own says almost nothing — DrawIndexedPrimitive carries
// only a primitive type and some counts. Everything needed to reconstruct
// what was actually drawn (where it is in the world, what it looks like,
// whether it is 3D scene geometry or a 2D overlay) lives in state that was
// set by *earlier* calls. This mirrors that state so each draw can be
// captured with its full context attached.
//
// pes6.exe drives the pipeline entirely through fixed function: no vertex
// shaders exist in the binary, so world/view/projection matrices set via
// SetTransform are the real object transforms. That is what makes the
// scene reconstructible for ray tracing.
#pragma once

#include "../d3d8/d3d8_min.h"
#include <cstdint>

namespace Capture
{
    // Identity matrix used to seed the shadow before the game sets anything.
    extern const D3DMATRIX kIdentityMatrix;

    struct StreamBinding
    {
        IDirect3DVertexBuffer8* buffer;
        uint32_t                stride;
    };

    struct LightState
    {
        D3DLIGHT8 light;
        bool      enabled;
        bool      everSet;
    };

    // The number of lights we shadow. D3D8 allows more, but the fixed
    // function pipeline caps simultaneous lights well below this and the
    // game never approaches it.
    static const uint32_t kMaxTrackedLights = 16;

    struct DeviceState
    {
        // ── Transforms ───────────────────────────────────────────────────
        // World is D3DTS_WORLD (256). Only WORLD0 is tracked as the primary
        // object transform; WORLD1..3 matter only for vertex blending, which
        // is recorded separately via D3DRS_VERTEXBLEND.
        D3DMATRIX world;
        D3DMATRIX view;
        D3DMATRIX projection;
        D3DMATRIX worldBlend[3];       // WORLD1..WORLD3
        D3DMATRIX textureMatrix[D3D8_TEXTURE_STAGES];

        // ── Fixed-function state ─────────────────────────────────────────
        uint32_t renderState[D3DRS_SHADOW_COUNT];
        uint32_t stageState[D3D8_TEXTURE_STAGES][D3DTSS_SHADOW_COUNT];

        // ── Bindings ─────────────────────────────────────────────────────
        // Textures are stored as the *real* underlying pointers so they can
        // be looked up in the resource registry.
        IDirect3DBaseTexture8*  texture[D3D8_TEXTURE_STAGES];
        StreamBinding           stream[D3D8_MAX_STREAMS];
        IDirect3DIndexBuffer8*  indexBuffer;
        uint32_t                baseVertexIndex;

        // SetVertexShader is called with an FVF code in this game, never a
        // shader handle — FVF codes have the low bit clear and handles are
        // allocated with it set, which is how the two are told apart.
        uint32_t vertexShader;
        uint32_t pixelShader;

        // ── Misc ─────────────────────────────────────────────────────────
        D3DVIEWPORT8 viewport;
        D3DMATERIAL8 material;
        LightState   lights[kMaxTrackedLights];

        // ── Lifecycle ────────────────────────────────────────────────────
        // Resets to the D3D8 documented defaults. Called at device creation
        // and after a successful Reset().
        void SetDefaults();

        // True when the currently bound vertex format is a real FVF (rather
        // than a created vertex-shader handle).
        bool VertexShaderIsFvf() const;

        // Convenience accessors used by the capture writer.
        uint32_t Fvf() const { return VertexShaderIsFvf() ? vertexShader : 0u; }

        // True when the active vertex format is pre-transformed screen space
        // (D3DFVF_XYZRHW) — i.e. this draw is 2D overlay, not scene geometry.
        bool DrawIsScreenSpace() const;
    };
}
