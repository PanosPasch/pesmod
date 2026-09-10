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

// d3d8_guids.cpp
//
// The two interface IIDs the proxies must answer to in QueryInterface.
//
// These are the real DirectX 8 values, not invented ones — if the game (or a
// wrapper such as d3d8to9, which is sitting disabled in the game folder) asks
// for IID_IDirect3DDevice8 and gets E_NOINTERFACE back, it will fail to
// attach. They are suffixed _PESMod only to avoid colliding with a system
// d3d8.h should one ever end up in the include path.
#include "d3d8_min.h"

// {1DD9E8DA-1C77-4d40-B0CF-98FEFDFF9512}
extern const GUID IID_IDirect3D8_PESMod =
    { 0x1dd9e8da, 0x1c77, 0x4d40,
      { 0xb0, 0xcf, 0x98, 0xfe, 0xfd, 0xff, 0x95, 0x12 } };

// {7385E5DF-8FE8-41d5-86B6-D7B48547B6CF}
extern const GUID IID_IDirect3DDevice8_PESMod =
    { 0x7385e5df, 0x8fe8, 0x41d5,
      { 0x86, 0xb6, 0xd7, 0xb4, 0x85, 0x47, 0xb6, 0xcf } };
