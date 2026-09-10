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

// hooks_registry.cpp
#include "hooks_registry.h"
#include "player_hooks.h"
#include "menu_hooks.h"
#include "club_hooks.h"
#include "league_teams_hook.h"
#include "../utils/logger.h"
#include "../utils/config.h"
#include "MinHook/include/MinHook.h"
 
void HooksRegistry::InstallAll()
{
    Logger::Log("[Hooks] Installing hooks...");
 
    // Each module's Register() function creates and enables its own hooks.
    // To disable a group of hooks, comment out the relevant line.
    ClubHooks::Register();
    LeagueTeamsHook::Register();

    // Optional / experimental hook groups (disabled by default):
    // MenuHooks::Register();
    // PlayerHooks::Register();

    Logger::Log("[Hooks] All hooks installed.");
}
 
void HooksRegistry::RemoveAll()
{
    MH_DisableHook(MH_ALL_HOOKS);
    Logger::Log("[Hooks] All hooks removed.");
}

