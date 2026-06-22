// src/hooks/league_teams_hook.cpp
//
// Rewrite of FUN_009d2ec0 — "SetLeagueTeamCount"
//
// This function determines how many teams are displayed for a league page.
// It reads league/formation state globals, computes a league index, sets
// team count and display limit globals, and updates UI state flags.
//
// All addresses are hardcoded from Ghidra analysis of PES4 (FUN_009d2ec0).
// Convention: __stdcall, no arguments, no return value.
//
#include "league_teams_hook.h"
#include "MinHook/include/MinHook.h"
#include "../utils/logger.h"
#include <windows.h>
#include <cstdint>

// =============================================================================
// SECTION 1 — Game global variable accessors
//
// Each macro dereferences a hardcoded absolute address as the appropriate
// type. Using macros rather than pointer variables keeps the code visually
// close to the Ghidra pseudocode and makes it easy to cross-reference.
//
// Naming convention:
//   GWORD(addr)  — 16-bit signed short  (Ghidra "short" / word ptr)
//   GDWORD(addr) — 32-bit signed int    (Ghidra "int"   / dword ptr)
//   GBYTE(addr)  — 8-bit signed char    (Ghidra "byte"  / byte ptr, sign-extended)
//
// All addresses from Ghidra analysis of PES4.exe.
// =============================================================================

#define GWORD(addr)   (*reinterpret_cast<int16_t*>(addr))
#define GDWORD(addr)  (*reinterpret_cast<int32_t*>(addr))
#define GBYTE(addr)   (*reinterpret_cast<int8_t* >(addr))

// ── Input globals (read to compute league index and mode) ────────────────────

// Contributes to league index: leagueID component (byte, sign-extended to int)
// Ghidra: DAT_03a74050   MOVSX EAX, byte ptr [DAT_03a74050]
#define LEAGUE_ID_MAJOR     GBYTE(0x03a74050)

// Contributes to league index: offset component (byte, sign-extended to int)
// Ghidra: DAT_03a74048   MOVSX ECX, byte ptr [DAT_03a74048]
#define LEAGUE_ID_MINOR     GBYTE(0x03a74048)

// Formation / league type enum (0–5+)
// Controls the switch that picks team count (18 or 20)
// Ghidra: DAT_03a741fe   MOV BP, word ptr [DAT_03a741fe]
#define LEAGUE_TYPE         GWORD(0x03a741fe)

// Mode flag — when == 4 triggers Master League / Cup display overrides
// Ghidra: DAT_03becc60
#define GAME_MODE           GDWORD(0x03becc60)

// ── Team count / display limit globals (read and written) ────────────────────

// Maximum number of teams for current league config
// Ghidra: DAT_03a748a6   (word)
#define MAX_TEAMS           GWORD(0x03a748a6)

// Display team count (used for page layout)
// Ghidra: DAT_03a7489e   (word)
#define DISPLAY_TEAM_COUNT  GWORD(0x03a7489e)

// Current active team count value (read at function start, used for comparison)
// Ghidra: DAT_03a74a46   (word)
#define ACTIVE_USER_TEAM_COUNT   GWORD(0x03a74a46)

// Effective team count limit (written during clamping logic)
// Ghidra: DAT_03a74a4e   (word, written as _DAT_03a74a4e in pseudocode)
#define EFFECTIVE_TEAM_COUNT GWORD(0x03a74a4e)

// Used when iVar2 == 1: resets to 1 (formation/arrangement index?)
// Ghidra: DAT_03a743a6   (word, _DAT_03a743a6 in pseudocode)
#define IS_FULL_SEASON   GWORD(0x03a743a6)

// ── Match/animation state globals ────────────────────────────────────────────

// Ghidra: DAT_03a75938   (_DAT_03a75938, dword)
#define BALL_OPTION_DISABLED       GDWORD(0x03a75938)

// Ghidra: DAT_03a75970   (_DAT_03a75970, dword)
#define BALL_OPTION_SHOW_RANDOM       GDWORD(0x03a75970)

// Ghidra: DAT_03a7592e   (word)
#define BALL_SETTING_SET       GWORD(0x03a7592e)

// ── UI / navigation flag globals ─────────────────────────────────────────────

// Controls whether the "next page" / scroll button is shown (1 = show, 0 = hide)
// Ghidra: DAT_03a74700   (_DAT_03a74700, dword)
#define CUP_OPTION_DISABLED    GDWORD(0x03a74700)

// Paired with CUP_OPTION_DISABLED — cleared when button is shown
// Ghidra: DAT_03a746f6   (_DAT_03a746f6, word)
#define CUP_DURING_SEASON   GWORD(0x03a746f6)

// ── Master League / Cup mode override globals ─────────────────────────────────

// Ghidra: DAT_03a748a8   (_DAT_03a748a8, dword)
#define NUMBER_OF_TEAMS_OPTION_DISABLED       GDWORD(0x03a748a8)

// Ghidra: DAT_03a74558   (_DAT_03a74558, dword)
#define ELIGIBLE_TEAMS_OPTION_DISABLED       GDWORD(0x03a74558)

// Ghidra: DAT_03a7454e   (_DAT_03a7454e, word)
#define ELIGIBLE_TEAMS_TYPE       GWORD(0x03a7454e)

// Ghidra: DAT_03a743b0   (_DAT_03a743b0, dword)
#define SEASON_OPTION_DISABLED       GDWORD(0x03a743b0)

// =============================================================================
// SECTION 2 — Constants
// =============================================================================

// The league index value that triggers team count reconfiguration
static const int  LEAGUE_INDEX_RECONFIGURE = 1;

// League type values from the switch statement
static const int16_t LEAGUE_TYPE_18_TEAMS  = 1;  // case 1: 18 teams
// cases 0,2,3,4,5 and default: 20 teams

// Team count values
static const int16_t TEAM_COUNT_20         = 20;
static const int16_t TEAM_COUNT_18         = 18;

// Match state constant used in the DAT_03a741fe == 2 branch
static const int16_t BALL_SETTING_SPECIFIC_LALIGA    = 0xc;

// Game mode value that triggers ML/Cup overrides
static const int32_t GAME_MODE_ML_CUP      = 4;

// Minimum team count required to show the next-page button
static const int16_t CUP_DURING_SEASON_MAX_USER_TEAMS = 2;  // "if (1 < DAT_03a74a46)"

//Team types
static const int8_t NATIONAL_TEAMS = 0;
static const int8_t CLUB_TEAMS = 1;
static const int8_t ALL_TEAMS = 2;

// =============================================================================
// SECTION 3 — Hook infrastructure
// =============================================================================

typedef void (__stdcall *FN_SetLeagueTeamCount)();
static FN_SetLeagueTeamCount orig_SetLeagueTeamCount = nullptr;

// =============================================================================
// SECTION 4 — The replacement function
//
// Structured to match the Ghidra pseudocode as closely as possible.
// Each block is labelled with the corresponding Ghidra label or address
// so you can cross-reference directly.
//
// __stdcall: callee cleans stack. No arguments. Ghidra confirms: RET (no operand
// would be RETN 0 — check the actual RETN at 009d3048: it is plain C3 = RET,
// meaning __cdecl or __stdcall with no stack args. Ghidra marked __stdcall.)
// =============================================================================

void __stdcall hook_SetLeagueTeamCount()
{
    // ── Read inputs ──────────────────────────────────────────────────────────
    // Ghidra:
    //   sVar1 = DAT_03a74a46                       (save current active count)
    //   iVar2 = DAT_03a74050 * 0xd + DAT_03a74048  (league index)
    //
    // The MOVSX instructions in the ASM sign-extend bytes to 32-bit ints
    // before the multiply. We replicate that with explicit int casts.

    const int16_t savedActiveCount = ACTIVE_USER_TEAM_COUNT;  // sVar1
    const int     leagueIndex      = (int)LEAGUE_ID_MAJOR * 0xd
                                   + (int)LEAGUE_ID_MINOR;  // iVar2

    Logger::Log("[LeagueTeams] leagueIndex=%d leagueType=%d activeCount=%d gameMode=%d",
        leagueIndex, (int)LEAGUE_TYPE, (int)savedActiveCount, GAME_MODE);

    // ── League index == 1: reconfigure team counts ───────────────────────────
    // Ghidra: if (iVar2 != 1) goto LAB_009d2f75;
    // Only enters the switch when this specific league index is active.

    if (leagueIndex == LEAGUE_INDEX_RECONFIGURE)
    {
        // Switch on league type to set MAX_TEAMS and DISPLAY_TEAM_COUNT.
        // Ghidra switch on DAT_03a741fe:
        //
        //   case 1:          18 teams, then fall into shared setup
        //   case 0,2,3,4:    20 teams, fall into shared setup
        //   case 5:          20 teams, skip the ACTIVE_USER_TEAM_COUNT/ARRANGEMENT reset
        //   default (>5):    falls to caseD_6 directly (no count change)

        switch (LEAGUE_TYPE)
        {
        case 1:
            // 18-team league (e.g. a smaller national league)
            MAX_TEAMS          = TEAM_COUNT_18;
            DISPLAY_TEAM_COUNT = TEAM_COUNT_18;
            Logger::Log("[LeagueTeams] League type 1: setting 18 teams");

            // Shared setup for cases 0,1,2,3,4 — resets active count and
            // arrangement index to 1.
            // Ghidra LAB_009d2f27:
            //   DAT_03a74a46 = 1;
            //   _DAT_03a743a6 = 1;
            ACTIVE_USER_TEAM_COUNT = 1;
            IS_FULL_SEASON = 1;
            break;

        case 0:
            MAX_TEAMS = 14;
            DISPLAY_TEAM_COUNT = 14;
            Logger::Log("[LeagueTeams] Superleague set to 14 teams");
            ACTIVE_USER_TEAM_COUNT = 0;
            IS_FULL_SEASON = 1;
            break;
        case 2:
        case 3:
        case 4:
            // 20-team leagues (standard size)
            MAX_TEAMS          = TEAM_COUNT_20;
            DISPLAY_TEAM_COUNT = TEAM_COUNT_20;
            Logger::Log("[LeagueTeams] League type %d: setting 20 teams", (int)LEAGUE_TYPE);

            // Same shared setup as case 1
            ACTIVE_USER_TEAM_COUNT = 1;
            IS_FULL_SEASON = 1;
            break;

        case 5:
            // 20-team league but WITHOUT resetting ACTIVE_USER_TEAM_COUNT/ARRANGEMENT_INDEX.
            // The original jumps directly to caseD_6 (the clamp logic) after setting
            // counts, bypassing LAB_009d2f27.
            MAX_TEAMS          = TEAM_COUNT_20;
            DISPLAY_TEAM_COUNT = TEAM_COUNT_20;
            Logger::Log("[LeagueTeams] League type 5: setting 20 teams (no active count reset)");
            break;

        default:
            // League type > 5: no count changes. Falls through to clamp logic.
            Logger::Log("[LeagueTeams] League type %d: no count change (default)",
                (int)LEAGUE_TYPE);
            break;
        }

        // ── Clamp ACTIVE_USER_TEAM_COUNT to MAX_TEAMS (caseD_6 / LAB_009d2f56) ──
        // Ghidra:
        //   _DAT_03a74a4e = DAT_03a748a6;         (EFFECTIVE = MAX)
        //   if (DAT_03a748a6 < DAT_03a74a46)       (if MAX < ACTIVE)
        //       DAT_03a74a46 = DAT_03a748a6;       (    ACTIVE = MAX)
        //
        // In the ASM: CMP DX, CX  where DX=ACTIVE_USER_TEAM_COUNT, CX=MAX_TEAMS
        // JLE skips the clamp (active already <= max, nothing to do)

        EFFECTIVE_TEAM_COUNT = MAX_TEAMS;
        if (MAX_TEAMS < ACTIVE_USER_TEAM_COUNT)
        {
            ACTIVE_USER_TEAM_COUNT = MAX_TEAMS;
            Logger::Log("[LeagueTeams] Clamped ACTIVE_USER_TEAM_COUNT to %d", (int)MAX_TEAMS);
        }
    }
    
    if (leagueIndex == 5 && LEAGUE_TYPE == 0){
        MAX_TEAMS = 14;
        DISPLAY_TEAM_COUNT = 14;
        Logger::Log("[LeagueTeams] Superleague set to 14 teams");
        // ACTIVE_USER_TEAM_COUNT = 0;
        // IS_FULL_SEASON = 1;
    }

    // ── LAB_009d2f75: match state and page state updates ────────────────────
    // Reached either by the goto (leagueIndex != 1) or fall-through above.
    // Ghidra: if (DAT_03a741fe == 2) { ... } else { ... }

    if (LEAGUE_TYPE == 2)
    {
        // League type 2: Special setting for spanish league
        // (likely a cup or special competition format)
        BALL_OPTION_DISABLED = 0;
        BALL_OPTION_SHOW_RANDOM = 0;

        //BALL_SETTING_SET = BALL_SETTING_SPECIFIC_LALIGA;  // 0xc = 12

        

        Logger::Log("[LeagueTeams] League type 2: match state initialised (A=1 B=0 C=12)");
    }
    else
    {
        BALL_OPTION_DISABLED = 0;

        if (leagueIndex == 1)
        {
            // leagueIndex == 1, league type != 2:
            // Reset match state C if it was at its init value, then set B = 1
            // Ghidra:
            //   if (DAT_03a7592e == 0xc) DAT_03a7592e = 0;
            //   _DAT_03a75970 = 1;
            //   goto LAB_009d2fad;
            if (BALL_SETTING_SET == BALL_SETTING_SPECIFIC_LALIGA)
                BALL_SETTING_SET = 0;

            BALL_OPTION_SHOW_RANDOM = 0;

            Logger::Log("[LeagueTeams] leagueIndex=1, type!=2: BALL_OPTION_SHOW_RANDOM=1");

            // Jump directly to LAB_009d2fad — skip the iVar2==5 check below
            goto LAB_009d2fad;
        }
    }

    // ── iVar2 == 5: secondary clamp using DISPLAY_TEAM_COUNT ────────────────
    // Ghidra:
    //   if ((iVar2 == 5) && (_DAT_03a74a4e = DAT_03a7489e,
    //                         DAT_03a7489e < DAT_03a74a46))
    //       DAT_03a74a46 = DAT_03a7489e;
    //
    // Note: the assignment _DAT_03a74a4e = DAT_03a7489e happens unconditionally
    // when iVar2 == 5 (it is a C comma expression inside the if condition).

    if (leagueIndex == 5)
    {
        EFFECTIVE_TEAM_COUNT = DISPLAY_TEAM_COUNT;  // always assigned

        if (DISPLAY_TEAM_COUNT < ACTIVE_USER_TEAM_COUNT)
        {
            ACTIVE_USER_TEAM_COUNT = DISPLAY_TEAM_COUNT;
            Logger::Log("[LeagueTeams] leagueIndex=5: clamped ACTIVE to DISPLAY (%d)",
                (int)DISPLAY_TEAM_COUNT);
        }
    }

    // ── LAB_009d2fad: next-page button visibility ────────────────────────────
    // Ghidra:
    //   if ((sVar1 != DAT_03a74a46) || (iVar2 == 6))
    //   {
    //     if (DAT_03a74a46 < 2)  { _DAT_03a74700 = 0; }
    //     else                    { _DAT_03a74700 = 1; _DAT_03a746f6 = 0; }
    //   }
    //
    // Condition: either the active count changed during this call,
    // OR leagueIndex is specifically 6. In either case, re-evaluate
    // whether the next-page navigation button should be shown.

    LAB_009d2fad:
    if (savedActiveCount != ACTIVE_USER_TEAM_COUNT || leagueIndex == 6)
    {
        if (ACTIVE_USER_TEAM_COUNT < CUP_DURING_SEASON_MAX_USER_TEAMS)
        {
            CUP_OPTION_DISABLED = 0;
            Logger::Log("[LeagueTeams] Next-page button: HIDDEN (activeCount=%d)",
                (int)ACTIVE_USER_TEAM_COUNT);
        }
        else
        {
            CUP_OPTION_DISABLED = 1;
            CUP_DURING_SEASON = 0;
            Logger::Log("[LeagueTeams] Next-page button: SHOWN (activeCount=%d)",
                (int)ACTIVE_USER_TEAM_COUNT);
        }
    }

    // ── Master League / Cup mode override (DAT_03becc60 == 4) ───────────────
    // Ghidra:
    //   if (DAT_03becc60 == 4) {
    //     bVar3 = DAT_03a741fe != 5;
    //     if (bVar3) { _DAT_03a7454e = 1; }
    //     _DAT_03a748a8 = (uint)bVar3;
    //     _DAT_03a74558 = (uint)bVar3;
    //     _DAT_03a743b0 = (uint)bVar3;
    //     if (1 < DAT_03a74a46) { _DAT_03a74700 = 1; _DAT_03a746f6 = 0; }
    //   }
    //
    // bVar3 is true (1) when league type is NOT 5 — it acts as a
    // "not-cup-mode" flag that enables several display elements.

    if (GAME_MODE == GAME_MODE_ML_CUP)
    {
        const bool onlyClubLeague = (LEAGUE_TYPE != 5);  // bVar3

        if (false && onlyClubLeague)
            ELIGIBLE_TEAMS_TYPE = CLUB_TEAMS;          // DAT_03a7454e

        NUMBER_OF_TEAMS_OPTION_DISABLED = onlyClubLeague ? 1 : 0;   // DAT_03a748a8
        ELIGIBLE_TEAMS_OPTION_DISABLED = onlyClubLeague ? 1 : 0;   // DAT_03a74558
        SEASON_OPTION_DISABLED = onlyClubLeague ? 1 : 0;   // DAT_03a743b0

        NUMBER_OF_TEAMS_OPTION_DISABLED = 0;
        ELIGIBLE_TEAMS_OPTION_DISABLED = 0;
        SEASON_OPTION_DISABLED = 0;

        // Override next-page button: show it if more than 1 team active
        if (ACTIVE_USER_TEAM_COUNT > 1)
        {
            CUP_OPTION_DISABLED = 1;
            CUP_DURING_SEASON = 0;
        }

        Logger::Log("[LeagueTeams] ML/Cup override: onlyClubLeague=%d nextPage=%d",
            (int)onlyClubLeague, CUP_OPTION_DISABLED);
    }

    //Panos code
    CUP_OPTION_DISABLED = 0;
    CUP_DURING_SEASON = 1;

    Logger::Log("[LeagueTeams] Complete: activeCount=%d effectiveCount=%d nextPage=%d",
        (int)ACTIVE_USER_TEAM_COUNT, (int)EFFECTIVE_TEAM_COUNT, CUP_OPTION_DISABLED);
}

// =============================================================================
// SECTION 5 — Hook registration
// =============================================================================

void LeagueTeamsHook::Register()
{
    static const uintptr_t ADDR_SetLeagueTeamCount = 0x009d2ec0;

    Logger::Log("[LeagueTeams] Installing hook at 0x%08X...", ADDR_SetLeagueTeamCount);

    MH_STATUS s = MH_CreateHook(
        reinterpret_cast<LPVOID>(ADDR_SetLeagueTeamCount),
        reinterpret_cast<LPVOID>(&hook_SetLeagueTeamCount),
        reinterpret_cast<LPVOID*>(&orig_SetLeagueTeamCount)
    );

    if (s != MH_OK)
    {
        Logger::Log("[LeagueTeams] ERROR: MH_CreateHook failed (%d)", s);
        return;
    }

    s = MH_EnableHook(reinterpret_cast<LPVOID>(ADDR_SetLeagueTeamCount));
    if (s != MH_OK)
    {
        Logger::Log("[LeagueTeams] ERROR: MH_EnableHook failed (%d)", s);
        return;
    }

    Logger::Log("[LeagueTeams] Hook installed OK.");
}