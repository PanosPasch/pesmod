# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copyright (C) 2026 Panagiotis Paschalis
#
# This file is part of PESMod.
#
# PESMod is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option)
# any later version.
#
# PESMod is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along
# with PESMod.  If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.20)
project(PESMod VERSION 1.0.0 LANGUAGES CXX)
 
# ── CRITICAL: 32-bit build ───────────────────────────────────────────────
# PES5 and PES6 are 32-bit processes. The ASI must also be 32-bit.
# If you load a 64-bit DLL into a 32-bit process, Windows refuses silently.
set(CMAKE_GENERATOR_PLATFORM Win32)
 
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
 
# ── Source files ─────────────────────────────────────────────────────────
set(SOURCES
    src/dllmain.cpp
    src/mod_core.cpp
    src/hooks/hooks_registry.cpp
    src/hooks/player_hooks.cpp
    src/hooks/menu_hooks.cpp
    src/hooks/club_hooks_set_team_list.cpp
    src/hooks/club_hooks_slots.cpp
    src/hooks/club_hooks_menu.cpp
    src/hooks/club_hooks_screen.cpp
    src/hooks/club_hooks_kit_data.cpp
    src/hooks/club_hooks_squad_data.cpp
    src/hooks/club_hooks_panel_destroy.cpp
    src/hooks/club_hooks_panel.cpp
    src/hooks/club_hooks_panel_v2.cpp
    src/hooks/club_hooks_panel_nav.cpp
    src/hooks/club_hooks_panel_refresh.cpp
    src/hooks/custom_logo_loader.cpp
    src/hooks/custom_team_loader.cpp
    src/hooks/club_hooks_leagues.cpp
    src/hooks/club_hooks_register.cpp
    src/hooks/league_teams_hook.cpp
    src/patching/patcher.cpp
    src/utils/logger.cpp
    src/utils/config.cpp
)
 
# ── Build as shared library ───────────────────────────────────────────────
add_library(PESMod SHARED ${SOURCES})
 
# ── Include paths ────────────────────────────────────────────────────────
target_include_directories(PESMod PRIVATE
    src
    include
)
 
# ── Link MinHook static library + Windows Imaging (WIC) for PNG decode ────
target_link_libraries(PESMod PRIVATE
    ${CMAKE_SOURCE_DIR}/include/MinHook/lib/MinHook.x86.lib
    windowscodecs   # WIC (IWICImagingFactory) — custom logo PNG decode
    ole32           # CoCreateInstance / CoInitializeEx
)
 
# ── Output as .asi (not .dll) ────────────────────────────────────────────
# This is the only real difference from a standard DLL project.
# The ASI Loader calls LoadLibraryA on any *.asi file it finds,
# so the extension is cosmetic — the file is a standard Win32 DLL.
set_target_properties(PESMod PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
)
 
# ── Post-build: optionally deploy to the game folder ─────────────────────
# Set -DGAME_DIR=<path> to your PES5/PES6 install. When that folder exists,
# CMake copies PESMod.asi *and* the required MinHook.x86.dll there after
# every successful build. When it does not exist, deployment is skipped so
# a fresh checkout still builds cleanly.
set(GAME_DIR "C:/Games/PES6" CACHE PATH "Path to PES5 or PES6 game folder")

if(EXISTS "${GAME_DIR}")
    add_custom_command(TARGET PESMod POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:PESMod> "${GAME_DIR}/PESMod.asi"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/include/MinHook/lib/MinHook.x86.dll"
            "${GAME_DIR}/MinHook.x86.dll"
        COMMENT "Deploying PESMod.asi and MinHook.x86.dll to ${GAME_DIR}"
    )
else()
    message(STATUS
        "PESMod: GAME_DIR '${GAME_DIR}' not found - skipping auto-deploy. "
        "Pass -DGAME_DIR=<game folder> to enable it.")
endif()
 
# ── Compiler flags ───────────────────────────────────────────────────────
target_compile_definitions(PESMod PRIVATE
    WIN32
    _WINDOWS
    _USRDLL
    # Uncomment for debug builds:
    # PESMOD_DEBUG
)
 
# RTTI is disabled (/GR-) for smaller mod code — nothing here uses it.
# Exceptions are enabled (/EHsc): the STL containers and the INI parser
# (std::stof / std::stoi in utils/config.cpp) rely on them.
if(MSVC)
    target_compile_options(PESMod PRIVATE /W4 /GR- /EHsc)
endif()


