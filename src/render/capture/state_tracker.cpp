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

// state_tracker.cpp
#include "state_tracker.h"
#include "../d3d8/d3d8_util.h"
#include <cstring>

namespace
{
    // Several render states are floats stored in a DWORD slot. D3D8 passes
    // them as the raw bit pattern, so defaults have to be seeded the same way.
    inline uint32_t AsDword(float f)
    {
        uint32_t d;
        memcpy(&d, &f, sizeof(d));
        return d;
    }

    // ── D3D8 enum values needed only for seeding defaults ────────────────
    // Declaring them locally keeps d3d8_min.h free of enums nothing else uses.
    const uint32_t kFillSolid          = 3;   // D3DFILL_SOLID
    const uint32_t kShadeGouraud       = 2;   // D3DSHADE_GOURAUD
    const uint32_t kBlendOne           = 2;   // D3DBLEND_ONE
    const uint32_t kBlendZero          = 1;   // D3DBLEND_ZERO
    const uint32_t kCullCCW            = 3;   // D3DCULL_CCW
    const uint32_t kCmpLessEqual       = 4;   // D3DCMP_LESSEQUAL
    const uint32_t kCmpAlways          = 8;   // D3DCMP_ALWAYS
    const uint32_t kStencilOpKeep      = 1;   // D3DSTENCILOP_KEEP
    const uint32_t kFogNone            = 0;   // D3DFOG_NONE
    const uint32_t kMcsMaterial        = 0;   // D3DMCS_MATERIAL
    const uint32_t kMcsColor1          = 1;   // D3DMCS_COLOR1
    const uint32_t kMcsColor2          = 2;   // D3DMCS_COLOR2
    const uint32_t kVertexBlendDisable = 0;   // D3DVBF_DISABLE
    const uint32_t kPatchEdgeDiscrete  = 0;   // D3DPATCHEDGE_DISCRETE
    const uint32_t kBlendOpAdd         = 1;   // D3DBLENDOP_ADD

    const uint32_t kTopDisable         = 1;   // D3DTOP_DISABLE
    const uint32_t kTopSelectArg1      = 2;   // D3DTOP_SELECTARG1
    const uint32_t kTopModulate        = 4;   // D3DTOP_MODULATE
    const uint32_t kTaCurrent          = 1;   // D3DTA_CURRENT
    const uint32_t kTaTexture          = 2;   // D3DTA_TEXTURE
    const uint32_t kTexAddressWrap     = 1;   // D3DTADDRESS_WRAP
    const uint32_t kTexFilterNone      = 0;   // D3DTEXF_NONE
    const uint32_t kTexFilterPoint     = 1;   // D3DTEXF_POINT
}

namespace Capture
{

const D3DMATRIX kIdentityMatrix = {{{
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
}}};

void DeviceState::SetDefaults()
{
    memset(this, 0, sizeof(*this));

    world      = kIdentityMatrix;
    view       = kIdentityMatrix;
    projection = kIdentityMatrix;
    for (int i = 0; i < 3; ++i) worldBlend[i] = kIdentityMatrix;
    for (int i = 0; i < D3D8_TEXTURE_STAGES; ++i) textureMatrix[i] = kIdentityMatrix;

    // ── Render states, per the D3D8 documented defaults ──────────────────
    uint32_t* rs = renderState;
    rs[D3DRS_ZENABLE]                  = 1;            // TRUE when a depth buffer exists
    rs[D3DRS_FILLMODE]                 = kFillSolid;
    rs[D3DRS_SHADEMODE]                = kShadeGouraud;
    rs[D3DRS_LINEPATTERN]              = 0;
    rs[D3DRS_ZWRITEENABLE]             = TRUE;
    rs[D3DRS_ALPHATESTENABLE]          = FALSE;
    rs[D3DRS_LASTPIXEL]                = TRUE;
    rs[D3DRS_SRCBLEND]                 = kBlendOne;
    rs[D3DRS_DESTBLEND]                = kBlendZero;
    rs[D3DRS_CULLMODE]                 = kCullCCW;
    rs[D3DRS_ZFUNC]                    = kCmpLessEqual;
    rs[D3DRS_ALPHAREF]                 = 0;
    rs[D3DRS_ALPHAFUNC]                = kCmpAlways;
    rs[D3DRS_DITHERENABLE]             = FALSE;
    rs[D3DRS_ALPHABLENDENABLE]         = FALSE;
    rs[D3DRS_FOGENABLE]                = FALSE;
    rs[D3DRS_SPECULARENABLE]           = FALSE;
    rs[D3DRS_FOGCOLOR]                 = 0;
    rs[D3DRS_FOGTABLEMODE]             = kFogNone;
    rs[D3DRS_FOGSTART]                 = AsDword(0.0f);
    rs[D3DRS_FOGEND]                   = AsDword(1.0f);
    rs[D3DRS_FOGDENSITY]               = AsDword(1.0f);
    rs[D3DRS_ZBIAS]                    = 0;
    rs[D3DRS_RANGEFOGENABLE]           = FALSE;
    rs[D3DRS_STENCILENABLE]            = FALSE;
    rs[D3DRS_STENCILFAIL]              = kStencilOpKeep;
    rs[D3DRS_STENCILZFAIL]             = kStencilOpKeep;
    rs[D3DRS_STENCILPASS]              = kStencilOpKeep;
    rs[D3DRS_STENCILFUNC]              = kCmpAlways;
    rs[D3DRS_STENCILREF]               = 0;
    rs[D3DRS_STENCILMASK]              = 0xFFFFFFFFu;
    rs[D3DRS_STENCILWRITEMASK]         = 0xFFFFFFFFu;
    rs[D3DRS_TEXTUREFACTOR]            = 0xFFFFFFFFu;
    rs[D3DRS_CLIPPING]                 = TRUE;
    rs[D3DRS_LIGHTING]                 = TRUE;
    rs[D3DRS_AMBIENT]                  = 0;
    rs[D3DRS_FOGVERTEXMODE]            = kFogNone;
    rs[D3DRS_COLORVERTEX]              = TRUE;
    rs[D3DRS_LOCALVIEWER]              = TRUE;
    rs[D3DRS_NORMALIZENORMALS]         = FALSE;
    rs[D3DRS_DIFFUSEMATERIALSOURCE]    = kMcsColor1;
    rs[D3DRS_SPECULARMATERIALSOURCE]   = kMcsColor2;
    rs[D3DRS_AMBIENTMATERIALSOURCE]    = kMcsMaterial;
    rs[D3DRS_EMISSIVEMATERIALSOURCE]   = kMcsMaterial;
    rs[D3DRS_VERTEXBLEND]              = kVertexBlendDisable;
    rs[D3DRS_CLIPPLANEENABLE]          = 0;
    rs[D3DRS_SOFTWAREVERTEXPROCESSING] = FALSE;
    rs[D3DRS_POINTSIZE]                = AsDword(1.0f);
    rs[D3DRS_POINTSIZE_MIN]            = AsDword(1.0f);
    rs[D3DRS_POINTSPRITEENABLE]        = FALSE;
    rs[D3DRS_POINTSCALEENABLE]         = FALSE;
    rs[D3DRS_MULTISAMPLEANTIALIAS]     = TRUE;
    rs[D3DRS_MULTISAMPLEMASK]          = 0xFFFFFFFFu;
    rs[D3DRS_PATCHEDGESTYLE]           = kPatchEdgeDiscrete;
    rs[D3DRS_COLORWRITEENABLE]         = 0x0000000Fu;
    rs[D3DRS_TWEENFACTOR]              = AsDword(0.0f);
    rs[D3DRS_BLENDOP]                  = kBlendOpAdd;

    // ── Texture stage states ─────────────────────────────────────────────
    // Only stage 0 starts enabled; every later stage defaults to DISABLE,
    // which is how the fixed-function pipeline knows where the chain ends.
    for (uint32_t s = 0; s < D3D8_TEXTURE_STAGES; ++s)
    {
        uint32_t* ss = stageState[s];
        ss[D3DTSS_COLOROP]       = (s == 0) ? kTopModulate    : kTopDisable;
        ss[D3DTSS_COLORARG1]     = kTaTexture;
        ss[D3DTSS_COLORARG2]     = kTaCurrent;
        ss[D3DTSS_ALPHAOP]       = (s == 0) ? kTopSelectArg1  : kTopDisable;
        ss[D3DTSS_ALPHAARG1]     = kTaTexture;
        ss[D3DTSS_ALPHAARG2]     = kTaCurrent;
        ss[D3DTSS_TEXCOORDINDEX] = s;
        ss[D3DTSS_ADDRESSU]      = kTexAddressWrap;
        ss[D3DTSS_ADDRESSV]      = kTexAddressWrap;
        ss[D3DTSS_ADDRESSW]      = kTexAddressWrap;
        ss[D3DTSS_BORDERCOLOR]   = 0;
        ss[D3DTSS_MAGFILTER]     = kTexFilterPoint;
        ss[D3DTSS_MINFILTER]     = kTexFilterPoint;
        ss[D3DTSS_MIPFILTER]     = kTexFilterNone;
        ss[D3DTSS_MIPMAPLODBIAS] = AsDword(0.0f);
        ss[D3DTSS_MAXMIPLEVEL]   = 0;
        ss[D3DTSS_MAXANISOTROPY] = 1;
        ss[D3DTSS_RESULTARG]     = kTaCurrent;
    }

    // ── Material ─────────────────────────────────────────────────────────
    // D3D8's default material is white diffuse with everything else black.
    // The game overwrites this constantly; it only matters for the very
    // first frames before any SetMaterial call.
    material.Diffuse.r = material.Diffuse.g = material.Diffuse.b = 1.0f;
    material.Diffuse.a = 0.0f;

    // Viewport is filled in by the proxy from the present parameters, since
    // its default is "the whole back buffer" and that size is not known here.
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
}

bool DeviceState::VertexShaderIsFvf() const
{
    // D3D8 overloads SetVertexShader: an FVF code always has bit 0
    // (D3DFVF_RESERVED0) clear, while handles from CreateVertexShader have
    // it set. This is the same discriminator d3d8to9 and the runtime use.
    return (vertexShader & D3DFVF_RESERVED0) == 0;
}

} // namespace Capture
