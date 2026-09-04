// =============================================================================
// club_hooks_panel_v2.cpp — clean rewrite of hook_CreateLeagueSelectionPanel
//
// This is a Reorganised + commented + extended replacement for the
// hook in `club_hooks_panel.cpp`. The original file is kept as a
// historical reference; only ONE of the two should be installed at a
// time (controlled in `club_hooks_register.cpp`).
//
// What's different from v1
// ------------------------
//
//   1. Step 9 ("league-panel container loop") is now configurable:
//      iterates `g_LeaguePanelCount` times instead of a hardcoded 3.
//      The first 3 iterations match stock behaviour exactly (same
//      texture from TABLE_NODE_DATA[side*3 + i], same anchor position).
//      Iterations 3+ create extra parent panels for league slots 20+
//      using the [panel] pos_x/y/z keys cached in `g_PanelSlotLayouts`.
//
//   2. Per-page [panel] override: each parent panel (page) optionally
//      reads its position from `leagues/<firstSlotOfPage>.ini`'s
//      [panel] section. Slot N belongs to page floor(N / 8); the page's
//      anchor is sourced from slot (page * 8). E.g. page 3 → slot 24,
//      page 4 → slot 32. Drop pos_x/y/z into `leagues/24.ini` to
//      position the 4th container.
//
//   3. The function body is reorganised into named sections with helper
//      inlines, no logging clutter, and section comments that explain
//      the structural intent rather than just the ASM.
//
//   4. Compatibility: identical behaviour for the stock 20-slot case
//      (`g_LeaguePanelCount == 3`, no [panel] overrides). Backward-
//      compatible with all in-place hooks (DestroyPanel, PanelNavUpdate,
//      etc.) — same panel struct layout, same trailer offsets.
//
// What's NOT changed
// ------------------
// All other steps (1-8, 10-18) are byte-identical to the original
// hook in `club_hooks_panel.cpp`. The TEAM-grid, scroll list, scroll
// controller, bone attachments, display items, and trailer mirror are
// all unchanged. Only Step 9 has new logic.
//
// See docs/INTERNALS.md "Layer C" for the architecture
// background and docs/INTERNALS.md for the live status
// of the league-slot positioning effort.
// =============================================================================

#include "club_hooks_common.h"
#include "../utils/logger.h"

#include <intrin.h>      // _ReturnAddress for the CreateChildNode hook
#include <vector>

// We share the trampoline pointer with v1 — only one of them is
// installed via INSTALL_HOOK. The pointer is defined in
// club_hooks_register.cpp.
extern FN_CreateLeagueSelectionPanel_t orig_CreateLeagueSelectionPanel;

// Trampoline for the CreateChildNode instrumentation hook. Defined here
// (alongside the hook body) rather than in club_hooks_register.cpp so
// the hook + its state stay in one translation unit.
FN_CreateChildNode_t orig_CreateChildNode = nullptr;

// =============================================================================
// Local helpers
// =============================================================================

namespace {

// Stock constants from the original FUN_00b09050.
constexpr uint32_t kSubObjVTable = 0x00B08B40;
constexpr uint32_t kSubObjString = 0x00BB5DB0;
constexpr int      kRenderNodeAssetId = 0x10385;

inline uint32_t ReadDword(uintptr_t addr)
{
    return *reinterpret_cast<uint32_t*>(addr);
}
inline uint8_t  ReadByte(uintptr_t addr)
{
    return *reinterpret_cast<uint8_t*>(addr);
}
inline float    ReadFloat(uintptr_t addr)
{
    return *reinterpret_cast<float*>(addr);
}

inline int TrailerOffsetA(int slotCount)
{
    return PANEL_ORIG_TRAILER_A + (slotCount - 20) * PANEL_SLOT_STRIDE;
}
inline int TrailerOffsetB(int slotCount)
{
    return PanelTrailerB_ByteOff(slotCount);
}

// Apply the standard "background panel" node-prop pattern.
// Mirrors the original Step 8 block (texB) when isOpaqueLayer == true,
// and the Step 9 inner block otherwise.
inline void ConfigureBackgroundChildNode(uint32_t child,
                                         uint32_t texID,
                                         int      propC)
{
    if (fn_SetNodeTexture) fn_SetNodeTexture(child, texID);
    if (fn_NodePropA)      fn_NodePropA(child, 0);
    if (fn_NodePropC)      fn_NodePropC(child, propC);
    if (fn_NodePropB)      fn_NodePropB(child, 0);
    if (fn_NodePropD)      fn_NodePropD(child, 0);
}

// Build a 4-float position vector. We always supply (x, y, z, w).
// `w` defaults to 1.0 (homogeneous coord), z is the panel's z-depth.
inline void BuildPosVec(float x, float y, float z, float w, float out[4])
{
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

// Returns the [panel] override position for slot 0 of `page`, or
// `nullptr` if no override exists. Page 0..2 are the stock pages
// (slots 0/8/16); page 3+ map to slots 24/32/...
const PanelSlotLayout* GetPageOverride(int page)
{
    int firstSlotOfPage = page * SLOTS_PER_LEAGUE_PAGE;
    return GetLeaguePanelLayout(firstSlotOfPage);
}

}  // namespace


// =============================================================================
// hook_CreateLeagueSelectionPanel_v2 — replacement for FUN_00b09050.
// =============================================================================
uint32_t* __cdecl hook_CreateLeagueSelectionPanel_v2(uint32_t* param_1,
                                                      uint32_t* param_2,
                                                      float*    param_3)
{
    // ── Resolve side flag ────────────────────────────────────────────────
    // Original: if param_1 == NULL the function falls back to side=2.
    int side;
    {
        uintptr_t sideRaw = reinterpret_cast<uintptr_t>(param_1);
        if (sideRaw == 0) sideRaw = 2;
        side = static_cast<int>(sideRaw);
    }

    // We only handle sides 0/1/2 — anything else falls through to the
    // game's original (preserves correctness for unexpected callers).
    if (side < 0 || side > 2)
    {
        Logger::Log("[PanelV2] Unexpected side=%d, forwarding to original.",
                    side);
        return orig_CreateLeagueSelectionPanel
            ? orig_CreateLeagueSelectionPanel(param_1, param_2, param_3)
            : nullptr;
    }

    Logger::Log("[PanelV2] side=%d slots=%d panel_count=%d",
                side, MAX_PANEL_SLOTS, g_LeaguePanelCount);

    // ── Step 1: Allocate panel memory ────────────────────────────────────
    int allocSize = PANEL_BASE_ALLOC
                  + (MAX_PANEL_SLOTS - 20) * PANEL_SLOT_STRIDE;

    uint32_t* panel = fn_GameAlloc ? fn_GameAlloc(1, allocSize) : nullptr;
    if (!panel)
    {
        Logger::Log("[PanelV2] alloc failed (%d bytes)", allocSize);
        return nullptr;
    }
    std::memset(panel, 0, allocSize);

    auto baseB = [&](int off) -> uint8_t* {
        return reinterpret_cast<uint8_t*>(panel) + off;
    };
    auto DW = [&](int off) -> uint32_t& {
        return *reinterpret_cast<uint32_t*>(baseB(off));
    };
    auto F  = [&](int off) -> float& {
        return *reinterpret_cast<float*>(baseB(off));
    };
    auto BY = [&](int off) -> uint8_t& {
        return *reinterpret_cast<uint8_t*>(baseB(off));
    };

    int trailerA = TrailerOffsetA(MAX_PANEL_SLOTS);
    int trailerB = TrailerOffsetB(MAX_PANEL_SLOTS);

    // ── Step 2: Display sub-object ───────────────────────────────────────
    // ASM passes 6 args; the typedef in common.h lists 5. Cast to the
    // 6-arg signature for correctness (extra arg = vtable string).
    using FN_CreateSubObj6 = uint32_t* (__cdecl*)(int, int, int, int, int, int);
    FN_CreateSubObj6 fn_CreateSubObj6 = reinterpret_cast<FN_CreateSubObj6>(
        reinterpret_cast<void*>(fn_CreateDisplaySubObject));

    uint32_t* subObj = fn_CreateSubObj6
        ? fn_CreateSubObj6(0, kSubObjVTable, 0, 4, 0, kSubObjString)
        : nullptr;

    DW(trailerB) = reinterpret_cast<uint32_t>(subObj);
    if (fn_InitDisplaySubObject && subObj)
        fn_InitDisplaySubObject(reinterpret_cast<int>(subObj));

    // ── Step 3: Slot data initialisation (TeamSlotBuffer per slot) ───────
    // For each of MAX_PANEL_SLOTS slots: zero teamIDs[20] (with 0xFFFF
    // sentinel), write the name-string sentinel, and set displayID =
    // 0xFFFFFFFF.
    {
        uint32_t nameSentinel = ReadDword(0x00BB5DA8);
        uint8_t  flagByte     = ReadByte (0x00BB5DAC);

        uint8_t* namePtr = baseB(0x19E4);
        for (int i = 0; i < MAX_PANEL_SLOTS; ++i)
        {
            uint32_t* teamIDs = reinterpret_cast<uint32_t*>(namePtr - 0xA0);
            for (int j = 0; j < TEAM_SLOT_COUNT; ++j)
                teamIDs[j] = 0xFFFF;

            *reinterpret_cast<uint32_t*>(namePtr)         = nameSentinel;
            namePtr[4]                                    = flagByte;
            *reinterpret_cast<uint32_t*>(namePtr + 0x400) = 0xFFFFFFFF;

            namePtr += PANEL_SLOT_STRIDE;
        }
    }

    // ── Step 4: Header fields (state flags) ──────────────────────────────
    DW(0x0000) = reinterpret_cast<uint32_t>(param_2);
    DW(0x0040) = 0;
    DW(0x0044) = 1;
    DW(0x00E8) = 1;
    DW(0x1900) = 1;
    DW(0x1904) = 1;
    DW(0x1908) = 0;
    DW(0x190C) = 0;        // mode flag (0 = league sel, !=0 = team sel)
    DW(0x1918) = 1;
    DW(0x1930) = 1;
    DW(0x1940) = 0;        // current cursor index (P1/P2)
    DW(trailerA) = 0;

    // ── Step 5: Team grid cols/rows (DW(0x00F0)/DW(0x00F4)) ──────────────
    // Stock: side==0 → 5×4, side==1/2 → 4×5. Both = 20 cells. The grid
    // belongs to the TEAM-selection screen (Screen 3); the league screen
    // doesn't read these directly. Exposed via setup.ini for moddability.
    if (side == 0)
    {
        DW(0x00F0) = static_cast<uint32_t>(g_PanelF0_Side0);
        DW(0x00F4) = static_cast<uint32_t>(g_PanelF4_Side0);
    }
    else
    {
        DW(0x00F0) = static_cast<uint32_t>(g_PanelF0_Side1);
        DW(0x00F4) = static_cast<uint32_t>(g_PanelF4_Side1);
    }

    constexpr float kLocalBounds = 36.0f;
    float localBoundsA = kLocalBounds;
    float localBoundsB = kLocalBounds;

    // ── Step 6: Side + z-depth ───────────────────────────────────────────
    DW(0x00EC) = static_cast<uint32_t>(side);
    DW(0x003C) = *reinterpret_cast<uint32_t*>(&param_3[2]);

    // ── Step 7: Main render node ─────────────────────────────────────────
    uint32_t mainNode = fn_CreateRenderNode
        ? fn_CreateRenderNode(kRenderNodeAssetId)
        : 0;
    DW(0x0068) = mainNode;

    // ── Step 8: Primary texture child (childA at panel[+0x84]) ───────────
    {
        uint32_t texA   = ReadDword(TABLE_TEXTURE_A + side * 4);
        uint32_t childA = fn_GetChildNode ? fn_GetChildNode(mainNode) : 0;
        ConfigureBackgroundChildNode(childA, texA, /*propC=*/2);
        // Stock value: NodePropB = 1 here (Step 8 is the only one that
        // sets propB=1; Steps 9 & 10 use 0).
        if (fn_NodePropB) fn_NodePropB(childA, 1);
        DW(0x0084) = childA;
    }

    // =========================================================================
    // Step 9 — League-panel container loop (THIS IS WHERE V2 DIFFERS)
    // =========================================================================
    //
    // For each of `g_LeaguePanelCount` iterations we:
    //   (a) read a texture ID from TABLE_NODE_DATA[side*3 + i] (stock
    //       data only covers i in [0,3); for i >= 3 we read the wrap-
    //       around / fallback — see below).
    //   (b) if the texture isn't 0xFFFFFFFF, create a child of the main
    //       render node, apply the standard background-child props, and
    //       store the handle in panel[+0x6C + i*4].
    //   (c) compute the parent-panel anchor position. Iterations 0..2
    //       use the original behaviour (param_3 anchor); iterations 3+
    //       look up `GetPageOverride(i)` (= leagues/<i*8>.ini's [panel]
    //       pos_x/y/z) and use those when available, falling back to
    //       the param_3 anchor when not.
    //   (d) call fn_SetNodePosition with the resolved position.
    //
    // The resulting parent panels are what the cursor anchors off when
    // navigating to a league slot. With g_LeaguePanelCount auto-grown
    // by LoadLeaguesSetup() to ceil(MAX_PANEL_SLOTS / 8), every slot
    // has a parent — slots 20+ are no longer rendered at the screen
    // centre.
    //
    // NOTE: panel[+0x6C], [+0x70], [+0x74] are the stock 3 slots in the
    // panel struct. Iterations 3+ overshoot that range and write into
    // [+0x78], [+0x7C]... — these bytes are within the panel allocation
    // (sized PANEL_BASE_ALLOC = 0x7620 + per-slot extension) and are
    // unused by stock code, so reusing them is safe. The destructor
    // (FUN_00b0ac50) only iterates the original 3 slots.
    // =========================================================================
    {
        uint32_t* tablePtr = reinterpret_cast<uint32_t*>(
            TABLE_NODE_DATA + side * 12);
        uint32_t* nodeSlotDW = reinterpret_cast<uint32_t*>(baseB(0x6C));

        const int panelCount = g_LeaguePanelCount;
        for (int i = 0; i < panelCount; ++i)
        {
            // (a) Texture ID. Stock table has 3 entries per side; for
            //     extras we reuse the LAST stock entry (index 2). This
            //     means the 4th+ panels visually look like the 3rd
            //     stock panel, which is usually a small "ML" / "Misc"
            //     background. Users who want a different look can patch
            //     TABLE_NODE_DATA at runtime; for now reuse keeps it
            //     simple.
            const int tableIdx = (i < 3) ? i : 2;
            const uint32_t texID = tablePtr[tableIdx];

            uint32_t child = 0;
            if (texID != 0xFFFFFFFF)
            {
                child = fn_GetChildNode ? fn_GetChildNode(mainNode) : 0;
                ConfigureBackgroundChildNode(child, texID, /*propC=*/0);
                nodeSlotDW[i] = child;
            }

            // (b) Position resolution.
            //     Default: panel anchor (param_3[0..1], DW(0x003C), 1.0).
            //     Override: GetPageOverride(i) if non-null AND has_pos.
            float posX = param_3[0];
            float posY = param_3[1];
            float posZ;
            // Z is read as raw uint32 from DW(0x003C) — preserves any
            // non-finite bit pattern the original may have used.
            *reinterpret_cast<uint32_t*>(&posZ) = DW(0x003C);

            const PanelSlotLayout* override_ = GetPageOverride(i);
            if (override_ && override_->has_pos)
            {
                posX = override_->pos_x;
                posY = override_->pos_y;
                posZ = override_->pos_z;
                Logger::Log("[PanelV2] Step 9 panel %d: override "
                            "(%.3f, %.3f, %.3f) from leagues/%d.ini",
                            i, posX, posY, posZ,
                            i * SLOTS_PER_LEAGUE_PAGE);
            }

            float posVec[4];
            BuildPosVec(posX, posY, posZ, 1.0f, posVec);

            if (fn_SetNodePosition && nodeSlotDW[i] != 0)
                fn_SetNodePosition(nodeSlotDW[i], posVec);
        }
    }

    // ── Step 10: Secondary render node (childB at panel[+0x88]) ──────────
    {
        uint32_t texB   = ReadDword(TABLE_TEXTURE_B + side * 4);
        uint32_t childB = fn_GetChildNode ? fn_GetChildNode(mainNode) : 0;
        ConfigureBackgroundChildNode(childB, texB, /*propC=*/2);
        DW(0x0088) = childB;

        // ASM detail: this call passes param_3 directly (caller's pos
        // vec, all 4 floats). We mirror that exactly.
        if (fn_SetNodePosition) fn_SetNodePosition(childB, param_3);
    }

    // ── Step 11: Bone attachment from TABLE_TEXTURE_C[side] ──────────────
    {
        int32_t animID = *reinterpret_cast<int32_t*>(
            TABLE_TEXTURE_C + side * 4);
        uint32_t* boneAttach = nullptr;
        if (animID >= 0)
        {
            uint32_t bh = fn_LookupBone ? fn_LookupBone(DW(0x0088), animID) : 0;
            int*     bp = fn_ResolveBone ? fn_ResolveBone(bh) : nullptr;
            if (bp)
                boneAttach = fn_AttachBone
                    ? fn_AttachBone(DW(0x0088), bp, animID) : nullptr;
        }
        DW(0x008C) = reinterpret_cast<uint32_t>(boneAttach);
        if (fn_SetBoneVisibility && boneAttach)
            fn_SetBoneVisibility(boneAttach[3], 0);
    }

    // ── Step 12: Scroll list from bone A (L1/R1 side-button widget) ──────
    {
        float localVec[4] = {};
        int16_t boneIDA = *reinterpret_cast<int16_t*>(
            TABLE_BONE_ID_A + side * 4);
        if (fn_GetBoneWorldPos)
            fn_GetBoneWorldPos(static_cast<int>(DW(0x0088)),
                               localVec, boneIDA);

        uint32_t scrollCount = (side == 0) ? 10 : 2;
        uint32_t* scrollList = fn_CreateScrollList
            ? fn_CreateScrollList(scrollCount,
                                  reinterpret_cast<int>(localVec))
            : nullptr;
        DW(0x0064) = reinterpret_cast<uint32_t>(scrollList);

        const float kFloatConst_B7D6BC = ReadFloat(0x00B7D6BC);

        if (fn_SetScrollScale && scrollList)
            fn_SetScrollScale(reinterpret_cast<int>(scrollList),
                              F(0x003C) - kFloatConst_B7D6BC);
        if (fn_SetScrollFlag && scrollList)
            fn_SetScrollFlag(reinterpret_cast<int>(scrollList), '\0');
    }

    // ── Step 13: Side==0 extra bone attachment (anim ID 9) ───────────────
    if (side == 0)
    {
        uint32_t* ba2 = nullptr;
        uint32_t bh2  = fn_LookupBone ? fn_LookupBone(DW(0x0088), 9) : 0;
        int*     bp2  = fn_ResolveBone ? fn_ResolveBone(bh2)         : nullptr;
        if (bp2)
            ba2 = fn_AttachBone ? fn_AttachBone(DW(0x0088), bp2, 9) : nullptr;
        DW(0x0090) = reinterpret_cast<uint32_t>(ba2);
        if (fn_SetBoneVisibility && ba2)
            fn_SetBoneVisibility(ba2[3], 0);
    }

    // ── Step 14: Finalise panel layout ───────────────────────────────────
    if (fn_FinalisePanelLayout) fn_FinalisePanelLayout(panel);

    // ── Step 15: Pagination scroll controller (TEAM-grid container) ──────
    {
        float localVec[4] = {};
        int16_t boneIDB = *reinterpret_cast<int16_t*>(
            TABLE_BONE_ID_B + side * 4);
        if (fn_GetBoneWorldPos)
            fn_GetBoneWorldPos(static_cast<int>(DW(0x0088)),
                               localVec, boneIDB);

        uint32_t* scrollCtrl = fn_CreateScrollController
            ? fn_CreateScrollController(2, localVec, DW(0x00F0), DW(0x00F4))
            : nullptr;
        DW(0x0054) = reinterpret_cast<uint32_t>(scrollCtrl);

        if (fn_SetScrollCtrlScale && scrollCtrl)
            fn_SetScrollCtrlScale(reinterpret_cast<int*>(scrollCtrl),
                                  F(0x003C));
        if (fn_LinkScrollCtrlToSlots && scrollCtrl)
            fn_LinkScrollCtrlToSlots(reinterpret_cast<int*>(scrollCtrl),
                                     reinterpret_cast<int>(baseB(0x1944)),
                                     MAX_PANEL_SLOTS);
        if (fn_SetScrollBounds && scrollCtrl)
            fn_SetScrollBounds(reinterpret_cast<int*>(scrollCtrl),
                               localBoundsB, localBoundsA);
    }

    // ── Step 16: Colour / alpha bytes ────────────────────────────────────
    BY(0x4C) = 0x20; BY(0x4D) = 0x20; BY(0x4E) = 0x20;
    BY(0x4F) = 0xFF; BY(0x50) = 0xFF; BY(0x51) = 0xFF;
    BY(0x52) = 0xFF; BY(0x53) = 0xFF;

    // ── Step 17: Display items (panel[+0x58, +0x5C, +0x60]) ──────────────
    {
        const float kFloatConst_B7D6BC = ReadFloat(0x00B7D6BC);
        uint32_t* itemSlotPtr = reinterpret_cast<uint32_t*>(baseB(0x58));

        for (int i = 0; i < 2; ++i)
        {
            uint32_t* item = fn_CreateDisplayItem
                ? fn_CreateDisplayItem(8) : nullptr;
            itemSlotPtr[i] = reinterpret_cast<uint32_t>(item);
            if (fn_InitDisplayItem && item) fn_InitDisplayItem(item, 0);
        }

        uint32_t* item3 = fn_CreateDisplayItem
            ? fn_CreateDisplayItem(8) : nullptr;
        DW(0x0060) = reinterpret_cast<uint32_t>(item3);
        if (fn_SetItemScale && item3)
            fn_SetItemScale(reinterpret_cast<int>(item3),
                            F(0x003C) + kFloatConst_B7D6BC);
        if (fn_InitDisplayItem && item3) fn_InitDisplayItem(item3, 0);
    }

    // ── Step 18: Register subObj's panel back-pointer ────────────────────
    if (fn_GetPanelRegistrySlot && subObj)
    {
        uint32_t* slot = fn_GetPanelRegistrySlot(subObj);
        if (slot) *slot = reinterpret_cast<uint32_t>(panel);
    }

    // ── Final trailer mirror (when MAX_PANEL_SLOTS > 20) ─────────────────
    // Same rationale as v1: write the subObj pointer at BOTH the true
    // (shifted) trailer offset AND the original fixed offset 0x761C, so
    // unhooked code paths that read 0x761C still see a valid pointer.
    if (MAX_PANEL_SLOTS != 20)
    {
        DW(PANEL_ORIG_TRAILER_B) = reinterpret_cast<uint32_t>(subObj);
    }

    Logger::Log("[PanelV2] complete handle=0x%08X (side=%d, slots=%d, "
                "panels=%d)",
                reinterpret_cast<uintptr_t>(panel),
                side, MAX_PANEL_SLOTS, g_LeaguePanelCount);
    return panel;
}

// =============================================================================
// hook_BuildLeaguePanelVisualSlots — dynamic logo/control binding.
//
// Stock FUN_00b083b0 is entered with the panel pointer in ESI, not via a
// normal stack argument. The naked thunk below preserves that convention
// but does NOT call the original anymore: the stock tail writes its League
// selectable-count to fixed offset 0x7614, which overlaps slot 20 once the
// panel allocation grows. The C helper below is the full replacement.
// =============================================================================

namespace {

using FN_ResolveDisplayTexture = int       (__cdecl *)(int displayId);
using FN_CreateUiQuadNode      = uint32_t* (__cdecl *)(int* owner);
using FN_SetUiNodeEnabled      = void      (__cdecl *)(uint32_t* node, int enabled);
using FN_SetUiNodeQuad         = void      (__cdecl *)(uint32_t* node, float* rect);
using FN_SetUiNodeTexture      = void      (__cdecl *)(uint32_t* node, int texture);
using FN_SetUiNodeUv           = void      (__cdecl *)(uint32_t* node, float* uv);
using FN_CreateUiQuadOwner     = uint32_t* (__cdecl *)();
using FN_SetUiOwnerSide        = void      (__cdecl *)(uint32_t* owner, uint8_t side);
using FN_SetUiOwnerScale       = void      (__cdecl *)(uint32_t* owner, float scale);
using FN_SetUiOwnerEnabled     = void      (__cdecl *)(uint32_t* owner, int enabled);
using FN_SetRichNodeEnabled    = void      (__cdecl *)(uint32_t node, int enabled);
using FN_IsRichNodeEnabled     = uint8_t   (__cdecl *)(uint32_t node);
using FN_GetRichNodePosition   = void      (__cdecl *)(uint32_t node, float* pos);
using FN_SetAttachmentSide     = void      (__cdecl *)(uint32_t attachmentData, uint8_t side);
using FN_SetAttachmentName     = void      (__cdecl *)(uint32_t attachmentData, uint8_t* name);

constexpr uintptr_t kLeagueSlotControlIds = 0x00E86130;
constexpr uintptr_t kLeaguePageAttachmentIds = 0x00E86220;
constexpr uintptr_t kLeaguePageControlIdsC = 0x00E8627C;
constexpr uintptr_t kLeaguePageControlIdsA = 0x00E86280;
constexpr uintptr_t kLeaguePageControlIdsB = 0x00E86284;
constexpr int kPanelLeagueSelectableCountOrig = 0x7614;

static FN_ResolveDisplayTexture ResolveDisplayTexture()
{
    return reinterpret_cast<FN_ResolveDisplayTexture>(0x00953630);
}

static FN_CreateUiQuadNode CreateUiQuadNode()
{
    return reinterpret_cast<FN_CreateUiQuadNode>(0x0094f390);
}

static FN_SetUiNodeEnabled SetUiNodeEnabled()
{
    return reinterpret_cast<FN_SetUiNodeEnabled>(0x00951310);
}

static FN_SetUiNodeQuad SetUiNodeQuad()
{
    return reinterpret_cast<FN_SetUiNodeQuad>(0x00951430);
}

static FN_SetUiNodeTexture SetUiNodeTexture()
{
    return reinterpret_cast<FN_SetUiNodeTexture>(0x00951e00);
}

static FN_SetUiNodeUv SetUiNodeUv()
{
    return reinterpret_cast<FN_SetUiNodeUv>(0x00951d70);
}

static FN_CreateUiQuadOwner CreateUiQuadOwner()
{
    return reinterpret_cast<FN_CreateUiQuadOwner>(0x0094f750);
}

static FN_SetUiOwnerSide SetUiOwnerSide()
{
    return reinterpret_cast<FN_SetUiOwnerSide>(0x0094f6d0);
}

static FN_SetUiOwnerScale SetUiOwnerScale()
{
    return reinterpret_cast<FN_SetUiOwnerScale>(0x0094f6a0);
}

static FN_SetUiOwnerEnabled SetUiOwnerEnabled()
{
    return reinterpret_cast<FN_SetUiOwnerEnabled>(0x0094f5a0);
}

static FN_SetRichNodeEnabled SetRichNodeEnabled()
{
    return reinterpret_cast<FN_SetRichNodeEnabled>(0x00950330);
}

static FN_IsRichNodeEnabled IsRichNodeEnabled()
{
    return reinterpret_cast<FN_IsRichNodeEnabled>(0x009505e0);
}

static FN_GetRichNodePosition GetRichNodePosition()
{
    return reinterpret_cast<FN_GetRichNodePosition>(0x00950400);
}

static FN_SetAttachmentSide SetAttachmentSide()
{
    return reinterpret_cast<FN_SetAttachmentSide>(0x009560b0);
}

static FN_SetAttachmentName SetAttachmentName()
{
    return reinterpret_cast<FN_SetAttachmentName>(0x00955b80);
}

struct ExtraVisualSlotNodes
{
    uint32_t* panel = nullptr;
    std::vector<uint32_t> nodes;
};

static std::vector<ExtraVisualSlotNodes> g_ExtraVisualSlotNodes;

static std::vector<uint32_t>& ExtraVisualNodesForPanel(uint32_t* panel)
{
    for (auto& entry : g_ExtraVisualSlotNodes) {
        if (entry.panel == panel) return entry.nodes;
    }

    g_ExtraVisualSlotNodes.push_back(ExtraVisualSlotNodes{});
    ExtraVisualSlotNodes& entry = g_ExtraVisualSlotNodes.back();
    entry.panel = panel;
    entry.nodes.assign(MAX_PANEL_SLOTS > 20 ? MAX_PANEL_SLOTS - 20 : 0, 0);
    return entry.nodes;
}

static uint8_t* PanelBytes(uint32_t* panel)
{
    return reinterpret_cast<uint8_t*>(panel);
}

static int PanelSide(uint32_t* panel)
{
    return *reinterpret_cast<int*>(PanelBytes(panel) + 0x00EC);
}

static int SlotDisplayId(uint32_t* panel, int slot)
{
    return *reinterpret_cast<int*>(PanelBytes(panel) + 0x1DE4
        + slot * PANEL_SLOT_STRIDE);
}

static uint8_t* LeagueSlotBase(uint32_t* panel, int slot)
{
    return PanelBytes(panel) + 0x1944 + slot * PANEL_SLOT_STRIDE;
}

static int LeagueSlotTeamId(uint32_t* panel, int slot, int teamIndex)
{
    if (!panel || slot < 0 || slot >= MAX_PANEL_SLOTS) return 0xFFFF;
    if (teamIndex < 0 || teamIndex >= TEAM_SLOT_COUNT) return 0xFFFF;
    return *reinterpret_cast<int*>(LeagueSlotBase(panel, slot)
        + teamIndex * sizeof(int));
}

static int LeagueSelectableCountOffset()
{
    // Stock writes the League selectable-count at 0x7614, immediately
    // before the two trailer dwords. With MAX_PANEL_SLOTS > 20 that fixed
    // address lands inside slot 20, so the whole post-slot trailer must
    // slide by the same amount as the slot array.
    return kPanelLeagueSelectableCountOrig
        + (MAX_PANEL_SLOTS - 20) * PANEL_SLOT_STRIDE;
}

static int& LeagueSelectableCountRef(uint32_t* panel)
{
    return *reinterpret_cast<int*>(PanelBytes(panel)
        + LeagueSelectableCountOffset());
}

static uint32_t* StockVisualSlotNodePtr(uint32_t* panel, int slot)
{
    if (slot < 0 || slot >= 20) return nullptr;
    return reinterpret_cast<uint32_t*>(PanelBytes(panel) + 0x0098
        + slot * sizeof(uint32_t));
}

static uint32_t& ExtraVisualSlotNodeRef(uint32_t* panel, int slot)
{
    std::vector<uint32_t>& nodes = ExtraVisualNodesForPanel(panel);
    const int index = slot - 20;
    if (index >= static_cast<int>(nodes.size()))
        nodes.resize(index + 1, 0);
    return nodes[index];
}

static uint32_t* VisualSlotOwner(uint32_t* panel)
{
    return *reinterpret_cast<uint32_t**>(PanelBytes(panel) + 0x0094);
}

static uint32_t ParentNodeForLeagueSlot(uint32_t* panel, int slot)
{
    int page = slot / SLOTS_PER_LEAGUE_PAGE;
    if (page < 0) return 0;

    // V2 creates one Step-9 parent per 8-seat page at panel[+0x6C+i*4].
    // Stock only used pages 0..2; pages 3+ are our dynamic extension.
    if (page >= g_LeaguePanelCount) page = g_LeaguePanelCount - 1;
    if (page >= 2) page = 2;
    if (page < 0) return 0;

    return *reinterpret_cast<uint32_t*>(PanelBytes(panel) + 0x006C
        + page * sizeof(uint32_t));
}

static int16_t StockLeagueSlotControlId(int side, int slot)
{
    if (side < 0) side = 0;
    if (side > 2) side = 2;
    if (slot < 0) slot = 0;
    if (slot > 19) slot = 19;

    return *reinterpret_cast<int16_t*>(kLeagueSlotControlIds
        + (side * 20 + slot) * sizeof(uint32_t));
}

static int LeagueControlIdForSlot(uint32_t* panel, int slot)
{
    if (slot < 20) return StockLeagueSlotControlId(PanelSide(panel), slot);

    const int page = slot / SLOTS_PER_LEAGUE_PAGE;
    const int indexInPage = slot % SLOTS_PER_LEAGUE_PAGE;

    // Rich OPD pages reserve one container/control rect before the 8 seat
    // rects: page 0 seats are 47..54, page 1 are 56..63, page 2 are
    // 65..72. Stock only used 65..68 because only slots 16..19 existed.
    return 47 + page * 9 + indexInPage;
}

static int16_t LeaguePageControlIdFromTable(uintptr_t table, int side, int page)
{
    if (side < 0) side = 0;
    if (side > 2) side = 2;
    if (page < 0) page = 0;
    if (page > 2) page = 2;

    // Stock BuildLeaguePanelVisualSlots indexes these as side*3+page,
    // but each int16 id is padded to a 4-byte table entry.
    return *reinterpret_cast<int16_t*>(table
        + (side * 3 + page) * sizeof(uint32_t));
}

static bool ApplyControlRectOverride(uint32_t parentNode, int slot, float* rect)
{
    const PanelSlotLayout* layout = GetLeaguePanelLayout(slot);
    if (!layout || !layout->has_control_rect) return false;

    // The OPD control x/y are LOCAL offsets: GetRichControlWorldRect returns
    // (parent node's world position + local), and it is the node position that
    // shifts the away-side grid to the right. Apply the INI x/y in that same
    // local space (node position + override) so overridden controls inherit
    // the side offset too, instead of pinning an absolute screen position.
    // Width/height carry no node offset (they are sizes), so INI values there
    // are used as-is. Base stays (0,0) if the node has no position, in which
    // case the override degrades to the old absolute behaviour.
    float base[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (parentNode) GetRichNodePosition()(parentNode, base);

    if (layout->has_control_x)      rect[0] = base[0] + layout->control_x;
    if (layout->has_control_y)      rect[1] = base[1] + layout->control_y;
    if (layout->has_control_width)  rect[2] = layout->control_width;
    if (layout->has_control_height) rect[3] = layout->control_height;
    return true;
}

static uint32_t PageNodeForLeaguePage(uint32_t* panel, int page)
{
    if (!panel || page < 0 || page >= g_LeaguePanelCount) return 0;
    return *reinterpret_cast<uint32_t*>(PanelBytes(panel) + 0x006C
        + page * sizeof(uint32_t));
}

static bool LeagueSlotHasDisplay(uint32_t* panel, int slot)
{
    if (!panel || slot < 0 || slot >= MAX_PANEL_SLOTS) return false;
    return SlotDisplayId(panel, slot) != -1;
}

static bool LeaguePhysicalPageHasDisplaySlots(uint32_t* panel, int physicalPage)
{
    if (!panel || physicalPage < 0 || physicalPage > 2) return false;

    const int start = physicalPage * SLOTS_PER_LEAGUE_PAGE;
    int end = start + SLOTS_PER_LEAGUE_PAGE;
    if (physicalPage == 2) end = MAX_PANEL_SLOTS;

    for (int slot = start; slot < end && slot < MAX_PANEL_SLOTS; ++slot) {
        if (LeagueSlotHasDisplay(panel, slot)) return true;
    }
    return false;
}

static int FirstDisplaySlotOnPage(uint32_t* panel, int logicalPage)
{
    if (!panel || logicalPage < 0) return -1;

    const int start = logicalPage * SLOTS_PER_LEAGUE_PAGE;
    const int end = start + SLOTS_PER_LEAGUE_PAGE;
    for (int slot = start; slot < end && slot < MAX_PANEL_SLOTS; ++slot) {
        if (LeagueSlotHasDisplay(panel, slot)) return slot;
    }
    return -1;
}

static void CorrectLeagueSelectedSlotForDynamicPages(uint32_t* panel)
{
    if (!panel) return;

    int& currentPage = *reinterpret_cast<int*>(PanelBytes(panel) + 0x00F8);
    const int maxPage = (MAX_PANEL_SLOTS - 1) / SLOTS_PER_LEAGUE_PAGE;
    if (currentPage < 0) currentPage = 0;
    if (currentPage > maxPage) currentPage = maxPage;

    const int cursorIndex = *reinterpret_cast<int*>(PanelBytes(panel) + 0x1940);
    if (cursorIndex < 0 || cursorIndex > 1) return;

    int* selectedSlot = reinterpret_cast<int*>(PanelBytes(panel) + 0x1914
        + cursorIndex * 0x18);

    int slot = selectedSlot ? *selectedSlot : -1;
    const int pageStart = currentPage * SLOTS_PER_LEAGUE_PAGE;
    const int pageEnd = pageStart + SLOTS_PER_LEAGUE_PAGE;
    const bool slotInPage = slot >= pageStart
        && slot < pageEnd
        && slot < MAX_PANEL_SLOTS
        && LeagueSlotHasDisplay(panel, slot);

    if (!slotInPage) {
        slot = FirstDisplaySlotOnPage(panel, currentPage);
        if (slot < 0) {
            for (int step = 1; step <= maxPage; ++step) {
                const int candidatePage = (currentPage + step) % (maxPage + 1);
                slot = FirstDisplaySlotOnPage(panel, candidatePage);
                if (slot >= 0) {
                    currentPage = candidatePage;
                    break;
                }
            }
        }
    }

    if (slot < 0 || !selectedSlot) return;

    *selectedSlot = slot;
    const int column = slot % 4;
    const int row = slot / 4;
    *reinterpret_cast<int*>(PanelBytes(panel) + 0x1938) = column;
    *reinterpret_cast<int*>(PanelBytes(panel) + 0x1920) = column;
    *reinterpret_cast<int*>(PanelBytes(panel) + 0x193C) = row;
    *reinterpret_cast<int*>(PanelBytes(panel) + 0x1924) = row;
}

static int ApplyDynamicPageChromeFromOpd(uint32_t* panel)
{
    if (!panel || !fn_GetBoneWorldPos || !fn_SetNodePosition) return 0;

    uint32_t layoutNode = *reinterpret_cast<uint32_t*>(
        PanelBytes(panel) + 0x0084);
    if (!layoutNode) return 0;

    const int side = PanelSide(panel);
    int applied = 0;

    float basePos[4] = {};
    const uint32_t firstPageNode = PageNodeForLeaguePage(panel, 0);
    if (firstPageNode) GetRichNodePosition()(firstPageNode, basePos);

    // Stock only owns three physical page/chrome nodes. In the current
    // expanded layout, slots after 19 intentionally reuse the third
    // physical page node, so page 2's visibility scan covers the rest of
    // the expanded League-slot array.
    int visibleIndex = 0;
    for (int physicalPage = 0; physicalPage < 3; ++physicalPage) {
        uint32_t pageNode = PageNodeForLeaguePage(panel, physicalPage);
        if (!pageNode) continue;

        const bool shouldEnable = LeaguePhysicalPageHasDisplaySlots(
            panel, physicalPage);

        SetRichNodeEnabled()(pageNode, shouldEnable ? 1 : 0);
        if (!shouldEnable || !IsRichNodeEnabled()(pageNode)) continue;

        float rect[4] = {};
        const int tablePage = visibleIndex > 2 ? 2 : visibleIndex;
        const int16_t controlId = LeaguePageControlIdFromTable(
            kLeaguePageControlIdsA, side, tablePage);
        if (!fn_GetBoneWorldPos(static_cast<int>(layoutNode),
                                rect,
                                controlId)) {
            Logger::Log("[PanelV2] page chrome physical=%d skipped: "
                        "tableA control id %d not found on layout=0x%08X",
                        physicalPage, controlId, layoutNode);
            continue;
        }

        float pos[4] = {
            basePos[0] + rect[0],
            basePos[1] + rect[1],
            *reinterpret_cast<float*>(PanelBytes(panel) + 0x003C),
            1.0f,
        };
        fn_SetNodePosition(pageNode, pos);
        ++applied;

        Logger::Log("[PanelV2] page chrome physical=%d controlA=%d node=0x%08X "
                    "pos=(%.3f, %.3f, %.3f) enabled=%d",
                    physicalPage, controlId, pageNode,
                    pos[0], pos[1], pos[2], shouldEnable ? 1 : 0);
        ++visibleIndex;
    }

    const int16_t probeA0 = LeaguePageControlIdFromTable(
        kLeaguePageControlIdsA, side, 0);
    const int16_t probeB0 = LeaguePageControlIdFromTable(
        kLeaguePageControlIdsB, side, 0);
    const int16_t probeC0 = LeaguePageControlIdFromTable(
        kLeaguePageControlIdsC, side, 0);
    Logger::Log("[PanelV2] page chrome tables side=%d A0=%d B0=%d C0=%d",
                side, probeA0, probeB0, probeC0);

    return applied;
}

static int ApplyLeaguePhysicalPageVisibility(uint32_t* panel)
{
    if (!panel) return 0;

    int visibleGroups = 0;
    for (int physicalPage = 0; physicalPage < 3; ++physicalPage) {
        const uint32_t pageNode = PageNodeForLeaguePage(panel, physicalPage);
        if (!pageNode) continue;

        const bool visible = LeaguePhysicalPageHasDisplaySlots(
            panel, physicalPage);
        SetRichNodeEnabled()(pageNode, visible ? 1 : 0);
        if (visible) ++visibleGroups;
    }

    *reinterpret_cast<int*>(PanelBytes(panel) + 0x18FC) = visibleGroups;
    return visibleGroups;
}

static uint32_t* CreateRichAttachment(uint32_t parentNode, int assetId)
{
    if (!parentNode || assetId < 0 || !fn_LookupBone
        || !fn_ResolveBone || !fn_AttachBone) {
        return nullptr;
    }

    uint32_t boneHandle = fn_LookupBone(parentNode,
        static_cast<uint32_t>(assetId));
    int* bone = fn_ResolveBone(boneHandle);
    if (!bone) return nullptr;

    return fn_AttachBone(parentNode, bone, static_cast<uint32_t>(assetId));
}

static void BuildLeaguePageAttachments(uint32_t* panel)
{
    if (!panel) return;

    const int side = PanelSide(panel);
    for (int physicalPage = 0; physicalPage < 3; ++physicalPage) {
        const int assetId = *reinterpret_cast<int*>(kLeaguePageAttachmentIds
            + (side * 3 + physicalPage) * sizeof(uint32_t));
        const uint32_t parentNode = PageNodeForLeaguePage(panel, physicalPage);

        uint32_t* attachment = CreateRichAttachment(parentNode, assetId);
        *reinterpret_cast<uint32_t**>(PanelBytes(panel) + 0x78
            + physicalPage * sizeof(uint32_t)) = attachment;

        if (!attachment) continue;

        SetAttachmentSide()(attachment[3],
            *reinterpret_cast<uint8_t*>(PanelBytes(panel) + 0x40));
        SetAttachmentName()(attachment[3],
            PanelBytes(panel) + 0x00FC + physicalPage * 0x400);
    }
}

static void BuildLeagueLogoOwner(uint32_t* panel)
{
    if (!panel) return;

    uint32_t* owner = CreateUiQuadOwner()();
    *reinterpret_cast<uint32_t**>(PanelBytes(panel) + 0x0094) = owner;
    if (!owner) return;

    SetUiOwnerSide()(owner,
        *reinterpret_cast<uint8_t*>(PanelBytes(panel) + 0x40));

    uint32_t page0 = PageNodeForLeaguePage(panel, 0);
    if (page0 && fn_GetBoneScale) {
        const int16_t firstControl = StockLeagueSlotControlId(PanelSide(panel), 0);
        const float scale = fn_GetBoneScale(static_cast<int>(page0), firstControl);
        SetUiOwnerScale()(owner, scale);
    }

    SetUiOwnerEnabled()(owner, 1);
}

static int CountSelectableLeagueSlots(uint32_t* panel)
{
    if (!panel) return 1;

    const bool modeUsesFirstTeam =
        *reinterpret_cast<int*>(PanelBytes(panel) + 0x1908) == 1;
    int count = 0;

    for (int slot = 0; slot < MAX_PANEL_SLOTS; ++slot) {
        if (SlotDisplayId(panel, slot) == -1) continue;

        if (modeUsesFirstTeam) {
            const int team0 = LeagueSlotTeamId(panel, slot, 0);
            if (team0 == 0xFFFF || team0 == 0x011E) continue;
        } else {
            if (LeagueSlotTeamId(panel, slot, 1) == 0xFFFF) continue;
        }

        ++count;
    }

    if (count == 0) count = 1;
    LeagueSelectableCountRef(panel) = count;
    return count;
}

static void ApplyLowLeagueCountChrome(uint32_t* panel, int selectableCount)
{
    if (!panel || selectableCount >= 3) return;

    if (fn_SetScrollFlag) {
        fn_SetScrollFlag(*reinterpret_cast<int*>(PanelBytes(panel) + 0x64), 0);
    }

    uint32_t** slot = reinterpret_cast<uint32_t**>(PanelBytes(panel) + 0x8C);
    if (fn_DestroyBoneAttach && *slot) fn_DestroyBoneAttach(*slot);

    uint32_t* attachment = CreateRichAttachment(
        *reinterpret_cast<uint32_t*>(PanelBytes(panel) + 0x88), 0x0B);
    *slot = attachment;

    if (attachment && fn_SetBoneVisibility) {
        fn_SetBoneVisibility(attachment[3], 0);
    }
}

static void ApplyDefaultUv(uint32_t* node)
{
    // Mirrors the stock UV block built at 0x00B0881D before
    // SetUiNodeUvCoords. The node uses the full resolved texture.
    float uv[12] = {
        0.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
        1.0f, 1.0f,
    };
    SetUiNodeUv()(node, uv);
}

static void UpsertLeagueVisualSlot(uint32_t* panel, int slot)
{
    if (!panel || slot < 0 || slot >= MAX_PANEL_SLOTS) return;

    const int displayId = SlotDisplayId(panel, slot);
    if (displayId == -1) return;

    // Custom logo: if leagues/<slot>.png exists, build+register a texture under
    // this display id (once) so the resolve below picks it up. No-op otherwise.
    EnsureCustomLeagueLogo(slot, displayId);

    const int texture = ResolveDisplayTexture()(displayId);
    if (texture == 0) return;

    float rect[4] = {};
    const int controlId = LeagueControlIdForSlot(panel, slot);
    uint32_t parent = ParentNodeForLeagueSlot(panel, slot);
    bool haveRect = false;

    if (parent && fn_GetBoneWorldPos) {
        haveRect = fn_GetBoneWorldPos(static_cast<int>(parent),
                                      rect,
                                      static_cast<int16_t>(controlId)) != nullptr;
    }

    const bool hadOverride = ApplyControlRectOverride(parent, slot, rect);
    if (!haveRect && !hadOverride) {
        Logger::Log("[PanelV2] visual slot %d skipped: control id %d "
                    "not found on parent=0x%08X",
                    slot, controlId, parent);
        return;
    }

    uint32_t* stockNodeSlot = StockVisualSlotNodePtr(panel, slot);
    uint32_t& node = stockNodeSlot ? *stockNodeSlot
                                   : ExtraVisualSlotNodeRef(panel, slot);
    if (!node) {
        uint32_t* owner = VisualSlotOwner(panel);
        if (!owner) return;

        uint32_t* created = CreateUiQuadNode()(reinterpret_cast<int*>(owner));
        node = reinterpret_cast<uint32_t>(created);
        if (created) SetUiNodeEnabled()(created, 1);
    }
    if (!node) return;

    uint32_t* nodePtr = reinterpret_cast<uint32_t*>(node);
    SetUiNodeQuad()(nodePtr, rect);
    SetUiNodeTexture()(nodePtr, texture);
    ApplyDefaultUv(nodePtr);

    const PanelSlotLayout* layout = GetLeaguePanelLayout(slot);
    if (layout && layout->has_control_depth) {
        Logger::Log("[PanelV2] visual slot %d control=%d depth override "
                    "%.3f parsed (logo quad path uses x/y/width/height)",
                    slot, controlId, layout->control_depth);
    }

    if (slot >= 20 || hadOverride) {
        Logger::Log("[PanelV2] visual slot %d -> control=%d parent=0x%08X "
                    "rect=(%.3f, %.3f, %.3f, %.3f)%s",
                    slot, controlId, parent,
                    rect[0], rect[1], rect[2], rect[3],
                    hadOverride ? " [ini]" : "");
    }
}

} // namespace

// Exposed for the selection-cursor path (club_hooks_panel_refresh.cpp) so the
// cursor applies the same INI control-rect override the logo quads do — without
// it the cursor tracks the raw OPD control while the logo follows the override.
bool ApplyLeagueControlRectOverride(uint32_t parentNode, int slot, float* rect)
{
    return ApplyControlRectOverride(parentNode, slot, rect);
}

// Companion for the cursor's "scale" (really its Z-depth). GetBoneScale returns
// (control depth + subres depth) + node Z; SetItemScale writes it to the item's
// depth fields. The INI knob is control_depth. Mirror the x/y override: node Z
// (GetRichNodePosition[2]) + INI depth, so the override is a node-relative
// depth offset. No-op when the slot has no control_depth override.
bool ApplyLeagueControlDepthOverride(uint32_t parentNode, int slot, float* scale)
{
    if (!scale) return false;
    const PanelSlotLayout* layout = GetLeaguePanelLayout(slot);
    if (!layout || !layout->has_control_depth) return false;

    float base[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (parentNode) GetRichNodePosition()(parentNode, base);
    *scale = base[2] + layout->control_depth;
    return true;
}

void ClearExtraLeagueVisualSlotNodes(uint32_t* panel)
{
    for (auto it = g_ExtraVisualSlotNodes.begin();
         it != g_ExtraVisualSlotNodes.end();
         ++it) {
        if (it->panel == panel) {
            g_ExtraVisualSlotNodes.erase(it);
            return;
        }
    }
}

void UpdateExtraLeagueVisualSlotVisibility(uint32_t* panel)
{
    if (!panel) return;

    // The extra logo quads (slots >= 20) live outside the stock panel[+0x98..]
    // node array, so the refresh's stock show/hide pass never touches them.
    // The league list is ONE scrolling list — every league logo is visible
    // while browsing (regardless of the cursor's page) and only hidden in
    // team-selection mode (the chosen league's clubs take over the grid).
    // So mirror the stock pass exactly: use the same enable byte (panel[+0x44])
    // in league mode, 0 in team mode. This also keeps an inactive/away panel's
    // extras hidden, because its stock nodes (and thus this byte-driven pass)
    // are already hidden in that state.
    const bool    teamMode = *reinterpret_cast<int*>(PanelBytes(panel) + 0x190C) != 0;
    const uint8_t enable   = teamMode ? 0 : *(PanelBytes(panel) + 0x44);

    for (auto& entry : g_ExtraVisualSlotNodes) {
        if (entry.panel != panel) continue;

        for (int i = 0; i < static_cast<int>(entry.nodes.size()); ++i) {
            const uint32_t node = entry.nodes[i];
            if (!node) continue;
            SetUiNodeEnabled()(reinterpret_cast<uint32_t*>(node), enable);
        }
        return;
    }
}

void __cdecl hook_BuildLeaguePanelVisualSlots_C(uint32_t* panel)
{
    if (!panel) return;

    // Give slots with a leagues/<slot>.png but no logo id a synthetic id first,
    // so the visibility / selectable passes below treat them as populated.
    AssignSyntheticLogoIds(panel);

    const int side = PanelSide(panel);
    LeagueSelectableCountRef(panel) = 0;

    const int visibleGroups = ApplyLeaguePhysicalPageVisibility(panel);
    const int pageChromeUpdated = ApplyDynamicPageChromeFromOpd(panel);
    BuildLeaguePageAttachments(panel);
    BuildLeagueLogoOwner(panel);

    int updated = 0;
    for (int slot = 0; slot < MAX_PANEL_SLOTS; ++slot) {
        UpsertLeagueVisualSlot(panel, slot);
        ++updated;
    }

    const int selectableCount = CountSelectableLeagueSlots(panel);
    ApplyLowLeagueCountChrome(panel, selectableCount);

    *reinterpret_cast<int*>(PanelBytes(panel) + 0x48) = 1;
    if (visibleGroups == 1) {
        *reinterpret_cast<int*>(PanelBytes(panel) + 0x190C) = 1;
        *reinterpret_cast<int*>(PanelBytes(panel) + 0x48) = 1;
    }

    //CorrectLeagueSelectedSlotForDynamicPages(panel);
    //UpdateExtraLeagueVisualSlotVisibility(panel);

    Logger::Log("[PanelV2] BuildLeaguePanelVisualSlots full replacement "
                "side=%d slots=%d logos=%d pageChrome=%d visibleGroups=%d "
                "selectable=%d selectableOff=0x%X",
                side, MAX_PANEL_SLOTS, updated, pageChromeUpdated,
                visibleGroups, selectableCount,
                LeagueSelectableCountOffset());
}

__declspec(naked) void hook_BuildLeaguePanelVisualSlots_Naked()
{
    __asm {
        push esi
        call hook_BuildLeaguePanelVisualSlots_C
        add  esp, 4
        ret
    }
}


// =============================================================================
// hook_CreateChildNode — instrumentation hook for FUN_00950a00.
//
// What FUN_00950a00 does (renamed `CreateChildNode` in Ghidra):
//
//   `FN_GetChildNode` (= FUN_00950b40 / `CreateChildNode_Default`) is a
//   one-line wrapper that calls FUN_00950a00 with a hardcoded
//   `attachDefaultBehavior = 1`. Its sibling FUN_00950b50
//   (`CreateChildNode_Bare`) passes 0 and is unused by our codebase.
//
//   FUN_00950a00 is the underlying child-node creator. Given a parent
//   render-node handle, it allocates a new 0xA8-byte node struct via the
//   game allocator and initialises it:
//
//     * newNode[+0x2C] = parentNode
//     * If FUN_00952770(parent, 0) returns a render-group entry:
//         newNode[+0x48] = that entry
//         newNode[+0x44] = 0
//         FUN_00950260(newNode)        // push render-group context
//         alpha bytes seeded
//     * Position field cleared (FUN_00950890(node, NULL))
//     * Color RGBA = (0xFF, 0xFF, 0xFF, 0xFF)   (FUN_00950620)
//     * Scale     = (1, 1, 1, 1)                 (FUN_00950760)
//     * Rotation  = (0, 0, 0, 0)                 (FUN_009507c0)
//     * If `attachDefaultBehavior != 0`, register the default behavior
//       (vtable 0x95d410) on the node, stored at newNode[+0x30].
//     * Allocate a fresh node-id byte (FUN_0094f130)            -> [+0xA1]
//     * Allocate a global handle    (FUN_0094efb0)              -> [+0x34]
//       and register it in the global registry (vtable 0x95cf10).
//     * FUN_0095c7c0(node)            // local resource finalise
//     * FUN_00952860(parent, node)    // LINK into parent's child list
//     * FUN_0095c8d0(node)            // post-link finalise
//
//   Returns the new node pointer, or NULL if `parentNode` is 0.
//
// Why we hook it (Layer D investigation context):
//
//   The v2 panel creator's Step 9 calls `fn_GetChildNode` once per
//   iteration, expecting `g_LeaguePanelCount` distinct child nodes
//   parented off `panel[+0x68]`. We need to verify that:
//
//     1. each call returns a fresh, distinct node (not aliased);
//     2. the parent we pass each time is the SAME mainNode handle;
//     3. no other code path is creating extra panel children behind
//        our back during the same frame.
//
//   The hook logs `caller_pc / parentNode / attachBehavior / -> result`
//   for every call whose RETURN ADDRESS lies in the panel-UI subsystem
//   range (0x00b00000..0x00b1ffff). Calls from other systems (kit
//   pipeline, match HUD, etc.) are silenced so the log file stays
//   useful. A line cap (kCCN_MaxLogLines) prevents runaway logs if the
//   panel UI cycles many times in one session.
//
// Procedure:
//
//   1. Build with the INSTALL_HOOK line in `club_hooks_register.cpp`
//      uncommented.
//   2. Open the league-selection screen and let it draw at least once.
//   3. Inspect the log for the [CreateChildNode] block. Expected stock
//      pattern (3 calls per panel build, all from FUN_00b09050's Step 9):
//        caller≈0x00b09???  parent=<mainNode>  behavior=1  -> node=<unique>
//      followed by Step 10's single childB call (same shape).
//   4. With v2 + `g_LeaguePanelCount = 4`, expect 4 calls in Step 9
//      instead of 3 — all with the SAME `parent` and 4 DISTINCT
//      `result` handles. If any two results match, that confirms the
//      "aliased child" hypothesis and the v2 strategy is doomed.
//   5. Disable (comment out the INSTALL_HOOK line) once the data is in
//      hand — this hook fires for every UI element on every screen, so
//      leaving it on in production spams the log file.
//
// Footprint: same shape as `hook_SetNodePosition` (range filter + line
// cap + caller_pc log). Production behaviour is unchanged: we always
// forward to the trampoline first and return its result.
// =============================================================================
namespace {
    // Panel UI subsystem caller range. Verified against existing hook
    // addresses (FUN_00b09050, FUN_00b07b00, FUN_00b0f270, etc.).
    constexpr uintptr_t kCCN_PanelLoRange = 0x00b00000;
    constexpr uintptr_t kCCN_PanelHiRange = 0x00b20000;
    constexpr int       kCCN_MaxLogLines  = 800;
}

static int g_createChildNodeLogCount = 0;

uint32_t* __cdecl hook_CreateChildNode(uint32_t parentNode,
                                         char     attachDefaultBehavior)
{
    // Capture caller PC BEFORE forwarding — once we call into the
    // trampoline the hot return-address slot may be reused.
    const uintptr_t retAddr =
        reinterpret_cast<uintptr_t>(_ReturnAddress());

    // Forward first so semantics match the original even if logging is
    // skipped (range-out / cap-reached).
    uint32_t* result = orig_CreateChildNode
        ? orig_CreateChildNode(parentNode, attachDefaultBehavior)
        : nullptr;

    if (retAddr >= kCCN_PanelLoRange && retAddr < kCCN_PanelHiRange &&
        g_createChildNodeLogCount < kCCN_MaxLogLines)
    {
        Logger::Log("[CreateChildNode] caller=0x%08X parent=0x%08X "
                    "behavior=%d -> node=0x%08X",
                    static_cast<uint32_t>(retAddr),
                    parentNode,
                    static_cast<int>(attachDefaultBehavior),
                    reinterpret_cast<uint32_t>(result));
        ++g_createChildNodeLogCount;
        if (g_createChildNodeLogCount == kCCN_MaxLogLines)
        {
            Logger::Log("[CreateChildNode] log cap (%d) reached; further "
                        "calls silenced for the rest of this session.",
                        kCCN_MaxLogLines);
        }
    }
    return result;
}
