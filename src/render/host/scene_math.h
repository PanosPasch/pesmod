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

// scene_math.h
//
// Matrix helpers for turning the game's transforms into what a TLAS wants.
//
// ── Conventions, because mixing them up is silent ────────────────────────
//
// The game is Direct3D 8 and works in the **row-vector** convention:
// clip = v * M, with M stored row-major. Every matrix arriving over the scene
// stream is in that form.
//
// Note that this is *not* how the matrix sits in the game's shader constants.
// `m4x4 oPos, v0, c58` is four dot products against consecutive registers, so
// each register is a column of M, not a row. The producer transposes on the
// way in; see scene_conventions.h, which is where that belongs and where it
// is tested.
//
// Vulkan's VkTransformMatrixKHR is the opposite: a 3x4 matrix applied as
// p' = M * p, the **column-vector** convention. Converting between them is a
// transpose, and getting it wrong produces geometry that is subtly
// transposed rather than obviously broken — so it happens exactly once, here.
#pragma once

#include "../ipc/scene_protocol.h"

#include <cmath>
#include <cstring>

namespace Host
{
    namespace Math
    {
        typedef SceneIPC::Matrix4x4 Mat4;

        inline Mat4 Identity()
        {
            Mat4 m;
            memset(m.m, 0, sizeof(m.m));
            m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
            return m;
        }

        // Row-vector convention throughout: (a*b) means "apply a, then b".
        inline Mat4 Multiply(const Mat4& a, const Mat4& b)
        {
            Mat4 r;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                {
                    float s = 0.0f;
                    for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
                    r.m[i * 4 + j] = s;
                }
            return r;
        }

        // General 4x4 inverse. Returns false for a singular matrix, which is
        // what an all-zero or otherwise unset transform looks like.
        inline bool Inverse(const Mat4& in, Mat4& out)
        {
            const float* m = in.m;
            float inv[16];

            inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
                     + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
            inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
                     - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
            inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
                     + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
            inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
                     - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
            inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
                     - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
            inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
                     + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
            inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
                     - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
            inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
                     + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
            inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
                     + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
            inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
                     - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
            inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
                     + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
            inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
                     - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
            inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
                     - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
            inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
                     + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
            inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
                     - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
            inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
                     + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

            float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
            if (fabsf(det) < 1e-20f) return false;

            det = 1.0f / det;
            for (int i = 0; i < 16; ++i) out.m[i] = inv[i] * det;
            return true;
        }

        // A world transform must be affine: in row-vector form its last
        // *column* is (0,0,0,1). A recovered matrix that fails this did not
        // come from a correct view-projection, which is the only way to catch
        // a wrong VP before it silently misplaces the whole scene.
        inline bool IsAffine(const Mat4& m, float tolerance = 1e-3f)
        {
            return fabsf(m.m[3])  < tolerance &&
                   fabsf(m.m[7])  < tolerance &&
                   fabsf(m.m[11]) < tolerance &&
                   fabsf(m.m[15] - 1.0f) < tolerance;
        }

        // Row-vector row-major -> VkTransformMatrixKHR's column-vector 3x4.
        // This is the transpose described in the header comment.
        inline void ToVkTransform(const Mat4& src, float dst[12])
        {
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 4; ++col)
                    dst[row * 4 + col] = src.m[col * 4 + row];
        }

        // Largest absolute scale factor along any axis, used to sanity-check
        // a recovered transform before it is handed to the TLAS.
        inline float MaxAxisScale(const Mat4& m)
        {
            float best = 0.0f;
            for (int row = 0; row < 3; ++row)
            {
                const float len = sqrtf(m.m[row*4+0]*m.m[row*4+0] +
                                        m.m[row*4+1]*m.m[row*4+1] +
                                        m.m[row*4+2]*m.m[row*4+2]);
                if (len > best) best = len;
            }
            return best;
        }
    }
}
