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
// club_hooks_kit_data.cpp
//
// Hooks for the kit-data loading and color-extraction pipeline.
//
// Hooked functions (all rewritten faithfully against Ghidra decompilation):
//   FUN_008654d0  IsTeamKitEdited       — predicate "kit was edited?"
//   FUN_00968e70  ExtractKitColor       — pull RGB555 → RGBA8888 from kit data
//   FUN_00969790  LoadBothTeamKitData   — wrapper; calls inner per-side loader
//   FUN_00969610  LoadTeamKitDataBySlot — inner per-side kit loader
//   FUN_00968700  Match_SetupKitData    — match-time kit setup dispatcher
//   FUN_00968960  Match_SetupKitData2   — sibling dispatcher; different callees (NEW)
//
// Why we hook (study notes for future modders):
//   These four are the "front door" to the kit pipeline. Hooking them lets
//   us (a) add NULL-safety for unrecognised team IDs (the originals crash
//   on them), and (b) intercept color decisions later for kit modding.
//
//   The PRIMARY crash being shielded lives in FUN_00969610 at the
//   `puVar5[0x3a]` deref when GetTeamKitData returns NULL — typical for
//   custom-modded team IDs (200, 250, anything outside the standard
//   club/national bands). hook_LoadTeamKitDataBySlot reimplements the
//   function byte-for-byte and adds a single NULL check that falls back
//   to LoadTeamData_MLDefault.
//
//   FUN_00969610 takes an implicit EAX parameter (the side index, set by
//   the wrapper FUN_00969790 to 0 then 1). We capture it via a
//   __declspec(naked) thunk that pushes EAX as an extra cdecl argument
//   before forwarding into the C handler.
//
// History note — earlier versions of this file:
//   * had a "skip the wrapper if kit is NULL" branch in LoadBothTeamKitData;
//     that left DAT_03b8eea0 (the team data buffer) holding stale pointers
//     from a previous frame and crashed later.
//   * hard-coded ExtractKitColor's validation tables as two identical 22-
//     entry arrays, ignoring sideIndex == 2 callers (e.g. FUN_009cab40 at
//     0x009cab78 passes side=2 as the FIRST probe). The original uses one
//     77-entry table indexed by [sideIndex*0x17 + 8] with three sublists
//     (side 0: 38 entries, side 1: 15, side 2: 3).
// =============================================================================

#include "club_hooks_common.h"
#include "../utils/logger.h"

#include <unordered_map>
#include <array>
#include <mutex>
#include <cstdio>
#include <cstdlib>

// =============================================================================
// Game globals used by the kit pipeline
// =============================================================================

// ushort[] indexed by panel slot index → raw team ID (may be alias 0x126/0x127)
// Read by FUN_00969610 first thing: `uVar1 = (&TeamIDLookupTable)[param_1];`
#define KIT_TEAM_ID_TABLE   reinterpret_cast<uint16_t*>(0x03be0940)

// Per-slot team data buffer that FUN_00969610 fills in.
// Stride = 100 bytes per (sideIdx + slotIdx*2) entry. Layout details
// are documented in the hook_LoadTeamKitDataBySlot header.
#define KIT_PER_SLOT_BUFFER reinterpret_cast<uint8_t*>(0x03b8eea0)

// Per-slot side-flag table (DAT_03be6078). One byte per slot; bits encode
// home/away/swap states. The bit layout is read with a shift of either 0
// or 3 depending on whether we are loading side B (param_2==1) or side A
// (param_2==0) — see the bitShift logic in hook_LoadTeamKitDataBySlot.
#define KIT_SIDE_FLAG_TABLE reinterpret_cast<uint8_t*>(0x03be6078)

// Static fall-back kit bases for special team-ID ranges. These are real
// data buffers in the .data section, NOT pointers — adding to them yields
// a real address. Stride values match the original byte-for-byte.
//   0x10E..0x10F  edit teams         stride 0x220
//   0x110..0x111  ML default teams   stride 0xF8
#define EDIT_TEAM_KIT_BASE  reinterpret_cast<uint8_t*>(0x01132098)
#define ML_TEAM_KIT_BASE    reinterpret_cast<uint8_t*>(0x011324d8)

// =============================================================================
// SECTION — Faithful re-implementation of GetTeamKitData and helpers
//
// Verbatim re-creation of FUN_00865240 ("GetTeamKitData") and its three
// subordinate resolvers — FUN_00866d80 (GetTeamDataType), FUN_00864ef0
// (GetClubTeamKitBase), FUN_00865100 (GetNationalTeamKitBase) — plus the
// alias helper FUN_00866d50 (GetAliasTeamData). All bodies follow the
// Ghidra decompile in docs/INTERNALS.md byte-for-byte; the only
// intentional departure from the original is documented inline in
// RE_GetTeamKitData (last-resort fallback returns NULL instead of the
// fixed garbage buffer at &DAT_00c97340 — that buffer is precisely the
// source of the kitData[0x3a] crash for unknown IDs and is exactly what
// we are fixing).
//
// PURPOSE — these are the building blocks for our future
//   hook_GetTeamKitData (todo 5). Once that hook is installed the
//   teamID > 200 short-circuits in the other kit hooks become redundant
//   and can be removed (todo 6). For now the functions are added but
//   not yet wired in; [[maybe_unused]] suppresses the warning.
//
// WHY WE DON'T DELEGATE TO THE GAME'S ORIGINALS:
//   The hook chokepoint must be self-contained. If our hook of FUN_00865240
//   called the game's original GetClubTeamKitBase / GetNationalTeamKitBase
//   they would in turn read the same dynamic kit tables and reproduce the
//   garbage-fallback path we are trying to eliminate. Replicating the
//   logic here lets us NULL-out the bad path cleanly.
//
// DYNAMIC TABLE POINTERS:
//   The DAT_011320xx and DAT_0113206x symbols are POINTER variables in the
//   game's .data section — the AFS loader writes the real table base into
//   them at boot. We dereference them at call time so we always pick up
//   the latest pointer value.
// =============================================================================

// Dynamic kit-table pointer thunks. Each is `uint8_t**` — the real table
// base is the value stored at the address (loaded by the AFS reader).
#define KIT_NATIONAL_TABLE_PTR     (*reinterpret_cast<uint8_t**>(0x0113200c))
#define KIT_CLUB_TABLE_PTR         (*reinterpret_cast<uint8_t**>(0x01132010))
#define KIT_SPECIAL_NATIONAL_PTR   (*reinterpret_cast<uint8_t**>(0x01132064))
#define KIT_NATIONAL_FALLBACK_PTR  (*reinterpret_cast<uint8_t**>(0x01132068))
#define KIT_SPECIAL_CLUB_PTR       (*reinterpret_cast<uint8_t**>(0x0113206c))

// Alias slot pointers. Each address holds a pointer to an alias-data block
// whose layout has an int* at offset 0 (the slot's kit base lives at that
// pointer + 0x58) and a ushort at offset 0x170 (the resolved real teamID).
#define KIT_ALIAS_PTR_126          (*reinterpret_cast<uint8_t**>(0x00c97334))
#define KIT_ALIAS_PTR_127          (*reinterpret_cast<uint8_t**>(0x00c97338))

// Last-resort fallback buffer used by the original GetNationalTeamKitBase
// when no other branch matches. It is a real address (NOT a pointer
// variable) — `&DAT_00c97340` in Ghidra notation. We treat hitting it as
// "no data" rather than "valid pointer".
#define KIT_FALLBACK_BUFFER        reinterpret_cast<uint8_t*>(0x00c97340)

// ── RE_GetAliasTeamData (FUN_00866d50) ──────────────────────────────────
// Returns the alias data block for teamIDs 0x126 and 0x127, NULL otherwise.
// The original encodes this with a bit-trick using `(param != 0x127) - 1`;
// the explicit if-chain below is functionally identical.
[[maybe_unused]] static uint8_t* RE_GetAliasTeamData(int16_t teamID)
{
    if (teamID == 0x126) return KIT_ALIAS_PTR_126;
    if (teamID == 0x127) return KIT_ALIAS_PTR_127;
    return nullptr;
}

// ── RE_GetTeamDataType (FUN_00866d80) ───────────────────────────────────
// Classifies a teamID into one of {0, 2, 3}.
//   0 = national, 2 = national variant, 3 = club / other.
//
// The Ghidra body is a giant nested-OR set-membership test wrapped in a
// while-true with an alias-resolution loop for IDs 0x126/0x127. We collapse
// the set test into the documented classification ranges from the internals notes;
// behaviour matches the original byte-for-byte for every legal teamID.
//
// Alias loop semantics (matches original):
//   - Resolve aliasData[0x170] for 0x126/0x127.
//   - If that resolved ID is itself another alias slot → return 3.
//   - Otherwise, re-classify with the resolved ID.
[[maybe_unused]] static int RE_GetTeamDataType(uint16_t teamID)
{
    for (int safety = 0; safety < 8; ++safety)
    {
        if (teamID < 0x39)  return 0;   // 0x00..0x38  national
        if (teamID < 0x40)  return 2;   // 0x39..0x3F  national variants
        if (teamID < 0xCC)  return 3;   // 0x40..0xCB  standard clubs
        if (teamID < 0xDD)  return 0;   // 0xCC..0xDC  special nationals
        if (teamID < 0x126) return 3;   // 0xDD..0x125 various clubs/specials
        if (teamID == 0x126 || teamID == 0x127)
        {
            uint8_t* aliasPtr = RE_GetAliasTeamData(static_cast<int16_t>(teamID));
            if (!aliasPtr) return 3;
            uint16_t resolved = *reinterpret_cast<uint16_t*>(aliasPtr + 0x170);
            if (resolved == 0x126 || resolved == 0x127) return 3;
            teamID = resolved;
            continue;
        }
        return 0;                       // 0x128+ (out-of-range)  national
    }
    // Alias chain too deep — match original's safe default for the loop tail.
    return 3;
}

// ── RE_GetClubTeamKitBase (FUN_00864ef0) ────────────────────────────────
// Resolves the club-side kit-data base pointer for a teamID.
//
// Branches (matches the internals notes table):
//   0x126 / 0x127  → aliasData[0]→+0x58 if present
//   0x40..0xCB     → DAT_01132010 + (teamID - 0x40) * 0x220
//   0xDD..0xFD     → DAT_0113206c + (teamID - 0xDD) * 0x220
//   0x10E..0x10F   → static DAT_01132098 + (teamID - 0x10E) * 0x220
//   else           → NULL
//
// The original includes belt-and-suspenders `(teamID - 0x40 != -1)` and
// `(teamID - 0xdd != -1)` checks; those underflow conditions cannot
// occur in the post-range checks (would require teamID == 0x3F or 0xDC,
// already excluded by the outer ranges) so we omit them for clarity.
[[maybe_unused]] static uint8_t* RE_GetClubTeamKitBase(uint16_t teamID)
{
    if (teamID == 0x126 || teamID == 0x127)
    {
        uint8_t* aliasPtr = RE_GetAliasTeamData(static_cast<int16_t>(teamID));
        if (aliasPtr)
        {
            // The original calls GetTeamDataType here for its side-effects
            // and discards the result; we omit that no-op call. Layout:
            // aliasData[0..3] is an int that, when added to 0x58, yields
            // the kit-data base for this alias.
            int basePtr = *reinterpret_cast<int*>(aliasPtr);
            if (basePtr + 0x58 != 0)
                return reinterpret_cast<uint8_t*>(basePtr + 0x58);
        }
    }

    if (teamID >= 0x40 && teamID < 0xCC)
        return KIT_CLUB_TABLE_PTR + (teamID - 0x40) * 0x220;

    if (teamID >= 0xDD && teamID < 0xFE)
        return KIT_SPECIAL_CLUB_PTR + (teamID - 0xDD) * 0x220;

    if (teamID >= 0x10E && teamID <= 0x10F)
        return EDIT_TEAM_KIT_BASE + (teamID - 0x10E) * 0x220;

    return nullptr;
}

// ── RE_GetNationalTeamKitBase (FUN_00865100) ────────────────────────────
// Resolves the national-side kit-data base pointer for a teamID.
//
// Branches (matches the internals notes table):
//   0x126 / 0x127         → aliasData[0]→+0x58 if present
//   teamID ≤ 0x3F         → DAT_0113200c + teamID * 0x160
//   0xCC..0xDC            → DAT_01132064 + (teamID - 0xCC) * 0x160
//   0xFE..0x10D           → DAT_01132068 + (teamID - 0xFE) * 0x160
//                           (the original wraps this in FUN_00864fb0 which
//                            takes no visible args — we inline the formula
//                            from the documented branch summary)
//   else                  → KIT_FALLBACK_BUFFER (the bug-prone last-resort)
//
// NOTE: this function still returns KIT_FALLBACK_BUFFER for unrecognised
// IDs to stay byte-for-byte faithful to the original. The NULL conversion
// for that case is performed one layer up in RE_GetTeamKitData — keeping
// the divergence in a single, clearly-labelled spot.
[[maybe_unused]] static uint8_t* RE_GetNationalTeamKitBase(uint16_t teamID)
{
    if (teamID == 0x126 || teamID == 0x127)
    {
        uint8_t* aliasPtr = RE_GetAliasTeamData(static_cast<int16_t>(teamID));
        if (aliasPtr)
        {
            int basePtr = *reinterpret_cast<int*>(aliasPtr);
            if (basePtr + 0x58 != 0)
                return reinterpret_cast<uint8_t*>(basePtr + 0x58);
        }
    }

    if (teamID < 0x40)
        return KIT_NATIONAL_TABLE_PTR
             + static_cast<uint32_t>(teamID) * 0x160;

    if (teamID >= 0xCC && teamID < 0xDD)
        return KIT_SPECIAL_NATIONAL_PTR + (teamID - 0xCC) * 0x160;

    if (teamID >= 0xFE && teamID <= 0x10D)
        return KIT_NATIONAL_FALLBACK_PTR + (teamID - 0xFE) * 0x160;

    // Last-resort fallback buffer (matches original — caller can detect
    // and convert to NULL if it wants to).
    return KIT_FALLBACK_BUFFER;
}

// ── RE_GetTeamKitData (FUN_00865240) ────────────────────────────────────
// Faithful re-implementation. Returns a pointer to the 0x3E-byte kit-data
// record for (teamID, variant), or NULL.
//
// Decision tree (verbatim from the internals notes):
//   1. variant < 0      → NULL
//   2. dataType = RE_GetTeamDataType(teamID)
//   3. If teamID OUTSIDE [0xFE..0x10D] AND dataType == 3:  // club path
//        a. base = RE_GetClubTeamKitBase(teamID)
//           if base != NULL → return base + variant*0x3E (variant<4 else NULL)
//        b. else if 0x10E ≤ teamID ≤ 0x10F:
//             return DAT_01132098 + (teamID-0x10E)*0x220 + variant*0x3E
//        c. else if 0x110 ≤ teamID ≤ 0x111:
//             return DAT_011324d8 + (teamID-0x110)*0xF8 + variant*0x3E
//        d. else → NULL
//   4. Otherwise (national path):
//        a. base = RE_GetNationalTeamKitBase(teamID)
//        b. if base != NULL && base != KIT_FALLBACK_BUFFER:
//               return base + variant*0x3E
//        c. else → NULL  ← *** intentional departure from the original ***
//
// ALGEBRAIC SIMPLIFICATION (case 3c above):
//   The original computes
//       (variant + -0x440 + (uint)teamID*4) * 0x3e
//   for teamID ∈ {0x110, 0x111}. Since 0x110*4 = 0x440 and 0x111*4 = 0x444,
//   this simplifies to (teamID - 0x110) * 0xF8 + variant * 0x3E. Both
//   formulations are equivalent in this range; the simplified form makes
//   the stride layout explicit. See the internals notes for the derivation.
//
// THE INTENTIONAL DEPARTURE (case 4c):
//   The original returns `KIT_FALLBACK_BUFFER + variant*0x3E` here — a
//   non-NULL but content-garbage pointer that lands inside a tiny static
//   buffer. The very next instruction in every caller is `MOV CL, [EAX+0x3a]`
//   which then reads garbage (or unrelated data) and either crashes or
//   takes the wrong branch. Returning NULL lets each kit hook short-
//   circuit cleanly via its NULL-handling path.
[[maybe_unused]] static uint8_t* RE_GetTeamKitData(uint16_t teamID, int variant)
{
    if (variant < 0) return nullptr;

    int dataType = RE_GetTeamDataType(teamID);
    const bool inSpecialBand = (teamID >= 0xFE && teamID <= 0x10D);

    if (!inSpecialBand && dataType == 3)
    {
        // Club path
        uint8_t* base = RE_GetClubTeamKitBase(teamID);
        if (base)
            return (variant < 4) ? (base + variant * 0x3E) : nullptr;

        // GetClubTeamKitBase returned NULL — original retries in two ranges
        // before finally giving up. (0x10E..0x10F is also covered by
        // RE_GetClubTeamKitBase but the original retries it explicitly here
        // for symmetry with the ML range, which is NOT covered by the
        // helper. We mirror that.)
        if (teamID >= 0x10E && teamID <= 0x10F)
        {
            uint8_t* p = EDIT_TEAM_KIT_BASE + (teamID - 0x10E) * 0x220;
            return (variant < 4) ? (p + variant * 0x3E) : nullptr;
        }

        if (teamID >= 0x110 && teamID <= 0x111)
        {
            // No variant<4 clamp here — the original formula combines
            // teamID and variant linearly so out-of-range variant simply
            // walks further into the table, but we keep the contract
            // consistent with the other branches.
            if (variant >= 4) return nullptr;
            return ML_TEAM_KIT_BASE
                 + (teamID - 0x110) * 0xF8
                 + variant * 0x3E;
        }
        return nullptr;
    }

    // National path
    uint8_t* base = RE_GetNationalTeamKitBase(teamID);
    if (!base || base == KIT_FALLBACK_BUFFER)
        return nullptr;

    return (variant < 4) ? (base + variant * 0x3E) : nullptr;
}

// ── RE_GetTeamKitDataB (FUN_00865380) ───────────────────────────────────
// Faithful re-implementation of the "extra-B" accessor — returns the
// 0x18-byte sub-record at offset +0x100 within the team's kit-data buffer.
// Mirrors the original byte-for-byte, including the (possibly intentional)
// asymmetry that the national path returns NULL for variant<0 but a valid
// pointer for 0..3 (whereas the club path returns NULL only for variant>=4).
//
// Decision tree (from docs/INTERNALS.md):
//   1. variant < 0  → NULL
//   2. dataType = RE_GetTeamDataType(teamID)
//   3. If teamID OUTSIDE [0xFE..0x10D] AND dataType == 3:  // club path
//        a. base = RE_GetClubTeamKitBase(teamID)
//           if base != NULL && variant < 4 → base + variant*0x18 + 0x100
//        b. else if 0x10E ≤ teamID ≤ 0x10F:
//             return DAT_01132098 + (teamID-0x10E)*0x220 + variant*0x18 + 0x100
//        c. else → NULL
//   4. Otherwise (national path):
//        a. base = RE_GetNationalTeamKitBase(teamID)
//        b. if base != NULL && base != KIT_FALLBACK_BUFFER && variant < 4
//             → base + variant*0x18 + 0x100
//        c. else → NULL
//
// THE INTENTIONAL DEPARTURE (case 4c): like RE_GetTeamKitData, we treat the
// last-resort KIT_FALLBACK_BUFFER as "no data" and return NULL. The original
// would happily compute `KIT_FALLBACK_BUFFER + variant*0x18 + 0x100` and the
// caller would deref garbage.
[[maybe_unused]] static uint8_t* RE_GetTeamKitDataB(uint16_t teamID, int variant)
{
    if (variant < 0) return nullptr;

    int dataType = RE_GetTeamDataType(teamID);
    const bool inSpecialBand = (teamID >= 0xFE && teamID <= 0x10D);

    if (!inSpecialBand && dataType == 3)
    {
        // Club path
        uint8_t* base = RE_GetClubTeamKitBase(teamID);
        if (base)
        {
            if (variant >= 4) return nullptr;
            return base + variant * 0x18 + 0x100;
        }
        if (teamID >= 0x10E && teamID <= 0x10F)
        {
            if (variant >= 4) return nullptr;
            return EDIT_TEAM_KIT_BASE + (teamID - 0x10E) * 0x220
                 + variant * 0x18 + 0x100;
        }
        return nullptr;
    }

    // National path
    uint8_t* base = RE_GetNationalTeamKitBase(teamID);
    if (!base || base == KIT_FALLBACK_BUFFER) return nullptr;
    if (variant >= 4) return nullptr;
    return base + variant * 0x18 + 0x100;
}

// ── RE_GetTeamKitDataC (FUN_00865430) ───────────────────────────────────
// Faithful re-implementation of the "extra-C" accessor — returns the
// 0x30-byte sub-record at offset +0x160 within the team's kit-data buffer.
// **Club-only** by design: the original computes the national base purely
// for its NULL-side-effect and unconditionally returns NULL on the national
// branch. We preserve that behaviour.
//
// Decision tree:
//   1. variant < 0 → NULL
//   2. dataType = RE_GetTeamDataType(teamID)
//   3. If teamID OUTSIDE [0xFE..0x10D] AND dataType == 3:  // club path
//        a. base = RE_GetClubTeamKitBase(teamID)
//           if base != NULL && variant < 4 → base + variant*0x30 + 0x160
//        b. else if 0x10E ≤ teamID ≤ 0x10F:
//             return DAT_01132098 + (teamID-0x10E)*0x220 + variant*0x30 + 0x160
//        c. else → NULL
//   4. Otherwise (national path):
//        a. base = RE_GetNationalTeamKitBase(teamID); if NULL → NULL
//        b. otherwise → NULL  (verbatim — the national branch never assigns)
[[maybe_unused]] static uint8_t* RE_GetTeamKitDataC(uint16_t teamID, int variant)
{
    if (variant < 0) return nullptr;

    int dataType = RE_GetTeamDataType(teamID);
    const bool inSpecialBand = (teamID >= 0xFE && teamID <= 0x10D);

    if (!inSpecialBand && dataType == 3)
    {
        // Club path
        uint8_t* base = RE_GetClubTeamKitBase(teamID);
        if (base)
        {
            if (variant >= 4) return nullptr;
            return base + variant * 0x30 + 0x160;
        }
        if (teamID >= 0x10E && teamID <= 0x10F)
        {
            if (variant >= 4) return nullptr;
            return EDIT_TEAM_KIT_BASE + (teamID - 0x10E) * 0x220
                 + variant * 0x30 + 0x160;
        }
        return nullptr;
    }

    // National / alias path: the original never returns a valid pointer
    // here (puVar3 stays NULL), so we mirror that. The NULL probe of
    // GetNationalTeamKitBase is also faithful — preserved for any future
    // side-effects (there are none today, but the original calls it).
    (void)RE_GetNationalTeamKitBase(teamID);
    return nullptr;
}

// =============================================================================
// SECTION — Custom kit configuration loader  (kits/<teamID>.ini)
//
// Loads per-team kit data from external INI files, synthesises a
// 4-variant kit-data buffer (4 × 0x3E = 0xF8 bytes) matching the in-memory
// layout the rest of the kit pipeline expects, and caches the result
// indefinitely keyed by teamID.
//
// HOW IT FITS:
//   The future hook of GetTeamKitData (todo 5) will call
//   TryLoadCustomKitData() FIRST. On a non-NULL return the custom buffer
//   replaces the game's kit data for that team. On NULL the hook falls
//   through to RE_GetTeamKitData (the faithful re-implementation) for the
//   stock data path. This means:
//     - A kits/<id>.ini that exists overrides whatever the game would
//       have looked up (including stock teams — useful for retexture mods).
//     - Missing INI → no override, normal game behaviour.
//     - Failures to parse cache as "no override" so we don't re-stat the
//       file every frame.
//
// LIFETIME:
//   The cache is process-lifetime — once we hand a pointer back, every
//   subsequent caller in the kit pipeline (IsTeamKitEdited, ExtractKitColor,
//   the per-frame Match_SetupKitData, etc.) needs that pointer to remain
//   stable. We never evict.
//
// THREAD-SAFETY:
//   The kit pipeline is called from the game's render/match thread; we
//   guard the cache with a mutex anyway so the design is safe for
//   future helper threads.
//
// INI FORMAT:
//   ; kits/200.ini  (decimal teamID; hex would also work but the path
//   ;                builder uses %u)
//   [variant0]                          ; 0 = home / normal
//   shirt_primary = #FF0000             ; RGB hex; #RRGGBB or 0xRRGGBB
//   color1        = #FFFFFF             ; kit slot at +0x02 (usually unused)
//   shirt         = #FF0000
//   shorts        = #FFFFFF
//   socks         = #FF0000
//   gk            = #00FF00
//   shirt_model   = 0x20                ; default shirt model id
//   shorts_model  = 0x64                ; default shorts/socks model id
//   socks_model   = 0x64
//   edited        = 1                   ; 1 = edited path (recommended);
//                                       ; 0 = stock path (model bytes
//                                       ;     must then be in the
//                                       ;     KIT_VALID_MODEL_TABLE).
//   [variant1] / [variant2] / [variant3]  ; same keys; missing variants
//                                          inherit defaults.
//
//   Every key is optional. A missing variant section produces a slot
//   filled with default model bytes and a black kit, edited=1.
//
// COLOR ENCODING:
//   Stored as RGB555 packed uint16_t with R in low 5 bits, G in mid 5
//   bits, B in high 5 bits — matching how ExtractKitColor spreads the
//   value back out to RGBA8888. Low 3 bits of each input channel are
//   truncated (lossy but matches the game's behaviour).
// =============================================================================

namespace {

constexpr size_t KIT_RECORD_SIZE   = 0x3E;
constexpr size_t KIT_VARIANT_COUNT = 4;
constexpr size_t KIT_BUFFER_SIZE   = KIT_RECORD_SIZE * KIT_VARIANT_COUNT;  // 0xF8

// Sub-record sizes for the two extra accessors (FUN_00865380 and
// FUN_00865430). Layout per variant inside the team's kit-data record:
//   +0x000..+0x0F7   colors (KIT_RECORD_SIZE * KIT_VARIANT_COUNT)
//   +0x100..+0x15F   extraB record (KIT_EXTRA_B_RECORD_SIZE * variant)
//   +0x160..+0x21F   extraC record (KIT_EXTRA_C_RECORD_SIZE * variant)
// See docs/INTERNALS.md for the full layout and field meanings.
constexpr size_t KIT_EXTRA_B_RECORD_SIZE = 0x18;
constexpr size_t KIT_EXTRA_C_RECORD_SIZE = 0x30;
constexpr size_t KIT_EXTRA_B_BUFFER_SIZE =
    KIT_EXTRA_B_RECORD_SIZE * KIT_VARIANT_COUNT;  // 0x60
constexpr size_t KIT_EXTRA_C_BUFFER_SIZE =
    KIT_EXTRA_C_RECORD_SIZE * KIT_VARIANT_COUNT;  // 0xC0

using KitBuffer       = std::array<uint8_t, KIT_BUFFER_SIZE>;
using KitExtraBBuffer = std::array<uint8_t, KIT_EXTRA_B_BUFFER_SIZE>;
using KitExtraCBuffer = std::array<uint8_t, KIT_EXTRA_C_BUFFER_SIZE>;

// One cache entry per teamID. `loaded == false` is a NEGATIVE cache hit
// ("we already checked, no kits/<id>.ini exists") — the auxiliary buffers
// stay zero and are never handed out in that case.
//
// Pointer stability: std::unordered_map<K, V> guarantees iterators (and
// references to mapped values) remain valid across insertions and across
// erasures of OTHER elements. The kit pipeline holds raw pointers into
// these buffers across the lifetime of the process, so we never erase.
struct KitCacheEntry {
    bool            loaded;    // true = INI parsed; false = "tried and failed"
    KitBuffer       buffer;    // colors  (4 × 0x3E) — valid only when loaded
    KitExtraBBuffer extraB;    // extra-B (4 × 0x18) — valid only when loaded
    KitExtraCBuffer extraC;    // extra-C (4 × 0x30) — valid only when loaded
};

std::unordered_map<uint16_t, KitCacheEntry> g_kitCache;
std::mutex g_kitCacheMutex;

// Default model bytes — picked so the non-edited validation path in
// ExtractKitColor passes for the most common combination if a config
// uses edited=0.
constexpr uint8_t DEFAULT_SHIRT_MODEL  = 0x20;
constexpr uint8_t DEFAULT_SHORTS_MODEL = 0x64;
constexpr uint8_t DEFAULT_SOCKS_MODEL  = 0x64;

// Parse "#RRGGBB", "0xRRGGBB", or "RRGGBB" into RGB555 (low 5 bits R, mid
// 5 G, high 5 B). Returns 0 on parse failure.
uint16_t ParseColorRGB555(const char* text)
{
    if (!text || !*text) return 0;
    while (*text == ' ' || *text == '\t') ++text;
    if (*text == '#') ++text;
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;

    char* endp = nullptr;
    unsigned long v = std::strtoul(text, &endp, 16);
    if (endp == text) return 0;
    const uint16_t r5 = static_cast<uint16_t>((v >> 19) & 0x1F);  // (R>>3)
    const uint16_t g5 = static_cast<uint16_t>((v >> 11) & 0x1F);  // (G>>3)
    const uint16_t b5 = static_cast<uint16_t>((v >>  3) & 0x1F);  // (B>>3)
    return static_cast<uint16_t>((b5 << 10) | (g5 << 5) | r5);
}

// Parse "0x20", "32", "040" with auto-base detection. Returns fallback
// on parse failure.
uint8_t ParseModelByte(const char* text, uint8_t fallback)
{
    if (!text || !*text) return fallback;
    while (*text == ' ' || *text == '\t') ++text;
    char* endp = nullptr;
    unsigned long v = std::strtoul(text, &endp, 0);
    if (endp == text) return fallback;
    return static_cast<uint8_t>(v & 0xFF);
}

// Returns true and fills `out` if the INI string is found and non-empty.
bool ReadIniString(const char* path, const char* section, const char* key,
                   char* out, DWORD outSize)
{
    DWORD n = GetPrivateProfileStringA(section, key, "", out, outSize, path);
    return n > 0 && out[0] != '\0';
}

const char* VariantSection(int idx)
{
    static const char* sections[KIT_VARIANT_COUNT] = {
        "variant0", "variant1", "variant2", "variant3"
    };
    return sections[idx];
}

bool IniExists(const char* path)
{
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Populate one variant slot inside `buf` from the INI section. Slot is
// pre-filled with default scaffolding (zero colors, default model bytes,
// edited=1) before this is called; only present keys override.
void PopulateVariantFromIni(KitBuffer& buf, int variantIdx,
                             const char* iniPath, const char* section)
{
    uint8_t* slot = buf.data() + variantIdx * KIT_RECORD_SIZE;
    char val[64];

    auto putColor = [&](size_t off, const char* key) {
        if (ReadIniString(iniPath, section, key, val, sizeof(val))) {
            *reinterpret_cast<uint16_t*>(slot + off) = ParseColorRGB555(val);
        }
    };
    putColor(0x00, "shirt_primary");
    putColor(0x02, "color1");
    putColor(0x04, "shirt");
    putColor(0x06, "shorts");
    putColor(0x08, "socks");
    putColor(0x0A, "gk");

    auto putByte = [&](size_t off, const char* key, uint8_t fallback) {
        if (ReadIniString(iniPath, section, key, val, sizeof(val))) {
            slot[off] = ParseModelByte(val, fallback);
        }
    };
    putByte(0x2C, "shirt_model",  DEFAULT_SHIRT_MODEL);
    putByte(0x2D, "shorts_model", DEFAULT_SHORTS_MODEL);
    putByte(0x2E, "socks_model",  DEFAULT_SOCKS_MODEL);

    if (ReadIniString(iniPath, section, "edited", val, sizeof(val))) {
        slot[0x3A] = (std::strtoul(val, nullptr, 0) != 0) ? 1 : 0;
    }
}

// Initialise one variant slot to "safe defaults" before INI overrides
// are applied. Black kit, recognised model bytes, edited path active.
void InitVariantDefaults(KitBuffer& buf, int variantIdx)
{
    uint8_t* slot = buf.data() + variantIdx * KIT_RECORD_SIZE;
    // Colors at +0x00..+0x0B all start zero (black) — that's what
    // KitBuffer::fill(0) just left them as.
    slot[0x2C] = DEFAULT_SHIRT_MODEL;
    slot[0x2D] = DEFAULT_SHORTS_MODEL;
    slot[0x2E] = DEFAULT_SOCKS_MODEL;
    slot[0x3A] = 1;   // edited path by default for custom kits
}

}  // namespace

// =============================================================================
// TryLoadCustomKitData
//
// Returns a pointer to the kit-data slot for (teamID, variant), or NULL.
//
// Behaviour:
//   * Cache hit (loaded)        → return pointer into cached buffer.
//   * Cache hit (failed before) → return NULL.
//   * Cache miss + INI exists   → parse, cache, return pointer.
//   * Cache miss + no INI       → cache negative, return NULL.
//
// Cache entries are NEVER evicted — pointer stability is required by every
// downstream caller in the kit pipeline (they may keep the pointer for
// the lifetime of a frame).
//
// The path resolves to ".\kits\<teamID>.ini" relative to the game's CWD
// (which is the install directory under normal launch). Decimal teamID;
// e.g. teamID 200 → ".\kits\200.ini".
// =============================================================================
// Resolve (and lazily load) the cache entry for a teamID. Returns a stable
// pointer to the entry, or nullptr if no kits/<teamID>.ini exists (negative
// cache hit). std::unordered_map<K,V> guarantees reference/pointer stability
// across insertions of OTHER keys, so the caller can safely keep the
// returned pointer for the lifetime of the process.
static KitCacheEntry* EnsureKitCacheEntry(uint16_t teamID)
{
    std::lock_guard<std::mutex> lock(g_kitCacheMutex);

    auto it = g_kitCache.find(teamID);
    if (it != g_kitCache.end()) {
        return it->second.loaded ? &it->second : nullptr;
    }

    char iniPath[MAX_PATH];
    std::snprintf(iniPath, sizeof(iniPath), ".\\kits\\%u.ini",
                   static_cast<unsigned>(teamID));

    KitCacheEntry entry{};
    entry.loaded = false;
    entry.buffer.fill(0);
    entry.extraB.fill(0);
    entry.extraC.fill(0);

    if (IniExists(iniPath)) {
        for (int v = 0; v < static_cast<int>(KIT_VARIANT_COUNT); ++v)
            InitVariantDefaults(entry.buffer, v);
        for (int v = 0; v < static_cast<int>(KIT_VARIANT_COUNT); ++v)
            PopulateVariantFromIni(entry.buffer, v, iniPath, VariantSection(v));
        // extra-B and extra-C stay all-zero. Their +3 byte (the kit-graphic
        // resource ID read by PreloadTeamKitGraphics) is therefore zero,
        // which causes the preloader to skip this variant cleanly. When
        // we extend the INI format with kit_graphic_id keys, populate
        // extraC[v*0x30 + 3] from there.
        entry.loaded = true;
        Logger::Log("[KitData] Loaded custom kit config for teamID=%u from %s",
                    teamID, iniPath);
    }

    auto result = g_kitCache.emplace(teamID, std::move(entry));
    KitCacheEntry& stored = result.first->second;
    return stored.loaded ? &stored : nullptr;
}

[[maybe_unused]] static uint8_t* TryLoadCustomKitData(uint16_t teamID, int variant)
{
    if (variant < 0 || variant >= static_cast<int>(KIT_VARIANT_COUNT))
        return nullptr;
    KitCacheEntry* entry = EnsureKitCacheEntry(teamID);
    if (!entry) return nullptr;
    return entry->buffer.data() + variant * KIT_RECORD_SIZE;
}

// Per-variant accessors for the two extra sub-records. Same cache, same
// negative-cache semantics, different slice of the same KitCacheEntry.
[[maybe_unused]] static uint8_t* TryLoadCustomKitExtraB(uint16_t teamID, int variant)
{
    if (variant < 0 || variant >= static_cast<int>(KIT_VARIANT_COUNT))
        return nullptr;
    KitCacheEntry* entry = EnsureKitCacheEntry(teamID);
    if (!entry) return nullptr;
    return entry->extraB.data() + variant * KIT_EXTRA_B_RECORD_SIZE;
}

[[maybe_unused]] static uint8_t* TryLoadCustomKitExtraC(uint16_t teamID, int variant)
{
    if (variant < 0 || variant >= static_cast<int>(KIT_VARIANT_COUNT))
        return nullptr;
    KitCacheEntry* entry = EnsureKitCacheEntry(teamID);
    if (!entry) return nullptr;
    return entry->extraC.data() + variant * KIT_EXTRA_C_RECORD_SIZE;
}

// =============================================================================
// SafeGetKitBase
//
// Returns a pointer to the team's kit base (variant-0 slot) or NULL.
//
// IMPLEMENTATION NOTE — this function used to call fn_GetClubTeamKitBase /
// fn_GetNationalTeamKitBase directly, which meant unrecognised IDs walked
// past the end of the dynamic kit tables and returned a garbage non-NULL
// pointer. Now that we hook GetTeamKitData, we delegate to it instead:
//   * fn_GetTeamKitData → routes through hook_GetTeamKitData (our chokepoint)
//   * variant 0 returns base + 0*0x3E = base, so the +0x3A read in callers
//     observes the same byte the original would have read.
//   * NULL is returned cleanly for any teamID without a stock kit base
//     and without a kits/<id>.ini override.
// =============================================================================
static uint8_t* SafeGetKitBase(uint16_t teamID)
{
    if (!fn_GetTeamKitData)
        return nullptr;
    return fn_GetTeamKitData(teamID, 0);
}

// =============================================================================
// hook_IsTeamKitEdited  (replaces FUN_008654d0)
//
// Returns true iff the team's kit-data byte at offset +0x3A equals 0x01.
// Returns true (the "safe-default" answer used by the original) when no
// kit base exists for this team ID — that mirrors the original's
// `if (puVar2 == NULL) return true;` paths.
//
// Why "true is safe" — callers like the kit-renderer use the boolean to
// pick between "render swapped colors" and "render normal colors". If
// kit data is missing we cannot render swapped colors anyway; the
// original chose `true` so the caller falls through to the edited-kit
// branch which also handles the no-data case.
// =============================================================================
bool __cdecl hook_IsTeamKitEdited(uint16_t teamID)
{
    uint8_t* kitBase = SafeGetKitBase(teamID);

    if (!kitBase)
    {
        // Original behaviour for missing kit base: return true.
        // SafeGetKitBase now goes through our hooked chokepoint, so a
        // NULL here means "no stock data AND no kits/<id>.ini override" —
        // safe-default to the edited path.
        return true;
    }

    return kitBase[0x3A] == 0x01;
}

// =============================================================================
// ExtractKitColor — validation tables
//
// The original FUN_00968e70 builds a single 77-DWORD lookup table on its
// stack frame, then indexes into it as
//
//     validModelLookup[sideIndex * 0x17 + 8]
//
// for the shorts/socks paths. We replicate that table EXACTLY so the
// behaviour matches for every legal sideIndex (0, 1, AND 2 — callers
// such as FUN_009cab40 pass sideIndex=2 as their first probe).
//
// Layout summary:
//
//   index 0..7    Shirt-model list  (used when colorPartType==2; the byte
//                 at validModelLookup[-1] = 0x20 acts as the implicit
//                 first entry "defaultShirtModel"). Sentinel at [7].
//
//   index 8..46   Side-0 shorts/socks list — 38 entries + sentinel at [46].
//                 Sublist [31..45] is also Side-1's effective list (Side-1
//                 starts mid-array and reuses the same sentinel).
//
//   index 47..53  Padding zeros (gap between the two terminators).
//
//   index 54..57  Side-2 shorts/socks list — 3 entries + sentinel at [57].
//                 Side-2 starts at [2*0x17+8 = 0x36 = 54].
//
//   index 58..76  Unused (zeroed by the original for stack hygiene).
//
// Indexing for the shorts/socks scan:
//
//   sideIndex   start index   walks until terminator at
//   ─────────   ───────────   ──────────────────────────
//      0        8             46  (38 entries scanned)
//      1        31            46  (15 entries scanned)
//      2        54            57  (3 entries scanned)
//
// If you change MAX_PANEL_SLOTS or add a new side, ALSO extend this table
// — the lookup is hard-indexed by sideIndex.
// =============================================================================

// The single 77-entry table, laid out in dword indices 0..76.
// 0xFFFFFFFF acts as terminator for whichever sublist hits it first.
static const uint32_t KIT_VALID_MODEL_TABLE[77] = {
    // ── Shirt list [0..7] ────────────────────────────────────────────────
    0x22, 0x23, 0x24, 0x26, 0x27, 0x28, 0x29, 0xFFFFFFFF,
    // ── Side 0 shorts/socks list [8..46] ─────────────────────────────────
    /* [ 8] */ 0x64, /* [ 9] */ 0x65, /* [10] */ 0x67, /* [11] */ 0x68,
    /* [12] */ 0x69, /* [13] */ 0x43, /* [14] */ 0x44, /* [15] */ 0x45,
    /* [16] */ 0x46, /* [17] */ 0x47, /* [18] */ 0x48, /* [19] */ 0x49,
    /* [20] */ 0x4A, /* [21] */ 0x4C, /* [22] */ 0x4D, /* [23] */ 0x6A,
    /* [24] */ 0x6C, /* [25] */ 0x6D, /* [26] */ 0x6F, /* [27] */ 0x71,
    /* [28] */ 0x4B, /* [29] */ 0x6E, /* [30] */ 0x6F,
    // Side-1 sublist starts here — [31..45]
    /* [31] */ 0x43, /* [32] */ 0x44, /* [33] */ 0x45, /* [34] */ 0x46,
    /* [35] */ 0x47, /* [36] */ 0x48, /* [37] */ 0x49, /* [38] */ 0x4A,
    /* [39] */ 0x4C, /* [40] */ 0x4D, /* [41] */ 0x6A, /* [42] */ 0x6C,
    /* [43] */ 0x6D, /* [44] */ 0x6F, /* [45] */ 0x71,
    /* [46] */ 0xFFFFFFFF,                               // side 0 + side 1 terminator
    // ── Padding zeros [47..53] ──────────────────────────────────────────
    0, 0, 0, 0, 0, 0, 0,
    // ── Side 2 shorts/socks list [54..57] ───────────────────────────────
    /* [54] */ 0x4B, /* [55] */ 0x6E, /* [56] */ 0x70,
    /* [57] */ 0xFFFFFFFF,                               // side 2 terminator
    // ── Unused tail [58..76] ────────────────────────────────────────────
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// Implicit "first" shirt-model value, stored in the original at the dword
// just below validModelLookup[0] (defaultShirtModel = 0x20). The shirt
// scan begins by comparing kitData[0x2C] against this value.
static const uint32_t KIT_DEFAULT_SHIRT_MODEL = 0x20;

// Walk the table starting at `startIdx`, comparing each entry to `model`
// until either a match (return true) or the 0xFFFFFFFF terminator
// (return false). Matches the original's do/while structure.
static bool ScanModelTable(uint8_t model, int startIdx)
{
    if (startIdx < 0 || startIdx >= 77)
        return false;
    for (int i = startIdx; i < 77; ++i)
    {
        uint32_t entry = KIT_VALID_MODEL_TABLE[i];
        if (entry == 0xFFFFFFFF)
            return false;
        if (entry == model)
            return true;
    }
    return false;
}

// =============================================================================
// hook_ExtractKitColor  (replaces FUN_00968e70)
//
// Pulls a single RGB555 color from a team's kit-data buffer and writes it
// out as 4 bytes [R, G, B, 0xFF] into outRGBA.
//
// Parameters mirror the original:
//   outRGBA       — destination buffer, ≥4 bytes; nullptr → return 0
//   teamID        — raw team ID (NOT alias-resolved)
//   kitVariant    — 0..3, picks one of four kit variants in the data
//                   (0=home/normal, 1=away/normal, 2=home/swapped,
//                    3=away/swapped — the swap bit comes from
//                    DAT_03be6078).
//   colorPartType — which color slot to read:
//                       0 = shirt primary
//                       1 = (unused / passes through)
//                       2 = shirt model-validated
//                       3 = shorts (model-validated for non-edited)
//                       4 = socks  (model-validated for non-edited)
//                       5 = goalkeeper
//   sideIndex     — picks which sublist of KIT_VALID_MODEL_TABLE to walk
//                   for the shorts/socks model check (0, 1, or 2).
//
// Returns 1 on success, 0 on validation failure or missing kit data.
//
// Behaviour split (matches the original exactly):
//   * If kit is NOT edited:
//       - colorPartType==2 → validate shirt model (kitData[0x2C]) against
//         KIT_DEFAULT_SHIRT_MODEL then KIT_VALID_MODEL_TABLE[0..7].
//       - colorPartType==3 → validate shorts model (kitData[0x2D]) against
//         KIT_VALID_MODEL_TABLE[sideIndex*0x17+8 .. terminator].
//       - colorPartType==4 → validate socks model (kitData[0x2E]) against
//         the same per-side sublist.
//       - any other colorPartType → no validation (uses value as-is).
//   * If kit IS edited:
//       - colorPartType 0 ↔ 3 are SWAPPED.
//       - colorPartType 5 passes through unchanged.
//       - colorPartType 1, 2, or 4 → return 0.
//
//   Final extraction: read uint16_t at kitData[colorPartType*2] as RGB555,
//   spread to RGBA8888 by left-shifting each channel by 3 bits (5→8 bit
//   expansion; the low 3 bits are left zero — matches the original; if
//   you want proper rounding use replicate-low-bits, but DO NOT change
//   it without checking caller-visible color tests).
// =============================================================================
uint32_t __cdecl hook_ExtractKitColor(char* outRGBA, uint16_t teamID,
                                       int kitVariant, int colorPartType,
                                       int sideIndex)
{
    // Guard: invalid output or missing kit data → bail (returns 0 like original).
    if (!outRGBA || !fn_GetTeamKitData)
        return 0;

    uint8_t* kitData = fn_GetTeamKitData(teamID, kitVariant);
    if (!kitData)
        return 0;

    const bool edited = hook_IsTeamKitEdited(teamID);

    if (!edited)
    {
        // ── Non-edited path: optional model validation per colorPartType ──
        if (colorPartType == 2)
        {
            // Shirt model scan: implicit first entry is KIT_DEFAULT_SHIRT_MODEL
            // (0x20); on miss, walk KIT_VALID_MODEL_TABLE from index 0.
            const uint8_t shirtModel = kitData[0x2C];
            if (shirtModel != (uint8_t)KIT_DEFAULT_SHIRT_MODEL &&
                !ScanModelTable(shirtModel, 0))
                return 0;
        }
        else if (colorPartType == 3)
        {
            // Shorts model scan: start at sideIndex*0x17 + 8 within table.
            const uint8_t shortsModel = kitData[0x2D];
            const int startIdx = sideIndex * 0x17 + 8;
            if (!ScanModelTable(shortsModel, startIdx))
                return 0;
        }
        else if (colorPartType == 4)
        {
            // Socks model scan: same sublist as shorts.
            const uint8_t socksModel = kitData[0x2E];
            const int startIdx = sideIndex * 0x17 + 8;
            if (!ScanModelTable(socksModel, startIdx))
                return 0;
        }
        // colorPartType 0, 1, 5 fall through with NO validation.
    }
    else
    {
        // ── Edited-kit path: remap or reject ──
        if (colorPartType == 0)
            colorPartType = 3;
        else if (colorPartType == 3)
            colorPartType = 0;
        else if (colorPartType != 5)
            return 0;
    }

    // ── RGB555 → RGBA8888 (low 3 bits filled with 0, matches original) ──
    const uint16_t rgb555 = *reinterpret_cast<uint16_t*>(kitData + colorPartType * 2);
    outRGBA[0] = static_cast<char>((rgb555 & 0x1F)         << 3);  // R
    outRGBA[1] = static_cast<char>(((rgb555 >> 5)  & 0x1F) << 3);  // G
    outRGBA[2] = static_cast<char>(((rgb555 >> 10) & 0x1F) << 3);  // B
    outRGBA[3] = static_cast<char>(0xFF);                          // A

    return 1;
}

// =============================================================================
// hook_LoadBothTeamKitData  (wrapper of FUN_00969610, original at FUN_00969790)
//
// Original assembly (kept here for study — see disassembly of 0x00969790):
//
//     PUSH ESI
//     MOV  ESI, [ESP+8]      ; ESI = slotIndex
//     PUSH 0                 ; param2 = 0   (variant base / "side B" flag)
//     PUSH ESI               ; param1 = slotIndex
//     XOR  EAX, EAX          ; EAX = 0     ← side index for inner call
//     CALL FUN_00969610      ; LoadTeamKitDataBySlot(slotIndex, 0)  with EAX=0
//     PUSH 1                 ; param2 = 1
//     PUSH ESI
//     MOV  EAX, 1            ; EAX = 1     ← side index for inner call
//     CALL FUN_00969610      ; LoadTeamKitDataBySlot(slotIndex, 1)  with EAX=1
//     ADD  ESP, 0x10
//     POP  ESI
//     RET
//
// IMPORTANT: EAX is the SIDE INDEX, set explicitly by this wrapper. The
// inner function reads it via `int in_EAX;` as the row selector for its
// per-slot buffer math:
//     iVar10 = (in_EAX + slotIndex * 2) * 100;
// (Earlier notes claiming EAX was a "match context base" were wrong.)
//
// What this hook does today:
//   Forward to the trampoline unconditionally. The trampoline preserves
//   the wrapper's XOR/MOV-EAX instructions, so EAX is set correctly for
//   each inner call — we don't need a naked thunk on this wrapper.
//
// What this hook deliberately does NOT do:
//   We do NOT short-circuit on missing kit data. A previous version
//   `return`-ed early when GetTeamKitData(teamID, 0) was NULL, which
//   left DAT_03b8eea0 (the team data buffer at base+slotIndex*200 bytes)
//   filled with stale data from earlier frames. That caused crashes one
//   screen later. The original wrapper itself is crash-free; the only
//   crash is inside FUN_00969610 on `puVar5[0x3a]` for unrecognised
//   team IDs (see FUTURE WORK below).
//
// Why we still hook it:
//   So the registration plumbing (trampoline, INSTALL_HOOK) is in place
//   and we have a single chokepoint where future kit-modding logic can
//   land (e.g. logging which slots loaded, swapping team IDs at the
//   wrapper boundary, or pre-fetching custom kit data).
// =============================================================================
void __cdecl hook_LoadBothTeamKitData(int slotIndex)
{
    // (Trampoline-only forward — see header comment for rationale.)
    if (orig_LoadBothTeamKitData)
        orig_LoadBothTeamKitData(slotIndex);
}

// =============================================================================
// hook_LoadTeamKitDataBySlot  (replaces FUN_00969610)
//
// Inner per-side kit loader. The wrapper FUN_00969790 calls this twice per
// slot (once with EAX=0, once with EAX=1); EAX is the side index, set
// explicitly by the wrapper. We capture it via a __declspec(naked) thunk
// and pass it as an explicit third cdecl argument here.
//
// THE REASON THIS HOOK EXISTS:
//   The original derefs `puVar5[0x3a]` where puVar5 = GetTeamKitData(...).
//   GetTeamKitData returns NULL for unrecognised team IDs (custom-modded
//   slots like 200, 250, anything outside 0..0x111). The original crashes;
//   we add a single NULL-guard and route to LoadTeamData_MLDefault, which
//   produces a safe default buffer.
//
// FAITHFUL REIMPLEMENTATION:
//   The body below mirrors FUN_00969610's decompile EXCEPT for the one
//   NULL-safety branch. Buffer offsets, bit-shifts, weather byte stamping,
//   side-flag math, and dispatch order are all preserved. If you need
//   to change anything, re-read the disassembly at 0x00969610 first —
//   the original is dense but very mechanical.
//
// PER-SLOT BUFFER LAYOUT (base = KIT_PER_SLOT_BUFFER, stride 100 bytes per
// (sideIdx + slotIdx*2) entry):
//   buf+0x01  = (byte)slotIdx
//   buf+0x0e  = (short) ((short)param_2 + resolvedID*2) * 6
//   buf+0x10..buf+0x33  zeroed (10 dwords; scratch for callees)
//   buf+0x11  = (param_2 == 0) ? 1 : 0
//   buf+0x34  = (sideFlag >> bitShift)       & 1
//   buf+0x35  = (sideFlag >> (bitShift + 1)) & 1
//   buf+0x36  = (sideFlag >> (bitShift + 2)) & 1
//   buf+0x38..buf+0x61  zeroed (10 dwords + 1 word; team data area)
//   buf+0x62  = current weather condition byte
//
//   sideFlag = KIT_SIDE_FLAG_TABLE[slotIdx]
//   bitShift = (param_2 == 0) ? 3 : 0
//
// MODE/ONLINE GATE (matches the original branches exactly):
//   We are "in ML or online" when MODE_FLAG == 5  OR  IsOnlineMode == 1.
//   When in ML/online AND resolvedID is NOT 0x110 / 0x111:
//       → call LoadTeamData_MLDefault and return early.
//   Otherwise fall through to dispatch.
//
//   IMPORTANT: in normal exhibition (not ML, not online), this gate is
//   skipped entirely — every team goes to dispatch. Earlier drafts had
//   the inverted condition; the disassembly at 0x009696f4 (JZ to the
//   resolvedID check) is the authoritative source.
//
// DISPATCH (after the gate; uses kitData = GetTeamKitData(teamID, ...)):
//   if kitData[0x3a] == 0:
//       if HasCustomFormation(slotIdx) != 0:
//           formationIdx = HasCustomFormation(slotIdx) - 1   // re-called
//           LoadTeamData_CustomFormation(buf, teamID, formationIdx, param_2)
//       else:
//           LoadTeamData_Normal(buf, teamID, param_2, slotIdx)
//   else:
//       LoadTeamData_EditedKit(buf, teamID, param_2)
//
// The double HasCustomFormation call mirrors the original — it does NOT
// cache the result; both reads see the same value in practice but the
// pattern is preserved for byte-for-byte fidelity.
// =============================================================================
extern "C" void __cdecl hook_LoadTeamKitDataBySlot(int slotIndex,
                                                    int param_2,
                                                    int sideIndex)
{
    // ── Lookup raw team ID for this slot ──────────────────────────────────
    const uint16_t teamID = KIT_TEAM_ID_TABLE[slotIndex];

    // ── Compute per-slot buffer base (matches the original's iVar10) ──────
    const int iVar10 = (sideIndex + slotIndex * 2) * 100;
    uint8_t* pBuf = KIT_PER_SLOT_BUFFER + iVar10;

    // ── Resolve team ID alias (0x126/0x127 → real ID) ─────────────────────
    // If the resolver is missing we leave resolvedID=0; the mode gate
    // will then route to MLDefault for any non-0x110/0x111 team in
    // ML/online mode, which is the safest available fallback.
    short resolvedID = 0;
    if (fn_ResolveTeamID)
        resolvedID = fn_ResolveTeamID(teamID);

    // ── Zero team-data region (offsets +0x38..+0x61, 10 dwords + 1 word) ──
    // Original uses STOSD;STOSW with EDI=ESI+0x38, ECX=10.
    memset(pBuf + 0x38, 0, 10 * 4 + 2);

    // ── Zero scratch region (offsets +0x10..+0x33, 10 dwords) ─────────────
    memset(pBuf + 0x10, 0, 10 * 4);

    // ── Stamp weather condition byte at +0x62 ─────────────────────────────
    if (fn_GetWeatherCondition)
        pBuf[0x62] = static_cast<uint8_t>(fn_GetWeatherCondition());

    // ── Compute side-flag bits and stamp slot/side metadata ───────────────
    const uint8_t sideFlag = KIT_SIDE_FLAG_TABLE[slotIndex];
    const int     bitShift = (param_2 == 0) ? 3 : 0;
    const uint8_t bit0     = (sideFlag >>  bitShift)      & 1;
    const uint8_t bit1     = (sideFlag >> (bitShift + 1)) & 1;
    const uint8_t bit2     = (sideFlag >> (bitShift + 2)) & 1;

    pBuf[0x34] = bit0;
    pBuf[0x35] = bit1;
    pBuf[0x36] = bit2;
    pBuf[0x11] = (param_2 == 0) ? 1 : 0;
    pBuf[0x01] = static_cast<uint8_t>(slotIndex);
    *reinterpret_cast<int16_t*>(pBuf + 0x0E) = static_cast<int16_t>(
        (static_cast<int16_t>(param_2) + resolvedID * 2) * 6);

    // ── Look up the kit-data buffer for this team + variant ───────────────
    // Variant index folds the home/away (param_2) flag with the swap bit
    // (bit0). bit0=1 selects the swapped half of the 4-variant kit table.
    uint8_t* kitData = nullptr;
    if (fn_GetTeamKitData)
        kitData = fn_GetTeamKitData(teamID, param_2 + bit0 * 2);

    // ── Mode/online gate ──────────────────────────────────────────────────
    // Normal exhibition (mode != 5 AND not online) → fall through to
    // dispatch unconditionally. Otherwise, only the two "ML default"
    // reserved IDs (0x110, 0x111) are allowed past this gate.
    bool inMLOrOnline = (MODE_FLAG == ML_CLUB_SCREEN_MODE);
    if (!inMLOrOnline)
    {
        const int online = fn_IsOnlineMode ? fn_IsOnlineMode() : 0;
        inMLOrOnline = (online == 1);
    }
    if (inMLOrOnline && resolvedID != 0x111 && resolvedID != 0x110)
    {
        if (fn_LoadTeamData_MLDefault)
            fn_LoadTeamData_MLDefault(reinterpret_cast<char*>(pBuf));
        return;
    }

    // ── NULL-safety guard (ADDED — not in original) ───────────────────────
    // If GetTeamKitData returned NULL the original would crash on the
    // kitData[0x3a] read below. With hook_GetTeamKitData installed, NULL
    // means "no stock data AND no kits/<id>.ini override" — fall back to
    // MLDefault (safe + already populated for any team).
    if (!kitData)
    {
        if (fn_LoadTeamData_MLDefault)
            fn_LoadTeamData_MLDefault(reinterpret_cast<char*>(pBuf));
        return;
    }

    // ── Dispatch ──────────────────────────────────────────────────────────
    if (kitData[0x3A] == 0)
    {
        // Non-edited kit. Custom formation takes priority over normal load.
        char hasCF = 0;
        if (fn_HasCustomFormation)
            hasCF = fn_HasCustomFormation(static_cast<uint8_t>(slotIndex));

        if (hasCF != 0)
        {
            // Re-call to fetch the formation index (matches the original's
            // double call — it doesn't cache the result).
            uint8_t cfIdx = 0;
            if (fn_HasCustomFormation)
                cfIdx = static_cast<uint8_t>(
                    fn_HasCustomFormation(static_cast<uint8_t>(slotIndex)));
            if (fn_LoadTeamData_CustomFormation)
                fn_LoadTeamData_CustomFormation(reinterpret_cast<char*>(pBuf),
                                                 teamID,
                                                 static_cast<int>(cfIdx) - 1,
                                                 param_2);
            return;
        }

        if (fn_LoadTeamData_Normal)
            fn_LoadTeamData_Normal(reinterpret_cast<char*>(pBuf),
                                    teamID, param_2, slotIndex);
        return;
    }

    // Edited kit
    if (fn_LoadTeamData_EditedKit)
        fn_LoadTeamData_EditedKit(reinterpret_cast<char*>(pBuf),
                                   teamID, param_2);
}

// =============================================================================
// hook_LoadTeamKitDataBySlot_Naked  — __declspec(naked) thunk
//
// Captures the implicit EAX side-index parameter and forwards everything
// to the cdecl C handler above.
//
// On entry from MinHook's installed jump (caller is FUN_00969790):
//     [esp+0]  = retaddr
//     [esp+4]  = slotIndex (caller's param_1)
//     [esp+8]  = param_2   (caller's param_2)
//     EAX      = sideIndex (set by the wrapper to 0 then 1)
//
// Cdecl arg-push order is right-to-left, so we push sideIndex first
// (becomes arg3), then param_2 (arg2), then slotIndex (arg1), then call.
// After the call we discard the 3 pushed args with `add esp, 12` and
// return — caller (cdecl) cleans up its own 2 args.
//
// STACK MATH (verifies the [esp+12] reads):
//   After  push eax              : caller's slotIndex now at [esp+8],
//                                  param_2 now at [esp+12].
//   After  push [esp+12] (p_2)   : slotIndex now at [esp+12],
//                                  pushed param_2 at [esp+0].
//   After  push [esp+12] (slot)  : slotIndex correctly pushed last.
//
// EAX is caller-saved in cdecl, so we are not required to preserve it
// across the call, and the wrapper FUN_00969790 explicitly re-loads EAX
// (`MOV EAX,1`) before the second invocation regardless.
// =============================================================================
extern "C" __declspec(naked) void hook_LoadTeamKitDataBySlot_Naked()
{
    __asm
    {
        push eax                        ; sideIndex (arg3)
        push dword ptr [esp + 12]       ; param_2   (arg2)
        push dword ptr [esp + 12]       ; slotIndex (arg1)
        call hook_LoadTeamKitDataBySlot
        add  esp, 12
        ret
    }
}

// =============================================================================
// hook_Match_SetupKitData  (replaces FUN_00968700)
//
// Tiny per-frame dispatcher — the actual decompile is just:
//
//     puVar1 = GetTeamKitData(param_2, (int)param_3);
//     if (puVar1[0x3a] != 0)
//         FUN_009683c0(param_3, param_1, param_2);   // edited-kit setup
//                                                     //   (fastcall: ECX=param_3,
//                                                     //    stack=param_1,param_2)
//     else
//         FUN_00968690(param_1, puVar1, param_3, param_4);  // non-edited setup
//                                                            //   (cdecl)
//
// THE CRASH being shielded:
//   GetTeamKitData returns a NON-NULL but garbage pointer for team IDs that
//   live above the standard 0..0x111 band — typical for custom-modded
//   teams (>200). The very next instruction (`MOV CL, [EAX+0x3a]` at
//   0x00968711) then derefs that bad pointer and the game crashes.
//
//   This is the same pattern as IsTeamKitEdited / ExtractKitColor /
//   LoadTeamKitDataBySlot: a NULL check alone is not enough — the
//   range guard `teamID > 200` is what actually shields the crash.
//
// FIX:
//   For team IDs in the custom range, short-circuit and return without
//   calling the original. Both downstream callees deref the kit-data
//   pointer themselves, so we cannot "use defaults" inside the original
//   without supplying a synthesized buffer. Skipping cleanly is the only
//   safe option until a custom-kit-loader is wired in here.
//
//   For every team in the standard range we forward straight through to
//   the trampoline so the original behaviour (edited vs non-edited
//   dispatch) is preserved byte-for-byte.
//
// FUTURE WORK — custom kit loading:
//   This hook is the natural chokepoint for loading custom kit
//   configurations from text files for modded team IDs (200, 250, etc.).
//   When that work lands, replace the early `return` in the >200 branch
//   with code that:
//     1. Reads a per-team config file (RGB values, shirt model IDs, etc.)
//     2. Synthesizes a kit-data buffer in our own memory
//     3. Calls FUN_00968690 directly with that buffer (non-edited path)
//        OR FUN_009683c0 with the appropriate fastcall trampoline.
//   Keep the `param_2 > 200` guard — it remains the entry condition for
//   the custom path.
//
// PARAMETERS (per Ghidra decompile of FUN_00968700):
//   param_1  — undefined4. Some context handle/pointer; passed unchanged
//              to both callees.
//   param_2  — ushort. The team ID (raw, NOT alias-resolved).
//   param_3  — void*. Used as the variant index passed to GetTeamKitData
//              (cast to int). Also passed to both callees.
//   param_4  — int. Only used by the non-edited callee FUN_00968690.
//
// CALLING CONVENTION:
//   __cdecl. The disassembly's prologue (PUSH ESI; PUSH EDI; ... POP EDI;
//   POP ESI; RET) and matching `ADD ESP, N` after each internal call
//   confirm the caller cleans up.
// =============================================================================
void __cdecl hook_Match_SetupKitData(uint32_t param_1, uint16_t param_2,
                                       void*    param_3, int      param_4)
{
    // ── NULL-probe via our chokepoint ────────────────────────────────────
    // The original derefs `puVar1[0x3a]` immediately after fetching kit
    // data, with no NULL check. With hook_GetTeamKitData in place, a NULL
    // return means "no stock data AND no kits/<id>.ini override" — there
    // is nothing safe to dispatch on, so skip cleanly. (For a custom team
    // with an INI file, fn_GetTeamKitData returns the synthesised buffer
    // and the second call inside the original is a cache hit.)
    if (fn_GetTeamKitData)
    {
        const int variant = static_cast<int>(reinterpret_cast<intptr_t>(param_3));
        if (!fn_GetTeamKitData(param_2, variant))
            return;
    }

    // ── Forward to the original trampoline ───────────────────────────────
    if (orig_Match_SetupKitData)
        orig_Match_SetupKitData(param_1, param_2, param_3, param_4);
}

// =============================================================================
// hook_Match_SetupKitData2  (replaces FUN_00968960)
//
// STRUCTURAL TWIN of hook_Match_SetupKitData (FUN_00968700). Same four-arg
// cdecl signature, same GetTeamKitData → kitData[0x3a] dispatch, but
// routes to a different pair of callees:
//
//     puVar1 = GetTeamKitData(param_2, (int)param_3);
//     if (puVar1[0x3a] != 0)
//         FUN_00968410(param_3);                        // edited-kit path
//                                                        //   (fastcall: ECX=param_3)
//     else
//         FUN_009688e0(param_1, puVar1, param_4);        // non-edited path
//                                                        //   (cdecl)
//
// We don't yet know what's different about THIS dispatcher vs FUN_00968700
// (likely a different kit-component pipeline — shorts vs shirts, home vs
// away, or one of the per-player rendering passes), but the difference
// doesn't matter for crash mitigation: both functions deref the kit-data
// pointer right after fetching it, and both blow up at IDs > 200.
//
// THE CRASH being shielded:
//   At 0x00968971 the function does `MOV CL, [EAX+0x3a]` immediately after
//   GetTeamKitData. For team IDs > 200 the lookup returns a non-NULL but
//   garbage pointer, and the deref crashes. Same root cause as
//   FUN_00968700 / FUN_00969610 / FUN_008654d0 / FUN_00968e70.
//
// FIX:
//   Identical to hook_Match_SetupKitData — for `param_2 > 200` short-circuit
//   and return; otherwise forward to the trampoline so the original
//   edited-vs-non-edited dispatch is preserved verbatim.
//
// FUTURE WORK:
//   When the custom kit-loader work in hook_Match_SetupKitData lands, mirror
//   it here: read the same per-team config, synthesize a kit-data buffer,
//   and dispatch manually to FUN_009688e0 (or FUN_00968410 for the edited
//   path). The two hooks should share a single config-reader helper —
//   factor it out at that point rather than duplicating per dispatcher.
// =============================================================================
void __cdecl hook_Match_SetupKitData2(uint32_t param_1, uint16_t param_2,
                                        void*    param_3, int      param_4)
{
    // ── NULL-probe via our chokepoint ────────────────────────────────────
    // Same situation as FUN_00968700: the original derefs `puVar1[0x3a]`
    // straight after fetching kit data. NULL from the chokepoint means
    // "no data" — skip cleanly. See hook_Match_SetupKitData for context.
    if (fn_GetTeamKitData)
    {
        const int variant = static_cast<int>(reinterpret_cast<intptr_t>(param_3));
        if (!fn_GetTeamKitData(param_2, variant))
            return;
    }

    // ── Forward to the original trampoline ───────────────────────────────
    if (orig_Match_SetupKitData2)
        orig_Match_SetupKitData2(param_1, param_2, param_3, param_4);
}

// =============================================================================
// hook_GetTeamKitData  (replaces FUN_00865240)
//
// THE central kit-data chokepoint. Every kit hook in the pipeline
// (IsTeamKitEdited, ExtractKitColor, LoadTeamKitDataBySlot,
// Match_SetupKitData, Match_SetupKitData2) ultimately calls into this
// function to fetch a (teamID, variant) → kitData slot pointer. By owning
// this call we own all of them.
//
// RESOLUTION ORDER:
//   1. Custom INI override (kits/<teamID>.ini) via TryLoadCustomKitData.
//      If a config exists, its synthesised buffer is returned. This wins
//      over EVERYTHING — even stock teams (which is the whole point of
//      retexture mods).
//   2. Faithful re-creation (RE_GetTeamKitData) for the standard ranges.
//      Returns NULL for unknown IDs and for hits on the original's
//      garbage fallback buffer.
//
// WHY WE DON'T CALL orig_GetTeamKitData:
//   The original returns `&DAT_00c97340 + variant*0x3E` for unrecognised
//   IDs — a non-NULL but garbage pointer. The very next instruction in
//   most callers is `MOV CL, [EAX+0x3a]` which then reads garbage and
//   crashes. RE_GetTeamKitData mirrors the original byte-for-byte for
//   every legal range, but converts that one garbage path to NULL so
//   downstream callers can NULL-check and short-circuit cleanly.
//
// CALLING CONVENTION:
//   __cdecl, matching the original. Two cdecl args, no implicit
//   register parameter (unlike LoadTeamKitDataBySlot which uses EAX —
//   verified by Ghidra's prototype and by every caller pushing both
//   args before calling). Therefore no naked thunk needed.
//
// EFFECTS ON OTHER HOOKS:
//   With this hook installed, every `fn_GetTeamKitData(...)` call site
//   in the existing kit hooks (IsTeamKitEdited, ExtractKitColor, etc.)
//   automatically routes through this function — no change needed in
//   the callers. The `teamID > 200` short-circuits in those hooks
//   become redundant once this hook returns NULL safely; clean-up of
//   those is tracked separately as future work.
// =============================================================================
uint8_t* __cdecl hook_GetTeamKitData(uint16_t teamID, int variant)
{
    // ── 1. Custom INI override ───────────────────────────────────────────
    if (uint8_t* custom = TryLoadCustomKitData(teamID, variant))
        return custom;

    // ── 2. Faithful stock-data path with NULL-safety ─────────────────────
    return RE_GetTeamKitData(teamID, variant);
}

// =============================================================================
// hook_GetTeamKitDataB  (replaces FUN_00865380)
//
// Companion chokepoint to hook_GetTeamKitData. The original returns a
// per-(team, variant) pointer to the 0x18-byte sub-record at offset +0x100
// within the team's kit-data buffer. Like its sibling at FUN_00865430, the
// original walks an unbounded table for unknown team IDs and produces a
// non-NULL but garbage pointer, which the caller `FUN_00809a50` then
// dereferences (writing 0xFFFFFFFF sentinels into +8..+0x17).
//
// RESOLUTION ORDER:
//   1. Custom INI override (kits/<teamID>.ini → cached extra-B buffer).
//      The current INI format does not yet populate this region; the
//      buffer is zero-initialised, which is the safe scaffolding the
//      caller's default-init pass expects.
//   2. Faithful re-creation (RE_GetTeamKitDataB) for legitimate stock IDs.
//      Returns NULL for unknown IDs (one intentional departure from the
//      original's last-resort garbage path; see RE_GetTeamKitDataB).
//
// CALLING CONVENTION: __cdecl, two args, no implicit register params.
// =============================================================================
uint8_t* __cdecl hook_GetTeamKitDataB(uint16_t teamID, int variant)
{
    if (uint8_t* custom = TryLoadCustomKitExtraB(teamID, variant))
        return custom;
    return RE_GetTeamKitDataB(teamID, variant);
}

// =============================================================================
// hook_GetTeamKitDataC  (replaces FUN_00865430)
//
// THE chokepoint that fixes the second crash family — the deref at
// 0x00967BC9 (`MOV DL, [ESI + 0x3]` inside PreloadTeamKitGraphics, with
// ESI = result of FUN_00865430). The original returns a 0x30-byte
// sub-record at offset +0x160 within the team's kit-data buffer for the
// club path, and NULL for the national path; for unknown club-range IDs
// it returns a wild pointer that the caller derefs unconditionally.
//
// RESOLUTION ORDER:
//   1. Custom INI override (kits/<teamID>.ini → cached extra-C buffer).
//      Buffer is zero-initialised by default, which means the +3 byte
//      (kit-graphic resource ID) is zero, which makes
//      PreloadTeamKitGraphics skip this variant cleanly.
//   2. Faithful re-creation (RE_GetTeamKitDataC) for legitimate stock IDs.
//
// CALLING CONVENTION: __cdecl, two args, no implicit register params.
// =============================================================================
uint8_t* __cdecl hook_GetTeamKitDataC(uint16_t teamID, int variant)
{
    if (uint8_t* custom = TryLoadCustomKitExtraC(teamID, variant))
        return custom;
    return RE_GetTeamKitDataC(teamID, variant);
}

// =============================================================================
// hook_FillKitVariantSlot  (replaces FUN_00967170)
//
// Crash family #3 — the third crash that surfaced after hook_GetTeamKitData{B,C}
// were installed. Faulting EIP is `0x00967188`:
//
//   00967184  MOV  EAX, [ESI + EBX*4 + 0x8]   ; src->kitDataB[slot]
//   00967188  MOVZX ECX, byte ptr [EAX + 0x3] ; ← NULL deref
//
// The crash chain is:
//   1. InitMatchKitContext (FUN_00966d50) populates the global match-kit
//      context struct (0x3a683f0) at offsets +8..+0x14 with the results of
//      GetTeamKitDataB(teamID, 0..3).
//   2. With our hooks installed, GetTeamKitDataB returns NULL for any
//      team ID that has no `kits/<id>.ini` override and isn't in the
//      stock dynamic-allocated table — so the +8..+0x14 slots are NULL.
//   3. BuildMatchKitDescriptors (FUN_00967d90) then iterates those slots.
//      Its skip condition is `(mode == 0 && ptr != NULL && ptr[2] == 0)` —
//      a NULL pointer fails the second clause and falls into the else
//      branch, dispatching to FillKitVariantSlot which derefs ptr+3.
//
// The dispatcher's skip path writes three zeros per slot:
//   dst[0x30 + slot*4] = 0   (puVar7[-0x2b] in the decompile)
//   dst[0x08 + slot*4] = 0   (the OUTPUT pointer slot, mirrors src layout)
//   dst[0xdc + slot*4] = 0   (*puVar7)
//
// Our hook reproduces that exact skip-path output write when the source
// pointer is NULL, then returns. Otherwise we forward to the trampoline.
//
// RESOLUTION ORDER:
//   1. If src->kitData[slot] is NULL → write the skip zeros and return.
//   2. Otherwise → forward to orig_FillKitVariantSlot trampoline.
//
// CALLING CONVENTION: __cdecl, four args. The dispatcher pushes:
//   PUSH variantIdx ; PUSH src ; PUSH dst ; PUSH slotIdx ; CALL FUN_00967170
// (See disassembly at 0x00967e33..0x00967e3b.)
//
// orig_FillKitVariantSlot / orig_FillKitTeamSlot are declared extern in
// club_hooks_common.h and defined in club_hooks_register.cpp.
// =============================================================================
void __cdecl hook_FillKitVariantSlot(int slotIdx, int dst, int src, int variantIdx)
{
    // src struct: pointer at +8+slotIdx*4 is the GetTeamKitDataB result.
    // Note: FUN_00967170's iVar6 = slotIdx + 2 makes both `src + 8 + slotIdx*4`
    // and `src + iVar6*4` resolve to the SAME address — one NULL check
    // covers both reads.
    uint8_t** kitDataSlot = reinterpret_cast<uint8_t**>(src + 8 + slotIdx * 4);
    if (*kitDataSlot == nullptr) {
        // Mirror dispatcher skip path (BuildMatchKitDescriptors @
        // 0x00967e1a..0x00967e2b). variantIdx is intentionally ignored
        // here — the dispatcher's skip path doesn't depend on it either.
        (void)variantIdx;
        *reinterpret_cast<int32_t*>(dst + 0x30 + slotIdx * 4) = 0;
        *reinterpret_cast<int32_t*>(dst + 0x08 + slotIdx * 4) = 0;
        *reinterpret_cast<int32_t*>(dst + 0xdc + slotIdx * 4) = 0;
        return;
    }
    orig_FillKitVariantSlot(slotIdx, dst, src, variantIdx);
}

// =============================================================================
// hook_FillKitTeamSlot  (replaces FUN_00966f70)
//
// Sibling of hook_FillKitVariantSlot for the `+0..+4` slot range of the
// match-kit context (the per-team data pointers, populated elsewhere in
// the pipeline — they may also be NULL when InitMatchKitContext's
// teamID-range guard skips population). FUN_00966f70 doesn't have a
// built-in NULL guard like FUN_009672c0 (FillKitExtraSlot) does; it
// derefs `pcVar2[2]` unconditionally at 0x00966fa5.
//
// Calling convention is __thiscall:
//   ECX = slotIdx ; PUSH src ; PUSH dst ; CALL FUN_00966f70
// (See dispatcher disassembly 0x00967dd8..0x00967de0.)
//
// MinHook can't replace a __thiscall directly with a C function whose
// first param is in ECX, so we use a __declspec(naked) thunk: it pulls
// ECX into a 4th cdecl arg and tail-calls the C handler. The handler
// then either short-circuits (NULL src ptr → mirror skip-path zeros) or
// forwards to a small naked trampoline that pushes args back in
// __thiscall order before calling orig_FillKitTeamSlot.
//
// Skip-path writes (dispatcher @ 0x00967dc8..0x00967dd4):
//   dst[0x30 + slot*4] = 0   (puVar7[-0x29] when puVar7 = dst + 0xd4)
//   dst[0x00 + slot*4] = 0   (mirror of source pointer slot in output)
//   dst[0xd4 + slot*4] = 0   (*puVar7)
//
// The slot-0 write address `dst[0x00]` is the start of the same field
// the inner function writes — see disassembly 0x00966ffe (`MOV [EDI+EBX*4], EAX`).
// =============================================================================
extern "C" void __cdecl hook_FillKitTeamSlot_C(int dst, int src, int slotIdx)
{
    uint8_t** teamDataSlot = reinterpret_cast<uint8_t**>(src + slotIdx * 4);
    if (*teamDataSlot == nullptr) {
        *reinterpret_cast<int32_t*>(dst + 0x30 + slotIdx * 4) = 0;
        *reinterpret_cast<int32_t*>(dst + 0x00 + slotIdx * 4) = 0;
        *reinterpret_cast<int32_t*>(dst + 0xd4 + slotIdx * 4) = 0;
        return;
    }
    // Forward to original trampoline. orig_FillKitTeamSlot is the raw
    // __thiscall — call via inline asm to set ECX correctly.
    __asm
    {
        mov  ecx, slotIdx
        push src
        push dst
        call dword ptr [orig_FillKitTeamSlot]
        add  esp, 8
    }
}

// Naked bridge: dispatcher calls us with ECX=slotIdx, [ESP+4]=dst,
// [ESP+8]=src. We re-push the args in cdecl order (slotIdx as a 3rd
// stack arg) and tail into hook_FillKitTeamSlot_C, which preserves the
// __thiscall caller's stack discipline (caller does ADD ESP,8 after).
extern "C" __declspec(naked) void hook_FillKitTeamSlot_Naked()
{
    __asm
    {
        // Read original __thiscall args off the dispatcher's stack frame:
        //   [esp+0]  = return addr (back to dispatcher)
        //   [esp+4]  = dst   (1st pushed arg)
        //   [esp+8]  = src   (2nd pushed arg)
        //   ecx      = slotIdx (this)
        push ecx                  // slotIdx (cdecl arg 3)
        push dword ptr [esp + 0xC] // src    (cdecl arg 2)
        push dword ptr [esp + 0xC] // dst    (cdecl arg 1)
        call hook_FillKitTeamSlot_C
        add  esp, 0xC             // clean up cdecl args
        ret                       // dispatcher's `ADD ESP,8` cleans the
                                  // original __thiscall pushes
    }
}
