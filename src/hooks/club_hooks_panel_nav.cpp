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

// club_hooks_panel_nav.cpp — hooked panel navigation update function
//
// Replaces FUN_00b0ac40 (the __cdecl wrapper around FUN_00b09b80).
// FUN_00b0ac40 is a thin wrapper: push esi / mov esi,[esp+8] / call inner / pop esi / ret
// We hook the wrapper to get the panel pointer as a clean __cdecl parameter.
//
// FUN_00b09b80 is the main per-frame navigation handler for the league/team
// selection panel. It has two major branches:
//   1. panel[0x643] == 0: LEAGUE SELECTION mode (navigate between leagues)
//   2. panel[0x643] != 0: TEAM SELECTION mode (navigate teams within a league)
//
// Both branches contain hardcoded bounds of 0x13 (19) and 0x14 (20) for the
// 20-slot league limit. We replace those with MAX_PANEL_SLOTS - 1 and
// MAX_PANEL_SLOTS respectively.
//
// The page system divides leagues into pages of 4 slots each (columns per row).
// Pages: page 0 = slots 0-7, page 1 = slots 8-15, page 2 = slots 16-19.
// We generalise this with a helper function.

#include "club_hooks_common.h"
#include "../utils/logger.h"

// ---------------------------------------------------------------------------
// Original trampoline and sub-function pointers are declared in
// club_hooks_common.h and defined in club_hooks_register.cpp.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// FireInputEvent — calling-convention bridge for FUN_009b8980 (= fn_InputEvent).
//
// FUN_009b8980 is NOT __cdecl. From its disassembly:
//   * Entry `PUSH ECX` saves the caller's ECX as a function-local. That value
//     is later read back at `MOV ECX, [ESP+4]` (009b89fe) and used as the
//     `this`-style argument forwarded into FUN_009b8330 in the default
//     uVar3-dispatch branch.
//   * The first stack arg ([ESP+4] at entry, i.e. arg1 in cdecl numbering)
//     is the **event code** that the function dispatches on.
//   * Function exits with plain `RET` (not `RET 4`) → caller cleans the
//     stack args.
//
// So the convention is "ECX = `this` (used in some branches), arg on stack,
// caller cleanup". MSVC's `__thiscall` is callee-cleanup so it doesn't fit;
// `__cdecl` doesn't pass anything in ECX. Our pre-existing typedef declared
// it `__cdecl(void*, uint32_t)` which puts our `this`-value at [esp+4] and
// our event code at [esp+8] — i.e. the function reads our `this` as the
// event code, and our actual event code is ignored. That's why the
// navigation sounds stopped playing: every call dispatched on a garbage
// event code.
//
// This naked thunk emits the right machine code: load ECX from [esp+4] of
// the cdecl-shaped wrapper, push the event code as the function's first
// stack arg, call FUN_009b8980, then clean our pushed arg (caller-cleanup).
// Call sites use FireInputEvent(this, eventCode) just like the old
// fn_InputEvent calls — the typedef stays untouched (other translation
// units may still use it through a code path that happens not to deref
// ECX, and changing it globally would ripple).
// ---------------------------------------------------------------------------
static __declspec(naked) void FireInputEvent(void* /*this_*/,
                                              uint32_t /*eventCode*/)
{
    __asm {
        // cdecl entry stack: [retaddr, this_, eventCode, ...]
        push [esp+8]                  // push eventCode → becomes [esp+4] for callee
        mov  ecx, [esp+8]             // this_ (after our push, original [esp+4] is now [esp+8])
        call dword ptr [fn_InputEvent]
        add  esp, 4                   // clean our pushed eventCode (callee did plain RET)
        ret
    }
}

// ---------------------------------------------------------------------------
// Global input flag arrays (set to 1 by the button callbacks in FinalisePanelLayout)
// ---------------------------------------------------------------------------
static inline int& InputFlag(uintptr_t base, int panelID) {
    return *reinterpret_cast<int*>(base + panelID * 4);
}
#define INPUT_UP_A(id)    InputFlag(0x03b49140, id)  // DAT_03b49140 — up (league page)
#define INPUT_UP_B(id)    InputFlag(0x03b49160, id)  // DAT_03b49160 — down (league page)
#define INPUT_LEFT(id)    InputFlag(0x03b49180, id)  // DAT_03b49180 — left (team nav)
#define INPUT_RIGHT(id)   InputFlag(0x03b491a0, id)  // DAT_03b491a0 — right (team nav)
#define INPUT_SCROLL_UP(id)   InputFlag(0x03b49200, id)  // DAT_03b49200 — scroll up (league)
#define INPUT_SCROLL_DN(id)   InputFlag(0x03b49220, id)  // DAT_03b49220 — scroll down (league)
#define INPUT_CONFIRM(id) InputFlag(0x03b491c0, id)  // DAT_03b491c0 — confirm/select
#define INPUT_CANCEL(id)  InputFlag(0x03b491e0, id)  // DAT_03b491e0 — cancel/back

// ---------------------------------------------------------------------------
// Panel field accessors (byte offsets, same as panel creator)
// ---------------------------------------------------------------------------
#define P_DW(panel, off) (*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(panel) + (off)))
#define P_INT(panel, off) (*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(panel) + (off)))

// Cursor state (per-player, indexed by panel[0x1940] which is 0 or 1)
// Each cursor occupies 6 dwords at panel + 0x1910 + cursorIdx*24
//   [+0x00] = 0x1910: team position (inner grid cursor)
//   [+0x04] = 0x1914: league slot index
//   [+0x08] = 0x1918: (flag)
//   [+0x0C] = 0x191C: (flag)
//   [+0x10] = 0x1920: (unused?)
//   [+0x14] = 0x1924: page row index
static inline int CursorBase(uint32_t* panel) {
    int idx = P_INT(panel, 0x1940);
    return 0x1910 + idx * 24;
}
static inline int& CurTeamPos(uint32_t* panel)   { return P_INT(panel, CursorBase(panel) + 0x00); }
static inline int& CurLeagueSlot(uint32_t* panel) { return P_INT(panel, CursorBase(panel) + 0x04); }
static inline int& CurFlag1C(uint32_t* panel)    { return P_INT(panel, CursorBase(panel) + 0x0C); }
static inline int& CurPageRow(uint32_t* panel)    { return P_INT(panel, CursorBase(panel) + 0x14); }

// Pagination state (per-player, at panel + 0x648 + cursorIdx*24... actually
// it's at panel[cursorIdx*3 + 0x324]*2 which is panel[(cursorIdx*6 + 0x648)])
// Let me derive from the ASM more carefully:
//   unaff_ESI[(iVar9 * 3 + 0x324) * 2]  where iVar9 = panel[0x650]
//   = panel[iVar9 * 6 + 0x648]
// So the column-within-page value is at cursor offset +0x648 relative to panel,
// indexed by player. Let's compute it:
static inline int& CurColumnInPage(uint32_t* panel) {
    int idx = P_INT(panel, 0x1940);
    return P_INT(panel, idx * 24 + 0x1290); // (idx*3 + 0x324)*8 = idx*24 + 0x1920... 
    // Actually: (idx * 3 + 0x324) * 2 as DWORD index = (idx*3 + 0x324)*8 as byte offset
    // = idx*24 + 0x1920 byte offset. Wait, that's 0x1920 which overlaps CurPageRow.
    // Let me re-derive. panel[(idx*3 + 0x324) * 2]:
    //   dword index = (idx*3 + 0x324) * 2
    //   byte offset = (idx*3 + 0x324) * 8
    //   for idx=0: byte offset = 0x324 * 8 = 0x1920
    //   for idx=1: byte offset = 0x327 * 8 = 0x1938
    // Hmm, but CurPageRow for idx=0 is at 0x1910 + 0*24 + 0x14 = 0x1924.
    // So 0x1920 != 0x1924. They don't overlap. Good.
}

// This is getting complex. Let me use a simpler approach — direct field access
// using the same indexing as the ASM, to avoid any miscalculation.

// ---------------------------------------------------------------------------
// Slot-to-page mapping helper (replaces all the hardcoded 0x13 page checks)
// Original: slot<8 → page 0, slot<16 → page 1, slot<=19 → page 2
// Generalised: pages of 8, with the last page covering whatever remains
// ---------------------------------------------------------------------------
static inline int SlotToPage(int slot) {
    if (slot < 8) return 0;
    if (slot < 16) return 1;
    // Original: ((0x13 < slot) - 1) & 3) - 1
    // This evaluates to: slot <= 0x13 → 2, slot > 0x13 → undefined (wraps)
    // For our purposes, everything >= 16 is page 2
    // If we want more pages for more slots, extend here:
    if (slot < 24) return 2;
    if (slot < 32) return 3;
    return (slot / 8);
}

// Max league slot index (0-based) and total page count for league
// navigation. These were `static const int` originally but MAX_PANEL_SLOTS
// is now a runtime variable (see club_hooks_common.h), so they're macros
// that re-evaluate at every use site instead.
#define MAX_LEAGUE_IDX  (MAX_PANEL_SLOTS - 1)
#define MAX_PAGES       ((MAX_PANEL_SLOTS + 3) / 4)  // ceil(slots/4)

// Page boundary table: for each page, [start, end] slot indices
// Original had: {0,7, 8,15, 16,19}
// We compute dynamically:
struct PageBounds {
    int start;
    int end;
};
static PageBounds GetPageBounds(int page) {
    PageBounds pb;
    pb.start = page * 8;
    pb.end = (page + 1) * 8 - 1;
    if (pb.end > MAX_LEAGUE_IDX) pb.end = MAX_LEAGUE_IDX;
    return pb;
}

// ---------------------------------------------------------------------------
// Check if a league slot is "populated" (has valid data)
// ASM checks: panel[slot * 0x129 + 0x779] != -1 (displayID at slot+0x400)
// and/or: panel[slot * 0x129 + 0x652] != 0xFFFF (first team ID)
// and/or: panel[slot * 0x129 + 0x651] != 0xFFFF (team ID at slot teamIDs[0])
// ---------------------------------------------------------------------------
static inline bool SlotHasDisplayID(uint32_t* panel, int slot) {
    // panel[slot * 0x129 + 0x779] — this is displayID field
    // 0x779 * 4 = 0x1DE4, and slot * 0x129 * 4 = slot * 0x4A4
    // So byte offset = slot * 0x4A4 + 0x1DE4
    return P_INT(panel, slot * 0x4A4 + 0x1DE4) != -1;
}
static inline int SlotTeamID0(uint32_t* panel, int slot) {
    // panel[slot * 0x129 + 0x651] at byte offset slot*0x4A4 + 0x1944
    return P_INT(panel, slot * 0x4A4 + 0x1944);
}
static inline int SlotTeamID1(uint32_t* panel, int slot) {
    // panel[slot * 0x129 + 0x652] at byte offset slot*0x4A4 + 0x1948
    return P_INT(panel, slot * 0x4A4 + 0x1948);
}
static inline int& SlotExpandFlag(uint32_t* panel, int slot) {
    // panel[slot * 0x129 + 0x665] at byte offset slot*0x4A4 + 0x1994
    return P_INT(panel, slot * 0x4A4 + 0x1994);
}

// Team position within a league's team grid, checking if occupied
static inline int TeamAtPos(uint32_t* panel, int leagueSlot, int teamPos) {
    // panel[leagueSlot * 0x129 + teamPos + 0x651]
    // byte offset: leagueSlot * 0x4A4 + teamPos * 4 + 0x1944
    return P_INT(panel, leagueSlot * 0x4A4 + teamPos * 4 + 0x1944);
}

// ---------------------------------------------------------------------------
// Trailer field (scroll direction indicator)
// Original: panel[0x1D86] = byte offset 0x7618
// We use the dynamic offset for >20 slots
// ---------------------------------------------------------------------------
static inline int& TrailerA(uint32_t* panel) {
    // For the nav function, this is always read/written at the FIXED offset
    // 0x7618 in the original ASM. Since we also mirror it there, use fixed.
    return P_INT(panel, 0x7618);
}

// ---------------------------------------------------------------------------
// hook_PanelNavUpdate — replacement for FUN_00b0ac40
//
// Complete reconstruction of FUN_00b09b80's logic with parameterised bounds.
// ---------------------------------------------------------------------------
uint32_t __cdecl hook_PanelNavUpdate(uint32_t* panel)
{
    if (!panel) return 0;

    // ── Shorthand accessors ──────────────────────────────────────────────
    int curIdx = P_INT(panel, 0x1940);  // current player index (0 or 1)
    int cb = curIdx * 24;               // cursor block byte offset within 0x1910 region

    // Cursor fields (byte offsets from panel base)
    int& teamPos    = P_INT(panel, 0x1910 + cb);  // [+0x644 dword] team grid position
    int& leagueSlot = P_INT(panel, 0x1914 + cb);  // [+0x645 dword] current league index
    int& flag191C   = P_INT(panel, 0x191C + cb);  // [+0x647 dword] input lock flag
    int& pageRow    = P_INT(panel, 0x1924 + cb);  // [+0x649 dword] page row index

    // Column-in-page: panel[(curIdx*3 + 0x324) * 2] → byte offset (curIdx*3+0x324)*8
    int colByteOff = (curIdx * 3 + 0x324) * 8;
    int& colInPage = P_INT(panel, colByteOff);

    int panelID = P_INT(panel, 0);       // panel[0] = side/ID
    int& dirtyFlag = P_INT(panel, 0x48); // panel[0x12] = refresh needed
    int& teamSelActive = P_INT(panel, 0x190C); // panel[0x643]
    int colCount = P_INT(panel, 0x00F0); // panel[0x3C] = columns in team grid
    int rowCount = P_INT(panel, 0x00F4); // panel[0x3D] = rows in team grid
    int& pageField = P_INT(panel, 0x00F8); // panel[0x3E] = current page number

    // ── Team-grid cell count ─────────────────────────────────────────────
    // The team grid (Screen 3, panel[0x643] != 0) reuses the same scroll
    // controller cells as the league grid: cellCount = cols * rows. Each
    // league has at most TEAM_SLOT_COUNT (= 20) teams in its TeamSlotBuffer
    // and the cell array is sized cols*rows. When MAX_PANEL_SLOTS > cellCount
    // (e.g. 25 leagues vs 20 cells), TEAM-mode navigation must wrap by
    // cellCount, NOT by MAX_PANEL_SLOTS — otherwise it indexes past both the
    // teamIDs[20] buffer and the cell array, producing the Crash 1 family
    // (FUN_00b0fcd0 with NULL `*cell` from past-array memory).
    //
    // LEAGUE-mode navigation correctly uses page math (`pageRow`, `colInPage`)
    // and league-slot wraps at MAX_PANEL_SLOTS, so it stays as-is — leagues
    // 20+ live in additional pages that the page-stepper handles natively.
    int teamCellCount = colCount * rowCount;
    if (teamCellCount <= 0) teamCellCount = TEAM_SLOT_COUNT; // safety

    int savedTeamPos = teamPos;
    int savedLeagueSlot = leagueSlot;

    // ── Page boundary table (local_58 in ASM) ────────────────────────────
    // Original: {0,7, 8,15, 16,19, ...} — 3 pages of (start,end) pairs
    // We build dynamically for MAX_PANEL_SLOTS
    int pageBoundsFlat[22]; // up to 11 pages * 2 = 22 entries, plenty
    memset(pageBoundsFlat, 0, sizeof(pageBoundsFlat));
    int numPages = 0;
    for (int p = 0; p * 8 < MAX_PANEL_SLOTS; p++) {
        pageBoundsFlat[p * 2]     = p * 8;                                    // start
        pageBoundsFlat[p * 2 + 1] = ((p + 1) * 8 - 1 < MAX_LEAGUE_IDX)
                                    ? (p + 1) * 8 - 1
                                    : MAX_LEAGUE_IDX;                          // end
        numPages = p + 1;
    }

    // ── Clear input flags if input lock is active ────────────────────────
    if (flag191C == 1) {
        int id4 = panelID * 4;
        *reinterpret_cast<int*>(0x03b49140 + id4) = 0;
        *reinterpret_cast<int*>(0x03b49160 + id4) = 0;
        *reinterpret_cast<int*>(0x03b49180 + id4) = 0;
        *reinterpret_cast<int*>(0x03b491a0 + id4) = 0;
        *reinterpret_cast<int*>(0x03b49200 + id4) = 0;
        *reinterpret_cast<int*>(0x03b49220 + id4) = 0;
    }

    // Reset trailer scroll direction indicator
    TrailerA(panel) = 0;

    // Extra logo quads are sidecar nodes, not stock page children.
    // Keep them aligned with panel[+0xF8]'s current league page.
    //UpdateExtraLeagueVisualSlotVisibility(panel);

    // ════════════════════════════════════════════════════════════════════
    // BRANCH: TEAM SELECTION MODE (panel[0x643] != 0)
    // ════════════════════════════════════════════════════════════════════
    if (teamSelActive != 0) {

        // ── Scroll UP through leagues (DAT_03b49200) ─────────────────
        if (INPUT_SCROLL_UP(panelID) == 1) {
            int startSlot = leagueSlot;
            do {
                leagueSlot--;
                if (leagueSlot == -1) {
                    leagueSlot = MAX_LEAGUE_IDX;  // wrap: was 0x13
                }
                pageField = SlotToPage(leagueSlot);
                int s = leagueSlot;
                if (SlotHasDisplayID(panel, s) && SlotTeamID1(panel, s) != 0xFFFF)
                    break;
                // Additional check for mode 1 (panel[0x642]=1)
                if (P_INT(panel, 0x1908) == 1) {
                    int t0 = SlotTeamID0(panel, s);
                    if (t0 != 0xFFFF && t0 != 0x11e)
                        break;
                }
            } while (startSlot != leagueSlot);

            goto team_post_scroll;
        }

        // ── Scroll DOWN through leagues (DAT_03b49220) ──────────────
        if (INPUT_SCROLL_DN(panelID) == 1) {
            int startSlot = leagueSlot;
            do {
                leagueSlot++;
                if (leagueSlot == MAX_PANEL_SLOTS) {  // wrap: was 0x14
                    leagueSlot = 0;
                }
                pageField = SlotToPage(leagueSlot);
                int s = leagueSlot;
                if (SlotHasDisplayID(panel, s) && SlotTeamID1(panel, s) != 0xFFFF)
                    break;
                if (P_INT(panel, 0x1908) == 1) {
                    int t0 = SlotTeamID0(panel, s);
                    if (t0 != 0xFFFF && t0 != 0x11e)
                        break;
                }
            } while (startSlot != leagueSlot);

            goto team_post_scroll;
        }

        // ── Navigate UP in team grid (DAT_03b49140) ──────────────────
        // Wrap by teamCellCount (= cols*rows = 20 stock), NOT MAX_PANEL_SLOTS.
        // teamIDs[] is structurally capped at TEAM_SLOT_COUNT and the cell
        // array at cols*rows; both are <= MAX_PANEL_SLOTS so we always pick
        // the safe (smaller) bound here.
        if (INPUT_UP_A(panelID) == 1) {
            do {
                teamPos -= colCount;
                if (teamPos < 0) {
                    teamPos += teamCellCount;     // was MAX_PANEL_SLOTS
                    TrailerA(panel) = 1;
                }
            } while (TeamAtPos(panel, leagueSlot, teamPos) == 0xFFFF);

            goto team_post_nav;
        }

        // ── Navigate DOWN in team grid (DAT_03b49160) ────────────────
        // Same bound rationale as the UP branch above.
        if (INPUT_UP_B(panelID) == 1) {
            do {
                teamPos += colCount;
                if (teamPos >= teamCellCount) {   // was teamPos > MAX_LEAGUE_IDX
                    teamPos -= teamCellCount;     // was MAX_PANEL_SLOTS
                    TrailerA(panel) = 2;
                }
            } while (TeamAtPos(panel, leagueSlot, teamPos) == 0xFFFF);

            goto team_post_nav;
        }

        // ── Navigate RIGHT within row (DAT_03b49180) ─────────────────
        if (INPUT_LEFT(panelID) == 1) {
            do {
                teamPos++;
                if (teamPos % colCount == 0) {
                    teamPos -= colCount;
                    TrailerA(panel) = 4;
                }
            } while (TeamAtPos(panel, leagueSlot, teamPos) == 0xFFFF);

            if (savedTeamPos != teamPos) {
                if (fn_InputEvent)
                    FireInputEvent(reinterpret_cast<void*>(savedTeamPos), 0x8400f802);
            }
            dirtyFlag = 1;
            goto team_check_confirm;
        }

        // ── Navigate LEFT within row (DAT_03b491a0) ──────────────────
        if (INPUT_RIGHT(panelID) == 1) {
            do {
                teamPos--;
                if (teamPos == -1) {
                    teamPos = colCount - 1;
                    TrailerA(panel) = 3;
                } else if (teamPos % colCount == colCount - 1) {
                    teamPos += colCount;
                    TrailerA(panel) = 3;
                }
            } while (TeamAtPos(panel, leagueSlot, teamPos) == 0xFFFF);

            if (savedTeamPos != teamPos) {
                if (fn_InputEvent)
                    FireInputEvent(reinterpret_cast<void*>(
                        leagueSlot * 0x129 + teamPos), 0x8400f802);
            }
            dirtyFlag = 1;
            goto team_check_confirm;
        }

        // ── Post-scroll fix-up ───────────────────────────────────────
        // (jumped to from the scroll up/down blocks above)
        if (false) {
    team_post_scroll:
            if (savedLeagueSlot != leagueSlot) {
                if (fn_InputEvent)
                    FireInputEvent(reinterpret_cast<void*>(&leagueSlot), 0x8400f803);
            }
            dirtyFlag = 1;
        }

        if (false) {
    team_post_nav:
            if (savedTeamPos != teamPos) {
                if (fn_InputEvent)
                    FireInputEvent(reinterpret_cast<void*>(curIdx * 3), 0x8400f802);
            }
            dirtyFlag = 1;
        }

    team_check_confirm:
        // ── Dirty flag validation loop ───────────────────────────────
        // If dirty, verify current position has valid team data. If not,
        // scan backwards through the team grid. Bounds use teamCellCount
        // here too (the cell array / teamIDs[] cap), not MAX_PANEL_SLOTS:
        // larger bounds would let teamPos wrap past the structural buffer
        // end and crash downstream code.
        if (dirtyFlag == 1) {
            int attempts = 0;
            while (attempts < teamCellCount) {   // was MAX_PANEL_SLOTS
                if (TeamAtPos(panel, leagueSlot, teamPos) != 0xFFFF)
                    break;
                teamPos--;
                if (teamPos < 0) {
                    teamPos = teamCellCount - 1;  // was MAX_LEAGUE_IDX
                }
                dirtyFlag = 1;
                attempts++;
            }

            // If still no valid position, scan forward through league slots
            if (TeamAtPos(panel, leagueSlot, teamPos) == 0xFFFF) {
                for (int s = 0; s < MAX_PANEL_SLOTS; s++) {  // was 0x14
                    if (SlotTeamID0(panel, s) != 0xFFFF) {
                        teamPos = 0;
                        break;
                    }
                    leagueSlot++;
                    if (leagueSlot == MAX_PANEL_SLOTS) {  // was 0x14
                        leagueSlot = 0;
                    }
                }
            }

            // Update page row from league slot
            pageRow = (leagueSlot + (leagueSlot < 0 ? 3 : 0)) >> 2;
            // Update column-in-page
            uint32_t col = leagueSlot & 0x80000003;
            if ((int)col < 0) col = (col - 1 | 0xFFFFFFFC) + 1;
            colInPage = col;
        }

        pageField = SlotToPage(leagueSlot);
        //UpdateExtraLeagueVisualSlotVisibility(panel);

        // ── Confirm button (DAT_03b491c0) ────────────────────────────
        if (INPUT_CONFIRM(panelID) == 1) {
            int idx = leagueSlot * 0x129 + teamPos;
            if (SlotExpandFlag(panel, 0) == 0) {
                // Check panel-relative expand flag
                int& ef = P_INT(panel, idx * 4 + 0x1994);
                if (ef == 0) {
                    ef = 1;
                    dirtyFlag = 1;
                    if (fn_InputEvent)
                        FireInputEvent(reinterpret_cast<void*>(idx), 0x84008400);
                    return 2;
                }
            }
            if (fn_InputEvent)
                FireInputEvent(nullptr, 0x8c008005);
        }

        // ── Cancel button (DAT_03b491e0) ─────────────────────────────
        if (INPUT_CANCEL(panelID) != 1)
            return 0;

        dirtyFlag = 1;
        int curSlotIdx = leagueSlot;
        int expandState = P_INT(panel, curSlotIdx * 0x4A4 + leagueSlot * 4 + 0x1994);
        // Actually let me use the exact ASM pattern:
        int compositeIdx = leagueSlot * 0x129 + teamPos;
        int es = P_INT(panel, compositeIdx * 4 + 0x1994);

        if (es == 1) {
            P_INT(panel, compositeIdx * 4 + 0x1994) = 0;
            if (fn_InputEvent) FireInputEvent(nullptr, 0x84008401);
            return 1;
        }
        if (es == 2) {
            if (P_INT(panel, 0x18FC) == 1) {
                if (fn_InputEvent) FireInputEvent(nullptr, 0x8c008005);
                return 0;
            }
            teamPos = 0;
            teamSelActive = 0;
            if (fn_InputEvent) FireInputEvent(nullptr, 0x84008401);
            return 0;
        }
        if (es == 0) {
            if (P_INT(panel, 0x18FC) != 1) {
                teamPos = 0;
                teamSelActive = 0;
                if (fn_InputEvent) FireInputEvent(nullptr, 0x84008401);
                return 0;
            }
            int cr = fn_ConfirmCheck ? fn_ConfirmCheck(0, reinterpret_cast<int>(panel)) : 0;
            if (cr != 0) {
                if (fn_InputEvent) FireInputEvent(nullptr, 0x8c008005);
                return 0;
            }
            if (fn_InputEvent) FireInputEvent(nullptr, 0x84008401);
            return 3;
        }

        return 0;
    }

    // ════════════════════════════════════════════════════════════════════
    // BRANCH: LEAGUE SELECTION MODE (panel[0x643] == 0)
    // ════════════════════════════════════════════════════════════════════

    int savedLSlot = leagueSlot;
    int savedPage = pageField;
    bool bFound = false;
    int localDirtyCount = 0;

    // ── Page UP (DAT_03b49140) ───────────────────────────────────────
    if (INPUT_UP_A(panelID) == 1) {
        do {
            pageRow--;
            if (pageRow < 0) {
                pageRow = MAX_PAGES - 1;  // was 4
                TrailerA(panel) = 1;
            }
            // Recompute league slot from column + pageRow
            leagueSlot = colInPage + pageRow * 4;

            int newPage = SlotToPage(leagueSlot);
            if (newPage != savedPage) {
                int pg = SlotToPage(leagueSlot);
                int scanStart = pageBoundsFlat[pg * 2];
                int scanEnd = pageBoundsFlat[pg * 2 + 1];
                for (int s = scanStart; s <= scanEnd; s++) {
                    if (SlotHasDisplayID(panel, s)) {
                        localDirtyCount++;
                        break;
                    }
                }
            }
            savedPage = SlotToPage(leagueSlot);

            if (localDirtyCount > 1) {
                // Overshoot recovery: scan backwards
                do {
                    int testSlot = savedLSlot - 4;
                    if (testSlot < 0) testSlot += MAX_PANEL_SLOTS;  // was 0x14
                    pageRow = (testSlot + (testSlot < 0 ? 3 : 0)) >> 2;
                    int pg = SlotToPage(colInPage + pageRow * 4);
                    for (int col = pageBoundsFlat[pg * 2]; col <= pageBoundsFlat[pg * 2 + 1]; col++) {
                        int testIdx = pageRow * 4 + (col - pageBoundsFlat[pg * 2]);
                        if (testIdx < MAX_PANEL_SLOTS && SlotHasDisplayID(panel, testIdx)) {
                            bFound = true;
                            break;
                        }
                    }
                    savedLSlot = testSlot;
                } while (!bFound);
                leagueSlot = colInPage + pageRow * 4;
            }

        } while (!SlotHasDisplayID(panel, leagueSlot));
        dirtyFlag = 1;
    }

    // ── Page DOWN (DAT_03b49160) ─────────────────────────────────────
    else if (INPUT_UP_B(panelID) == 1) {
        do {
            pageRow++;
            if (pageRow > MAX_PAGES - 1) {  // was > 4
                pageRow = 0;
                TrailerA(panel) = 2;
            }
            leagueSlot = colInPage + pageRow * 4;

            int newPage = SlotToPage(leagueSlot);
            if (newPage != savedPage) {
                int pg = SlotToPage(leagueSlot);
                int scanStart = pageBoundsFlat[pg * 2];
                int scanEnd = pageBoundsFlat[pg * 2 + 1];
                for (int s = scanStart; s <= scanEnd; s++) {
                    if (SlotHasDisplayID(panel, s)) {
                        localDirtyCount++;
                        break;
                    }
                }
            }
            savedPage = SlotToPage(leagueSlot);

            if (localDirtyCount > 1) {
                do {
                    int testSlot = savedLSlot;
                    if (testSlot < 16) {
                        testSlot += 8;
                        if (testSlot >= MAX_PANEL_SLOTS) testSlot -= 4;  // was 0x14, then sub 4
                    } else {
                        testSlot = 0;
                    }
                    pageRow = (testSlot + (testSlot < 0 ? 3 : 0)) >> 2;
                    int pg = SlotToPage(colInPage + pageRow * 4);
                    for (int col = pageBoundsFlat[pg * 2]; col <= pageBoundsFlat[pg * 2 + 1]; col++) {
                        int testIdx = pageRow * 4 + (col - pageBoundsFlat[pg * 2]);
                        if (testIdx < MAX_PANEL_SLOTS && SlotHasDisplayID(panel, testIdx)) {
                            bFound = true;
                            break;
                        }
                    }
                    savedLSlot = testSlot;
                } while (!bFound);
                leagueSlot = colInPage + pageRow * 4;
            }

        } while (!SlotHasDisplayID(panel, leagueSlot));
        dirtyFlag = 1;
    }

    // ── Update page from current slot ────────────────────────────────
    pageField = SlotToPage(leagueSlot);
    //UpdateExtraLeagueVisualSlotVisibility(panel);

    // ── Column RIGHT (DAT_03b49180) ──────────────────────────────────
    if (INPUT_LEFT(panelID) == 1) {
        do {
            colInPage++;
            if (colInPage >= 4) {  // was > 3 → wrap at 4 columns
                colInPage = 0;
                TrailerA(panel) = 4;
            }
            leagueSlot = colInPage + pageRow * 4;
        } while (leagueSlot < MAX_PANEL_SLOTS && !SlotHasDisplayID(panel, leagueSlot));

        dirtyFlag = 1;
    }
    // ── Column LEFT (DAT_03b491a0) ───────────────────────────────────
    else if (INPUT_RIGHT(panelID) == 1) {
        do {
            colInPage--;
            if (colInPage < 0) {
                colInPage = 3;
                TrailerA(panel) = 3;
            }
            leagueSlot = colInPage + pageRow * 4;
        } while (leagueSlot < MAX_PANEL_SLOTS && !SlotHasDisplayID(panel, leagueSlot));

        dirtyFlag = 1;
    }

    // ── Notify if slot changed ───────────────────────────────────────
    if (savedLSlot != leagueSlot) {
        if (fn_InputEvent)
            FireInputEvent(reinterpret_cast<void*>(leagueSlot * 0x4A4), 0x8400f802);
    }

    // ── Confirm (DAT_03b491c0) — enter team selection ────────────────
    if (INPUT_CONFIRM(panelID) == 1) {
        if (fn_InputEvent)
            FireInputEvent(nullptr, 0x84008400);

        int s = leagueSlot;
        if ((SlotTeamID1(panel, s) != 0xFFFF) ||
            (P_INT(panel, 0x1908) == 1)) {
            // Has teams or is in ML mode
            if (SlotTeamID0(panel, s) != 0x11e) {
                teamSelActive = 1;
                teamPos = 0;
                dirtyFlag = 1;
                return 0;
            }
        }
        // Slot has expand functionality
        if (SlotExpandFlag(panel, s) != 0)
            return 0;
        SlotExpandFlag(panel, s) = 1;
        dirtyFlag = 1;
        return 2;
    }

    // ── Cancel (DAT_03b491e0) — back out ─────────────────────────────
    if (INPUT_CANCEL(panelID) != 1)
        return 0;

    dirtyFlag = 1;
    int s = leagueSlot;

    if (SlotTeamID1(panel, s) == 0xFFFF && P_INT(panel, 0x1908) == 0) {
        // No teams in this slot
        int ef = SlotExpandFlag(panel, s);
        if (ef == 1) {
            SlotExpandFlag(panel, s) = 0;
            if (fn_InputEvent) FireInputEvent(nullptr, 0x84008401);
            return 1;
        }
        int cr = fn_ConfirmCheck ? fn_ConfirmCheck(0, reinterpret_cast<int>(panel)) : 0;
        if (cr != 0)
            return 0;
    } else {
        // Check if any expand flags are set across all slots/teams.
        // Outer scans leagues (MAX_PANEL_SLOTS); inner scans team grid
        // positions, which is structurally capped at TEAM_SLOT_COUNT
        // (the teamIDs[] / cell-array shape). Original ASM had 0x14 here
        // for both — using MAX_PANEL_SLOTS in the inner is a leftover from
        // the global rename and was logically wrong (still in-bounds of
        // the 0x7620-byte panel allocation, so harmless, but reads other
        // slot fields' memory).
        for (int si = 0; si < MAX_PANEL_SLOTS; si++) {
            for (int ti = 0; ti < TEAM_SLOT_COUNT; ti++) {
                if (P_INT(panel, si * 0x4A4 + ti * 4 + 0x1994) == 1)
                    return 0;
            }
        }
    }

    if (P_INT(panel, 0xE8) != 1)
        return 3;

    if (fn_InputEvent) FireInputEvent(nullptr, 0x84008401);
    return 3;
}
