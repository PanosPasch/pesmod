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

// patcher.h
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
 
namespace Patcher
{
    // ── Core write primitive ─────────────────────────────────────────────
    // Writes <size> bytes from <data> to virtual address <address>.
    // Temporarily changes memory protection to allow writes to .text.
    // Flushes CPU instruction cache after writing.
    // Returns true on success.
    bool WriteBytes(uintptr_t address, const void* data, size_t size);
 
    // ── Convenience helpers ──────────────────────────────────────────────
    bool NopRegion(uintptr_t address, size_t count);       // Fill with 0x90
    bool WriteByte(uintptr_t address, uint8_t value);
    bool WriteWord(uintptr_t address, uint16_t value);     // 2 bytes LE
    bool WriteDword(uintptr_t address, uint32_t value);    // 4 bytes LE
    bool WriteFloat(uintptr_t address, float value);
 
    // ── Patch application ────────────────────────────────────────────────
    // Reads back bytes and confirms they match expected (sanity check)
    bool VerifyBytes(uintptr_t address, const uint8_t* expected, size_t size);
 
    // Apply all patches for this mod
    void ApplyAll();
}

