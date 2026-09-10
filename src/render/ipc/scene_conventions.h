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

// scene_conventions.h
//
// How to read the game's transforms out of its vertex shader constants.
//
// This lives beside the wire format, and not in the 32-bit producer that uses
// it, for one reason: it was wrong for a long time and nothing could see it.
// The producer is inside the game and has no test harness; the 64-bit host
// does. Putting the interpretation here lets the host's self-test assert it
// against real captured values.
//
// ── The convention ───────────────────────────────────────────────────────
//
// Every world-space vertex shader in pes6.exe transforms with one
// instruction:
//
//     m4x4 oPos, v0, c58
//
// which the hardware expands into four dot products:
//
//     dp4 oPos.x, v0, c58        dp4 oPos.y, v0, c59
//     dp4 oPos.z, v0, c60        dp4 oPos.w, v0, c61
//
// so clip.j = dot(v0, c(58+j)). Each register is a *column* of the row-vector
// matrix, not a row — which is simply the usual Direct3D practice of
// transposing a matrix before uploading it as shader constants.
//
// ── Why this is worth a header of its own ────────────────────────────────
//
// Reading the four registers as rows yields the transpose, and a transposed
// perspective matrix does not look broken. It moves the projective terms from
// the last column into the last row, so instead of scattering geometry it
// crushes every vertex toward the screen centre: a plausible-looking image of
// nothing. Measured on a 405-draw match frame, read as columns 37% of
// referenced vertices land inside the frustum; read as rows, 0.0% do, with
// every NDC coordinate inside +/-0.02.
#pragma once

#include "scene_protocol.h"

namespace SceneIPC
{
    // The first register of the m4x4 block. Fixed by the game's shader
    // bytecode, not by anything configurable.
    const uint32_t kClipTransformRegister = 58;

    // Builds the row-vector world-to-clip matrix from four consecutive
    // constant registers. `regs` is the shadowed constant file, indexed by
    // register number.
    inline void ClipTransformFromConstants(const float regs[][4], uint32_t base,
                                           Matrix4x4& out)
    {
        for (uint32_t row = 0; row < 4; ++row)
            for (uint32_t col = 0; col < 4; ++col)
                out.m[row * 4 + col] = regs[base + col][row];
    }

    // Is `m` plausibly a world-to-clip matrix in row-vector form?
    //
    // For a Direct3D perspective projection P, the last column is (0,0,1,0),
    // so VP's last column is the view matrix's third column: the camera's
    // forward axis, which is unit length. Multiplying by an object's world
    // transform scales that by the object's scale, so it stays O(1) for this
    // game's content.
    //
    // Transposed, the last column is the translation row instead. On a real
    // match frame it measures 5185 against a true value of 1.0000, so one
    // cheap test separates the two readings with three orders of magnitude to
    // spare.
    inline bool ClipTransformLooksSane(const Matrix4x4& m)
    {
        const float x = m.m[3], y = m.m[7], z = m.m[11];
        const float len2 = x * x + y * y + z * z;
        return len2 > 1.0e-6f && len2 < 100.0f;   // length in (0.001, 10)
    }
}
