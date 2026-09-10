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

// =============================================================================
// custom_team_loader.cpp — on-the-fly custom team info from teams/<ID>.ini
//
// Pairs with the existing squads/<ID>.ini (players) and kits/<ID>.ini (kits):
// this supplies the team's IDENTITY — the fields the game keeps in the club
// team record — so a team loaded with an id that isn't in the stock list no
// longer shows as NULL.
//
// Team-data architecture (reverse-engineered):
//   * Club team record  — 0x58 bytes each, array at DAT_01132018, indexed
//     (teamId - 0x40) for ids 0x40..0xCB. Read via GetClubTeamRecord
//     (FUN_00862fc0); the name resolver GetTeamName (FUN_00863570) returns
//     record+0x00 (full name) or record+0x31 (short name). Record layout:
//        +0x00  full name (49 bytes, null-terminated)
//        +0x31  short name (~5 bytes, null-terminated)
//        +0x36  team id (u16)
//        +0x38  "edited" flag
//        +0x3C  edit-slot index (dup of +0x40)
//        +0x40  edit-slot / stadium index (GetTeamEditSlotIdx returns this)
//        +0x48  kit ref, home  (FUN_00863c50 slot 0)
//        +0x4C  kit ref, away  (FUN_00863c50 slot 1)
//        +0x50  per-team byte  (default from DAT_01130f08[id])
//        +0x51  per-team byte  (default from DAT_01130de0[id])
//   * Kit design  — GetClubTeamKitBase (handled by kits/<ID>.ini).
//   * Players     — GetTeamPlayerID/Attr (handled by squads/<ID>.ini).
//
// This loader covers the NAME + short name for any id by hooking GetTeamName.
// The numeric record fields are optional keys, applied for in-range ids.
// =============================================================================

#include "club_hooks_common.h"
#include "../utils/logger.h"

#include <unordered_map>
#include <cstdio>

namespace {

struct CustomTeam {
    bool  exists = false;
    char  fullName[64]  = {0};   // record +0x00
    char  shortName[16] = {0};   // record +0x31
};

std::unordered_map<int, CustomTeam> g_teams;

// Parse teams/<id>.ini once, cache the result (including "no file").
const CustomTeam* GetCustomTeam(int teamId)
{
    auto it = g_teams.find(teamId);
    if (it != g_teams.end())
        return it->second.exists ? &it->second : nullptr;

    CustomTeam& t = g_teams[teamId];   // inserts (default: exists=false)

    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), ".\\teams\\%d.ini", teamId);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return nullptr;   // no file for this id; cached as not-exists

    // GetPrivateProfileString handles ';' comments natively.
    GetPrivateProfileStringA("team", "name", "", t.fullName, sizeof(t.fullName), path);
    GetPrivateProfileStringA("team", "short_name", "", t.shortName, sizeof(t.shortName), path);
    if (t.fullName[0] == '\0')
        return nullptr;   // a name is required; otherwise leave the team stock

    t.exists = true;
    Logger::Log("[CustomTeam] teams/%d.ini name='%s' short='%s'",
                teamId, t.fullName, t.shortName);
    return &t;
}

} // namespace

// FUN_00863570 — GetTeamName(teamId, nameType, param3): nameType 0 = full name,
// 1 = short name. Returns a pointer to the name string. For a team that has a
// teams/<id>.ini we return our own (stable) buffer; everything else forwards.
char* __cdecl hook_GetTeamName(uint16_t teamId, int nameType, int param3)
{
    const CustomTeam* t = GetCustomTeam(teamId);
    if (t) {
        if (nameType == 0) return const_cast<char*>(t->fullName);
        if (nameType == 1) return const_cast<char*>(t->shortName);
        // Other nameType values are game-internal lookups — forward those.
    }
    return orig_GetTeamName
        ? orig_GetTeamName(teamId, nameType, param3)
        : nullptr;
}

// -----------------------------------------------------------------------------
// Team crest — teams/<ID>.png
//
// The club selection (team list, and the home/away match info) loads every
// team's crest through FUN_00b3beb0(scratchId, teamId, 1, 1): it registers the
// crest into a scratch texture id (0x7346..0x73ef), the caller copies the 32x32
// 8bpp palette + pixels into its cell struct, then FUN_00b3be00 releases the
// scratch. A team whose id isn't in the stock crest data (e.g. 3000/3001) loads
// nothing, so the cell draws the white flag.
//
// We hook the resolver: for a team with a teams/<ID>.png we build the crest
// ourselves and register it under the same scratch id (RegisterCustomCrest,
// which mirrors the stock lifecycle — the game frees the scratch per use), then
// return that node so the caller copies our pixels. Everything else forwards.
// -----------------------------------------------------------------------------
int __cdecl hook_CrestResolve(int scratchId, uint16_t teamId, int p3, int p4)
{
    const int node = RegisterCustomCrest(scratchId, teamId);
    if (node)
        return node;

    return orig_CrestResolve
        ? orig_CrestResolve(scratchId, teamId, p3, p4)
        : 0;
}

// -----------------------------------------------------------------------------
// Team badge (club emblem) — teams/<ID>.png
//
// The club-selection badge render FUN_00b0f990(teamId /*ECX*/, nodeStruct /*EAX*/,
// param_1) points its two badge nodes (small + large) at a texture chosen by the
// team's edit slot, via the atlas pipeline (FUN_00b0f8b0 -> FUN_009b02f0 ->
// FUN_009b0ba0). A custom team's edit slot is 0x1ca (out of the [0x124,0x1ba)
// range), so the atlas lookup returns nothing and the render falls back to the
// blank id 0x6a -> the white flag.
//
// We intercept the render: for a team that has a teams/<ID>.png we register that
// PNG as a normal texture (GetCustomEmblemNode) and set BOTH badge nodes to it
// with the default full (0,0,1,1) UV, exactly the way the stock path assigns the
// atlas texture (SetUiNodeTextureWithDefaultUv) — bypassing the atlas entirely.
// Everything else falls through to the original.
//
// There are two sibling entry points that draw the same badge nodes:
//   FUN_00b0f990(teamId /*ECX*/, nodeStruct /*EAX*/, param_1)  — register-args
//   FUN_00b0fa80(nodeStruct, teamId)                           — clean __cdecl
// Both share this nodeStruct layout (from FUN_00b0f8b0):
//   [0x00] -> sub-state S; S+0x14 = badge width, S+0x18 = badge height (float)
//   [0x04] = node handle, small badge
//   [0x08] = node handle, large badge
//   [0x0C] = cached slot
//   [0x10] = active byte; [0x11],[0x12] = per-node visibility bits
// -----------------------------------------------------------------------------
namespace {

using FN_SetNodeTexDefaultUv = void (__cdecl*)(int node, int tex);   // 0x00951e00
using FN_SetNodeQuadFromRect = void (__cdecl*)(int node, float* r);  // 0x00951430
using FN_SetNodeEnabled      = void (__cdecl*)(int node, int on);    // 0x00951310

const auto SetNodeTexDefaultUv = reinterpret_cast<FN_SetNodeTexDefaultUv>(0x00951e00);
const auto SetNodeQuadFromRect = reinterpret_cast<FN_SetNodeQuadFromRect>(0x00951430);
const auto SetNodeEnabled      = reinterpret_cast<FN_SetNodeEnabled>     (0x00951310);

// Draw teams/<id>.png onto both badge nodes, bypassing the atlas. Returns true
// if it handled the badge (custom team with a PNG), false to run the original.
bool DrawCustomBadge(int nodeStruct, int teamIdRaw)
{
    const int teamId = teamIdRaw & 0xFFFF;
    if (teamId == 0xFFFF || nodeStruct == 0)
        return false;

    const int tex = GetCustomEmblemNode(teamId);
    if (tex == 0)
        return false;                                  // stock team / no PNG -> original

    int*  ns  = reinterpret_cast<int*>(nodeStruct);
    int   S   = ns[0];
    float w   = *reinterpret_cast<float*>(S + 0x14);
    float h   = *reinterpret_cast<float*>(S + 0x18);
    float rect[4] = { 0.0f, 0.0f, w, h };

    *reinterpret_cast<uint8_t*>(nodeStruct + 0x10) = 1;    // active

    const int node1 = ns[1];                              // small badge
    const int node2 = ns[2];                              // large badge
    if (node1) { SetNodeTexDefaultUv(node1, tex); SetNodeQuadFromRect(node1, rect); }
    if (node2) { SetNodeTexDefaultUv(node2, tex); SetNodeQuadFromRect(node2, rect); }

    // Match the stock visibility gating (FUN_00b0f8b0 tail).
    const uint8_t vis = static_cast<uint8_t>(
        *reinterpret_cast<uint8_t*>(nodeStruct + 0x11) & 1);
    if (node1) SetNodeEnabled(node1, vis);
    if (node2) SetNodeEnabled(node2,
        *reinterpret_cast<uint8_t*>(nodeStruct + 0x12) & vis);

    ns[3] = 0x1ca;                                        // cache slot (unused on our path)
    return true;
}

} // namespace

// FUN_00b0fa80(nodeStruct, teamId) — clean __cdecl sibling. Direct hook.
void __cdecl hook_BadgeRenderCdecl(int nodeStruct, int teamId)
{
    if (DrawCustomBadge(nodeStruct, teamId))
        return;
    if (orig_BadgeRenderCdecl)
        orig_BadgeRenderCdecl(nodeStruct, teamId);
}

// C handler for the register-args FUN_00b0f990 path. Returns 1 if it drew a
// custom emblem (caller must not run the original), 0 to fall through.
extern "C" int __cdecl hook_BadgeRender_C(int teamIdRaw, int nodeStruct, int /*param_1*/)
{
    return DrawCustomBadge(nodeStruct, teamIdRaw) ? 1 : 0;
}

// Naked bridge for FUN_00b0f990. The game calls it with ECX = teamId (this),
// EAX = nodeStruct, [esp+4] = param_1, and returns with a plain RET (caller
// cleans the one stack arg). We forward (teamId, nodeStruct, param_1) to the C
// handler; if it drew a custom emblem we return, otherwise we restore ECX/EAX
// and tail into the original.
extern "C" __declspec(naked) void hook_BadgeRender_Naked()
{
    __asm
    {
        push eax                        // save nodeStruct
        push ecx                        // save teamId
        // stack: [esp]=teamId [+4]=nodeStruct [+8]=ret [+0xC]=param_1
        push dword ptr [esp + 0x0C]     // param_1
        push dword ptr [esp + 0x08]     // nodeStruct
        push dword ptr [esp + 0x08]     // teamId
        call hook_BadgeRender_C
        add  esp, 0x0C                  // pop the 3 cdecl args
        test eax, eax
        jnz  handled
        pop  ecx                        // restore teamId
        pop  eax                        // restore nodeStruct
        jmp  dword ptr [orig_BadgeRender]
    handled:
        add  esp, 8                     // discard saved teamId + nodeStruct
        ret                             // plain ret (caller cleans param_1)
    }
}

// -----------------------------------------------------------------------------
// Shared-setter fallback — covers the rest of the badge-render family
// (FUN_00b0fb40 / FUN_00b0fbc0 / FUN_00b0fde0), some of which receive a
// pre-computed slot and so never expose the team id. They all funnel through:
//   FUN_009b0390(state, teamId)  — turns the team id into an atlas slot
//   FUN_00b0f8b0(texState, slot) — sets the badge texture from that slot
// We record the team in the first and, when the second is asked for the custom
// slot 0x1ca, draw teams/<team>.png instead. The club-selection hooks above
// already short-circuit before FUN_00b0f8b0, so there is no double draw.
// -----------------------------------------------------------------------------
namespace { int g_lastBadgeTeam = 0xFFFF; }

// FUN_009b0390(state, teamId) — clean __cdecl. Record the team, forward through.
int __cdecl hook_ComputeBadgeSlot(int state, uint16_t teamId)
{
    g_lastBadgeTeam = teamId;
    return orig_ComputeBadgeSlot ? orig_ComputeBadgeSlot(state, teamId) : 0;
}

// Body for the FUN_00b0f8b0 thunk: draw the custom badge onto EDI's nodeStruct.
extern "C" int __cdecl hook_BadgeSet_C(int nodeStruct)
{
    return DrawCustomBadge(nodeStruct, g_lastBadgeTeam) ? 1 : 0;
}

// Naked bridge for FUN_00b0f8b0(texState /*[esp+4]*/, slot /*[esp+8]*/) with
// nodeStruct in EDI, plain RET. Only the custom slot 0x1ca is intercepted; all
// other slots (and custom teams without a PNG) run the original untouched.
extern "C" __declspec(naked) void hook_BadgeSet_Naked()
{
    __asm
    {
        cmp  dword ptr [esp + 8], 0x1ca     // slot == custom sentinel?
        jne  passthrough
        push edi                            // nodeStruct (EDI is callee-saved)
        call hook_BadgeSet_C
        add  esp, 4
        test eax, eax
        jz   passthrough
        mov  eax, 1                         // handled -> "texture set"
        ret
    passthrough:
        jmp  dword ptr [orig_BadgeSet]
    }
}
