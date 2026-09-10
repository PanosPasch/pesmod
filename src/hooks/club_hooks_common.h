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

#pragma once
// =============================================================================
// club_hooks_common.h
//
// Shared macros, game global accessors, struct definitions, constants,
// and extern declarations for all function pointers used across the
// club hooks translation units.
// =============================================================================

#include <windows.h>
#include <cstdint>
#include <cstring>

// =============================================================================
// SECTION 1 — Raw memory accessor macros
// =============================================================================

#define GDWORD(addr)   (*reinterpret_cast<int32_t* >(addr))
#define GWORD(addr)    (*reinterpret_cast<int16_t* >(addr))
#define GBYTE(addr)    (*reinterpret_cast<int8_t*  >(addr))
#define GUBYTE(addr)   (*reinterpret_cast<uint8_t* >(addr))

// =============================================================================
// SECTION 2 — Game global variable macros
// =============================================================================

// ── Mode / state flags ───────────────────────────────────────────────────────

// Master mode flag. 0=exhibition/league, 2=cup, 5=ML club screen
#define MODE_FLAG           GBYTE (0x03be12c9)

// Side-swap flag for availability checks (bit 0)
#define SIDE_SWAP_FLAG      GUBYTE(0x03b8e48e)

// Game mode: 4 = Master League / Cup (triggers logo preloading)
#define GAME_MODE_FLAG      GDWORD(0x03becc60)

// Two-player mode: 3 = split cursor between players
#define TWO_PLAYER_MODE     GBYTE (0x03be0aac)

// ── Display handles ──────────────────────────────────────────────────────────

// Single display handle used by LoadCompetitionMenuTeams (Cup/League front menu)
#define DISPLAY_HANDLE      GDWORD(0x03be6318)

// Per-side panel handles written by CreateLeagueSelectionPanel,
// read by LoadClubTeamSlots. Array of 2 int32 values.
// [0] = side 0 panel, [1] = side 1 panel.
#define DISPLAY_HANDLE_ARR  reinterpret_cast<int32_t*>(0x03be6310)

// ── Slot count output array ──────────────────────────────────────────────────

// Written by LoadClubTeamSlots: stores (fillCount-1) per slot as int8
#define SLOT_COUNT_OUT      reinterpret_cast<int8_t*>(0x03a7b8f0)

// ── Club selection screen globals ────────────────────────────────────────────

// Pointer to the UI layout object; layout node handle is at +0xc
#define UI_LAYOUT_OBJ       GDWORD(0x03be6308)

// Cursor/player position packed value; split on TWO_PLAYER_MODE==3
#define CURSOR_SPLIT_VAL    GDWORD(0x00d53998)

// ML/Cup timer base written during player state init
#define PLAYER_TIMER_BASE   GDWORD(0x03a79adc)

// Input navigation table base address
#define NAV_TABLE_BASE      (reinterpret_cast<void*>(0x03a79ae0))

// Display object link targets used by LinkDisplayObjects in InitClubSelectionScreen
#define DISPLAY_LINK_A      GDWORD(0x03be6300)
#define DISPLAY_LINK_B      0x03be62a0

// ── Slot data arrays (LoadClubTeamSlots main loop, byte-offset indexed) ──────
//
// ESI steps from 0 to 0x98 in increments of 8.
// Each array is int32; byte offset ESI selects the pair for that iteration.
//
static const uintptr_t BASE_SLOT_FIRST            = 0x00d5bef0; // firstTeamID
static const uintptr_t BASE_SLOT_LAST             = 0x00d5bef4; // lastTeamID
static const uintptr_t BASE_SLOT_UNLOCKABLES      = 0x00d5bf90; // extBase
static const uintptr_t BASE_SLOT_UNLOCKABLES_LAST = 0x00d5bf94; // extLast
static const uintptr_t BASE_SLOT_STR              = 0x00d5be50; // strHandle (int32/slot)
static const uintptr_t BASE_SLOT_DISPID           = 0x00d5bea0; // displayID (int32/slot)

// ── Panel creation lookup tables (side-indexed by side*4 or side*0xc) ────────
static const uintptr_t TABLE_TEXTURE_A  = 0x00e86274; // [side*4] primary texture ID
static const uintptr_t TABLE_TEXTURE_B  = 0x00e86244; // [side*4] secondary texture ID
static const uintptr_t TABLE_TEXTURE_C  = 0x00e86268; // [side*4] animation/bone anim ID
static const uintptr_t TABLE_BONE_ID_A  = 0x00e8625c; // [side*4] scroll list bone ID (short)
static const uintptr_t TABLE_BONE_ID_B  = 0x00e86250; // [side*4] pagination bone ID (short)
static const uintptr_t TABLE_NODE_DATA  = 0x00e860e8; // [side*12] 3 texture IDs per side

// =============================================================================
// SECTION 3 — Constants
// =============================================================================

// Number of team ID slots in one TeamSlotBuffer / panel slot entry
static const int TEAM_SLOT_COUNT = 20;

// Sentinel value for empty team slot
static const int CLUB_NULL_ID = 0xFFFF;

// Maximum unlockable/classic teams appended per league slot
static const int UNLOCKABLES_ALLOC_INDEX_MAX = 0x14;

// LoadClubTeamSlots loop: ESI runs 0..0x98 stepping by 8 (20 iterations)
static const int TEAM_ALLOCATION_BUFFER_MAX_SIZE  = 0xA0;
static const int TEAM_ALLOCATION_BUFFER_STEP      = 8;

// ESI threshold for classic/extra team unlock checks
static const int UNLOCKABLES_ALLOCATION_BUFFER_MAX_SIZE = 0x38;

// Panel slot count — RUNTIME variable. Defaults to 20 (the stock cap).
// `LoadLeaguesSetup()` (club_hooks_leagues.cpp) bumps this at startup
// based on the highest `leagues/<n>.ini` filename it finds: if any INI
// has id >= 20, MAX_PANEL_SLOTS becomes id+1 so the panel allocation,
// slot-init loop, scroll controller, and destructor trailer all scale
// to fit. The allocation formula, scroll controller link count, and
// destructor trailer offset already key off MAX_PANEL_SLOTS, so no
// other changes are needed when this grows.
//
// Was a `static const int` originally; converted to `extern int` so the
// leagues setup loader can adjust it once at startup before any panel
// is created. Anything keyed off this — MAX_LEAGUE_IDX / MAX_PAGES in
// club_hooks_panel_nav.cpp — was changed to a macro that re-evaluates
// against the live value.
extern int MAX_PANEL_SLOTS;

// How many `team_<n>` keys to parse from a `leagues/<id>.ini`. Capped
// internally to TEAM_SLOT_COUNT (the structural buffer size) so no
// changes to TeamSlotBuffer are needed if a setup.ini sets it higher.
extern int g_LeagueTeamSlotMax;

// Panel layout — DW(0x00F0) / DW(0x00F4) values written by
// hook_CreateLeagueSelectionPanel (club_hooks_panel.cpp ~line 189).
// Stock values mirror the original (side 0: 5/4, side 1+: 4/5).
// Override via `leagues/setup.ini` `[panel]` section.
extern int g_PanelF0_Side0;   // DW(0x00F0) when side == 0  (default 5)
extern int g_PanelF4_Side0;   // DW(0x00F4) when side == 0  (default 4)
extern int g_PanelF0_Side1;   // DW(0x00F0) when side != 0  (default 4)
extern int g_PanelF4_Side1;   // DW(0x00F4) when side != 0  (default 5)

// League-panel container count — number of "background panel" parents
// the panel creator's Step 9 loop spawns. Stock = 3 (those visually own
// the 8/8/4 league-slot seats: National 8, Club 8, Misc/ML 4 = 20 stock
// slots). When MAX_PANEL_SLOTS > 20 you can raise this so additional
// slots get a parent and don't fall to (0,0,0) / screen centre. Each
// extra panel gets its anchor position from
// `g_PanelSlotLayouts[firstSlotOfThatPage]`'s [panel] pos_x/y/z keys,
// where `firstSlotOfThatPage = panelIndex * SLOTS_PER_LEAGUE_PAGE`.
//
// Stock slot-to-panel mapping:
//   panel 0 -> slots 0..7   (8 seats)
//   panel 1 -> slots 8..15  (8 seats)
//   panel 2 -> slots 16..19 (4 seats)
//   panel 3 -> slots 20..23 (custom — drives extras)
//   panel 4 -> slots 24..27 (custom)
//   ...
extern int g_LeaguePanelCount;
static const int SLOTS_PER_LEAGUE_PAGE = 8;

// League logo preloading constants (InitClubSelectionScreen, GAME_MODE==4)
static const int LEAGUE_LOGO_SLOT_SIZE = 0x158;
static const int LEAGUE_LOGO_FLAG      = 5;

// Panel positioning bone IDs
static const int16_t PANEL_BONE_ID_SIDE0 = 3;
static const int16_t PANEL_BONE_ID_SIDE1 = 4;

// ML club screen mode identifier
static const int8_t ML_CLUB_SCREEN_MODE = 5;

// Name sentinel stored in uninitialised panel slots: "NONE" in little-endian
static const uint32_t SLOT_NAME_SENTINEL = 0x454E4F4E;

// Bone entry stride in the skeleton entry list
static const int BONE_ENTRY_SIZE = 0xe;

// ── Panel layout constants (used by both creator and destructor) ─────────────

// Original trailer byte offsets (for 20 slots)
static const int PANEL_ORIG_TRAILER_A = 0x7618;
static const int PANEL_ORIG_TRAILER_B = 0x761C;
static const int PANEL_SLOT_STRIDE    = 0x4A4;
static const int PANEL_BASE_ALLOC     = 0x7620;

// Compute the true trailer B byte offset for a given slot count
static inline int PanelTrailerB_ByteOff(int slotCount) {
    return PANEL_ORIG_TRAILER_B + (slotCount - 20) * PANEL_SLOT_STRIDE;
}
// Same as dword index (for uint32_t* array access)
static inline int PanelTrailerB_DwordIdx(int slotCount) {
    return PanelTrailerB_ByteOff(slotCount) / 4;
}

// =============================================================================
// SECTION 4 — Shared data structures
// =============================================================================

// Buffer passed as param_3 to FUN_00b07b00 (SetTeamList).
// FUN_00b07b00 reads:
//   [+0x000] 20 × int32 team IDs
//   [+0x0A0] null-terminated league name string
//   [+0x4A0] int32 display/logo ID
//
// Offsets confirmed by analysis of FUN_00b07b00's access pattern.
#pragma pack(push, 1)
struct TeamSlotBuffer
{
    int32_t teamIDs[TEAM_SLOT_COUNT];           // [+0x000] 20 slots × 4 bytes = 0x50
    uint8_t _pad[0xA0 - 0x50 - ((TEAM_SLOT_COUNT - 20) * 4)];  // [+0x050] 0x50 bytes gap
    char    name[0x400];                         // [+0x0A0] league name
    int32_t displayID;                           // [+0x4A0] logo/display ID
};
#pragma pack(pop)

static_assert(offsetof(TeamSlotBuffer, name)      == 0x0A0, "name offset wrong");
static_assert(offsetof(TeamSlotBuffer, displayID) == 0x4A0, "displayID offset wrong");
static_assert(sizeof(TeamSlotBuffer)              == 0x4A4, "struct size wrong");

// =============================================================================
// SECTION 5 — Shared helper functions
// =============================================================================

// Fill all 20 team ID slots with the sentinel (0xFFFF = empty)
static inline void FillSentinels(int* arr)
{
    for (int i = 0; i < TEAM_SLOT_COUNT; i++)
        arr[i] = CLUB_NULL_ID;
}

// Safe string copy; always null-terminates
static inline void CopyStr(char* dst, const char* src, int maxLen = 1024)
{
    int i = 0;
    while (i < maxLen - 1 && src[i])
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

// Read int32 at (base + byteOffset) — used for the stride-8 slot arrays
static inline int32_t SlotInt(uintptr_t base, int byteOffset)
{
    return *reinterpret_cast<int32_t*>(base + byteOffset);
}

// Compute total allocation size in bytes for a panel with slotCount slots.
// Derived from FUN_00b09050 layout analysis:
//   pre-slot header : 0x651 uint32s
//   per slot        : 0x129 uint32s (= 0x4A4 bytes)
//   post-slot trailer: 0x297 uint32s
//   total uint32s   : 0x8E8 + slotCount * 0x129
static inline int ComputePanelAllocSize(int slotCount)
{
    return (0x8E8 + slotCount * 0x129) * 4;
}

// =============================================================================
// SECTION 6 — Function pointer type declarations
// =============================================================================

// ── League / cup selectors ───────────────────────────────────────────────────
typedef int   (__cdecl *FN_GetLeagueSelector)  (int type);
typedef int   (__cdecl *FN_GetSelectedCupIdx)  (int type);

// ── String retrieval ─────────────────────────────────────────────────────────
// FUN_00861640: single-arg league string lookup by slot index
typedef char* (__cdecl *FN_GetLeagueString)    (int slotIndex);
// FUN_0094f2c0 / FUN_00783410: string by AFS handle
typedef char* (__cdecl *FN_GetStringByHandle)  (int handle);

// ── Team list / display ──────────────────────────────────────────────────────
// FUN_00b07b00: write TeamSlotBuffer into a panel slot
typedef void  (__cdecl *FN_SetTeamList)        (int handle, int slotIdx, int bufPtr);
// FUN_00b09590: set the highlighted/selected team in a slot
typedef void  (__cdecl *FN_SetSelectedTeam)    (int handle, int slotIdx, uint16_t teamID);
// FUN_00b0ad80: set navigation/display state
typedef void  (__cdecl *FN_SetNavState)        (int handle, int slot, int f1, int f2);
// FUN_009dcb00: post-load display refresh (no args)
typedef void  (__cdecl *FN_PostLoadUpdate)     ();
// FUN_00b07ea0: display reset/refresh
typedef void  (__cdecl *FN_DisplayReset)       (int handle, int slotIdx);
// FUN_00b07bc0: set a text label on the display
typedef void  (__cdecl *FN_SetLabel)           (int handle, int idx, const char* text);
// FUN_00b07c40: set display page/scroll state
typedef void  (__cdecl *FN_SetPage)            (int handle, int flag, int value);

// ── Mode / availability checks ───────────────────────────────────────────────
// FUN_008c0180: returns 0=normal/exhibition, non-zero=ML/Cup
typedef int   (__cdecl *FN_GetModeFlag)        ();
// FUN_009cc4b0: returns 1 if a classic/extra team is unlocked
typedef int   (__cdecl *FN_IsTeamUnlocked)     (uint16_t teamID);
// FUN_00a65af0 / FUN_00a65b50: team availability per side
typedef int   (__cdecl *FN_IsTeamAvail)        (uint16_t teamID);

// ── Logo preloading ──────────────────────────────────────────────────────────
// FUN_009dab20: preload a league logo texture
typedef void  (__cdecl *FN_LoadLeagueLogo)     (int slotSize, int displayID, int flag);

// ── UI resource loading ──────────────────────────────────────────────────────
// FUN_009c2780: load a UI script/resource by handle
typedef void  (__cdecl *FN_LoadUIResource)     (int resourceHandle);
// FUN_009edc40: initialise club selection display state
typedef void  (__cdecl *FN_InitClubDisplayState)();

// ── Panel creation and display ───────────────────────────────────────────────
// FUN_00b09050: create a league selection panel (we hook this)
typedef uint32_t* (__cdecl *FN_CreateDisplayPanel)(uint32_t* sideFlag,
                                                     uint32_t* cursorPos,
                                                     float*    posVec);
// FUN_00b07f70: activate/show a panel
typedef void  (__cdecl *FN_ShowPanel)          (int panelHandle, int flag);

// ── Bone / skeleton ──────────────────────────────────────────────────────────
// FUN_00955320: matrix transform a node (used by GetBoneWorldPosition)
typedef void  (__cdecl *FN_TransformNode)      (int nodeDataPtr);
// FUN_00950490 (hooked): find bone by ID, transform to world space
typedef float* (__cdecl *FN_GetBoneWorldPos)   (int objectPtr,
                                                  float* outVec4,
                                                  int16_t boneID);
// FUN_00950580: get bone scale value
typedef float (__cdecl *FN_GetBoneScale)       (int objectPtr, int16_t boneID);

// ── Player state (ML/Cup mode) ────────────────────────────────────────────────
typedef int   (__cdecl *FN_GetPlayerState)     (int playerIndex);
typedef void  (__cdecl *FN_InitPlayerState)    (int playerStatePtr);
typedef void  (__cdecl *FN_ResetPlayerSelection)(int flag);
typedef void  (__cdecl *FN_SetPlayerActive)    (int flag);
typedef void  (__cdecl *FN_SetPlayerInputMode) (int mode);
typedef void  (__cdecl *FN_SetPlayerCursorType)(char cursorType);
typedef void  (__cdecl *FN_SetNavContext)       (int contextID);
typedef void  (__cdecl *FN_LinkDisplayObjects) (int handleA, int handleB, int linkType);
typedef void  (__cdecl *FN_MLClubScreenInit)   (int flag);
typedef void  (__cdecl *FN_SetupNavMapping)    (void* navTable, int inputID);
typedef uint32_t (__stdcall *FN_TimeGetTime)   ();

// ── Panel allocation internals ───────────────────────────────────────────────
typedef uint32_t* (__cdecl *FN_GameAlloc)             (int flags, int sizeBytes);
typedef uint32_t* (__cdecl *FN_CreateDisplaySubObject)(int a, int vtableAddr,
                                                         int b, int c, int d);
typedef void      (__cdecl *FN_InitDisplaySubObject)  (int subObjPtr);
typedef uint32_t  (__cdecl *FN_CreateRenderNode)      (int assetID);
typedef uint32_t  (__cdecl *FN_GetChildNode)          (uint32_t renderNodeHandle);
// FUN_00950a00 — CreateChildNode (THE underlying child-node creator).
// `FN_GetChildNode` (= FUN_00950b40 / CreateChildNode_Default) is a 1-line
// wrapper that calls this with `attachDefaultBehavior = 1`; the unused
// sibling FUN_00950b50 / CreateChildNode_Bare passes 0.
//
//   parentNode             handle of the parent render node (must be != 0).
//   attachDefaultBehavior  if non-zero, register the default behavior
//                          vtable 0x95d410 at newNode[+0x30]; if zero,
//                          skip that registration.
//
// Allocates a 0xA8-byte node struct, initialises scale=(1,1,1,1),
// rotation=(0,0,0,0), color=(0xFF,0xFF,0xFF,0xFF), then links the new
// node into the parent's child list (FUN_00952860). Returns the new node
// pointer, or NULL if parentNode == 0. Verified by full disassembly +
// decompile of FUN_00950a00 (renamed CreateChildNode in Ghidra).
typedef uint32_t* (__cdecl *FN_CreateChildNode)       (uint32_t parentNode,
                                                         char     attachDefaultBehavior);
// FUN_009509b0 — SetNodeAsset (mis-named `fn_SetNodeTexture` in our code
// — kept for compat with existing call sites). It does NOT set a flat
// texture handle. `texID` (really an *asset ID*) is a small integer that
// indexes into the parent node's asset table:
//
//   parent[+0x0C]                = asset bank handle
//   parent[+0x0C][+8]            = { uint count, void* entries[count] }
//   parent[+0x0C][+8][1][assetID] = entry pointer (NULL if undefined here)
//
// The entry points to a frame block `{ ushort frameCount, ushort _pad,
// void* framesArray }`. After resolving, the function calls BindNodeFrame
// (FUN_00950260) which picks a frame (preserving the OLD frame index
// at node[+0x4E] — 0 for a fresh child) and stamps:
//   node[+0x44] = assetID            node[+0x48] = entry pointer
//   node[+0x20] = frame pointer      node[+0x1C] = sub-resource pointer
//   node[+0x4E] = frame index        node[+0xA2..+0xA5] = alpha bytes
//
// So an asset can carry MULTIPLE elements (frames, layouts, even
// pre-positioned "seats" for downstream cells). For league-panel
// `TABLE_NODE_DATA[side*3 + i]` entries, asset 8 (the third stock entry
// for side 0) is the panel banner art that bakes in the slot seats —
// changing it to 7 yields a different banner without those seats.
typedef void      (__cdecl *FN_SetNodeTexture)        (uint32_t nodeHandle, uint32_t texID);
typedef void      (__cdecl *FN_NodePropA)             (uint32_t node, int val);
typedef void      (__cdecl *FN_NodePropB)             (uint32_t node, int val);
typedef void      (__cdecl *FN_NodePropC)             (uint32_t node, int val);
typedef void      (__cdecl *FN_NodePropD)             (uint32_t node, int val);
typedef void      (__cdecl *FN_SetNodePosition)       (uint32_t nodeHandle, float* posVec4);
typedef uint32_t  (__cdecl *FN_LookupBone)            (uint32_t nodeHandle, uint32_t boneID);
typedef int*      (__cdecl *FN_ResolveBone)           (uint32_t boneHandle);
typedef uint32_t* (__cdecl *FN_AttachBone)            (uint32_t nodeHandle, int* bonePtr,
                                                         uint32_t boneID);
typedef void      (__cdecl *FN_SetBoneVisibility)     (uint32_t boneData, int visible);
typedef uint32_t* (__cdecl *FN_CreateScrollList)      (uint32_t scrollCount, int posData);
typedef void      (__cdecl *FN_SetScrollScale)        (int scrollListPtr, float scale);
typedef void      (__cdecl *FN_SetScrollFlag)         (int scrollListPtr, char flag);
typedef void      (__cdecl *FN_FinalisePanelLayout)   (uint32_t* panelPtr);
typedef uint32_t* (__cdecl *FN_CreateScrollController)(int pageSize, float* posVec,
                                                         uint32_t rowA, uint32_t rowB);
typedef void      (__cdecl *FN_SetScrollCtrlScale)    (int* scrollCtrl, float scale);
typedef void      (__cdecl *FN_LinkScrollCtrlToSlots) (int* scrollCtrl,
                                                         int slotArrayPtr, int slotCount);
typedef void      (__cdecl *FN_SetScrollBounds)       (int* scrollCtrl,
                                                         float boundsA, float boundsB);
// FUN_00b0fc60: set the on-screen position of one scroll-controller cell.
// `cell` points into the controller's cell array (each cell is 0x14 bytes,
// stride 20). `posXYZW` is a 4-float vector — the x/y are the cell's
// label-node position; z passes through to the icon-node; w is unused
// (the function writes 0 there). Used by Layer B per-slot panel-layout
// overrides — see ApplyPanelLayoutOverrides() in club_hooks_leagues.cpp.
typedef void      (__cdecl *FN_SetCellPosition)       (int* cell, float* posXYZW);
typedef uint32_t* (__cdecl *FN_CreateDisplayItem)     (int itemType);
typedef void      (__cdecl *FN_InitDisplayItem)       (uint32_t* itemPtr, int flag);
typedef void      (__cdecl *FN_SetItemScale)          (int itemPtr, float scale);
typedef uint32_t* (__cdecl *FN_GetPanelRegistrySlot)  (uint32_t* subObjPtr);

// ── Panel destructor internals (FUN_00b0ac50 sub-functions) ──────────────────
typedef void  (__cdecl *FN_PreDestroyCleanup)     (uint32_t* panel);        // 0x00b09b20
typedef void  (__cdecl *FN_DestroyScrollCtrl)     (uint32_t* ctrl);         // 0x00b0f400
typedef void  (__cdecl *FN_DestroyScrollList)     (uint32_t* list);         // 0x00b00d10
typedef void  (__cdecl *FN_DestroyDisplayItem)    (uint32_t handle);        // 0x00b00400
typedef void  (__cdecl *FN_DestroyBoneAttach)     (uint32_t* bone);         // 0x009553f0
typedef void  (__cdecl *FN_DestroyNodeChild)      (int* node);              // 0x009502b0
typedef void  (__cdecl *FN_ReleaseResource)       (int* owner, uint32_t* entry); // 0x0094f420
typedef void  (__cdecl *FN_FinalizeResourceOwner) (int* owner);             // 0x0094f780
typedef void  (__cdecl *FN_PreFreeSubObj)         (uint32_t subObjVal);     // 0x00b0bb60
typedef void  (__cdecl *FN_FreeSubObj)            (uint32_t* subObj);       // 0x00b0bb80
typedef void  (__cdecl *FN_GameFree)              (uint32_t* ptr);          // 0x0045bc50

// ── Panel navigation internals (FUN_00b09b80/FUN_00b0ac40 sub-functions) ─────
typedef void  (__cdecl *FN_InputEvent)            (void* param, uint32_t eventCode); // 0x009b8980
typedef int   (__cdecl *FN_ConfirmCheck)          (int flag, int panelPtr);           // 0x00b08370

// ── Kit data loading and color extraction ────────────────────────────────────
// FUN_008646b0: resolve alias team IDs (0x126/0x127 → real ID)
typedef short (__cdecl *FN_ResolveTeamID)         (short teamID);                    // 0x008646b0
// FUN_00866d80: get team data type (0=national, 2=national variant, 3=club/other)
typedef int   (__cdecl *FN_GetTeamDataType)       (uint16_t teamID);                 // 0x00866d80
// FUN_00864ef0: get club team kit data base pointer
typedef uint8_t* (__cdecl *FN_GetClubTeamKitBase) (uint16_t teamID);                 // 0x00864ef0
// FUN_00865100: get national team kit data base pointer
typedef uint8_t* (__cdecl *FN_GetNationalTeamKitBase)(uint16_t teamID);              // 0x00865100
// FUN_00865240: get kit data for a specific team+variant (stride 0x3E per variant)
typedef uint8_t* (__cdecl *FN_GetTeamKitDataPtr)  (uint16_t teamID, int variant);    // 0x00865240
// FUN_005c18b0: get weather condition (0, 1, or 2)
typedef uint32_t (__cdecl *FN_GetWeatherCondition)();                                // 0x005c18b0
// FUN_00966c70: check if slot has custom formation
typedef char  (__cdecl *FN_HasCustomFormation)     (uint8_t slotIndex);               // 0x00966c70
// FUN_00b15700: check if online mode is active
typedef int   (__cdecl *FN_IsOnlineMode)          ();                                // 0x00b15700
// FUN_00969490: load ML default team data
typedef void  (__cdecl *FN_LoadTeamData_MLDefault)(char* teamDataPtr);               // 0x00969490
// FUN_00969530: load team data with custom formation
typedef void  (__cdecl *FN_LoadTeamData_CustomFormation)(char* teamDataPtr,
                           uint16_t teamID, int formationIdx, int sideFlag);         // 0x00969530
// FUN_009692d0: load normal team data
typedef void  (__cdecl *FN_LoadTeamData_Normal)   (char* teamDataPtr,
                           uint16_t teamID, int sideFlag, int slotIndex);            // 0x009692d0
// FUN_009691d0: load team data with edited kit
typedef void  (__cdecl *FN_LoadTeamData_EditedKit)(char* teamDataPtr,
                           uint16_t teamID, int sideFlag);                           // 0x009691d0

// =============================================================================
// SECTION 7 — Extern declarations for all function pointers
//             (defined once in club_hooks_register.cpp)
// =============================================================================

extern FN_GetLeagueSelector   fn_GetLeagueSelector;
extern FN_GetSelectedCupIdx   fn_GetSelectedCupIdx;
extern FN_GetLeagueString     fn_GetLeagueString;
extern FN_GetStringByHandle   fn_GetStringByHandle;
extern FN_SetTeamList         fn_SetTeamList;
extern FN_SetSelectedTeam     fn_SetSelectedTeam;
extern FN_SetNavState         fn_SetNavState;
extern FN_PostLoadUpdate      fn_PostLoadUpdate;
extern FN_DisplayReset        fn_DisplayReset;
extern FN_SetLabel            fn_SetLabel;
extern FN_SetPage             fn_SetPage;
extern FN_GetModeFlag         fn_GetModeFlag;
extern FN_IsTeamUnlocked      fn_IsTeamUnlocked;
extern FN_IsTeamAvail         fn_AvailSide0;
extern FN_IsTeamAvail         fn_AvailSide1;
extern FN_LoadLeagueLogo      fn_LoadLeagueLogo;
extern FN_LoadUIResource      fn_LoadUIResource;
extern FN_InitClubDisplayState fn_InitClubDisplayState;
extern FN_CreateDisplayPanel  fn_CreateDisplayPanel;
extern FN_ShowPanel           fn_ShowPanel;
extern FN_TransformNode       fn_TransformNode;
extern FN_GetBoneWorldPos     fn_GetBoneWorldPos;
extern FN_GetBoneScale        fn_GetBoneScale;
extern FN_GetPlayerState      fn_GetPlayerState;
extern FN_InitPlayerState     fn_InitPlayerState;
extern FN_ResetPlayerSelection fn_ResetPlayerSelection;
extern FN_SetPlayerActive     fn_SetPlayerActive;
extern FN_SetPlayerInputMode  fn_SetPlayerInputMode;
extern FN_SetPlayerCursorType fn_SetPlayerCursorType;
extern FN_SetNavContext        fn_SetNavContext;
extern FN_LinkDisplayObjects  fn_LinkDisplayObjects;
extern FN_MLClubScreenInit    fn_MLClubScreenInit;
extern FN_SetupNavMapping     fn_SetupNavMapping;
extern FN_TimeGetTime         fn_TimeGetTime;
extern FN_GameAlloc           fn_GameAlloc;
extern FN_CreateDisplaySubObject fn_CreateDisplaySubObject;
extern FN_InitDisplaySubObject fn_InitDisplaySubObject;
extern FN_CreateRenderNode    fn_CreateRenderNode;
extern FN_GetChildNode        fn_GetChildNode;
extern FN_SetNodeTexture      fn_SetNodeTexture;
extern FN_NodePropA           fn_NodePropA;
extern FN_NodePropB           fn_NodePropB;
extern FN_NodePropC           fn_NodePropC;
extern FN_NodePropD           fn_NodePropD;
extern FN_SetNodePosition     fn_SetNodePosition;
extern FN_LookupBone          fn_LookupBone;
extern FN_ResolveBone         fn_ResolveBone;
extern FN_AttachBone          fn_AttachBone;
extern FN_SetBoneVisibility   fn_SetBoneVisibility;
extern FN_CreateScrollList    fn_CreateScrollList;
extern FN_SetScrollScale      fn_SetScrollScale;
extern FN_SetScrollFlag       fn_SetScrollFlag;
extern FN_FinalisePanelLayout fn_FinalisePanelLayout;
extern FN_CreateScrollController fn_CreateScrollController;
extern FN_SetScrollCtrlScale  fn_SetScrollCtrlScale;
extern FN_LinkScrollCtrlToSlots fn_LinkScrollCtrlToSlots;
extern FN_SetScrollBounds     fn_SetScrollBounds;
extern FN_SetCellPosition     fn_SetCellPosition;
extern FN_CreateDisplayItem   fn_CreateDisplayItem;
extern FN_InitDisplayItem     fn_InitDisplayItem;
extern FN_SetItemScale        fn_SetItemScale;
extern FN_GetPanelRegistrySlot fn_GetPanelRegistrySlot;

// Panel destructor sub-functions
extern FN_PreDestroyCleanup     fn_PreDestroyCleanup;
extern FN_DestroyScrollCtrl     fn_DestroyScrollCtrl;
extern FN_DestroyScrollList     fn_DestroyScrollList;
extern FN_DestroyDisplayItem    fn_DestroyDisplayItem;
extern FN_DestroyBoneAttach     fn_DestroyBoneAttach;
extern FN_DestroyNodeChild      fn_DestroyNodeChild;
extern FN_ReleaseResource       fn_ReleaseResource;
extern FN_FinalizeResourceOwner fn_FinalizeResOwner;
extern FN_PreFreeSubObj         fn_PreFreeSubObj;
extern FN_FreeSubObj            fn_FreeSubObj;
extern FN_GameFree              fn_GameFree;

// Panel navigation sub-functions
extern FN_InputEvent            fn_InputEvent;
extern FN_ConfirmCheck          fn_ConfirmCheck;

// Kit data loading sub-functions
extern FN_ResolveTeamID         fn_ResolveTeamID;
extern FN_GetTeamDataType       fn_GetTeamDataType;
extern FN_GetClubTeamKitBase    fn_GetClubTeamKitBase;
extern FN_GetNationalTeamKitBase fn_GetNationalTeamKitBase;
extern FN_GetTeamKitDataPtr     fn_GetTeamKitData;
extern FN_GetWeatherCondition   fn_GetWeatherCondition;
extern FN_HasCustomFormation    fn_HasCustomFormation;
extern FN_IsOnlineMode          fn_IsOnlineMode;
extern FN_LoadTeamData_MLDefault fn_LoadTeamData_MLDefault;
extern FN_LoadTeamData_CustomFormation fn_LoadTeamData_CustomFormation;
extern FN_LoadTeamData_Normal   fn_LoadTeamData_Normal;
extern FN_LoadTeamData_EditedKit fn_LoadTeamData_EditedKit;

// =============================================================================
// SECTION 8 — Extern declarations for original function trampolines
// =============================================================================

typedef void     (__cdecl  *FN_LoadClubTeamSlots_t)       (int side);
typedef void     (__stdcall *FN_LoadCompetitionMenuTeams_t)();
typedef void     (__cdecl  *FN_InitClubSelectionScreen_t) ();
typedef float*   (__cdecl  *FN_GetBoneWorldPosition_t)    (int, float*, int16_t);
// FUN_00950580 — GetBoneScale. Sibling to FUN_00950490 with the same
// missing-NULL-guard bug on `*(objectPtr + 0x1c)`. Hook reimplements
// faithfully + adds the guard. Returns float (via ST0 in cdecl x86).
typedef float    (__cdecl  *FN_GetBoneScale_t)            (int objectPtr,
                                                             int16_t boneID);
// FUN_00b0fcd0 — GetCellPosition. Cell-position getter inside the scroll
// controller's per-frame nav update. Has NULL guards on `param_1` and
// `param_2` but NOT on `*param_1` (the cell's back-pointer to the
// controller). When navigation indexes past the cell array end, *param_1
// reads zero from uninit memory and the next deref crashes. Hook adds
// the inner NULL guard.
typedef void*    (__cdecl  *FN_GetCellPosition_t)         (int* cell,
                                                             void* outBuf);
// FUN_00950350 — SetNodePosition (same signature as FN_SetNodePosition).
// Trampoline declared for the Layer C investigation: a logging hook is
// installed at this address to locate the LEAGUE-slot renderer.
// Hook just observes (logs caller + node + pos) and forwards via the
// trampoline, so production behaviour is unchanged.
typedef void     (__cdecl  *FN_SetNodePosition_t)         (uint32_t nodeHandle,
                                                             float* posVec4);
// FUN_00950a00 — CreateChildNode trampoline. Same signature as
// `FN_CreateChildNode` above; declared separately because MinHook needs
// a writable function pointer. The hook in club_hooks_panel_v2.cpp is
// instrumentation only: logs caller/parent/behavior/result for every
// call whose return address falls in the panel-UI range and then
// forwards through this trampoline. Production behaviour is unchanged.
typedef uint32_t*(__cdecl  *FN_CreateChildNode_t)         (uint32_t parentNode,
                                                             char     attachDefaultBehavior);
typedef uint32_t*(__cdecl  *FN_CreateLeagueSelectionPanel_t)(uint32_t*, uint32_t*, float*);
// FUN_00b083b0 uses ESI as an implicit panel pointer and no stack args.
// Our hook fully replaces the original because its fixed 0x7614 trailer
// write overlaps slot 20 when the panel grows past 20 League slots.
typedef void     (__cdecl  *FN_BuildLeaguePanelVisualSlots_t)();
typedef void     (__cdecl  *FN_DestroyPanel_t)            (uint32_t*);
typedef uint32_t (__cdecl  *FN_PanelNavUpdate_t)          (uint32_t*);
// FUN_00b08b40 — the panel refresh callback (installed as the display
// sub-object's "vtable" in the panel creator's Step 2). Called as
// FUN_00b08b40(subObj) with subObj[0] = panel. See club_hooks_panel_refresh.cpp.
typedef uint32_t (__cdecl  *FN_PanelRefresh_t)            (void**);
typedef void     (__cdecl  *FN_SetTeamList_t)             (int, int, int);
typedef void     (__cdecl  *FN_LoadBothTeamKitData_t)    (int slotIndex);
typedef uint32_t (__cdecl  *FN_ExtractKitColor_t)        (char*, uint16_t, int, int, int);
typedef bool     (__cdecl  *FN_IsTeamKitEdited_t)        (uint16_t);
// FUN_00863570 — GetTeamName(teamId, nameType, param3): full/short team name.
typedef char*    (__cdecl  *FN_GetTeamName_t)           (uint16_t, int, int);
// FUN_00b3beb0 — CrestResolve(scratchId, teamId, p3, p4): loads a team's crest
// into a scratch texture id (0x7346..0x73ef) and returns the registry node; the
// caller copies the 32x32 8bpp palette+pixels into its cell struct then releases
// the scratch (FUN_00b3be00). A 0 result makes the cell draw the white flag.
typedef int      (__cdecl  *FN_CrestResolve_t)          (int, uint16_t, int, int);

// FUN_00969610 — LoadTeamKitDataBySlot. Inner per-side kit loader.
// Takes an implicit side-index parameter in EAX, plus two cdecl args.
// We fully reimplement this in club_hooks_kit_data.cpp (see comments
// there for the full faithful body), so the trampoline pointer below
// is declared only because MH_CreateHook wants somewhere to write the
// trampoline address — we never actually call through it.
typedef void     (__cdecl  *FN_LoadTeamKitDataBySlot_t)  (int slotIndex, int param_2);

// FUN_00968700 — Match_SetupKitData. Tiny per-frame dispatcher that fetches
// kit data and routes to the edited-vs-non-edited callee based on
// kitData[0x3a]. Hooked to add the same teamID > 200 guard the rest of
// the kit pipeline uses; for any team in the standard range we forward
// straight through to the original.
typedef void     (__cdecl  *FN_Match_SetupKitData_t)     (uint32_t param_1,
                                                            uint16_t param_2,
                                                            void*    param_3,
                                                            int      param_4);

// FUN_00968960 — Match_SetupKitData2. Sibling of FUN_00968700: same
// 4-arg cdecl signature, same kitData[0x3a] dispatch pattern, but routes
// to a different pair of callees (FUN_00968410 edited, FUN_009688e0
// non-edited). Same crash, same fix: teamID > 200 → skip.
typedef void     (__cdecl  *FN_Match_SetupKitData2_t)    (uint32_t param_1,
                                                            uint16_t param_2,
                                                            void*    param_3,
                                                            int      param_4);

extern FN_LoadClubTeamSlots_t        orig_LoadClubTeamSlots;
extern FN_LoadCompetitionMenuTeams_t orig_LoadCompetitionMenuTeams;
extern FN_InitClubSelectionScreen_t  orig_InitClubSelectionScreen;
extern FN_GetBoneWorldPosition_t     orig_GetBoneWorldPosition;
extern FN_GetBoneScale_t             orig_GetBoneScale;
extern FN_GetCellPosition_t          orig_GetCellPosition;
extern FN_SetNodePosition_t          orig_SetNodePosition;
extern FN_CreateChildNode_t          orig_CreateChildNode;
extern FN_CreateLeagueSelectionPanel_t orig_CreateLeagueSelectionPanel;
extern FN_BuildLeaguePanelVisualSlots_t orig_BuildLeaguePanelVisualSlots;
extern FN_DestroyPanel_t             orig_DestroyPanel;
extern FN_PanelNavUpdate_t           orig_PanelNavUpdate;
extern FN_PanelRefresh_t             orig_PanelRefresh;
extern FN_GetTeamName_t              orig_GetTeamName;
extern FN_CrestResolve_t             orig_CrestResolve;
// FUN_00b0f990 — club-selection badge render (ECX=teamId, EAX=nodeStruct). We
// hook it with a naked thunk to draw teams/<ID>.png for custom teams.
typedef void (*FN_BadgeRender_t)();
extern FN_BadgeRender_t              orig_BadgeRender;
// FUN_00b0fa80 — clean __cdecl sibling of the badge render (nodeStruct, teamId).
typedef void (__cdecl *FN_BadgeRenderCdecl_t)(int nodeStruct, int teamId);
extern FN_BadgeRenderCdecl_t         orig_BadgeRenderCdecl;
// FUN_009b0390 — teamId -> atlas slot (records the team for the shared setter).
typedef int (__cdecl *FN_ComputeBadgeSlot_t)(int state, uint16_t teamId);
extern FN_ComputeBadgeSlot_t         orig_ComputeBadgeSlot;
// FUN_00b0f8b0 — shared badge-texture setter (nodeStruct in EDI). Naked thunk.
typedef void (*FN_BadgeSet_t)();
extern FN_BadgeSet_t                 orig_BadgeSet;
extern FN_SetTeamList_t              orig_SetTeamList;
extern FN_LoadBothTeamKitData_t      orig_LoadBothTeamKitData;
extern FN_ExtractKitColor_t          orig_ExtractKitColor;
extern FN_IsTeamKitEdited_t          orig_IsTeamKitEdited;
extern FN_LoadTeamKitDataBySlot_t    orig_LoadTeamKitDataBySlot;
extern FN_Match_SetupKitData_t       orig_Match_SetupKitData;
extern FN_Match_SetupKitData2_t      orig_Match_SetupKitData2;

// FUN_00865240 — GetTeamKitData. The reverse-engineered chokepoint.
// Trampoline pointer is allocated only because MH_CreateHook needs
// somewhere to write it; our hook fully reimplements the function via
// RE_GetTeamKitData + TryLoadCustomKitData, and never calls back through
// the original. (Calling back would re-introduce the
// `KIT_FALLBACK_BUFFER` garbage path we are explicitly fixing.)
extern FN_GetTeamKitDataPtr          orig_GetTeamKitData;

// FUN_00865380 / FUN_00865430 — GetTeamKitDataB / GetTeamKitDataC.
// Sibling chokepoints into the per-team kit buffer, returning the
// extra-B (offset +0x100, stride 0x18) and extra-C (offset +0x160,
// stride 0x30) sub-records respectively. Same trampoline-pointer
// rationale as orig_GetTeamKitData: declared only so MinHook can
// write somewhere; the hooks reimplement both functions fully.
extern FN_GetTeamKitDataPtr          orig_GetTeamKitDataB;
extern FN_GetTeamKitDataPtr          orig_GetTeamKitDataC;

// FUN_00967170 — FillKitVariantSlot. Per-slot inner function called by
// BuildMatchKitDescriptors (FUN_00967d90). __cdecl with 4 stack args.
// We forward to the trampoline whenever the source kit-data pointer is
// non-NULL; for NULL pointers we mirror the dispatcher's skip-path
// output writes and return.
typedef void (__cdecl *FN_FillKitVariantSlot_t)(int slotIdx, int dst,
                                                  int src, int variantIdx);
extern "C" FN_FillKitVariantSlot_t   orig_FillKitVariantSlot;

// FUN_00966f70 — FillKitTeamSlot. Per-slot inner function for the +0/+4
// "team-data pointer" range of the match-kit context. __thiscall with
// ECX=slotIdx and two stack args (dst, src). The naked thunk converts
// to cdecl + tail-calls hook_FillKitTeamSlot_C; the trampoline pointer
// is opaque (void*) because the typed __thiscall pointer can't be
// represented cleanly as a free function in MSVC.
typedef void* FN_FillKitTeamSlot_t;
extern "C" FN_FillKitTeamSlot_t      orig_FillKitTeamSlot;

// =============================================================================
// SECTION 9 — Forward declarations for hook functions
//             (called cross-unit, e.g. InitClubSelectionScreen calls LoadClubTeamSlots)
// =============================================================================

void     __cdecl  hook_LoadClubTeamSlots       (int side);
void     __stdcall hook_LoadCompetitionMenuTeams();
void     __cdecl  hook_InitClubSelectionScreen ();
float*   __cdecl  hook_GetBoneWorldPosition    (int objectPtr, float* outVec4,
                                                 int16_t boneID);
// FUN_00950580 hook — sibling of hook_GetBoneWorldPosition. Adds the
// missing NULL guard on `*(objectPtr+0x1c)` (skeletonNode) before any
// `[skeletonNode + N]` deref. Returns the same default value the
// original returns when objectPtr itself is NULL (= _DAT_00b82ed4),
// matching original "no-bone-data" semantics.
float    __cdecl  hook_GetBoneScale            (int objectPtr, int16_t boneID);
// FUN_00b0fcd0 hook — cell-position getter. The original NULL-guards
// `param_1` and `param_2` but not `*param_1` (cell's back-pointer to
// scroll controller). When navigation indexes past the cell-array end,
// uninit memory at `param_1` reads zero for `*param_1` and the next
// `[*param_1 + 0x14]` deref crashes. Hook adds the inner guard and
// returns NULL the same way the existing param-NULL fallthrough does.
void*    __cdecl  hook_GetCellPosition         (int* cell, void* outBuf);

// FUN_00950350 hook — instrumentation logger for SetNodePosition.
// Logs (caller_pc, node_handle, posVec4) for every call whose return
// address falls in the panel UI subsystem range, then forwards to the
// original. Used to locate the LEAGUE-slot renderer (Layer C). Hook is
// install-toggleable by commenting/uncommenting its INSTALL_HOOK line
// in club_hooks_register.cpp; remove once the renderer is identified.
void     __cdecl  hook_SetNodePosition         (uint32_t nodeHandle,
                                                  float*   posVec4);
// FUN_00950a00 hook — instrumentation logger for CreateChildNode.
// Logs (caller_pc, parentNode, attachBehavior, returnedNode) for every
// call whose return address falls in the panel-UI range, then forwards
// to the original. Used in Layer D investigation to map which v2 Step 9
// children are actually distinct nodes vs aliased. Hook lives in
// club_hooks_panel_v2.cpp and is install-toggleable via its INSTALL_HOOK
// line in club_hooks_register.cpp.
uint32_t* __cdecl hook_CreateChildNode         (uint32_t parentNode,
                                                  char     attachDefaultBehavior);
uint32_t* __cdecl hook_CreateLeagueSelectionPanel(uint32_t* param_1,
                                                   uint32_t* param_2,
                                                   float*    param_3);
// V2 — clean rewrite of the panel creator with configurable Step 9
// (league-panel container loop) driven by `g_LeaguePanelCount` and
// per-page [panel] overrides from `g_PanelSlotLayouts`. Lives in
// `club_hooks_panel_v2.cpp`. Only one of v1 / v2 should be installed
// at a time.
uint32_t* __cdecl hook_CreateLeagueSelectionPanel_v2(uint32_t* param_1,
                                                      uint32_t* param_2,
                                                      float*    param_3);
void     __cdecl  hook_BuildLeaguePanelVisualSlots_C(uint32_t* panel);
void              hook_BuildLeaguePanelVisualSlots_Naked();
// FUN_00b08b40 full replacement — see club_hooks_panel_refresh.cpp. Fixes the
// selection cursor for league slots >= 20 (stock uses the fixed 20/side seat
// control-id table; we resolve the same id the logo uses and clamp the page
// node). subObj[0] is the panel pointer.
uint32_t __cdecl  hook_PanelRefresh           (void** subObj);
void     __cdecl  hook_DestroyPanel            (uint32_t* param_1);
uint32_t __cdecl  hook_PanelNavUpdate         (uint32_t* panel);
void     __cdecl  hook_SetTeamList            (int param_1, int param_2, int param_3);
void     __cdecl  hook_LoadBothTeamKitData   (int slotIndex);
uint32_t __cdecl  hook_ExtractKitColor       (char* outRGBA, uint16_t teamID,
                                               int kitVariant, int colorPartType,
                                               int sideIndex);
bool     __cdecl  hook_IsTeamKitEdited       (uint16_t teamID);
// FUN_00863570 hook — custom team name/short name from teams/<ID>.ini.
// See custom_team_loader.cpp.
char*    __cdecl  hook_GetTeamName           (uint16_t teamId, int nameType, int param3);
// FUN_00b3beb0 hook — custom team crest from teams/<ID>.png.
// See custom_team_loader.cpp.
int      __cdecl  hook_CrestResolve          (int scratchId, uint16_t teamId, int p3, int p4);

// FUN_00b0f990 hook — custom club badge from teams/<ID>.png. The naked thunk is
// what MinHook patches in; hook_BadgeRender_C is the body. See custom_team_loader.cpp.
extern "C" void          hook_BadgeRender_Naked();
extern "C" int  __cdecl  hook_BadgeRender_C  (int teamIdRaw, int nodeStruct, int param_1);
void     __cdecl  hook_BadgeRenderCdecl      (int nodeStruct, int teamId);
int      __cdecl  hook_ComputeBadgeSlot      (int state, uint16_t teamId);
extern "C" void          hook_BadgeSet_Naked();
extern "C" int  __cdecl  hook_BadgeSet_C     (int nodeStruct);

// FUN_00969610 hook — see club_hooks_kit_data.cpp.
// extern "C" because the naked thunk references the cdecl handler in inline
// asm and we need stable, unmangled symbol names for both ends. The naked
// thunk is what MinHook actually patches in; the C handler is the body.
extern "C" void __cdecl hook_LoadTeamKitDataBySlot       (int slotIndex,
                                                           int param_2,
                                                           int sideIndex);
extern "C" void         hook_LoadTeamKitDataBySlot_Naked ();

// FUN_00968700 hook — see club_hooks_kit_data.cpp.
void __cdecl hook_Match_SetupKitData  (uint32_t param_1, uint16_t param_2,
                                        void*    param_3, int      param_4);

// FUN_00968960 hook — see club_hooks_kit_data.cpp.
void __cdecl hook_Match_SetupKitData2 (uint32_t param_1, uint16_t param_2,
                                        void*    param_3, int      param_4);

// FUN_00865240 hook — see club_hooks_kit_data.cpp.
// Replaces the game's GetTeamKitData with a custom-config-aware
// reimplementation. Tries kits/<teamID>.ini first, then falls through
// to a faithful re-creation of the original (RE_GetTeamKitData) which
// returns NULL for unknown IDs instead of the garbage fallback buffer.
uint8_t* __cdecl hook_GetTeamKitData(uint16_t teamID, int variant);

// FUN_00865380 hook — see club_hooks_kit_data.cpp.
// Replaces GetTeamKitDataB (extra-B sub-record, stride 0x18).
// Same INI-first / RE_*-fallback pattern as hook_GetTeamKitData.
uint8_t* __cdecl hook_GetTeamKitDataB(uint16_t teamID, int variant);

// FUN_00865430 hook — see club_hooks_kit_data.cpp.
// Replaces GetTeamKitDataC (extra-C sub-record, stride 0x30, contains
// the kit-graphic resource ID byte at +0x03 that PreloadTeamKitGraphics
// dereferences). Custom IDs return a zeroed buffer so the preloader
// skips them; stock IDs use the faithful RE_GetTeamKitDataC.
uint8_t* __cdecl hook_GetTeamKitDataC(uint16_t teamID, int variant);

// FUN_00967170 hook — see club_hooks_kit_data.cpp. Adds the missing
// NULL guard on the source kit-data pointer; mirrors the dispatcher's
// skip-path output writes when source is NULL, otherwise forwards.
void __cdecl hook_FillKitVariantSlot(int slotIdx, int dst, int src,
                                       int variantIdx);

// FUN_00966f70 hook — see club_hooks_kit_data.cpp. Same NULL-guard
// shape as hook_FillKitVariantSlot but for the +0/+4 team-data slots,
// and bridged through a __declspec(naked) thunk to handle the original
// __thiscall calling convention.
extern "C" void __cdecl hook_FillKitTeamSlot_C(int dst, int src,
                                                 int slotIdx);
extern "C" void         hook_FillKitTeamSlot_Naked();

// =============================================================================
// SECTION 10 — Squad / player-record accessors
//
// Reverse-engineered chokepoints for the squad / player-record pipeline.
// All five are __cdecl. Signatures verified against Ghidra decompiles
// (see docs/INTERNALS.md, "Squad-data pipeline" section).
//
// The hooks live in src/hooks/club_hooks_squad_data.cpp. Custom-team
// support (teamIDs that the stock player-ID table does not cover) is
// the goal: an INI under `squad/<teamID>.ini` declares the 32 player
// IDs for the team, and `player/<id>.ini` declares the player record
// for any custom player ID. Without these hooks, custom teams resolve
// to slot=0 / NULL records and matches start unplayable.
// =============================================================================

// FUN_00862710 — GetTeamPlayerID. Master player-ID accessor; returns
// the player ID (u16) for (teamID, slot, mode). Mode is clamped to
// {0, 1, 2} and selects which sub-table to read. See RE notes for the
// full dispatch tree.
typedef uint32_t (__cdecl *FN_GetTeamPlayerID_t)(uint32_t teamID,
                                                   uint8_t  slotIdx,
                                                   int      mode);

// FUN_00861b90 — GetPlayerRecord (worker). The REAL chokepoint: the
// thin wrapper at 0x00861d20 has only ~15 callers (mostly save flows
// and the alias-copy path), but the worker is called directly by 20+
// per-field accessors at match time (jersey, name, stats, ...). So
// hooking the wrapper alone misses match-time reads — see history note.
//
// Calling convention is non-standard: ECX = playerID, EAX = bank flag,
// no stack arguments, returns the record pointer in EAX. We bridge it
// into a regular C handler via __declspec(naked) thunks (the same shape
// as hook_FillKitTeamSlot_Naked / hook_LoadTeamKitDataBySlot_Naked in
// club_hooks_kit_data.cpp). The trampoline pointer is therefore opaque
// — calling it as a C function would corrupt the stack — and call sites
// must go through CallOrigGetPlayerRecord() defined in squad_data.cpp.
typedef void* FN_GetPlayerRecord_t;

// FUN_00862c10 — GetTeamPlayerAttr. Returns one byte (squad number,
// position, etc.) for (teamID, slot, mode). Returns 0xFF for invalid
// inputs.
typedef uint8_t (__cdecl *FN_GetTeamPlayerAttr_t)(uint32_t teamID,
                                                    uint32_t slotIdx,
                                                    int      mode);

// FUN_00862200 — SetTeamPlayerAttr. Writes one byte to the same slot
// the getter reads from. 4-arg cdecl; the last arg is treated as a
// low-byte even though declared as 4 bytes (Ghidra: undefined4).
typedef void (__cdecl *FN_SetTeamPlayerAttr_t)(uint32_t teamID,
                                                 uint32_t slotIdx,
                                                 int      mode,
                                                 uint32_t value);

// FUN_00861ad0 — SetTeamPlayerID. Writes a u16 into the slot returned
// by GetTeamPlayerIDPtr_Mode0(teamID, slotIdx). 3-arg cdecl.
typedef void (__cdecl *FN_SetTeamPlayerID_t)(uint32_t teamID,
                                               uint8_t  slotIdx,
                                               uint16_t playerID);

extern FN_GetTeamPlayerID_t        orig_GetTeamPlayerID;
extern "C" FN_GetPlayerRecord_t    orig_GetPlayerRecord;     // opaque — see typedef
extern FN_GetTeamPlayerAttr_t      orig_GetTeamPlayerAttr;
extern FN_SetTeamPlayerAttr_t      orig_SetTeamPlayerAttr;
extern FN_SetTeamPlayerID_t        orig_SetTeamPlayerID;

// Hook forward declarations. Bodies live in club_hooks_squad_data.cpp.
uint32_t __cdecl hook_GetTeamPlayerID  (uint32_t teamID, uint8_t  slotIdx,
                                          int mode);
uint8_t  __cdecl hook_GetTeamPlayerAttr(uint32_t teamID, uint32_t slotIdx,
                                          int mode);
void     __cdecl hook_SetTeamPlayerAttr(uint32_t teamID, uint32_t slotIdx,
                                          int mode, uint32_t value);
void     __cdecl hook_SetTeamPlayerID  (uint32_t teamID, uint8_t  slotIdx,
                                          uint16_t playerID);

// hook_GetPlayerRecord — naked thunk that bridges the original's
// non-standard ECX=playerID/EAX=bank convention into a regular cdecl
// C handler (`hook_GetPlayerRecord_C`). MinHook patches the thunk in
// at FUN_00861b90; the C handler is what does the INI lookup.
extern "C" void* __cdecl hook_GetPlayerRecord_C(uint16_t playerID, int bank);
extern "C" void          hook_GetPlayerRecord_Naked();

// =============================================================================
// SECTION 11 — Leagues / panel-slot INI loader (club_hooks_leagues.cpp)
//
// `leagues/setup.ini` configures global panel layout (F0/F4 cols/rows,
// team-slot parse cap) and is read once during ClubHooks::Register().
// `leagues/<id>.ini` configures one panel slot — name, displayID, and
// team_<n> entries; any IDs >= MAX_PANEL_SLOTS bump the panel cap so
// the new slots fit without any code changes.
// =============================================================================

// Called from ClubHooks::Register() before any other slot/panel work.
// Reads leagues/setup.ini (if present), scans leagues/*.ini for the
// highest id, and updates MAX_PANEL_SLOTS / g_PanelF0_*/F4_* /
// g_LeagueTeamSlotMax accordingly.
void LoadLeaguesSetup();

// Called from hook_LoadClubTeamSlots after the original-shape loop
// finishes. For every leagues/<n>.ini that exists in [0, MAX_PANEL_SLOTS),
// loads it and writes the result into panel slot `n` via fn_SetTeamList,
// overriding whatever the original loop placed there.
void ApplyLeagueIniOverrides(int side);

// Per-slot panel-layout cache — populated from leagues/<n>.ini [panel].
// Today this is consumed only as a parsed-and-cached scaffold; the actual
// override into the scroll controller's per-cell positions (FUN_00b0fc60)
// is a follow-up. See docs/INTERNALS.md "Layer B" for the
// wire-up plan.
struct PanelSlotLayout
{
    bool  has_pos    = false;   // pos_x/y/z were set
    bool  has_scale  = false;   // `scale` (uniform) was set
    bool  has_scale2 = false;   // `scale_y` (separate y) was set
    bool  has_control_rect = false; // any control_* rect override was set
    bool  has_control_x = false;
    bool  has_control_y = false;
    bool  has_control_depth = false;
    bool  has_control_width = false;
    bool  has_control_height = false;
    float pos_x      = 0.0f;
    float pos_y      = 0.0f;
    float pos_z      = 0.0f;
    float scale_x    = 1.0f;
    float scale_y    = 1.0f;
    float control_x      = 0.0f;
    float control_y      = 0.0f;
    float control_depth  = 0.0f;
    float control_width  = 0.0f;
    float control_height = 0.0f;
};

// Returns nullptr when slot is out of range or has no [panel] override
// keys. Pointer is valid until the next LoadLeaguesSetup().
const PanelSlotLayout* GetLeaguePanelLayout(int slot);

// Layer B wire-up — called by hook_CreateLeagueSelectionPanel after the
// scroll controller is built. For every slot in [0, MAX_PANEL_SLOTS) that
// has a `has_pos` PanelSlotLayout in the cache, overrides the per-cell
// position via FUN_00b0fc60 (= fn_SetCellPosition). No-op for slots
// without an override; safe to call when no INI overrides exist.
void ApplyPanelLayoutOverrides(uint32_t* scrollCtrl);

// Owned by club_hooks_panel_v2.cpp. Clears sidecar pointers used for
// dynamic visual logo nodes beyond the 20 stock panel fields.
void ClearExtraLeagueVisualSlotNodes(uint32_t* panel);

// Owned by club_hooks_panel_v2.cpp. Extra logo quads live outside the
// stock panel[+0x98..] node array, so they do not inherit the stock page
// show/hide pass. Call this whenever panel[+0xF8] (current league page)
// can change.
void UpdateExtraLeagueVisualSlotVisibility(uint32_t* panel);

// Owned by club_hooks_panel_v2.cpp. Applies the leagues/<n>.ini
// control_x/y/width/height override for `slot` onto a world rect from
// GetRichControlWorldRect, using `parentNode` for the node base (so x/y act as
// node-relative offsets that inherit the away-side shift). Returns true if an
// override was applied. Shared by the logo builder and the selection cursor.
bool ApplyLeagueControlRectOverride(uint32_t parentNode, int slot, float* rect);

// Owned by club_hooks_panel_v2.cpp. Applies the leagues/<n>.ini control_depth
// override onto the cursor's "scale" value (GetBoneScale returns the control's
// Z-depth; SetItemScale writes the item's depth), using `parentNode` for the
// node Z base. Returns true if an override was applied.
bool ApplyLeagueControlDepthOverride(uint32_t parentNode, int slot, float* scale);

// Owned by custom_logo_loader.cpp. If `leagues/<slot>.png` exists, decode it,
// build a .txs-format texture blob under `displayId`, and register it into the
// resolver registry — once per id. Call just before ResolveDisplayTexture so a
// custom PNG overrides the (otherwise missing) baked graphic for that slot.
void EnsureCustomLeagueLogo(int slot, int displayId);

// Owned by custom_logo_loader.cpp. Stamps a synthetic display id into any panel
// slot that has a `leagues/<slot>.png` but no assigned logo id (-1), so the slot
// becomes populated and its PNG can draw. Call at the very start of the visual-
// slot build, before visibility / selectable-count passes read the ids.
void AssignSyntheticLogoIds(uint32_t* panel);

// Owned by custom_logo_loader.cpp. Releases all custom textures registered this
// screen and clears the caches so the next entry rebuilds them fresh (the stock
// texture unload never touches ours). Call at the start of the screen init.
void UnloadCustomLeagueLogos();

// Owned by custom_logo_loader.cpp. Decodes `pngPath`, builds a .txs-format
// texture blob and registers it under `displayId` (once per screen, tracked for
// unload). Returns true if a texture is available under displayId afterwards.
// Shared by league logos (leagues/<n>.png) and team emblems (teams/<id>.png).
bool RegisterCustomPng(int displayId, const wchar_t* pngPath);

// Owned by custom_logo_loader.cpp. For a team that has a teams/<id>.png, decode
// it once (forced to 32x32 8bpp, cached per team) and register a FRESH blob
// under `scratchId` — the transient crest slot FUN_00b3beb0 uses. Returns the
// registry node (as int) for the caller to hand back, or 0 to fall back to the
// stock loader. Not tracked for unload: the game releases the scratch per use.
int RegisterCustomCrest(int scratchId, int teamId);

// Owned by custom_logo_loader.cpp. Register teams/<id>.png (once per screen,
// tracked for unload like a league logo) and return its texture registry node
// for the club-selection badge nodes, or 0 if the team has no custom emblem.
int GetCustomEmblemNode(int teamId);
