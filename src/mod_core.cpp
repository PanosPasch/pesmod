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

// mod_core.cpp
#include "mod_core.h"
#include "MinHook/include/MinHook.h"
#include "hooks/hooks_registry.h"
#include "patching/patcher.h"
#include "utils/logger.h"
#include "utils/config.h"
 
void ModInitialise(HMODULE hModule)
{
    // ── Step 1: Load configuration ────────────────────────────────────
    // Reads PESMod.ini from the game folder. Done first so logging
    // verbosity and the master enable flag are known before anything else.
    Config::Load("PESMod.ini");
    const bool verbose = Config::GetBool("debug",   "verbose_logging", false);
    const bool enabled = Config::GetBool("general", "enabled",         true);

    // ── Step 2: Initialise logger ─────────────────────────────────────
    // Always writes PESMod.log; only opens a live console when verbose.
    Logger::Init("PESMod.log", verbose);
    Logger::Log("[PESMod] ===========================");
    Logger::Log("[PESMod] PESMod ASI v1.0 loading...");
    Logger::Log("[PESMod] Configuration loaded.");

    // Respect the master switch — leave the game completely untouched
    // when disabled via PESMod.ini.
    if (!enabled)
    {
        Logger::Log("[PESMod] Disabled via PESMod.ini ([general] enabled=0); "
                    "no hooks installed.");
        return;
    }

    // ── Step 3: Initialise MinHook ────────────────────────────────────
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK)
    {
        Logger::Log("[PESMod] FATAL: MinHook init failed, status=%d", status);
        MessageBoxA(nullptr, "PESMod: MinHook initialisation failed.",
                    "PESMod", MB_OK | MB_ICONERROR);
        return;
    }
    Logger::Log("[PESMod] MinHook initialised.");

    // ── Step 4: Apply EXE patches ─────────────────────────────────────
    // Direct memory patches applied before hooks. None enabled by default.
    // Patcher::ApplyAll();

    // ── Step 5: Install function hooks ────────────────────────────────
    // All hooks registered here via MinHook.
    HooksRegistry::InstallAll();

    Logger::Log("[PESMod] Initialisation complete.");
    Logger::Log("[PESMod] ===========================");
}
 
void ModShutdown()
{
    Logger::Log("[PESMod] Shutting down...");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    Logger::Log("[PESMod] Shutdown complete.");
    Logger::Shutdown();
}

