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

// hooks_registry.cpp
#include "hooks_registry.h"
#include "player_hooks.h"
#include "menu_hooks.h"
#include "club_hooks.h"
#include "league_teams_hook.h"
#include "../render/render_hooks.h"
#include "../utils/logger.h"
#include "../utils/config.h"
#include "MinHook/include/MinHook.h"
 
void HooksRegistry::InstallAll()
{
    // Renderer only: leave every gameplay hook out and install nothing but
    // the D3D8 interception layer.
    //
    // The gameplay mods rewrite team, league and kit data in memory, which
    // changes what the game draws - different kits, different squads,
    // different stadium dressing. That is fine when the point is to play,
    // and unhelpful when the point is to tell whether the renderer is
    // reproducing the game: a difference on screen could be either. This
    // makes the renderer's output attributable to the renderer.
    const bool renderOnly = Config::GetBool("general", "render_only", false);

    Logger::Log(renderOnly ? "[Hooks] Installing hooks (renderer only)..."
                           : "[Hooks] Installing hooks...");

    // Each module's Register() function creates and enables its own hooks.
    // To disable a group of hooks, comment out the relevant line.
    if (!renderOnly)
    {
        ClubHooks::Register();
        LeagueTeamsHook::Register();

        // Optional / experimental hook groups (disabled by default):
        // MenuHooks::Register();
        // PlayerHooks::Register();
    }

    // D3D8 interception layer. Self-gating: does nothing unless
    // [render] enabled=1 in PESMod.ini, so this line is safe to leave in.
    RenderHooks::Register();

    Logger::Log("[Hooks] All hooks installed.");
}

void HooksRegistry::RemoveAll()
{
    // Give the renderer a chance to flush its session report before the
    // hooks that feed it are torn down.
    RenderHooks::Unregister();

    MH_DisableHook(MH_ALL_HOOKS);
    Logger::Log("[Hooks] All hooks removed.");
}

