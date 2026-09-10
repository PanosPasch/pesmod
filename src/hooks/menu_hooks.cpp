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

// src/hooks/menu_hooks.cpp
//
// Full replacement of menu_LoadPageTeams (FUN_00609520 — replace with your
// actual address from Ghidra).
//
// Original behaviour is completely superseded. We call the two side-effect
// functions (FUN_006094b0 and FUN_005ee610) exactly as the original did,
// and call db_CheckTeamIsUnlocked / _db_GetDB through resolved pointers.
//
#include "menu_hooks.h"
#include "MinHook/include/MinHook.h"
#include "../utils/logger.h"
#include "../utils/config.h"
#include "../patching/pattern_scan.h"
#include <cstdint>
#include <cstring>

// =============================================================================
// SECTION 1 — External game data and function addresses
//
// Every address below was obtained from Ghidra.
// REPLACE each address constant with the real value from YOUR binary.
// They are grouped here so the entire address surface of this hook is
// visible in one place and easy to update for a different EXE version.
// =============================================================================

// ── Global array that holds the team list for the menu ──────────────────────
// Ghidra name: DAT_04de9fa0
// Layout: [side * 0x41 + slotIndex] * 4 bytes per entry (stored as short/int)
// side 0 = player 1 picking,  side 1 = player 2 picking
// 0x41 = 65 slots per side
static const uintptr_t ADDR_TeamListArray  = 0x04de9fa0; // REPLACE
static const int       SLOTS_PER_SIDE      = 0x41;       // 65

// ── Classic team range (IDs 57–63 require unlock check) ─────────────────────
static const uint32_t  CLASSIC_TEAM_FIRST  = 57;
static const uint32_t  CLASSIC_TEAM_LAST   = 63;

// ── Special DB team ID (0xD1 = 209, added on page 4 when DB is inactive) ────
static const uint16_t  DB_TEAM_ID          = 0xD1;

// ── Second array used for side 1 on page 4 DB team entry ────────────────────
// Ghidra name: DAT_04dea1a8
static const uintptr_t ADDR_TeamListArray2 = 0x04dea1a8; // REPLACE

// The flags variable the original was passing as the pointer argument
// Ghidra name: DAT_00aa5d48
static const uintptr_t ADDR_MenuFlagsVar = 0x00aa5d48; // confirm in Ghidra


// =============================================================================
// SECTION 2 — External game function pointer types and addresses
//
// These four functions are called by the original and must be called by our
// replacement too. We resolve them once at hook-install time.
// =============================================================================

// FUN_006094b0 — called at the start of menu_LoadPageTeams.
// Likely clears/resets the team slot array for the given side.
// Convention: __cdecl, one int argument (the side/param_1).
// REPLACE address with your Ghidra value.
typedef void (__cdecl *FN_MenuPageSetup)(int side);
static FN_MenuPageSetup  fn_MenuPageSetup  = nullptr;
static const uintptr_t   ADDR_MenuPageSetup = 0x006094b0; // REPLACE

// FUN_005ee610 — called at the end of menu_LoadPageTeams (both exit paths).
// Likely triggers a menu refresh/redraw after the team list is populated.
// Convention: __cdecl, no arguments.
// REPLACE address with your Ghidra value.
typedef void (__cdecl *FN_MenuPageFinish)(uint32_t* pFlags, uint32_t mask);
static FN_MenuPageFinish fn_MenuPageFinish = nullptr;
static const uintptr_t   ADDR_MenuPageFinish = 0x005ee610; // REPLACE


// db_CheckTeamIsUnlocked — returns 1 if the classic team ID is unlocked.
// Convention: __cdecl, one int argument (teamID).
// REPLACE address with your Ghidra value.
typedef int (__cdecl *FN_CheckTeamUnlocked)(int teamID);
static FN_CheckTeamUnlocked fn_CheckTeamUnlocked  = nullptr;
static const uintptr_t      ADDR_CheckTeamUnlocked = 0x005fc1e0; // REPLACE

// _db_GetDB — returns 0 when the "DB" (master league database?) is inactive.
// Convention: __cdecl, no arguments.
// REPLACE address with your Ghidra value.
typedef int (__cdecl *FN_GetMasterLeagueLoadedTeam)();
static FN_GetMasterLeagueLoadedTeam         fn_GetMasterLeagueLoadedTeam           = nullptr;
static const uintptr_t  ADDR_GetMasterLeagueLoadedTeam         = 0x008c6170; // REPLACE

// =============================================================================
// SECTION 3 — Page range table
//
// Defines the first and last team ID for each page index (1–4).
// Extracted from the original switch statement.
// Storing it as a table makes it trivial to add pages or change ranges
// without touching any logic — just edit the table.
// =============================================================================

struct PageRange
{
    uint32_t firstTeamID;
    uint32_t lastTeamID;
};

// Index 0 is unused (pages are 1-based). Index 1–4 match pageIndex values.
static const PageRange PAGE_RANGES[] =
{
    { 0,   0   },  // [0] unused
    { 0,   63  },  // [1] page 1: teams   0 – 63
    { 64,  121 },  // [2] page 2: teams  64 – 121
    { 122, 179 },  // [3] page 3: teams 122 – 179
    { 180, 201 },  // [4] page 4: teams 180 – 201
};

static const int PAGE_COUNT = 4; // valid page indices: 1 .. PAGE_COUNT

// =============================================================================
// SECTION 4 — Helper: write a team ID into the flat array
//
// The original writes: (&DAT_04de9fa0)[(side * 0x41 + slot) * 4] = teamID
//
// The array stores entries as 4-byte slots but only the low 2 bytes (short)
// are used for the team ID. We replicate this exactly.
// =============================================================================

static inline void WriteTeamSlot(int side, int slot, uint16_t teamID)
{
    // Base pointer into the game's team list array
    int32_t* base = reinterpret_cast<int32_t*>(ADDR_TeamListArray);

    // Slot index formula from original: (side * 0x41 + slot) * 4 bytes
    // Since base is int32_t*, each element IS 4 bytes, so index directly:
    int index = (side * SLOTS_PER_SIDE + slot) * 2;

    // Write team ID into the low 16 bits of the 32-bit slot.
    // The original uses a short cast: = (short)firstTeamID
    // We zero the slot first then write the low 16 bits to match exactly.
    base[index] = static_cast<int32_t>(static_cast<int16_t>(teamID));
}

// Same write but targeting the second array (DAT_04dea1a8), used for
// side 1's DB team entry on page 4.
static inline void WriteTeamSlot2(int slot, uint16_t teamID)
{
    int32_t* base = reinterpret_cast<int32_t*>(ADDR_TeamListArray2);
    base[slot] = static_cast<int32_t>(static_cast<int16_t>(teamID));
}

// =============================================================================
// SECTION 5 — The replacement function
//
// This is a __cdecl function (the original takes two plain int args with no
// ECX object pointer — verify this in x32dbg by checking that ECX is not
// used as a struct pointer inside the function).
//
// If Ghidra/x32dbg shows it is actually __thiscall, change to __fastcall
// and add a void* edx_unused second parameter as described in the tutorial.
// =============================================================================

// Original function type for MinHook trampoline
typedef void (__cdecl *FN_menu_LoadPageTeams)(int param_1, int pageIndex);
static FN_menu_LoadPageTeams orig_menu_LoadPageTeams = nullptr;

void __cdecl hook_menu_LoadPageTeams(int side, int pageIndex)
{
    // ── Re-entrancy guard ────────────────────────────────────────────────────
    // If fn_MenuPageFinish triggers a recursive call back into this function,
    // we detect it here and call the original directly to break the cycle.
    static bool s_insideHook = false;

    if (s_insideHook)
    {
        Logger::Log("[MenuHook] Re-entrant call detected (side=%d page=%d) — "
                    "calling original directly.", side, pageIndex);
        if (orig_menu_LoadPageTeams)
            orig_menu_LoadPageTeams(side, pageIndex);
        return;
    }

    s_insideHook = true;
    // ── Guard: validate pageIndex ────────────────────────────────────────────
    // Original returns immediately on default (invalid page). We do the same.
    if (pageIndex < 1 || pageIndex > PAGE_COUNT)
    {
        Logger::Log("[MenuHook] LoadPageTeams called with invalid pageIndex=%d, ignoring.", pageIndex);
        return;
    }

    Logger::Log("[MenuHook] LoadPageTeams: side=%d page=%d", side, pageIndex);

    // ── Step 1: Call the page setup function (FUN_006094b0) ──────────────────
    // The original calls this unconditionally before doing anything else.
    // It likely resets the slot array for this side. We must call it first.
    if (fn_MenuPageSetup)
        fn_MenuPageSetup(side);
    else
        Logger::Log("[MenuHook] WARNING: fn_MenuPageSetup is null, skipping.");

    // ── Step 2: Get the team ID range for this page ──────────────────────────
    const PageRange& range = PAGE_RANGES[pageIndex];
    uint32_t teamID       = range.firstTeamID;
    const uint32_t lastID = range.lastTeamID;

    // ── Step 3: Iterate teams and populate slots ─────────────────────────────
    int slotIndex = 0; // tracks the next free position in the output array

    while (teamID <= lastID)
    {
        // ── Classic team check (IDs 57–63) ───────────────────────────────────
        // Classic teams are only shown if unlocked.
        // All other teams are always shown.
        bool isClassicTeam = (teamID >= CLASSIC_TEAM_FIRST &&
                              teamID <= CLASSIC_TEAM_LAST);

        if (isClassicTeam)
        {
            // Ask the game whether this classic team has been unlocked.
            // Only add it to the list if the answer is yes (return value == 1).
            // int unlocked = fn_CheckTeamUnlocked
            //     ? fn_CheckTeamUnlocked(static_cast<int>(teamID))
            //     : 0;
            int unlocked = 1;

            if (unlocked == 1)
            {
                WriteTeamSlot(side, slotIndex, static_cast<uint16_t>(teamID));
                slotIndex++;

                Logger::Log("[MenuHook]   Slot %d = team %d (classic, unlocked)",
                    slotIndex - 1, teamID);
            }
            else
            {
                Logger::Log("[MenuHook]   Team %d skipped (classic, locked)", teamID);
            }
        }
        else
        {
            // ── Page 3 layout quirk: skip to slot 40 at team 160 ─────────────
            // The original has a hard-coded jump to teamPositionBlock = 40
            // when on page 3 and teamID == 160. We preserve this exactly.
            // This likely exists to leave a visual gap in the menu grid
            // between two groups of teams on page 3.
            if (pageIndex == 3 && teamID == 160)
            {
                Logger::Log("[MenuHook]   Page 3 layout gap: advancing slot to 40 at team 160");
                slotIndex = 40;
            }

            WriteTeamSlot(side, slotIndex, static_cast<uint16_t>(teamID));
            slotIndex++;

            Logger::Log("[MenuHook]   Slot %d = team %d", slotIndex - 1, teamID);
        }

        teamID++;
    }

    // ── Step 4: Page 4 DB team special case ─────────────────────────────────
    // On page 4, if _db_GetDB() returns 0 (DB not active), append the
    // special DB team (ID 0xD1 = 209) at the current slot position.
    // The target array differs depending on which side is picking.
    if (pageIndex == 4)
    {
        int dbActive = fn_GetMasterLeagueLoadedTeam ? fn_GetMasterLeagueLoadedTeam() : 1; // assume active if fn missing

        if (dbActive == 0)
        {
            Logger::Log("[MenuHook]   Page 4: DB inactive, appending DB team (0xD1) at slot %d", slotIndex);

            if (side == 0)
            {
                // Side 0 writes to the primary array (DAT_04de9fa0)
                WriteTeamSlot(side, slotIndex, DB_TEAM_ID);
            }
            else
            {
                // Side 1 writes to the secondary array (DAT_04dea1a8)
                // Note: the original uses a flat slot index here (no side offset)
                WriteTeamSlot2(slotIndex, DB_TEAM_ID);
            }
        }
    }

    // ── Step 5: Call the page finish function (FUN_005ee610) ─────────────────
    // The original calls this on BOTH exit paths (the goto and the fall-through).
    // It is always called — we call it unconditionally here.
    Logger::Log("[MenuHook] About to call fn_MenuPageFinish...");

    if (fn_MenuPageFinish)
    {
        uint32_t* pFlags = reinterpret_cast<uint32_t*>(ADDR_MenuFlagsVar);
        uint32_t  mask   = (side == 0) ? (0x1):(0x2);

        Logger::Log("[MenuHook] Setting menu flags: *0x%08X |= 0x%X (side=%d)",
        ADDR_MenuFlagsVar, mask, side);

        fn_MenuPageFinish(pFlags, mask);

        Logger::Log("[MenuHook] Menu flags set OK.");
    }
    else
    {
        Logger::Log("[MenuHook] fn_MenuPageFinish is null, skipping.");
    }

    Logger::Log("[MenuHook] LoadPageTeams complete: %d slots filled.", slotIndex);
    s_insideHook = false;  // <- must be at every exit point

}

// =============================================================================
// SECTION 6 — Hook registration
// =============================================================================

void MenuHooks::Register()
{
    if (!Config::GetBool("general", "enabled", true))
    {
        Logger::Log("[MenuHook] Disabled by config, skipping.");
        return;
    }

    // ── Resolve all external function pointers ───────────────────────────────
    // We cast the known addresses directly to function pointers.
    // These are NOT hooked — we just want to call the original game functions.
    // If any address is 0 (not yet found), we log a warning but continue —
    // the hook guards against null pointers at call sites above.

    fn_MenuPageSetup    = reinterpret_cast<FN_MenuPageSetup>(ADDR_MenuPageSetup);
    fn_MenuPageFinish   = reinterpret_cast<FN_MenuPageFinish>(ADDR_MenuPageFinish);
    fn_CheckTeamUnlocked= reinterpret_cast<FN_CheckTeamUnlocked>(ADDR_CheckTeamUnlocked);
    fn_GetMasterLeagueLoadedTeam= reinterpret_cast<FN_GetMasterLeagueLoadedTeam>(ADDR_GetMasterLeagueLoadedTeam);

    if (ADDR_CheckTeamUnlocked == 0)
        Logger::Log("[MenuHook] WARNING: ADDR_CheckTeamUnlocked not set — classic teams will be hidden.");
    if (ADDR_GetMasterLeagueLoadedTeam == 0)
        Logger::Log("[MenuHook] WARNING: ADDR_GetDB not set — DB team will never appear on page 4.");

    // ── Find the target function address ─────────────────────────────────────
    // Address is for the retail PES6 1.0 EXE. If you target a different build,
    // re-locate it (e.g. with a pattern scan) and update the constant below.
    static const uintptr_t ADDR_menu_LoadPageTeams = 0x006094f0;

    // ── Create and enable the hook ───────────────────────────────────────────
    MH_STATUS s = MH_CreateHook(
        reinterpret_cast<LPVOID>(ADDR_menu_LoadPageTeams),
        reinterpret_cast<LPVOID>(&hook_menu_LoadPageTeams),
        reinterpret_cast<LPVOID*>(&orig_menu_LoadPageTeams)
    );

    if (s != MH_OK)
    {
        Logger::Log("[MenuHook] ERROR: MH_CreateHook failed for menu_LoadPageTeams (%d)", s);
        return;
    }

    s = MH_EnableHook(reinterpret_cast<LPVOID>(ADDR_menu_LoadPageTeams));
    if (s != MH_OK)
    {
        Logger::Log("[MenuHook] ERROR: MH_EnableHook failed for menu_LoadPageTeams (%d)", s);
        return;
    }

    Logger::Log("[MenuHook] menu_LoadPageTeams hook installed at 0x%08X.",
        ADDR_menu_LoadPageTeams);
}