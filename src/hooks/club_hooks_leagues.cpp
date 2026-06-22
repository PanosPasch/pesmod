// =============================================================================
// club_hooks_leagues.cpp
//
// INI-driven league / panel-slot loader. Two pieces:
//
//   1. LoadLeaguesSetup() — called once during ClubHooks::Register().
//      * Reads `leagues/setup.ini` and applies any global overrides
//        (panel F0/F4 columns/rows, team-slot parse cap).
//      * Scans `leagues/*.ini` (excluding setup.ini) for the highest
//        numeric filename. If that highest id >= 20, MAX_PANEL_SLOTS
//        is bumped to id+1 so the panel allocation grows enough to
//        hold the extra slots. Otherwise MAX_PANEL_SLOTS stays at 20.
//
//   2. ApplyLeagueIniOverrides(side) — called from hook_LoadClubTeamSlots
//      after the original-shape stride-8 loop finishes. For every
//      `leagues/<n>.ini` in the range [0, MAX_PANEL_SLOTS), this loads
//      the file and writes the resulting TeamSlotBuffer into panel slot
//      `n` via fn_SetTeamList — which is the same call the original
//      uses for that index, so we just override what it placed there.
//
// INI formats:
//
//   leagues/setup.ini  (all keys optional; defaults match stock)
//     [setup]
//     team_slot_count   = 20      ; how many team_<n> keys to parse per
//                                  ; league INI. Capped at TEAM_SLOT_COUNT
//                                  ; (the buffer's structural max).
//     [panel]
//     f0_side0          = 5       ; DW(0x00F0) for side 0  (cols/rows)
//     f4_side0          = 4       ; DW(0x00F4) for side 0
//     f0_side1          = 4       ; DW(0x00F0) for side != 0
//     f4_side1          = 5       ; DW(0x00F4) for side != 0
//
//   leagues/<id>.ini   (one per panel slot; id is the second arg passed
//                       to fn_SetTeamList — i.e. the panel slot index)
//     [league]
//     name        = "Premier League"
//     display_id  = 0             ; logo / display ID (any base)
//     team_0      = 100           ; teamID for slot 0 (any base)
//     team_1      = 101
//     ...
//     team_19     = 119
//
// Path resolution: relative to the game's CWD, same convention used by
// the squad/player and kit INI loaders.
// =============================================================================

#include "club_hooks_common.h"
#include "../utils/logger.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// =============================================================================
// Runtime globals (declared in club_hooks_common.h — definitions here so
// the symbols live next to the loader that owns them).
// =============================================================================

int MAX_PANEL_SLOTS    = 20;

int g_LeagueTeamSlotMax = 20;

int g_PanelF0_Side0 = 5;
int g_PanelF4_Side0 = 4;
int g_PanelF0_Side1 = 4;
int g_PanelF4_Side1 = 5;

// Sentinel -1 means "no explicit setup.ini value" — LoadLeaguesSetup()
// auto-computes from MAX_PANEL_SLOTS after the directory scan settles
// (it can't be done in ApplySetupIni because the slot count isn't
// known yet there).
int g_LeaguePanelCount = -1;

// =============================================================================
// Per-slot panel layout cache.
// PanelSlotLayout itself is declared in club_hooks_common.h so other
// translation units (e.g. the panel-creator hook applying overrides at
// the scroll-controller level) can read it. The vector is kept file-
// local because it's only filled here in LoadLeaguesSetup().
//
// Layer A (auto-growing g_PanelF4_* so cols*rows >= MAX_PANEL_SLOTS) is
// what gets new slots visible at default positions; this cache feeds
// Layer B (per-slot pos/size overrides applied after the scroll
// controller builds its grid).
// =============================================================================
// Indexed by slot id; resized in LoadLeaguesSetup() to MAX_PANEL_SLOTS.
// Slots without an INI override leave the entry at its default (all
// `has_*` false) — callers should treat that as "use stock layout".
static std::vector<PanelSlotLayout> g_PanelSlotLayouts;

namespace {

// =============================================================================
// INI helpers — same shape as the squad-data / kit-data loaders. We keep
// a private copy rather than pulling them from a shared header so this
// file stays self-contained.
// =============================================================================

bool IniExists(const char* path)
{
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadIniString(const char* path, const char* section, const char* key,
                   char* out, DWORD outSize)
{
    DWORD n = GetPrivateProfileStringA(section, key, "", out, outSize, path);
    return n > 0 && out[0] != '\0';
}

// strtoul-with-base-0 wrapper; returns `fallback` on parse failure or
// when the value is empty/missing.
int32_t ParseInt(const char* text, int32_t fallback)
{
    if (!text || !*text) return fallback;
    while (*text == ' ' || *text == '\t') ++text;
    char* endp = nullptr;
    long v = std::strtol(text, &endp, 0);
    if (endp == text) return fallback;
    return static_cast<int32_t>(v);
}

// strtof wrapper with the same fallback semantics. INI floats are written
// in plain decimal (e.g. "-3.5", "1.2e1"); locale-independent strtof is
// fine for the values we expect.
float ParseFloat(const char* text, float fallback)
{
    if (!text || !*text) return fallback;
    while (*text == ' ' || *text == '\t') ++text;
    char* endp = nullptr;
    float v = std::strtof(text, &endp);
    if (endp == text) return fallback;
    return v;
}

// =============================================================================
// Discover the highest numeric `leagues/<n>.ini` filename in the leagues
// folder. Returns -1 if no numeric INIs exist (or the folder is missing).
// We deliberately skip non-numeric names like setup.ini.
// =============================================================================
int ScanHighestLeagueIniId()
{
    int hi = -1;

    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(".\\leagues\\*.ini", &data);
    if (h == INVALID_HANDLE_VALUE) return hi;

    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        // Parse leading digits up to the first dot. Reject any name that
        // isn't pure digits before the extension (e.g. "setup.ini").
        bool allDigits = true;
        int  n = 0;
        const char* p = data.cFileName;
        if (!*p || *p == '.') { allDigits = false; }
        for (; *p && *p != '.'; ++p) {
            if (*p < '0' || *p > '9') { allDigits = false; break; }
            n = n * 10 + (*p - '0');
        }
        if (allDigits && n > hi) hi = n;
    } while (FindNextFileA(h, &data));

    FindClose(h);
    return hi;
}

// =============================================================================
// Apply leagues/setup.ini if it exists. Missing keys leave the global at
// its default. Sanity-clamp values that would corrupt the panel layout.
// =============================================================================
void ApplySetupIni()
{
    const char* path = ".\\leagues\\setup.ini";
    if (!IniExists(path)) return;

    char val[64];

    if (ReadIniString(path, "setup", "team_slot_count", val, sizeof(val))) {
        int v = ParseInt(val, g_LeagueTeamSlotMax);
        if (v < 1)                    v = 1;
        if (v > TEAM_SLOT_COUNT)      v = TEAM_SLOT_COUNT;
        g_LeagueTeamSlotMax = v;
    }

    if (ReadIniString(path, "panel", "f0_side0", val, sizeof(val)))
        g_PanelF0_Side0 = ParseInt(val, g_PanelF0_Side0);
    if (ReadIniString(path, "panel", "f4_side0", val, sizeof(val)))
        g_PanelF4_Side0 = ParseInt(val, g_PanelF4_Side0);
    if (ReadIniString(path, "panel", "f0_side1", val, sizeof(val)))
        g_PanelF0_Side1 = ParseInt(val, g_PanelF0_Side1);
    if (ReadIniString(path, "panel", "f4_side1", val, sizeof(val)))
        g_PanelF4_Side1 = ParseInt(val, g_PanelF4_Side1);

    // [panel] panel_count — overrides the Step 9 iteration count in the
    // panel creator. Stock = 3 (8/8/4 seats). Read here only if the key
    // is explicitly present; otherwise leave as the sentinel (-1) so
    // LoadLeaguesSetup() can auto-compute it AFTER the directory scan
    // bumps MAX_PANEL_SLOTS (we don't know the final slot count yet).
    if (ReadIniString(path, "panel", "panel_count", val, sizeof(val))) {
        int v = ParseInt(val, 3);
        if (v < 3) v = 3;
        g_LeaguePanelCount = v;  // explicit value, will be clamped later
    }

    Logger::Log("[Leagues] setup.ini: team_slot_max=%d "
                "f0/f4 side0=%d/%d side1=%d/%d",
                g_LeagueTeamSlotMax,
                g_PanelF0_Side0, g_PanelF4_Side0,
                g_PanelF0_Side1, g_PanelF4_Side1);
}

// =============================================================================
// Build a TeamSlotBuffer from leagues/<id>.ini. Returns true on success,
// false if the file doesn't exist or the [league] section is empty.
//
// Buffer is fully zeroed and team-ID-sentinel-filled before any keys are
// applied so partial INIs Just Work.
// =============================================================================
bool LoadOneLeagueIni(int id, TeamSlotBuffer& out)
{
    char iniPath[MAX_PATH];
    std::snprintf(iniPath, sizeof(iniPath), ".\\leagues\\%d.ini", id);
    if (!IniExists(iniPath)) return false;

    std::memset(&out, 0, sizeof(out));
    for (int i = 0; i < TEAM_SLOT_COUNT; ++i)
        out.teamIDs[i] = CLUB_NULL_ID;

    char val[256];
    bool any = false;

    // Name → out.name (ASCII; CopyStr null-terminates within the field).
    if (ReadIniString(iniPath, "league", "name", val, sizeof(val))) {
        CopyStr(out.name, val, sizeof(out.name));
        any = true;
    }

    // displayID — accept any base (0xN, decimal, octal).
    if (ReadIniString(iniPath, "league", "display_id", val, sizeof(val))) {
        out.displayID = ParseInt(val, 0);
        any = true;
    }

    // team_0..team_<g_LeagueTeamSlotMax-1> → teamIDs[].
    int parseCap = g_LeagueTeamSlotMax;
    if (parseCap > TEAM_SLOT_COUNT) parseCap = TEAM_SLOT_COUNT;

    for (int i = 0; i < parseCap; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "team_%d", i);
        if (ReadIniString(iniPath, "league", key, val, sizeof(val))) {
            out.teamIDs[i] = ParseInt(val, CLUB_NULL_ID);
            any = true;
        }
    }

    return any;
}

// =============================================================================
// Read leagues/<id>.ini's [panel] section into a PanelSlotLayout. Returns
// true if any [panel] key was present. Missing keys leave the layout's
// has_* flags false so callers know to use the stock grid placement.
//
// Schema:
//   [panel]
//   pos_x   = -3.5         ; world-space x offset for this slot
//   pos_y   =  1.2         ; world-space y offset
//   pos_z   =  0.0         ; world-space z (depth); optional
//   scale   =  1.0         ; uniform scale (sets both x/y)
//   scale_y =  1.5         ; optional separate y-scale; if set, `scale`
//                          ; alone applies to x only
//   control_x      =  9.0   ; optional visual-slot rich-control rect override
//   control_y      = 27.0
//   control_depth  = -100.0 ; parsed/cache only for now; logo quad uses x/y/w/h
//   control_width  = 32.0
//   control_height = 32.0
// =============================================================================
bool LoadOnePanelLayout(int id, PanelSlotLayout& out)
{
    char iniPath[MAX_PATH];
    std::snprintf(iniPath, sizeof(iniPath), ".\\leagues\\%d.ini", id);
    if (!IniExists(iniPath)) return false;

    char val[64];
    bool any = false;

    // Position keys — all three are independent. has_pos becomes true
    // if ANY of pos_x/y/z were specified; missing components default to 0
    // (caller can interpret that as "leave that axis at stock" via has_pos
    // + a per-axis flag if we ever need that level of granularity).
    bool gotX = ReadIniString(iniPath, "panel", "pos_x", val, sizeof(val));
    if (gotX) out.pos_x = ParseFloat(val, out.pos_x);
    bool gotY = ReadIniString(iniPath, "panel", "pos_y", val, sizeof(val));
    if (gotY) out.pos_y = ParseFloat(val, out.pos_y);
    bool gotZ = ReadIniString(iniPath, "panel", "pos_z", val, sizeof(val));
    if (gotZ) out.pos_z = ParseFloat(val, out.pos_z);
    if (gotX || gotY || gotZ) { out.has_pos = true; any = true; }

    if (ReadIniString(iniPath, "panel", "scale", val, sizeof(val))) {
        float s = ParseFloat(val, 1.0f);
        out.scale_x = s;
        out.scale_y = s;
        out.has_scale = true;
        any = true;
    }
    if (ReadIniString(iniPath, "panel", "scale_y", val, sizeof(val))) {
        out.scale_y = ParseFloat(val, out.scale_y);
        out.has_scale2 = true;
        any = true;
    }

    if (ReadIniString(iniPath, "panel", "control_x", val, sizeof(val))) {
        out.control_x = ParseFloat(val, out.control_x);
        out.has_control_x = true;
        out.has_control_rect = true;
        any = true;
    }
    if (ReadIniString(iniPath, "panel", "control_y", val, sizeof(val))) {
        out.control_y = ParseFloat(val, out.control_y);
        out.has_control_y = true;
        out.has_control_rect = true;
        any = true;
    }
    if (ReadIniString(iniPath, "panel", "control_depth", val, sizeof(val))) {
        out.control_depth = ParseFloat(val, out.control_depth);
        out.has_control_depth = true;
        out.has_control_rect = true;
        any = true;
    }
    if (ReadIniString(iniPath, "panel", "control_width", val, sizeof(val))) {
        out.control_width = ParseFloat(val, out.control_width);
        out.has_control_width = true;
        out.has_control_rect = true;
        any = true;
    }
    if (ReadIniString(iniPath, "panel", "control_height", val, sizeof(val))) {
        out.control_height = ParseFloat(val, out.control_height);
        out.has_control_height = true;
        out.has_control_rect = true;
        any = true;
    }

    return any;
}

}  // namespace

// =============================================================================
// Public entry points (declared in club_hooks_common.h).
// =============================================================================

void LoadLeaguesSetup()
{
    ApplySetupIni();

    int hi = ScanHighestLeagueIniId();
    if (hi >= 20) {
        MAX_PANEL_SLOTS = hi + 1;
        Logger::Log("[Leagues] Bumping MAX_PANEL_SLOTS to %d "
                    "(highest leagues/<n>.ini = %d).",
                    MAX_PANEL_SLOTS, hi);
    } else {
        MAX_PANEL_SLOTS = 20;
        Logger::Log("[Leagues] MAX_PANEL_SLOTS=20 (no INIs above id=19; "
                    "highest found=%d).", hi);
    }

    // ── League-panel container count (Step 9 iteration count) ──────────
    // Stock = 3 (8/8/4 = 20 slots). When MAX_PANEL_SLOTS > 20 we need
    // extra parent panels so slots 20+ don't fall to (0,0,0). If the user
    // didn't set [panel] panel_count in setup.ini (sentinel -1), auto-
    // compute it; otherwise honour the explicit value (clamped at >= 3).
    {
        int needed = (MAX_PANEL_SLOTS + SLOTS_PER_LEAGUE_PAGE - 1)
                     / SLOTS_PER_LEAGUE_PAGE;
        if (needed < 3) needed = 3;

        if (g_LeaguePanelCount < 0) {
            g_LeaguePanelCount = needed;
            Logger::Log("[Leagues] panel_count auto = %d "
                        "(ceil(%d slots / %d per page), min 3).",
                        g_LeaguePanelCount, MAX_PANEL_SLOTS,
                        SLOTS_PER_LEAGUE_PAGE);
        } else {
            if (g_LeaguePanelCount < 3) g_LeaguePanelCount = 3;
            Logger::Log("[Leagues] panel_count = %d (explicit; auto would "
                        "have been %d).", g_LeaguePanelCount, needed);
        }
    }

    // ── F0/F4 (g_PanelF*_Side*) DELIBERATELY NOT AUTO-GROWN ─────────────
    //
    // g_PanelF0_Side*/g_PanelF4_Side* configure the TEAM-SELECTION panel
    // (Screen 3 — the per-league team grid), not the league-selection
    // panel (Screen 1/2). An earlier auto-grow tied them to MAX_PANEL_SLOTS,
    // which is the LEAGUE count — that made Screen 3 draw garbage cells
    // for leagues with > f0*f4 leagues defined. The team grid is always
    // capped at TEAM_SLOT_COUNT (20) by the buffer shape, so f0*f4 should
    // stay at the user-chosen value (default 5/4 / 4/5).
    //
    // The "extra leagues render as a dot in the middle" symptom on Screen
    // 1/2 belongs to a SEPARATE panel — see WIP doc Task 1 for the
    // league-panel-creator hunt that owns those slot positions.

    // ── Per-slot [panel] override cache ──────────────────────────────────
    //
    // Today this populates g_PanelSlotLayouts but isn't wired into actual
    // rendering yet (see docs/INTERNALS.md, "Layer B"). The
    // INI keys are documented and stable, so users can fill them in now
    // and benefit when the wire-up lands.
    g_PanelSlotLayouts.assign(MAX_PANEL_SLOTS, PanelSlotLayout{});
    int withOverrides = 0;
    for (int slot = 0; slot < MAX_PANEL_SLOTS; ++slot) {
        if (LoadOnePanelLayout(slot, g_PanelSlotLayouts[slot]))
            ++withOverrides;
    }
    if (withOverrides > 0) {
        Logger::Log("[Leagues] Cached %d slot layout override(s) "
                    "(rendering wire-up is a follow-up).", withOverrides);
    }
}

const PanelSlotLayout* GetLeaguePanelLayout(int slot)
{
    if (slot < 0 || slot >= static_cast<int>(g_PanelSlotLayouts.size()))
        return nullptr;
    const PanelSlotLayout& L = g_PanelSlotLayouts[slot];
    if (!L.has_pos && !L.has_scale && !L.has_scale2 && !L.has_control_rect)
        return nullptr;
    return &L;
}

void ApplyLeagueIniOverrides(int side)
{
    if (!fn_SetTeamList) return;

    TeamSlotBuffer buf;
    int applied = 0;
    for (int slot = 0; slot < MAX_PANEL_SLOTS; ++slot) {
        if (!LoadOneLeagueIni(slot, buf)) continue;
        fn_SetTeamList(DISPLAY_HANDLE_ARR[side], slot,
                        reinterpret_cast<int>(&buf));
        ++applied;
        Logger::Log("[Leagues] side=%d slot=%d <- leagues/%d.ini "
                    "(name='%s' displayID=0x%X)",
                    side, slot, slot, buf.name,
                    static_cast<uint32_t>(buf.displayID));
    }
    if (applied > 0) {
        Logger::Log("[Leagues] side=%d: applied %d INI override(s).",
                    side, applied);
    }
}

// =============================================================================
// Layer B wire-up: rewrite per-cell positions on a freshly-built scroll
// controller. The controller's cell array layout (see decompile of
// FUN_00b0f270 / FUN_00b0fc60):
//   ctrl[8]  (byte +0x20) = total cell count
//   ctrl[9]  (byte +0x24) = pointer to cell array
//   each cell is 0x14 (20) bytes; cell[1]=label node, cell[2]=icon node.
//
// `fn_SetCellPosition(cell, posXYZW)` rewrites both nodes' positions —
// label gets posXYZW directly, icon gets posXYZW + ctrl-internal offset.
// We pass the INI's pos_x/y/z and zero for w to match how the original
// grid loop calls FUN_00b0fc60 (always 4 floats with w == 0).
//
// Cell-position coordinates are LOCAL to the controller's anchor (the
// bone-derived world position the panel was built at). The original grid
// loop fills cells with (col*cellW, row*cellH); our overrides replace
// that local offset with the user's pos_x/y/z.
// =============================================================================
void ApplyPanelLayoutOverrides(uint32_t* scrollCtrl)
{
    if (!scrollCtrl || !fn_SetCellPosition) return;
    if (g_PanelSlotLayouts.empty()) return;

    uint32_t cellCount = scrollCtrl[8];
    int*     cellBase  = reinterpret_cast<int*>(scrollCtrl[9]);
    if (!cellBase || cellCount == 0) return;

    int applied = 0;
    int cap = static_cast<int>(g_PanelSlotLayouts.size());
    if (cap > static_cast<int>(cellCount)) cap = static_cast<int>(cellCount);

    for (int slot = 0; slot < cap; ++slot)
    {
        const PanelSlotLayout& L = g_PanelSlotLayouts[slot];
        if (!L.has_pos) continue;  // keep stock grid placement

        // Each cell is 0x14 bytes; cell pointers are int* (treated as the
        // controller's per-cell struct base).
        int* cell = reinterpret_cast<int*>(
            reinterpret_cast<uint8_t*>(cellBase) + slot * 0x14);

        float posXYZW[4];
        posXYZW[0] = L.pos_x;
        posXYZW[1] = L.pos_y;
        posXYZW[2] = L.pos_z;
        posXYZW[3] = 0.0f;

        fn_SetCellPosition(cell, posXYZW);
        ++applied;
        Logger::Log("[Leagues] panel-pos override slot=%d -> "
                    "(%.3f, %.3f, %.3f)",
                    slot, L.pos_x, L.pos_y, L.pos_z);
    }
    if (applied > 0) {
        Logger::Log("[Leagues] applied %d panel-position override(s) "
                    "(of %d cached, %d cells).",
                    applied, static_cast<int>(g_PanelSlotLayouts.size()),
                    static_cast<int>(cellCount));
    }
}
