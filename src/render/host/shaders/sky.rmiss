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

#version 460
// glslang only honours #include when this is enabled first.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) rayPayloadInEXT HitPayload payload;

void main()
{
    // The background is not a surface, so it does not get the surface rig.
    //
    // Running GameLighting on a ray direction was a category error: that
    // function adds a directional term, a ground term and ambient because
    // that is what the game does to a *lit vertex*, and applying it to a
    // direction produced a flat brown sky.
    //
    // The game's own sky - a textured box around the camera - is traced by
    // the ray generation on the kMaskSky mask before it settles for this.
    // So this runs in two cases: a direction the box does not cover, and a
    // scene with no sky in it at all, such as the menus. Both want something
    // plausible rather than black, and a gradient between the game's own sky
    // and ground colours at least agrees with what the surfaces are lit by.
    const vec3 dir = normalize(gl_WorldRayDirectionEXT);

    // c93 points along the hemisphere axis and is not unit length, so it is
    // normalised here - unlike in the shading path, where the game dots it
    // raw and the magnitude is part of the result.
    const float t = clamp(dot(dir, normalize(scene.hemisphereAxis.xyz)) * 0.5 + 0.5,
                          0.0, 1.0);

    payload.colour = mix(scene.groundColor.rgb, scene.skyColor.rgb, t) *
                     scene.lightingScale.rgb + scene.ambient.rgb;
    payload.alpha  = 1.0;

    // Negative distance is how the ray generation knows this is the
    // background and that there is nothing further to peel.
    payload.dist   = -1.0;
}
