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
    Logger::Log("[Hooks] Installing hooks...");
 
    // Each module's Register() function creates and enables its own hooks.
    // To disable a group of hooks, comment out the relevant line.
    ClubHooks::Register();
    LeagueTeamsHook::Register();

    // D3D8 interception layer. Self-gating: does nothing unless
    // [render] enabled=1 in PESMod.ini, so this line is safe to leave in.
    RenderHooks::Register();

    // Optional / experimental hook groups (disabled by default):
    // MenuHooks::Register();
    // PlayerHooks::Register();

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

