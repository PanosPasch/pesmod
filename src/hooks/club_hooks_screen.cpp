// =============================================================================
// club_hooks_screen.cpp
//
// Hooks for:
//   FUN_009ee940 — InitClubSelectionScreen
//   FUN_00950490 — GetBoneWorldPosition (hook not installed by default)
//   FUN_00950350 — SetNodePosition (instrumentation; locate league renderer)
// =============================================================================

#include "club_hooks_common.h"
#include "../utils/logger.h"

#include <intrin.h>      // _ReturnAddress

FN_InitClubSelectionScreen_t orig_InitClubSelectionScreen = nullptr;
FN_GetBoneWorldPosition_t    orig_GetBoneWorldPosition    = nullptr;

static const int LEAGUE_LOGO_IDS[] = {
    0x4e9c, 0x4e9b, 0x4e98, 0x4e99, 0x4e9a,
    0x4e9d, 0x4e9e, 0x4e9f, 0x4ea0, 0x4ea1,
    0x4ea2, 0x4ea3, 0x4ea4, 0x4ea5, 0x4fc8, 0x4e31,
    0x4fd5, 0x4fd6, 0x4fd7, 0x4fd8, 0x4fd9, 0x4fda, 0x4fdb, 0x4fdc, 0x4fdd, 0x4fde, 0x4fdf, 0x4fe0, 0x4fe1, 0x4fe2, 0x4fe3
};
static const int LEAGUE_LOGO_COUNT = 31;
static uint32_t s_panelCapacity = 2;

// -----------------------------------------------------------------------------
// TransformShorts — inline reimplementation of FUN_00955320.
//
// FUN_00955320 (= fn_TransformNode) is NOT standard cdecl: it expects the
// output float buffer in EBX and the input-short count in EDI (Ghidra's
// decompile flags these as `unaff_EBX` / `unaff_EDI`). Calling it from C
// with cdecl ignores those registers, so the function writes 4 floats to
// whatever address EBX happened to hold at the call site — which crashes
// on screens where EBX isn't a writable address (e.g. EDI=4/EBX wild
// during per-frame UI nav on screens with no bone tables).
//
// Rather than write a __declspec(naked) thunk to set EBX/EDI properly,
// we inline the function's body: it's just `out[i] = (int16)in[i] *
// scale` for the first `count` slots, then unscaled `(float)(int16)in[i]`
// for any tail slots up to 4. _DAT_00b84970 is the fixed-point scale
// constant from the executable's data segment.
// -----------------------------------------------------------------------------
static const float kBoneShortToFloatScale =
    *reinterpret_cast<const float*>(0x00b84970);

static inline void TransformShorts(const int16_t* in, int count, float* out4)
{
    int i = 0;
    for (; i < count && i < 4; ++i)
        out4[i] = static_cast<float>(in[i]) * kBoneShortToFloatScale;
    for (; i < 4; ++i)
        out4[i] = static_cast<float>(in[i]);
}

// -----------------------------------------------------------------------------
// hook_GetBoneWorldPosition — replacement for FUN_00950490.
//
// Two changes vs. the original:
//
//   1. NULL-guard added on `skeletonNode = *(objectPtr + 0x1c)`. The
//      original crashes here when this pointer is NULL because it
//      unconditionally calls FUN_00955320(skeletonNode + 4), which then
//      does `MOVSX EDX, word ptr [ECX+EAX*2]` at 0x009553B0 with wild
//      ECX/EAX → access violation. The NULL case fires in extended-panel
//      scenarios (MAX_PANEL_SLOTS > 20): scrolling the league panel
//      beyond slot 19 invokes per-frame nav code that re-queries bone
//      positions for the focused slot via objects whose `+0x1c`
//      skeleton-table pointer is NULL.
//
//   2. fn_TransformNode call replaced with the inline TransformShorts
//      helper above. The original C decompile of FUN_00950490 hides the
//      fact that fn_TransformNode is non-cdecl (EBX = output, EDI =
//      count); calling it from a C cdecl wrapper writes outputs to a
//      garbage address and corrupts random memory on whatever screen
//      happens to be active when this hook fires.
// -----------------------------------------------------------------------------
float* __cdecl hook_GetBoneWorldPosition(int objectPtr,
                                          float* outVec4,
                                          int16_t boneID)
{
    if (objectPtr == 0 || outVec4 == nullptr) return nullptr;

    int skeletonNode = *reinterpret_cast<int*>(objectPtr + 0x1c);
    if (skeletonNode == 0) return nullptr;

    //Logger::Log("This is the skeletonNode: 0x%x ", skeletonNode);

    // First call equivalent: 2 scaled floats from skeleton's anchor shorts.
    // Original: MOV EDI, 0x2; LEA EBX, [ESP+0x2c]; CALL FUN_00955320.
    float skel[4];
    TransformShorts(reinterpret_cast<const int16_t*>(skeletonNode + 4),
                    2, skel);

    int entryList  = *reinterpret_cast<int*>    (skeletonNode + 0x14);
    int entryCount = *reinterpret_cast<uint8_t*>(skeletonNode + 0x2);
    if (entryCount == 0 || entryList == 0) return nullptr;

    int entryPtr = entryList;
    for (int i = 0; i < entryCount; i++, entryPtr += BONE_ENTRY_SIZE)
    {
        if (*reinterpret_cast<int16_t*>(entryPtr + 0x2) != boneID) continue;

        // Original: copies entry shorts into stack locals in this exact
        // order (x_lo, x_hi, y_lo, y_hi) then passes &local_28 to
        // FUN_00955320 with EDI=4 / EBX=local output buffer.
        int16_t in[4] = {
            *reinterpret_cast<int16_t*>(entryPtr + 0x4),
            *reinterpret_cast<int16_t*>(entryPtr + 0x6),
            *reinterpret_cast<int16_t*>(entryPtr + 0xa),
            *reinterpret_cast<int16_t*>(entryPtr + 0xc),
        };
        float entry[4];
        TransformShorts(in, 4, entry);

        float wx = *reinterpret_cast<float*>(objectPtr + 0x6c);
        float wy = *reinterpret_cast<float*>(objectPtr + 0x70);

        outVec4[0] = entry[0] + wx + skel[0];
        outVec4[1] = entry[1] + wy + skel[1];
        outVec4[2] = entry[2];
        outVec4[3] = entry[3];
        return outVec4;
    }
    return nullptr;
}

// -----------------------------------------------------------------------------
// hook_GetBoneScale — replacement for FUN_00950580.
//
// Sibling of FUN_00950490 with the same missing NULL guard pattern.
// The original checks `objectPtr == 0` and returns the default constant
// _DAT_00b82ed4, but does not check `*(objectPtr + 0x1c)` (skeletonNode)
// before reading `[skeletonNode + 2]` (entry count) and `[skeletonNode
// + 0x14]` (entry list). Crash 2b-B (2026-04-29) fires on this NULL
// deref when scrolling the league panel onto a slot whose render-node
// has no skeleton.
//
// Faithful reimplementation with the missing guard added. Returns
// _DAT_00b82ed4 in the NULL case — same value the function returns
// for `objectPtr == 0` and for "no matching bone found", so call sites
// already handle this fallthrough.
// -----------------------------------------------------------------------------
float __cdecl hook_GetBoneScale(int objectPtr, int16_t boneID)
{
    const float kBoneScaleDefault =
        *reinterpret_cast<const float*>(0x00b82ed4);

    if (objectPtr == 0) return kBoneScaleDefault;

    int skeletonNode = *reinterpret_cast<int*>(objectPtr + 0x1c);
    if (skeletonNode == 0) return kBoneScaleDefault;  // ← the missing guard

    int     entryList  = *reinterpret_cast<int*>    (skeletonNode + 0x14);
    uint8_t entryCount = *reinterpret_cast<uint8_t*>(skeletonNode + 0x2);

    int entryPtr = entryList;
    for (int i = 0; i < entryCount; ++i, entryPtr += BONE_ENTRY_SIZE)
    {
        if (boneID == *reinterpret_cast<int16_t*>(entryPtr + 0x2))
        {
            // Original returns:
            //   (float)((short)entry[+8] + (ushort)skeleton[+8])
            //   + *(float*)(objectPtr + 0x74)
            int32_t sum = static_cast<int32_t>(
                              *reinterpret_cast<int16_t*>(entryPtr + 0x8))
                        + static_cast<int32_t>(
                              *reinterpret_cast<uint16_t*>(skeletonNode + 0x8));
            return static_cast<float>(sum)
                 + *reinterpret_cast<float*>(objectPtr + 0x74);
        }
    }
    return kBoneScaleDefault;
}

// -----------------------------------------------------------------------------
// hook_GetCellPosition — replacement for FUN_00b0fcd0.
//
// Cell-position getter inside the scroll controller's per-frame nav
// update path. The original guards both params for NULL but does NOT
// guard `*param_1` (the cell's back-pointer to the controller). When
// navigation indexes past the cell-array end (slotIdx >= cellCount),
// `param_1` points to uninit memory where `*param_1` reads zero, and
// the subsequent `[*param_1 + 0x14]` AVs.
//
// Crash 1 (2026-04-29): EIP `0x00B0FD02` reading `[ecx+0x14]` with
// ECX=0 (= *param_1). Triggered by team-panel scroll-down passing
// slotIdx=21 when only 20 cells exist (4×5 grid).
//
// Faithful reimplementation; adds the inner guard. Returns NULL same
// way the existing param-NULL early-out does.
//
// Calls FUN_009519f0 (currently unhooked) via direct address — keeps
// the original's two-step:
//   1. write 4 floats into output buffer slot [0]/[1] from label-node
//      transform (param_2[0..1]).
//   2. fill output buffer slot [2]/[3] from the controller's offset
//      fields at [*param_1 + 0x14] / [*param_1 + 0x18].
// -----------------------------------------------------------------------------
typedef void (__cdecl *FN_LabelNodeTransform_t)(int labelNode, void* out4f);

void* __cdecl hook_GetCellPosition(int* cell, void* outBuf)
{
    if (cell == nullptr || outBuf == nullptr) return nullptr;

    // The missing inner guard: cell may be a valid pointer but pointing
    // past the end of the allocated cell array (slotIdx >= cellCount).
    // *cell is the back-pointer to the scroll controller; if the cell
    // was never initialised, this read returns zero.
    int* ctrl = reinterpret_cast<int*>(*cell);
    if (ctrl == nullptr) return nullptr;

    // FUN_009519f0(cell[1], &local_10) — writes the label render-node's
    // current transform into a 4-float local buffer. We can't easily
    // reproduce that without RE'ing FUN_009519f0, so call the original
    // function via its absolute address (untouched by our hooks).
    auto fn_LabelNodeTransform =
        reinterpret_cast<FN_LabelNodeTransform_t>(0x009519f0);

    float xform[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    fn_LabelNodeTransform(cell[1], xform);

    float* out = reinterpret_cast<float*>(outBuf);
    out[0] = xform[0];
    out[1] = xform[1];
    // [+0x14] / [+0x18] of the scroll controller are the per-page offset
    // floats `puVar1[5]` / `puVar1[6]` (decompile of FUN_00b0f270).
    out[2] = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ctrl) + 0x14);
    out[3] = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ctrl) + 0x18);
    return outBuf;
}

// -----------------------------------------------------------------------------
// hook_SetNodePosition — INSTRUMENTATION ONLY (Layer C investigation)
//
// Originally targeted FUN_00950350 (the "primary" position-setter), but
// the first instrumented log run (2026-04-30) showed only 6 calls during
// league-screen display — all from the panel-decoration loop in
// FUN_00b09050. Per-league-slot positioning never fired through 950350.
//
// Investigation led to FUN_00951330 — a SIBLING position-setter with
// identical signature (`void(int node, float* pos4)`) that writes to
// `node[+0x0C..+0x18]` and sets a dirty bit at `node[+0x11A]`. This is
// the function FUN_00b0fc60 uses for cell label/icon positioning, and
// it has ~80 callers including ~10 in the panel UI range (0x00b1xxxx).
// Hook RETARGETED to FUN_00951330 — same signature, just a different
// install address.
//
// Output rows look like:
//
//     [SetNodePos] caller=0x00b1XXXX node=0x12345678 pos=(  X.X,  Y.Y,  Z.Z, W.W)
//
// Procedure to identify the LEAGUE-slot renderer:
//   1. Build with this hook installed (its INSTALL_HOOK line in
//      `club_hooks_register.cpp` targets 0x00951330).
//   2. Run the game and open the league-selection screen with
//      MAX_PANEL_SLOTS > 20 (drop a `leagues/24.ini` to trigger).
//   3. Scroll the cursor through league slots. Watch the log.
//   4. Look for a caller address (probably in 0x00b1xxxx) that fires
//      multiple times per frame with positions varying per slot —
//      that's the renderer. Slots ≥ 20 will have pos = (0, 0, 0) or
//      repeat the same fallback ("smaller dots in the centre").
//   5. Decompile that caller in Ghidra to find the per-slot position
//      lookup. Hook it (or its callee) to read `g_PanelSlotLayouts`
//      from `club_hooks_leagues.cpp` and apply the [panel] override.
//   6. REMOVE this instrumentation (comment / delete the INSTALL_HOOK
//      line + this function) — FUN_00951330 fires every frame for
//      every UI element, so leaving it logged in production would
//      spam the log file.
//
// Filter rationale: the panel subsystem occupies roughly
// 0x00b00000..0x00b1ffff (verified against existing hook addresses:
// FUN_00b09050, FUN_00b07b00, FUN_00b0f270, etc.). Other FUN_00951330
// callers (in 0x006xxxx, 0x007xxxx, 0x008xxxx — match/replay/menu code)
// are filtered out. The cap (kMaxLogLines) keeps the log file usable;
// once hit, further calls are silently forwarded.
// -----------------------------------------------------------------------------
static const uintptr_t kPanelLoRange = 0x00b00000;
static const uintptr_t kPanelHiRange = 0x00b20000;
static const int       kMaxLogLines  = 800;
static int             g_setNodePosLogCount = 0;

void __cdecl hook_SetNodePosition(uint32_t nodeHandle, float* posVec4)
{
    uintptr_t retAddr = reinterpret_cast<uintptr_t>(_ReturnAddress());

    if (retAddr >= kPanelLoRange && retAddr < kPanelHiRange)
    {
        if (posVec4) {
            Logger::Log("[SetNodePos] caller=0x%08X node=0x%08X "
                        "pos=(%8.3f, %8.3f, %8.3f, %8.3f)",
                        static_cast<uint32_t>(retAddr),
                        nodeHandle,
                        posVec4[0], posVec4[1], posVec4[2], posVec4[3]);
        } else {
            Logger::Log("[SetNodePos] caller=0x%08X node=0x%08X pos=NULL",
                        static_cast<uint32_t>(retAddr), nodeHandle);
        }
        ++g_setNodePosLogCount;
        if (g_setNodePosLogCount == kMaxLogLines) {
            Logger::Log("[SetNodePos] log cap (%d lines) reached; further "
                        "calls silenced for the rest of this session.",
                        kMaxLogLines);
        }
    }

    if (orig_SetNodePosition)
        orig_SetNodePosition(nodeHandle, posVec4);
}

// -----------------------------------------------------------------------------
// hook_InitClubSelectionScreen
// -----------------------------------------------------------------------------
void __cdecl hook_InitClubSelectionScreen()
{
    Logger::Log("[ClubScreen] InitClubSelectionScreen. GameMode=%d", GAME_MODE_FLAG);

    // Step 1: Preload league logos in ML/Cup mode
    if (GAME_MODE_FLAG == 4 && fn_LoadLeagueLogo)
    {
        Logger::Log("[ClubScreen] Preloading %d league logos.", LEAGUE_LOGO_COUNT);
        for (int i = 0; i < LEAGUE_LOGO_COUNT; i++)
            fn_LoadLeagueLogo(LEAGUE_LOGO_SLOT_SIZE, LEAGUE_LOGO_IDS[i], LEAGUE_LOGO_FLAG);
    }

    // Step 2: Load UI resource in normal mode
    {
        int mode = fn_GetModeFlag ? fn_GetModeFlag() : 0;
        if (mode == 0 && fn_LoadUIResource)
            fn_LoadUIResource(0x12c0054);
    }

    // Step 3: Init display state
    if (fn_InitClubDisplayState) fn_InitClubDisplayState();

    // Step 4: Cursor positions
    uint32_t* cursorSide0;
    uint32_t* cursorSide1;
    if (TWO_PLAYER_MODE == 3)
    {
        cursorSide0 = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(CURSOR_SPLIT_VAL >> 8));
        cursorSide1 = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(CURSOR_SPLIT_VAL & 0xff));
    }
    else
    {
        cursorSide0 = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(CURSOR_SPLIT_VAL));
        cursorSide1 = cursorSide0;
    }

    // Step 5: Layout object node handle
    int layoutObjHandle = *reinterpret_cast<int*>(UI_LAYOUT_OBJ + 0xc);

    // Step 6: Side 0 panel
    {
        float posVec[4] = {};
        float scale = 0.0f;
        if (fn_GetBoneWorldPos) fn_GetBoneWorldPos(layoutObjHandle, posVec, PANEL_BONE_ID_SIDE0);
        if (fn_GetBoneScale)    scale = fn_GetBoneScale(layoutObjHandle, PANEL_BONE_ID_SIDE0);

        float panelPos[4] = { posVec[0], posVec[1], posVec[2], scale };
        Logger::Log("[ClubScreen] Panel 0 Drawn at: %f %f %f %f", panelPos[0], panelPos[1], panelPos[2], panelPos[3]);
        //Panos code
        // panelPos[0]=-200.0f;
        // panelPos[1]=-200.0f;
        // panelPos[2]=1.0f;
        // panelPos[3]=0.0f;

        if (fn_CreateDisplayPanel)
        {
            uint32_t* rawHandle = fn_CreateDisplayPanel(
                reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(s_panelCapacity)), 
                cursorSide1, 
                panelPos);
            // uint32_t* rawHandle =   orig_CreateLeagueSelectionPanel(
            //     reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(s_panelCapacity)), 
            //     cursorSide1, 
            //     panelPos);
            DISPLAY_HANDLE_ARR[0] = reinterpret_cast<int32_t>(rawHandle);
            Logger::Log("[ClubScreen] Side 0 panel: handle=0x%08X", (uintptr_t)rawHandle);

            if (fn_ShowPanel && rawHandle)
                fn_ShowPanel(reinterpret_cast<int>(rawHandle), 0);
        }

        hook_LoadClubTeamSlots(0);
    }

    // Step 7: Side 1 panel
    {
        float posVec[4] = {};
        float scale = 0.0f;
        if (fn_GetBoneWorldPos) fn_GetBoneWorldPos(layoutObjHandle, posVec, PANEL_BONE_ID_SIDE1);
        if (fn_GetBoneScale)    scale = fn_GetBoneScale(layoutObjHandle, PANEL_BONE_ID_SIDE1);

        float panelPos[4] = { posVec[0], posVec[1], posVec[2], scale };
        Logger::Log("[ClubScreen] Panel 1 Drawn at: %f %f %f %f", panelPos[0], panelPos[1], panelPos[2], panelPos[3]);
        if (fn_CreateDisplayPanel)
        {
            uint32_t* rawHandle = fn_CreateDisplayPanel(
                reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(s_panelCapacity)),
                cursorSide0, 
                panelPos);
            // uint32_t* rawHandle = orig_CreateLeagueSelectionPanel(
            //     reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(s_panelCapacity)),
            //     cursorSide0, 
            //     panelPos);
            DISPLAY_HANDLE_ARR[1] = reinterpret_cast<int32_t>(rawHandle);
            Logger::Log("[ClubScreen] Side 1 panel: handle=0x%08X", (uintptr_t)rawHandle);

            if (fn_ShowPanel && rawHandle)
            {
                fn_ShowPanel(reinterpret_cast<int>(rawHandle), 0);
                if (TWO_PLAYER_MODE == 3)
                    fn_ShowPanel(reinterpret_cast<int>(rawHandle), 0);
            }
        }

        hook_LoadClubTeamSlots(1);
    }

    // Step 8: ML/Cup player state
    {
        int mode = fn_GetModeFlag ? fn_GetModeFlag() : 0;
        if (mode != 0)
        {
            Logger::Log("[ClubScreen] ML/Cup: player state setup.");
            int ps = fn_GetPlayerState ? fn_GetPlayerState(0) : 0;
            if (fn_InitPlayerState && ps) fn_InitPlayerState(ps);
            if (fn_ResetPlayerSelection)  fn_ResetPlayerSelection(0);
            if (fn_SetPlayerActive)       fn_SetPlayerActive(1);
            if (fn_TimeGetTime)           PLAYER_TIMER_BASE = fn_TimeGetTime();
            if (fn_SetPlayerInputMode)    fn_SetPlayerInputMode(1);
            if (fn_SetPlayerCursorType)   fn_SetPlayerCursorType('\x01');
            if (fn_SetNavContext)          fn_SetNavContext(8);
            if (fn_LinkDisplayObjects)    fn_LinkDisplayObjects(DISPLAY_LINK_A, DISPLAY_LINK_B, 2);
        }
    }

    // Step 9: ML extra init
    if (MODE_FLAG == ML_CLUB_SCREEN_MODE && fn_MLClubScreenInit)
        fn_MLClubScreenInit(0);

    // Step 10: Reset selection state, nav mappings
    *reinterpret_cast<int32_t*>(0x03a7b8ec) = 0;
    *reinterpret_cast<int32_t*>(0x03a7b8e8) = 0;
    if (fn_SetupNavMapping)
    {
        fn_SetupNavMapping(NAV_TABLE_BASE, 4);
        fn_SetupNavMapping(NAV_TABLE_BASE, 8);
    }

    Logger::Log("[ClubScreen] InitClubSelectionScreen complete.");
}
