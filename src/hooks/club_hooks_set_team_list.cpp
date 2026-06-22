// club_hooks_set_team_list.cpp — hooked SetTeamList (FUN_00b07b00)
//
// Copies a TeamSlotBuffer into a panel slot at the given index.
// The original has a hardcoded loop of 20 for team ID copying.
// This hook replaces that with TEAM_SLOT_COUNT from club_hooks_common.h,
// allowing the number of teams per league slot to be adjusted.
//
// Layout within panel at slot param_2:
//   base = param_2 * 0x4A4 + param_1
//   [base + 0x1944] : team IDs (TEAM_SLOT_COUNT × 4 bytes)
//   [base + 0x19E4] : null-terminated league name string (copied byte by byte)
//   [base + 0x1DE4] : display/logo ID (4 bytes)

#include "club_hooks_common.h"
#include "../utils/logger.h"

// Trampoline is declared extern in club_hooks_common.h,
// defined in club_hooks_register.cpp.

void __cdecl hook_SetTeamList(int param_1, int param_2, int param_3)
{
    // Compute base offset into panel for this slot
    int base = param_2 * 0x4A4 + param_1;

    // ── Copy team IDs ────────────────────────────────────────────────────
    // Original: loop 0..19 (hardcoded 20)
    // We use TEAM_SLOT_COUNT so it can be adjusted.
    uint32_t* dst = reinterpret_cast<uint32_t*>(base + 0x1944);
    uint32_t* src = reinterpret_cast<uint32_t*>(param_3);
    for (int i = 0; i < TEAM_SLOT_COUNT; i++)
    {
        dst[i] = src[i];
    }

    // ── Copy league name string (null-terminated, byte by byte) ──────────
    // Source: param_3 + 0xA0
    // Destination: base + 0x19E4
    // The original ASM uses a relative-offset trick:
    //   iVar3 = (base + 0x19E4) - (param_3 + 0xA0)
    //   then writes pcVar4[iVar3] = *pcVar4 while advancing pcVar4
    // We just do a straightforward byte copy.
    char* srcName = reinterpret_cast<char*>(param_3 + 0xA0);
    char* dstName = reinterpret_cast<char*>(base + 0x19E4);
    do {
        char c = *srcName;
        *dstName = c;
        srcName++;
        dstName++;
        if (c == '\0') break;
    } while (true);

    // ── Copy display/logo ID ─────────────────────────────────────────────
    *reinterpret_cast<uint32_t*>(base + 0x1DE4) =
        *reinterpret_cast<uint32_t*>(param_3 + 0x4A0);
}