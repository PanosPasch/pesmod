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

// =============================================================================
// club_hooks_slots.cpp
//
// Hook for FUN_009ed730 — LoadClubTeamSlots
//
// Populates the per-side league/team slot arrays for the club selection panel.
// Called twice by InitClubSelectionScreen: once for side 0, once for side 1.
//
// Each of the 20 loop iterations (ESI = 0..0x98 step 8) reads a league's
// team ID range and availability data, fills a TeamSlotBuffer, then commits
// it to the display panel via FUN_00b07b00 (fn_SetTeamList).
// =============================================================================

#include "club_hooks_common.h"
#include "../utils/logger.h"

FN_LoadClubTeamSlots_t orig_LoadClubTeamSlots = nullptr;

// -----------------------------------------------------------------------------
// String handle table simulation
//
// The original function keeps a walking stack pointer (local_4c4) that starts
// 32 bytes before a small table {1,2,4,5,6} at [esp+20..esp+30].
// The pointer advances by 4 bytes every loop iteration regardless of whether
// the current slot uses it. Slots 0-7 dereference garbage (but those slots
// never have strHandle==-2). Slots 8-12 dereference {1,2,4,5,6} in order.
// We replicate this as a lookup table indexed by leagueSlotIndex (EBP).
// -----------------------------------------------------------------------------
static const int DYNAMIC_STRING_TABLE_REF[20] =
{
    0, 0, 0, 0, 0, 0, 0, 0,  // slots 0-7:  pointer in pre-table garbage region
    1, 2, 4, 5, 6,            // slots 8-12: the five real table values
    0, 0, 0, 0, 0, 0, 0       // slots 13-19: pointer past the table
};

// -----------------------------------------------------------------------------
// hook_LoadClubTeamSlots
// -----------------------------------------------------------------------------
void __cdecl hook_LoadClubTeamSlots(int side)
{
    Logger::Log("[ClubSlots] LoadClubTeamSlots side=%d", side);

    TeamSlotBuffer teamSlots;

    // local_4c0 (pageStart): display slot index at page boundaries 0x40 and 0x80
    int leagueSlotAssignedAllocation = 0;

    // local_4c8 (totalCount): sum of all fill counts across all iterations
    int totalCount = 0;

    // EBP: slot iterator 0..19
    int leagueSlotIndex = 0;

    // ESI: byte offset into the stride-8 data arrays, 0..0x98
    for (int teamAllocOffsetPointer = 0;
         teamAllocOffsetPointer < TEAM_ALLOCATION_BUFFER_MAX_SIZE;
         teamAllocOffsetPointer += TEAM_ALLOCATION_BUFFER_STEP)
    {
        // Reset buffer: zero everything, then fill team ID sentinels
        memset(&teamSlots, 0, sizeof(teamSlots));
        for (int i = 0; i < TEAM_SLOT_COUNT; i++)
            teamSlots.teamIDs[i] = CLUB_NULL_ID;

        int fillCount = 0; // EBX

        // Page boundary: record current display slot index
        if (teamAllocOffsetPointer == 0x40 || teamAllocOffsetPointer == 0x80)
            leagueSlotAssignedAllocation = leagueSlotIndex;

        // Mode check: 0=normal, non-zero=ML/Cup filtered
        int modeActive = fn_GetModeFlag ? fn_GetModeFlag() : 0;

        // Read this slot's data from the stride-8 arrays
        int32_t rangeFirst = SlotInt(BASE_SLOT_FIRST,            teamAllocOffsetPointer);
        int32_t rangeLast  = SlotInt(BASE_SLOT_LAST,             teamAllocOffsetPointer);
        int32_t extBase    = SlotInt(BASE_SLOT_UNLOCKABLES,      teamAllocOffsetPointer);
        int32_t extLast    = SlotInt(BASE_SLOT_UNLOCKABLES_LAST, teamAllocOffsetPointer);

        if (modeActive == 0)
        {
            // ── Normal/exhibition mode: direct fill, no availability check ──
            for (int32_t id = rangeFirst; id <= rangeLast; id++)
            {
                if (id >= 0)
                    teamSlots.teamIDs[fillCount] = id;
                fillCount++;
            }

            // Append unlocked classic/extra teams within the unlockable range
            if (teamAllocOffsetPointer <= UNLOCKABLES_ALLOCATION_BUFFER_MAX_SIZE)
            {
                for (int i = 0; i < UNLOCKABLES_ALLOC_INDEX_MAX; i++)
                {
                    if (extBase == -1 || extBase + i > extLast) break;
                    uint16_t tid = (uint16_t)(extBase + i);
                    if (fn_IsTeamUnlocked && fn_IsTeamUnlocked(tid) == 1)
                    {
                        if (extBase + i >= 0)
                            teamSlots.teamIDs[fillCount] = extBase + i;
                        fillCount++;
                    }
                }
            }
        }
        else
        {
            // ── Filtered mode (ML/Cup): per-team availability check ──
            bool swapSides = (SIDE_SWAP_FLAG & 1) != 0;

            for (int32_t id = rangeFirst; id <= rangeLast; id++)
            {
                // Skip sentinel team ID 0x11e (counts position but not a real team)
                if (id == 0x11e) { fillCount++; continue; }

                uint16_t uid = (uint16_t)id;
                int avail = 0;

                if (!swapSides)
                    avail = (side == 0 && fn_AvailSide0) ? fn_AvailSide0(uid)
                          : (fn_AvailSide1               ? fn_AvailSide1(uid) : 0);
                else
                    avail = (side == 1 && fn_AvailSide0) ? fn_AvailSide0(uid)
                          : (fn_AvailSide1               ? fn_AvailSide1(uid) : 0);

                if (avail != 0)
                {
                    if (id >= 0) teamSlots.teamIDs[fillCount] = id;
                    fillCount++;
                }
            }

            // Append unlocked classic/extra teams (filtered mode)
            if (teamAllocOffsetPointer <= UNLOCKABLES_ALLOCATION_BUFFER_MAX_SIZE)
            {
                for (int i = 0; i < UNLOCKABLES_ALLOC_INDEX_MAX; i++)
                {
                    if (extBase == -1 || extBase + i > extLast) break;

                    uint16_t tid = (uint16_t)((int16_t)extBase + (int16_t)i);
                    bool swap = (SIDE_SWAP_FLAG & 1) != 0;
                    int avail = 0;

                    if (!swap)
                        avail = (side == 0 && fn_AvailSide0) ? fn_AvailSide0(tid)
                              : (fn_AvailSide1               ? fn_AvailSide1(tid) : 0);
                    else
                        avail = (side == 1 && fn_AvailSide0) ? fn_AvailSide0(tid)
                              : (fn_AvailSide1               ? fn_AvailSide1(tid) : 0);

                    if (avail == 1)
                    {
                        if (extBase + i >= 0)
                            teamSlots.teamIDs[fillCount] = extBase + i;
                        fillCount++;
                    }
                }
            }
        }

        // ── String / name lookup ──────────────────────────────────────────
        // strHandle == -1: no name for this slot
        // strHandle == -2: use FUN_00861640 with value from DYNAMIC_STRING_TABLE_REF
        // strHandle >= 0 : use FUN_0094f2c0 (AFS string by handle)
        int32_t strHandle = *reinterpret_cast<int32_t*>(
            BASE_SLOT_STR + leagueSlotIndex * 4);

        Logger::Log("[ClubSlots] Slot %d (esi=0x%X): StrHandle=0x%X",
            leagueSlotIndex, teamAllocOffsetPointer, (uint32_t)strHandle);

        if (strHandle != -1)
        {
            const char* str = nullptr;

            if (strHandle == -2)
            {
                // Simulate *local_4c4 dereference — see DYNAMIC_STRING_TABLE_REF
                int tableVal = DYNAMIC_STRING_TABLE_REF[leagueSlotIndex];
                if (tableVal != 0 && fn_GetLeagueString)
                {
                    char* result = fn_GetLeagueString(tableVal);
                    Logger::Log("[ClubSlots] Slot %d: FUN_00861640(%d) -> '%s'",
                        leagueSlotIndex, tableVal, result ? result : "(null)");
                    str = result;
                }
            }
            else
            {
                if (fn_GetStringByHandle)
                    str = fn_GetStringByHandle(strHandle);
            }

            if (str) CopyStr(teamSlots.name, str, sizeof(teamSlots.name));
        }

        // ── Display / logo ID ─────────────────────────────────────────────
        // local_4 in the original: written into TeamSlotBuffer at +0x4A0,
        // read by FUN_00b07b00 and stored into the panel slot's display ID field
        teamSlots.displayID = *reinterpret_cast<int32_t*>(
            BASE_SLOT_DISPID + leagueSlotIndex * 4);

        // ── Commit to display ─────────────────────────────────────────────
        // Only if the first team slot is populated (not the sentinel)
        if (teamSlots.teamIDs[0] != CLUB_NULL_ID)
        {
            if (fn_SetTeamList)
            {
                fn_SetTeamList(DISPLAY_HANDLE_ARR[side],
                               leagueSlotAssignedAllocation,
                               reinterpret_cast<int>(&teamSlots));

                Logger::Log("[ClubSlots] Slot %d (esi=0x%X): %d teams, "
                            "displaySlot=%d displayID=0x%X",
                    leagueSlotIndex, teamAllocOffsetPointer, fillCount,
                    leagueSlotAssignedAllocation, teamSlots.displayID);
            }
            leagueSlotAssignedAllocation++;
        }

        // Update running totals and per-slot count output
        totalCount += fillCount;
        SLOT_COUNT_OUT[leagueSlotIndex] = (int8_t)(fillCount - 1);
        leagueSlotIndex++;

    } // end main loop

    // ── Fallback: filtered mode returned zero teams ───────────────────────
    // If ML/Cup mode is active but no teams were found across all slots,
    // fill all 20 slots unconditionally from the stride-8 data arrays.
    {
        int modeCheck = fn_GetModeFlag ? fn_GetModeFlag() : 0;
        if (modeCheck != 0 && totalCount == 0)
        {
            Logger::Log("[ClubSlots] Fallback fill (filtered mode, 0 teams).");

            for (int s = 0; s < TEAM_SLOT_COUNT; s++)
            {
                memset(&teamSlots, 0, sizeof(teamSlots));
                for (int i = 0; i < TEAM_SLOT_COUNT; i++)
                    teamSlots.teamIDs[i] = CLUB_NULL_ID;

                // Fallback uses ESI*8 scale (s*8 not s*4+4)
                int32_t first = *reinterpret_cast<int32_t*>(BASE_SLOT_FIRST + s * 8);
                int32_t last  = *reinterpret_cast<int32_t*>(BASE_SLOT_LAST  + s * 8);

                int idx = 0;
                for (int32_t id = first; id <= last; id++)
                    if (id >= 0) teamSlots.teamIDs[idx++] = id;

                int32_t fbStr = *reinterpret_cast<int32_t*>(BASE_SLOT_STR + s * 4);
                if (fbStr != -1)
                {
                    const char* str = nullptr;
                    if (fbStr == -2 && fn_GetLeagueString)
                    {
                        int tv = DYNAMIC_STRING_TABLE_REF[s];
                        if (tv != 0) str = fn_GetLeagueString(tv);
                    }
                    else if (fn_GetStringByHandle)
                        str = fn_GetStringByHandle(fbStr);

                    if (str) CopyStr(teamSlots.name, str, sizeof(teamSlots.name));
                }

                teamSlots.displayID = *reinterpret_cast<int32_t*>(
                    BASE_SLOT_DISPID + s * 4);

                if (fn_SetTeamList)
                    fn_SetTeamList(DISPLAY_HANDLE_ARR[side], s,
                                   reinterpret_cast<int>(&teamSlots));
            }
        }
    }

    // ── INI overrides ─────────────────────────────────────────────────────
    // For every leagues/<n>.ini in [0, MAX_PANEL_SLOTS), load it and override
    // panel slot n via fn_SetTeamList. Same call the original loop uses, so
    // this just replaces what was placed there earlier in this function.
    ApplyLeagueIniOverrides(side);


    // ── Page labels ───────────────────────────────────────────────────────
    // Set the three tab labels: "National" (0), "Clubs" (1), "ML" (2)
    // Label 2 only shown in normal/exhibition mode (modeActive == 0)
    if (fn_GetStringByHandle && fn_SetLabel)
    {
        int hdl = DISPLAY_HANDLE_ARR[side];
        fn_SetLabel(hdl, 0, fn_GetStringByHandle(0x6c006e));
        fn_SetLabel(hdl, 1, fn_GetStringByHandle(0x6c006f));

        int mc = fn_GetModeFlag ? fn_GetModeFlag() : 0;
        if (mc == 0)
        {
            // ASM: PUSH DAT_012c0043 — address 0x012c0043 as a literal handle value
            const char* label2 = fn_GetStringByHandle(0x012c0043);
            if (label2) fn_SetLabel(hdl, 2, label2);
        }
    }

    // ── Scroll page activation ────────────────────────────────────────────
    if (fn_SetPage)
        fn_SetPage(DISPLAY_HANDLE_ARR[side], 1, 0);

    Logger::Log("[ClubSlots] Complete. totalCount=%d", totalCount);
}
