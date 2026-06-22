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
