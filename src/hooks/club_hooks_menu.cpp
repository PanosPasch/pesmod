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
// club_hooks_menu.cpp
//
// Hook for FUN_009e25a0 — LoadCompetitionMenuTeams
//
// Populates the competition/cup front-end team selection menus.
// Routes between Cup mode (MODE_FLAG==2, calls FUN_009d3bc0) and
// League/Exhibition mode (MODE_FLAG!=2, calls FUN_009d2880).
//
// ASM note: CMP [DAT_03be12c9],2 / JNZ LAB_009e2a81
//   JNZ takes the branch when MODE_FLAG != 2 -> that branch is normal mode.
//   Fall-through (MODE_FLAG == 2) is Cup mode.
//   Ghidra's pseudocode had the two paths inverted.
// =============================================================================

#include "club_hooks_common.h"
#include "../utils/logger.h"

FN_LoadCompetitionMenuTeams_t orig_LoadCompetitionMenuTeams = nullptr;

void __stdcall hook_LoadCompetitionMenuTeams()
{
    Logger::Log("[CompMenu] LoadCompetitionMenuTeams. ModeFlag=%d", (int)MODE_FLAG);

    TeamSlotBuffer teamSlots;
    FillSentinels(teamSlots.teamIDs);

    // =========================================================================
    // MODE_FLAG == 2: Cup mode (calls FUN_009d3bc0)
    // =========================================================================
    if (MODE_FLAG == 2)
    {
        int selectedCup = fn_GetSelectedCupIdx ? fn_GetSelectedCupIdx(1) : -1;
        Logger::Log("[CompMenu] Cup mode. selectedCup=%d", selectedCup);

        switch (selectedCup)
        {
        // ── case 1: World Cup — national teams only, navigate to national list
        case 1:
            if (fn_SetNavState) fn_SetNavState(DISPLAY_HANDLE, 1, 1, 0);
            break;

        // ── case 2: European cup — two pages (Euro teams + unlockable extras)
        case 2:
        {
            // First list: Euro teams
            FillSentinels(teamSlots.teamIDs);
            {
                int32_t first = GDWORD(0x00d5bef0);
                int32_t last  = GDWORD(0x00d5bef4);
                int idx = 0;
                for (int32_t id = first; id <= last; id++)
                    teamSlots.teamIDs[idx++] = id;
            }
            if (fn_GetStringByHandle)
                CopyStr(teamSlots.name, fn_GetStringByHandle(GDWORD(0x00d5be50)));
            if (fn_SetTeamList)
                fn_SetTeamList(DISPLAY_HANDLE, 0, reinterpret_cast<int>(&teamSlots));
            if (fn_SetSelectedTeam)
                fn_SetSelectedTeam(DISPLAY_HANDLE, 0, (uint16_t)teamSlots.teamIDs[0]);

            // Second list: another Euro group + unlockable classic teams
            FillSentinels(teamSlots.teamIDs);
            {
                int32_t first = GDWORD(0x00d5bef8);
                int32_t last  = GDWORD(0x00d5befc);
                int idx = 0;
                for (int32_t id = first; id <= last; id++)
                    teamSlots.teamIDs[idx++] = id;

                if (fn_GetStringByHandle)
                    CopyStr(teamSlots.name, fn_GetStringByHandle(GDWORD(0x00d5be54)));

                int32_t extBase = GDWORD(0x00d5bf98);
                int32_t extLast = GDWORD(0x00d5bf9c);
                for (int i = 0; i < UNLOCKABLES_ALLOC_INDEX_MAX; i++)
                {
                    if (extBase == -1 || extBase + i > extLast) break;
                    if (fn_IsTeamUnlocked &&
                        fn_IsTeamUnlocked((uint16_t)(extBase + i)) == 1)
                        teamSlots.teamIDs[idx++] = extBase + i;
                }
            }
            if (fn_SetTeamList)
                fn_SetTeamList(DISPLAY_HANDLE, 1, reinterpret_cast<int>(&teamSlots));
            break;
        }

        // ── case 3: Cup with a single team group (e.g. Asian Cup)
        case 3:
        {
            FillSentinels(teamSlots.teamIDs);
            int32_t first = GDWORD(0x00d5bf00);
            int32_t last  = GDWORD(0x00d5bf04);
            int idx = 0;
            for (int32_t id = first; id <= last; id++) teamSlots.teamIDs[idx++] = id;
            if (fn_GetStringByHandle)
                CopyStr(teamSlots.name, fn_GetStringByHandle(GDWORD(0x00d5be58)));
            if (fn_SetTeamList)
                fn_SetTeamList(DISPLAY_HANDLE, 0, reinterpret_cast<int>(&teamSlots));
            if (fn_SetSelectedTeam)
                fn_SetSelectedTeam(DISPLAY_HANDLE, 0, (uint16_t)teamSlots.teamIDs[0]);
            break;
        }

        // ── case 4: Cup with unlock check (e.g. Copa Libertadores)
        case 4:
        {
            FillSentinels(teamSlots.teamIDs);
            int32_t first = GDWORD(0x00d5bf08);
            int32_t last  = GDWORD(0x00d5bf0c);
            int idx = 0;
            for (int32_t id = first; id <= last; id++) teamSlots.teamIDs[idx++] = id;
            if (fn_GetStringByHandle)
                CopyStr(teamSlots.name, fn_GetStringByHandle(GDWORD(0x00d5be5c)));
            {
                int32_t extBase = GDWORD(0x00d5bfa8);
                int32_t extLast = GDWORD(0x00d5bfac);
                for (int i = 0; i < UNLOCKABLES_ALLOC_INDEX_MAX; i++)
                {
                    if (extBase == -1 || extBase + i > extLast) break;
                    if (fn_IsTeamUnlocked &&
                        fn_IsTeamUnlocked((uint16_t)(extBase + i)) == 1)
                        teamSlots.teamIDs[idx++] = extBase + i;
                }
            }
            if (fn_SetTeamList)
                fn_SetTeamList(DISPLAY_HANDLE, 0, reinterpret_cast<int>(&teamSlots));
            if (fn_SetSelectedTeam)
                fn_SetSelectedTeam(DISPLAY_HANDLE, 0, (uint16_t)teamSlots.teamIDs[0]);
            break;
        }

        // ── case 5: Cup with a single group (e.g. African Nations)
        case 5:
        {
            FillSentinels(teamSlots.teamIDs);
            int32_t first = GDWORD(0x00d5bf10);
            int32_t last  = GDWORD(0x00d5bf14);
            int idx = 0;
            for (int32_t id = first; id <= last; id++) teamSlots.teamIDs[idx++] = id;
            if (fn_GetStringByHandle)
                CopyStr(teamSlots.name, fn_GetStringByHandle(GDWORD(0x00d5be60)));
            if (fn_SetTeamList)
                fn_SetTeamList(DISPLAY_HANDLE, 0, reinterpret_cast<int>(&teamSlots));
            if (fn_SetSelectedTeam)
                fn_SetSelectedTeam(DISPLAY_HANDLE, 0, (uint16_t)teamSlots.teamIDs[0]);
            break;
        }

        // ── case 6: Konami Cup — routes by club mode selector
        //   cupClubMode==0 -> navigate to national list (SetNavState 1,1,0)
        //   cupClubMode==1 -> navigate to club list    (SetNavState 0,1,1)
        //   cupClubMode==2 -> FALL THROUGH into case 0 (all teams, SetNavState 1,1,1)
        //   This is a deliberate C switch fall-through — do NOT add break for cupClubMode==2
        case 6:
        {
            int cupClubMode = fn_GetSelectedCupIdx ? fn_GetSelectedCupIdx(2) : -1;
            Logger::Log("[CompMenu] Cup mode case 6. cupClubMode=%d", cupClubMode);

            if (cupClubMode == 0)
            {
                if (fn_SetNavState) fn_SetNavState(DISPLAY_HANDLE, 1, 1, 0);
                break;
            }
            else if (cupClubMode == 1)
            {
                if (fn_SetNavState) fn_SetNavState(DISPLAY_HANDLE, 0, 1, 1);
                break;
            }
            // cupClubMode == 2: intentional fall-through into case 0
            [[fallthrough]];
        }

        // ── case 0: All teams (Reebok Cup / fall-through from case 6 sub==2)
        case 0:
            if (fn_SetNavState) fn_SetNavState(DISPLAY_HANDLE, 1, 1, 1);
            break;

        default:
            break;
        }
    }
    // =========================================================================
    // MODE_FLAG != 2: League / Exhibition mode (calls FUN_009d2880)
    // =========================================================================
    else
    {
        int leagueCase = fn_GetLeagueSelector ? fn_GetLeagueSelector(1) : -1;
        Logger::Log("[CompMenu] League mode. leagueCase=%d", leagueCase);

        // ASM switch table at 009e2a92:
        //   0 -> DAT_00d5bf40/44, FUN_00861640(4)
        //   1 -> DAT_00d5bf48/4c, FUN_00861640(5)
        //   2 -> DAT_00d5bf50/54, FUN_00861640(6)
        //   3 -> DAT_00d5bf38/3c, FUN_00861640(2)
        //   4 -> DAT_00d5bf30/34, FUN_00861640(1)
        //   5 -> sub-mode routing (National/Club/All)

        bool doSetList0 = false;

        switch (leagueCase)
        {
        case 0:
        {
            FillSentinels(teamSlots.teamIDs);
            int32_t first = GDWORD(0x00d5bf40), last = GDWORD(0x00d5bf44);
            int idx = 0;
            for (int32_t id = first; id <= last; id++) teamSlots.teamIDs[idx++] = id;
            if (fn_GetLeagueString) CopyStr(teamSlots.name, fn_GetLeagueString(4));
            doSetList0 = true;
            break;
        }
        case 1:
        {
            FillSentinels(teamSlots.teamIDs);
            int32_t first = GDWORD(0x00d5bf48), last = GDWORD(0x00d5bf4c);
            int idx = 0;
            for (int32_t id = first; id <= last; id++) teamSlots.teamIDs[idx++] = id;
            if (fn_GetLeagueString) CopyStr(teamSlots.name, fn_GetLeagueString(5));
            doSetList0 = true;
            break;
        }
        case 2:
        {
            FillSentinels(teamSlots.teamIDs);
            int32_t first = GDWORD(0x00d5bf50), last = GDWORD(0x00d5bf54);
            int idx = 0;
            for (int32_t id = first; id <= last; id++) teamSlots.teamIDs[idx++] = id;
            if (fn_GetLeagueString) CopyStr(teamSlots.name, fn_GetLeagueString(6));
            doSetList0 = true;
            break;
        }
        case 3:
        {
            FillSentinels(teamSlots.teamIDs);
            int32_t first = GDWORD(0x00d5bf38), last = GDWORD(0x00d5bf3c);
            int idx = 0;
            for (int32_t id = first; id <= last; id++) teamSlots.teamIDs[idx++] = id;
            if (fn_GetLeagueString) CopyStr(teamSlots.name, fn_GetLeagueString(2));
            doSetList0 = true;
            break;
        }
        case 4:
        {
            FillSentinels(teamSlots.teamIDs);
            int32_t first = GDWORD(0x00d5bf30), last = GDWORD(0x00d5bf34);
            int idx = 0;
            for (int32_t id = first; id <= last; id++) teamSlots.teamIDs[idx++] = id;
            if (fn_GetLeagueString) CopyStr(teamSlots.name, fn_GetLeagueString(1));
            doSetList0 = true;
            break;
        }
        case 5:
        {
            // International League — routes by club/national/all sub-mode
            //   sub==0 -> National only   (SetNavState 1,1,0)
            //   sub==1 -> Clubs only      (SetNavState 0,1,1)
            //   sub==2 -> All teams       (SetNavState 1,1,1)

            /** Default Konami Code */
            // int clubTeamMode = fn_GetLeagueSelector ? fn_GetLeagueSelector(3) : -1;
            // Logger::Log("[CompMenu] League case 5. clubTeamMode=%d", clubTeamMode);
            // if      (clubTeamMode == 0) { if (fn_SetNavState) fn_SetNavState(DISPLAY_HANDLE, 1, 1, 0); }
            // else if (clubTeamMode == 1) { if (fn_SetNavState) fn_SetNavState(DISPLAY_HANDLE, 0, 1, 1); }
            // else if (clubTeamMode == 2) { if (fn_SetNavState) fn_SetNavState(DISPLAY_HANDLE, 1, 1, 1); }
            // break;

            /** Panos Modified Code */
            FillSentinels(teamSlots.teamIDs);
            teamSlots.teamIDs[0] = 1;
            teamSlots.teamIDs[1] = 50;
            teamSlots.teamIDs[2] = 80;
            teamSlots.teamIDs[3] = 100;
            teamSlots.teamIDs[4] = 200;
            CopyStr(teamSlots.name, "Test Competition Here");
            doSetList0 = false;
            fn_SetTeamList(DISPLAY_HANDLE, 0, reinterpret_cast<int>(&teamSlots));

            FillSentinels(teamSlots.teamIDs);
            teamSlots.teamIDs[0] = 2;
            teamSlots.teamIDs[1] = 52;
            teamSlots.teamIDs[2] = 82;
            teamSlots.teamIDs[3] = 102;
            teamSlots.teamIDs[4] = 202;
            CopyStr(teamSlots.name, "Test Competition Here B Page");
            fn_SetTeamList(DISPLAY_HANDLE, 1, reinterpret_cast<int>(&teamSlots));
            break;
        }
        default:
            break;
        }

        // LAB_009e2dff: all cases 0-4 commit their team list to display slot 0
        if (doSetList0 && fn_SetTeamList)
        {
            fn_SetTeamList(DISPLAY_HANDLE, 0, reinterpret_cast<int>(&teamSlots));
            Logger::Log("[CompMenu] League list committed, first=%d",
                teamSlots.teamIDs[0]);
        }
    }

    // =========================================================================
    // caseD_7 — post-load update (all paths converge here)
    // =========================================================================
    if (fn_PostLoadUpdate) fn_PostLoadUpdate();

    // ── Conditional display reset ─────────────────────────────────────────
    if (MODE_FLAG == 2)
    {
        int s1 = fn_GetSelectedCupIdx ? fn_GetSelectedCupIdx(1) : -1;
        if (s1 == 6)
        {
            int s2 = fn_GetSelectedCupIdx ? fn_GetSelectedCupIdx(2) : -1;
            if (s2 == 2 && fn_DisplayReset)
            {
                fn_DisplayReset(DISPLAY_HANDLE, 0);
                return;
            }
        }
        else if (s1 == 0 && fn_DisplayReset)
        {
            fn_DisplayReset(DISPLAY_HANDLE, 0);
            return;
        }
    }
    else
    {
        int ls = fn_GetLeagueSelector ? fn_GetLeagueSelector(1) : -1;
        if (ls == 5)
        {
            int sub = fn_GetLeagueSelector ? fn_GetLeagueSelector(3) : -1;
            if (sub == 2 && fn_DisplayReset)
                fn_DisplayReset(DISPLAY_HANDLE, 0);
        }
    }

    Logger::Log("[CompMenu] Complete.");
}
