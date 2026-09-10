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
#extension GL_EXT_ray_tracing : require

// Reaching this shader means the shadow ray hit nothing, so the surface is
// lit. The payload starts at 0 (occluded) and is only raised here, which is
// what makes RayFlags TERMINATE_ON_FIRST_HIT safe to use.
layout(location = 1) rayPayloadInEXT float shadowVisibility;

void main()
{
    shadowVisibility = 1.0;
}
