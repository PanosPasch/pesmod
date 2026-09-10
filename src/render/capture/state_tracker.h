// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 Panagiotis Paschalis
//
// This file is part of PESMod.
//
// PESMod is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// PESMod is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with PESMod.  If not, see <https://www.gnu.org/licenses/>.

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
// pes6.exe uses two different pipelines, and the shadow has to cover both:
//
//   • 2D menus and HUD — fixed function, bound with an FVF code. Here the
//     SetTransform matrices are the real transforms.
//
//   • the 3D scene — real vs.1.1 vertex shaders, bound as a handle from
//     CreateVertexShader with both a declaration *and* a function. The binary
//     contains no shader bytecode because it assembles the sources at runtime
//     through the statically linked D3DX8 assembler.
//
// The distinction matters for scene reconstruction: a vertex shader ignores
// SetTransform entirely and takes its transforms from constant registers, so
// for 3D draws it is `vsConstants` below — not `world`/`view`/`projection` —
// that describes where the geometry actually lands.
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

    // Vertex shader constant registers we shadow. vs.1.1 guarantees at least
    // 96, and that is where the real object/camera transforms live for this
    // game — SetTransform does not drive shader-based draws.
    static const uint32_t kMaxVsConstants = 96;

    // ps.1.x exposes 8 constant registers (c0..c7).
    static const uint32_t kMaxPsConstants = 8;

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

        // The raw SetVertexShader argument, which D3D8 overloads: either an
        // FVF code or a handle from CreateVertexShader. FVF codes have bit 0
        // clear and handles have it set, which is how the two are told apart.
        // This game uses FVFs for 2D menu draws and declaration handles for
        // the 3D scene, so both appear here.
        uint32_t vertexShader;
        uint32_t pixelShader;

        // ── Vertex shader constants ──────────────────────────────────────
        // c0..c95. `vsConstantsHighWater` is the highest register index ever
        // written plus one, so the capture can dump only the live portion.
        float    vsConstants[kMaxVsConstants][4];
        uint32_t vsConstantsHighWater;

        float    psConstants[kMaxPsConstants][4];
        uint32_t psConstantsHighWater;

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

        // The bound FVF, or 0 when a declaration handle is bound instead.
        // Note that 0 is therefore ambiguous — read `vertexShader` directly to
        // tell "no format bound" from "a declaration". Classifying a draw as
        // 2D or 3D needs the declaration registry, so it lives in the capture
        // layer rather than here.
        uint32_t Fvf() const { return VertexShaderIsFvf() ? vertexShader : 0u; }
    };
}
