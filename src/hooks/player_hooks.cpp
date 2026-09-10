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

// player_hooks.cpp
#include "player_hooks.h"
#include "MinHook/include/MinHook.h"
#include "../utils/logger.h"
#include "../utils/config.h"
#include "../patching/pattern_scan.h"
#include <cstdint>
 
// ─────────────────────────────────────────────────────────────────────────
// PlayerData struct — partial definition from Ghidra analysis.
// Only fields we care about are defined here.
// The char padding covers fields not yet identified — do NOT remove it.
// ─────────────────────────────────────────────────────────────────────────
struct PlayerData
{
    int    playerID;          // +0x00
    int    teamID;            // +0x04
    float  posX;              // +0x08
    float  posY;              // +0x0C
    float  posZ;              // +0x10
    char   _unknown1[0x14];   // +0x14 to +0x27 (unidentified fields)
    float  stamina;           // +0x28  range 0.0 (exhausted) to 1.0 (full)
    float  staminaDrainRate;  // +0x2C  base drain per second
    char   _unknown2[0x50];   // +0x30 to +0x7F (unidentified)
    int    formAttribute;     // +0x80  1=Excellent 2=Good 3=Normal 4=Bad 5=Terrible
    // ... rest of struct not yet mapped
};
 
// ─────────────────────────────────────────────────────────────────────────
// Hook: StaminaDrain
// ─────────────────────────────────────────────────────────────────────────
 
// Byte pattern for the function prologue.
// This is more reliable than a hardcoded address across EXE versions.
// Obtain this from the first 10-12 bytes of the function in Ghidra / x32dbg.
// Replace the pattern and mask here with what YOU find in your binary.
static const char*  PAT_StaminaDrain = "\x55\x8B\xEC\x83\xEC\x08\xF3\x0F\x10\x41\x28";
static const char*  MSK_StaminaDrain = "xxxxxxxxxxx";
 
typedef void (__fastcall *FN_StaminaDrain)(PlayerData* pPlayer, void* edx, float deltaTime);
static FN_StaminaDrain orig_StaminaDrain = nullptr;
 
void __fastcall hook_StaminaDrain(PlayerData* pPlayer, void* edx, float deltaTime)
{
    // Guard: never dereference a null pointer
    if (!pPlayer)
    {
        if (orig_StaminaDrain) orig_StaminaDrain(pPlayer, edx, deltaTime);
        return;
    }
 
    // Read config values (Config::GetFloat is cheap — values are cached internally)
    const float drainMult = Config::GetFloat("stamina", "drain_multiplier", 1.0f);
    const float minStamina = Config::GetFloat("stamina", "minimum_stamina", 0.0f);
 
    // Apply drain multiplier to the delta time before passing to original.
    // The original function calculates: drain = staminaDrainRate * deltaTime
    // By scaling deltaTime, we scale the drain proportionally.
    const float modifiedDelta = deltaTime * drainMult;
 
    // Call original with modified argument
    orig_StaminaDrain(pPlayer, edx, modifiedDelta);
 
    // Enforce the minimum stamina floor AFTER the original has run
    if (pPlayer->stamina < minStamina)
        pPlayer->stamina = minStamina;
 
#ifdef PESMOD_DEBUG
    if (Config::GetBool("debug", "verbose_logging", false))
    {
        Logger::Log("[Stamina] Player %d: stamina=%.3f (drain mult=%.2f)",
            pPlayer->playerID, pPlayer->stamina, drainMult);
    }
#endif
}
 
// ─────────────────────────────────────────────────────────────────────────
// Registration — called from HooksRegistry::InstallAll()
// ─────────────────────────────────────────────────────────────────────────
static void InstallStaminaDrainHook()
{
    // Use pattern scan for version resilience.
    // If scan fails, we fall back to the known address for the retail EXE.
    uintptr_t fnAddr = ScanExe(PAT_StaminaDrain, MSK_StaminaDrain);
 
    if (fnAddr == 0)
    {
        // Fallback: known address for PES6 retail (EXAMPLE — replace with your own)
        Logger::Log("[PESMod] StaminaDrain pattern not found, using fallback address.");
        fnAddr = 0x00456780; // REPLACE WITH YOUR ACTUAL ADDRESS
    }
    else
    {
        Logger::Log("[PESMod] StaminaDrain found via pattern at 0x%08X", fnAddr);
    }
 
    MH_STATUS s = MH_CreateHook((LPVOID)fnAddr,
                                (LPVOID)&hook_StaminaDrain,
                                (LPVOID*)&orig_StaminaDrain);
    if (s != MH_OK)
    {
        Logger::Log("[PESMod] ERROR: StaminaDrain MH_CreateHook failed (%d)", s);
        return;
    }
 
    s = MH_EnableHook((LPVOID)fnAddr);
    if (s != MH_OK)
    {
        Logger::Log("[PESMod] ERROR: StaminaDrain MH_EnableHook failed (%d)", s);
        return;
    }
 
    Logger::Log("[PESMod] StaminaDrain hook installed OK.");
}
 
void PlayerHooks::Register()
{
    if (!Config::GetBool("general", "enabled", true)) return;
    InstallStaminaDrainHook();
    // Add more player hook registrations here as you discover them
}

