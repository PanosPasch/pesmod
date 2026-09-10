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

#pragma once

// =============================================================================
// club_hooks.h
// Master header for all club/league selection screen hooks.
//
// Split into logical units:
//   club_hooks_common.h   — shared types, macros, structs, helpers
//   club_hooks_slots.cpp  — FUN_009ed730: LoadClubTeamSlots
//   club_hooks_menu.cpp   — FUN_009e25a0: LoadCompetitionMenuTeams
//   club_hooks_screen.cpp — FUN_009ee940: InitClubSelectionScreen
//                           FUN_00950490: GetBoneWorldPosition
//   club_hooks_panel.cpp  — FUN_00b09050: CreateLeagueSelectionPanel
//   club_hooks_register.cpp — all hook/pointer registration
// =============================================================================

namespace ClubHooks
{
    void Register();
}
