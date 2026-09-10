// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 Panagiotis Paschalis
//
// This file is part of PESMod.
//
// PESMod is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// PESMod is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along
// with PESMod.  If not, see <https://www.gnu.org/licenses/>.

// pattern_scan.h
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
 
// Scan a memory range for a byte pattern with wildcards.
// pattern: hex bytes as string, "??" = wildcard
// Example: "55 8B EC ?? ?? 83 EC 10" matches PUSH EBP; MOV EBP,ESP; [any 2]; SUB ESP,16
inline uintptr_t PatternScan(
    uintptr_t start, uintptr_t end,
    const char* pattern, const char* mask)
{
    size_t patLen = strlen(mask);
    for (uintptr_t addr = start; addr < end - patLen; ++addr)
    {
        bool found = true;
        for (size_t i = 0; i < patLen; ++i)
        {
            if (mask[i] != '?' && ((BYTE*)addr)[i] != (BYTE)pattern[i])
            {
                found = false;
                break;
            }
        }
        if (found) return addr;
    }
    return 0; // not found
}
 
// Helper: scan the main EXE module
inline uintptr_t ScanExe(const char* pattern, const char* mask)
{
    HMODULE hExe = GetModuleHandleA(nullptr); // nullptr = main EXE
    auto* dosHdr = (IMAGE_DOS_HEADER*)hExe;
    auto* ntHdr  = (IMAGE_NT_HEADERS*)((uintptr_t)hExe + dosHdr->e_lfanew);
    uintptr_t base = (uintptr_t)hExe;
    uintptr_t size = ntHdr->OptionalHeader.SizeOfImage;
    return PatternScan(base, base + size, pattern, mask);
}