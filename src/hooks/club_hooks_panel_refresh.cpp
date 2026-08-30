// =============================================================================
// club_hooks_panel_refresh.cpp — full replacement for FUN_00b08b40
//
// FUN_00b08b40 is the league/team selection panel's per-refresh handler.
// It is NOT a stack-argument function in the usual sense: the panel creator
// installs its address as the display sub-object's refresh callback in Step 2
// (`kSubObjVTable = 0x00B08B40`, see club_hooks_panel_v2.cpp). The game calls
// it as `FUN_00b08b40(subObj)` where `subObj[0]` is the panel pointer.
//
// Structure of the original (verified by full disassembly of pes6.exe):
//
//   panel = subObj[0]
//   if (panel[+0x94] == 0)   BuildLeaguePanelVisualSlots(panel)   // first refresh
//   if (panel[+0x48] == 1) {                                      // dirty
//       if (panel[+0x190C] == 0)  <LEAGUE-selection rebuild>
//       else                      <TEAM-selection rebuild>
//   }
//   panel[+0x48] = 0
//   clear the 8 per-player input-flag arrays
//   return 0
//
// Why we replace it
// -----------------
// The LEAGUE branch positions the selection CURSOR (the box that follows the
// highlighted league, panel[+0x60]) by looking up a *control rect* in the
// current page node's rich sub-resource:
//
//     controlId = kLeagueSlotControlIds[side*20 + slot]      // 0x00E86130
//     pageNode  = panel[+0x6C + currentPage*4]
//     GetRichControlWorldRect(pageNode, rect, controlId)     // -> cursor pos
//     GetBoneScale          (pageNode,       controlId)      // -> cursor scale
//
// That table is a *fixed 20-entries-per-side* array. For league slots >= 20
// the index `side*20 + slot` runs past the side's row, so the cursor asks for
// a controlId that does not exist in the page-2 sub-resource (10/19) — where
// the extra seat controls 69,70,71,… actually live — so GetRichControlWorldRect
// returns NULL, the rect stays zero, and the box snaps to the node origin
// (screen centre). The LOGO for those slots works because
// hook_BuildLeaguePanelVisualSlots_C resolves the id with the 47 + page*9 + idx
// formula (LeagueControlIdForSlot). This replacement makes the cursor use the
// *same* id (and clamps the page-node index to the 3 real page nodes), so the
// cursor tracks the extra slots exactly like the logo.
//
// Two other >20-slot correctness fixes fold in naturally here:
//   * the TEAM branch's "league selectable count" read at the FIXED offset
//     0x7614 slides to the dynamic offset (0x7614 grows with the slot array);
//   * the page-node visibility scan (stock FUN_00b082f0, reimplemented as
//     RefreshPageNodeVisibility) scans page 2 over slots 16..MAX_PANEL_SLOTS-1
//     instead of 16..19, so a page whose only populated seats are >= 20 still
//     shows.
//
// Everything else is a faithful C translation of the stock body; the callee
// semantics were recovered from the decompiler and are noted inline.
// =============================================================================

#include "club_hooks_common.h"
#include "../utils/logger.h"

// -----------------------------------------------------------------------------
// Callees not already exposed as fn_* pointers in club_hooks_common.h.
// Addresses verified against pes6.exe / Ghidra.
// -----------------------------------------------------------------------------
namespace {

// FUN_00b0f580(scrollCtrl, flag): stores `flag` into scrollCtrl[+0x2C]
// (SetScrollCtrlEnabled). Called with 0 in LEAGUE mode, f44 in TEAM mode.
using FN_SetScrollCtrlEnabled = void (__cdecl*)(int scrollCtrl, int flag);
// FUN_00950330(node, enabled): enable/disable a rich render node.
using FN_SetRichNodeEnabled   = void (__cdecl*)(uint32_t node, int enabled);
// FUN_00951310(node, enabled): enable/disable a UI quad node.
using FN_SetUiNodeEnabled     = void (__cdecl*)(uint32_t node, int enabled);
// FUN_00955b80(attachmentData, name): set an attachment's name string.
using FN_SetAttachmentName     = void (__cdecl*)(uint32_t attachmentData,
                                                 const void* name);
// FUN_00b00510(item, rect4): set a display item's rect/position.
using FN_SetItemRect           = void (__cdecl*)(uint32_t* item,
                                                 const float* rect);
// FUN_00b006e0(item, out4): read a display item's stored rect into out[0..3].
using FN_GetItemRect           = void (__cdecl*)(uint32_t* item, float* out);
// FUN_00b0f460(scrollCtrl, index) -> cell pointer (cell stride 0x14).
using FN_GetScrollCell         = int  (__cdecl*)(int scrollCtrl, int index);
// FUN_00b0fd30(cell, colorBytes): set a scroll cell's colour.
using FN_SetScrollCellColor    = void (__cdecl*)(int cell, const void* color);
// FUN_00b0f750(scrollCtrl, out4, cellIndex): world rect of a scroll cell.
using FN_GetScrollCellWorldPos = void (__cdecl*)(int scrollCtrl, float* out,
                                                 int cellIndex);
// FUN_00863660(teamId, 0, 0) -> team name string.
using FN_GetTeamName           = const void* (__cdecl*)(uint16_t teamId,
                                                        int a, int b);
// FUN_0041c320 == strncpy(dst, src, n); use the game's own copy so the
// page-name buffer refresh is byte-identical to the stock body.
using FN_StrNCpy               = char* (__cdecl*)(char* dst, const char* src,
                                                  unsigned int n);

const auto SetScrollCtrlEnabled = reinterpret_cast<FN_SetScrollCtrlEnabled>(0x00b0f580);
const auto SetRichNodeEnabled   = reinterpret_cast<FN_SetRichNodeEnabled>  (0x00950330);
const auto SetUiNodeEnabled     = reinterpret_cast<FN_SetUiNodeEnabled>    (0x00951310);
const auto SetAttachmentName    = reinterpret_cast<FN_SetAttachmentName>   (0x00955b80);
const auto SetItemRect          = reinterpret_cast<FN_SetItemRect>         (0x00b00510);
const auto GetItemRect          = reinterpret_cast<FN_GetItemRect>         (0x00b006e0);
const auto GetScrollCell        = reinterpret_cast<FN_GetScrollCell>       (0x00b0f460);
const auto SetScrollCellColor   = reinterpret_cast<FN_SetScrollCellColor>  (0x00b0fd30);
const auto GetScrollCellWorldPos= reinterpret_cast<FN_GetScrollCellWorldPos>(0x00b0f750);
const auto GetTeamName          = reinterpret_cast<FN_GetTeamName>         (0x00863660);
const auto GameStrNCpy          = reinterpret_cast<FN_StrNCpy>            (0x0041c320);

// Stock control-id table (int16 padded to 4-byte entries), and the two float
// constants the stock body loads directly.
constexpr uintptr_t kLeagueSlotControlIds = 0x00E86130;

inline float FloatConst_B7D6BC() { return *reinterpret_cast<float*>(0x00B7D6BC); }
inline float FloatConst_B82ED4() { return *reinterpret_cast<float*>(0x00B82ED4); }

// The stock League selectable-count lives immediately before the two trailer
// dwords (offset 0x7614). With MAX_PANEL_SLOTS > 20 that fixed offset lands
// inside slot 20, so it slides with the slot array — same rule the panel
// creator and BuildLeaguePanelVisualSlots use.
inline int LeagueSelectableCountOffset() {
    return 0x7614 + (MAX_PANEL_SLOTS - 20) * PANEL_SLOT_STRIDE;
}

// -----------------------------------------------------------------------------
// Panel field accessors (byte offsets from the panel base).
// -----------------------------------------------------------------------------
inline uint8_t*  PB (uint32_t* p, int off) { return reinterpret_cast<uint8_t*>(p) + off; }
inline uint32_t& PDW(uint32_t* p, int off) { return *reinterpret_cast<uint32_t*>(PB(p, off)); }
inline int&      PIN(uint32_t* p, int off) { return *reinterpret_cast<int*>     (PB(p, off)); }
inline float&    PF (uint32_t* p, int off) { return *reinterpret_cast<float*>   (PB(p, off)); }
inline uint8_t&  PBY(uint32_t* p, int off) { return *reinterpret_cast<uint8_t*> (PB(p, off)); }

inline int  CursorIndex(uint32_t* p)              { return PIN(p, 0x1940); }
inline int  SelectedSlot(uint32_t* p)             { return PIN(p, 0x1914 + CursorIndex(p) * 24); }
inline int  TeamPos(uint32_t* p)                  { return PIN(p, 0x1910 + CursorIndex(p) * 24); }
inline int  DisplayId(uint32_t* p, int slot)      { return PIN(p, 0x1DE4 + slot * PANEL_SLOT_STRIDE); }
inline uint8_t* SlotName(uint32_t* p, int slot)   { return PB (p, 0x19E4 + slot * PANEL_SLOT_STRIDE); }
inline uint8_t* PageNameBuf(uint32_t* p, int page){ return PB (p, 0x00FC + page * 0x400); }
inline uint32_t PageNode(uint32_t* p, int page)   { return PDW(p, 0x006C + page * 4); }

// Attachment records store their live handle at record[+0x0C] (i.e. [3]).
inline uint32_t AttachHandle(uint32_t attachRecord) {
    return reinterpret_cast<uint32_t*>(attachRecord)[3];
}

// The stock per-side seat control table (slots 0..19), and the id our own
// logo path (LeagueControlIdForSlot) assigns to slots >= 20. They MUST match
// so the cursor and the logo resolve to the same control rect.
inline int16_t StockSeatControlId(int side, int slot) {
    if (side < 0) side = 0; if (side > 2) side = 2;
    if (slot < 0) slot = 0; if (slot > 19) slot = 19;
    return *reinterpret_cast<int16_t*>(
        kLeagueSlotControlIds + (side * 20 + slot) * sizeof(uint32_t));
}
inline int LeagueCursorControlId(int side, int slot) {
    if (slot < 20) return StockSeatControlId(side, slot);
    return 47 + (slot / SLOTS_PER_LEAGUE_PAGE) * 9 + (slot % SLOTS_PER_LEAGUE_PAGE);
}

// Reimplementation of FUN_00b082f0: for each of the 3 physical page nodes,
// if its page owns a populated slot, set that node's visibility to `enable`.
// Stock scanned page 2 over slots 16..19; we scan 16..MAX_PANEL_SLOTS-1 so a
// page whose only populated seats are >= 20 still toggles.
void RefreshPageNodeVisibility(uint32_t* panel, int enable) {
    for (int s = 0; s < 8 && s < MAX_PANEL_SLOTS; ++s)
        if (DisplayId(panel, s) != -1) { SetRichNodeEnabled(PageNode(panel, 0), enable); break; }
    for (int s = 8; s < 16 && s < MAX_PANEL_SLOTS; ++s)
        if (DisplayId(panel, s) != -1) { SetRichNodeEnabled(PageNode(panel, 1), enable); break; }
    for (int s = 16; s < MAX_PANEL_SLOTS; ++s)
        if (DisplayId(panel, s) != -1) { SetRichNodeEnabled(PageNode(panel, 2), enable); break; }
}

// -----------------------------------------------------------------------------
// LEAGUE-selection rebuild (panel[+0x190C] == 0).
// -----------------------------------------------------------------------------
void RebuildLeagueSelection(uint32_t* panel) {
    const uint8_t f44 = PBY(panel, 0x44);

    // Disable the team-selection chrome that the league screen does not use.
    SetScrollCtrlEnabled(PDW(panel, 0x54), 0);   // scroll controller off
    RefreshPageNodeVisibility(panel, f44);        // show populated page nodes
    SetRichNodeEnabled(PDW(panel, 0x88), 0);      // childB off
    if (fn_SetScrollFlag) fn_SetScrollFlag(static_cast<int>(PDW(panel, 0x64)), 0);

    // Hide the 20 stock visual-slot quads (the logo owner redraws them).
    for (int i = 0; i < 20; ++i)
        SetUiNodeEnabled(PDW(panel, 0x98 + i * 4), f44);

    // Reset the 3 page-header name buffers to their defaults (the default
    // template sits 0xC00 bytes past each buffer), then stamp the selected
    // league's name into the current page's header.
    for (int i = 0; i < 3; ++i)
        GameStrNCpy(reinterpret_cast<char*>(PageNameBuf(panel, i)),
                    reinterpret_cast<char*>(PageNameBuf(panel, i) + 0xC00), 0x400);

    if (PIN(panel, 0x1904) == 1) {
        const int slot = SelectedSlot(panel);
        // Only 3 physical page-header buffers exist (pages 0/1/2). Every league
        // slot >= 20 renders on the page-2 node (ParentNodeForLeagueSlot clamps
        // page >= 2 -> 2), so its name belongs on the page-2 header. Without
        // this clamp, currentPage 3+ writes PAST the 3 buffers into a lower
        // page's default-name template (0xFC + page*0x400 aliases
        // PageNameBuf(page-3) + 0xC00), which then leaks onto that page's header
        // on the next refresh.
        int page = PIN(panel, 0xF8);
        if (page > 2) page = 2;
        GameStrNCpy(reinterpret_cast<char*>(PageNameBuf(panel, page)),
                    reinterpret_cast<char*>(SlotName(panel, slot)), 0x400);
    }

    // Push the page-header names onto the 3 page attachments.
    for (int i = 0; i < 3; ++i) {
        const uint32_t attach = PDW(panel, 0x78 + i * 4);
        if (attach) SetAttachmentName(AttachHandle(attach), PageNameBuf(panel, i));
    }

    // Hide the two bone attachments.
    if (fn_SetBoneVisibility) {
        fn_SetBoneVisibility(AttachHandle(PDW(panel, 0x8C)), 0);
        const uint32_t ba2 = PDW(panel, 0x90);
        if (ba2) fn_SetBoneVisibility(AttachHandle(ba2), 0);
    }

    // ── Cursor box (panel[+0x60]) — THE FIX ──────────────────────────────
    // Stock: controlId = table[side*20 + slot], pageNode = panel[0x6C+page*4].
    // For slot >= 20 use the same id the logo uses and clamp the page-node
    // index to the 3 real page nodes (extra pages reuse page 2's node).
    {
        const int side = PIN(panel, 0xEC);
        const int slot = SelectedSlot(panel);
        const int page = PIN(panel, 0xF8);

        const int16_t controlId =
            static_cast<int16_t>(LeagueCursorControlId(side, slot));
        const int pnodeIdx = (slot < 20) ? page : (page > 2 ? 2 : page);
        const uint32_t pageNode = PageNode(panel, pnodeIdx);

        float rect[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (fn_GetBoneWorldPos)
            fn_GetBoneWorldPos(static_cast<int>(pageNode), rect, controlId);

        // Follow the same leagues/<n>.ini control-rect override the logo quad
        // uses, so the cursor tracks overridden (incl. away-side offset)
        // positions instead of the raw OPD control. No-op when the slot has no
        // override.
        ApplyLeagueControlRectOverride(pageNode, slot, rect);

        float scale = 0.0f;
        if (fn_GetBoneScale)
            scale = fn_GetBoneScale(static_cast<int>(pageNode), controlId);

        // Follow the leagues/<n>.ini control_depth override for the cursor's
        // depth ("scale" here is the item Z, not a size). No-op without one.
        ApplyLeagueControlDepthOverride(pageNode, slot, &scale);

        SetItemRect(reinterpret_cast<uint32_t*>(PDW(panel, 0x60)), rect);
        if (fn_SetItemScale)
            fn_SetItemScale(static_cast<int>(PDW(panel, 0x60)), scale);
    }

    // Init the cursor item and the two side display items.
    if (fn_InitDisplayItem) {
        fn_InitDisplayItem(reinterpret_cast<uint32_t*>(PDW(panel, 0x60)),
                           PIN(panel, 0x1900) == 1 ? f44 : 0);
        for (int i = 0; i < 2; ++i)
            fn_InitDisplayItem(reinterpret_cast<uint32_t*>(PDW(panel, 0x58 + i * 4)), 0);
    }
}

// -----------------------------------------------------------------------------
// TEAM-selection rebuild (panel[+0x190C] != 0). Faithful translation of the
// stock body; only the selectable-count offset is made dynamic.
// -----------------------------------------------------------------------------
void RebuildTeamSelection(uint32_t* panel) {
    const uint8_t f44 = PBY(panel, 0x44);

    RefreshPageNodeVisibility(panel, 0);          // hide league page nodes
    SetRichNodeEnabled(PDW(panel, 0x88), f44);    // childB on

    // Stock read panel[0x7614]; slide it to the dynamic selectable-count slot.
    if (PIN(panel, LeagueSelectableCountOffset()) != 1) {
        if (fn_SetScrollFlag)
            fn_SetScrollFlag(static_cast<int>(PDW(panel, 0x64)), f44);
    }

    // Hide the 20 stock visual-slot quads.
    for (int i = 0; i < 20; ++i)
        SetUiNodeEnabled(PDW(panel, 0x98 + i * 4), 0);

    // Bind the scroll controller to the current league's team buffer.
    const int slot = SelectedSlot(panel);
    if (fn_LinkScrollCtrlToSlots)
        fn_LinkScrollCtrlToSlots(reinterpret_cast<int*>(PDW(panel, 0x54)),
                                 reinterpret_cast<int>(PB(panel, 0x1944 + slot * PANEL_SLOT_STRIDE)),
                                 TEAM_SLOT_COUNT);
    SetScrollCtrlEnabled(PDW(panel, 0x54), f44);

    // Colour each of the 20 team cells by its per-(slot,team) expand flag:
    //   flag 0            -> "available"  colour  (panel[+0x50], white)
    //   flag 1 or 2       -> "selected"   colour  (panel[+0x4C], dark)
    //   otherwise         -> leave as-is
    for (int i = 0; i < TEAM_SLOT_COUNT; ++i) {
        const int s = SelectedSlot(panel);
        const int flag = PIN(panel, 0x1994 + s * PANEL_SLOT_STRIDE + i * 4);
        const void* color = nullptr;
        if (flag == 0)                 color = PB(panel, 0x50);
        else if (flag >= 1 && flag <= 2) color = PB(panel, 0x4C);
        if (color) {
            const int cell = GetScrollCell(static_cast<int>(PDW(panel, 0x54)), i);
            SetScrollCellColor(cell, color);
        }
    }

    // Show the primary bone attachment and stamp the selected league name.
    if (fn_SetBoneVisibility)
        fn_SetBoneVisibility(AttachHandle(PDW(panel, 0x8C)), f44);
    SetAttachmentName(AttachHandle(PDW(panel, 0x8C)), SlotName(panel, SelectedSlot(panel)));

    // Secondary attachment: show it and stamp the highlighted team's name.
    const uint32_t ba2 = PDW(panel, 0x90);
    if (ba2) {
        if (fn_SetBoneVisibility) fn_SetBoneVisibility(AttachHandle(ba2), 1);
        const int s   = SelectedSlot(panel);
        const int tp  = TeamPos(panel);
        const uint16_t teamId = *reinterpret_cast<uint16_t*>(
            PB(panel, 0x1944 + s * PANEL_SLOT_STRIDE + tp * 4));
        const void* name = GetTeamName(teamId, 0, 0);
        SetAttachmentName(AttachHandle(ba2), name);
    }

    if (fn_InitDisplayItem)
        fn_InitDisplayItem(reinterpret_cast<uint32_t*>(PDW(panel, 0x60)), 0);

    // Position the two team-grid marker items from their scroll cells.
    const float scaleBias = FloatConst_B7D6BC();
    {
        float vec[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        GetScrollCellWorldPos(static_cast<int>(PDW(panel, 0x54)), vec, PIN(panel, 0x1910));
        SetItemRect(reinterpret_cast<uint32_t*>(PDW(panel, 0x58)), vec);
        if (fn_SetItemScale)
            fn_SetItemScale(static_cast<int>(PDW(panel, 0x58)), PF(panel, 0x3C) + scaleBias);
    }
    {
        float vec[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        GetScrollCellWorldPos(static_cast<int>(PDW(panel, 0x54)), vec, PIN(panel, 0x1928));
        SetItemRect(reinterpret_cast<uint32_t*>(PDW(panel, 0x5C)), vec);
        if (fn_SetItemScale)
            fn_SetItemScale(static_cast<int>(PDW(panel, 0x5C)), PF(panel, 0x3C) + scaleBias);
    }

    // Per-player marker enable: for each player's item, if the player's
    // selected slot matches this cursor's slot AND the item's stored z is a
    // real on-screen position (NOT the off-screen sentinel float 0x00B82ED4)
    // AND the player's flag == 1, enable with f44; otherwise disable. Mirrors
    // the stock fp-compare + jnp at 0x00B08FC4: pos[2] == sentinel takes the
    // DISABLE branch, so the enable case is the inequality (which also covers
    // the unordered/NaN case, exactly as jnp does).
    for (int k = 0; k < 2; ++k) {
        uint32_t* item = reinterpret_cast<uint32_t*>(PDW(panel, 0x58 + k * 4));
        const int cmpSlot = PIN(panel, 0x1914 + k * 0x18);   // player k selected slot
        int enable = 0;
        if (SelectedSlot(panel) == cmpSlot) {
            float pos[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            GetItemRect(item, pos);
            const int playerFlag = PIN(panel, 0x1918 + k * 0x18);
            if (pos[2] != FloatConst_B82ED4() && playerFlag == 1)
                enable = f44;
        }
        if (fn_InitDisplayItem) fn_InitDisplayItem(item, enable);
    }
}

// Clear the 8 per-player input-flag arrays for this panel's id (panel[0]).
void ClearInputFlags(uint32_t* panel) {
    const int idx4 = static_cast<int>(PDW(panel, 0)) << 2;
    static const uintptr_t kFlagArrays[8] = {
        0x03b49140, 0x03b49160, 0x03b49180, 0x03b491a0,
        0x03b491c0, 0x03b491e0, 0x03b49200, 0x03b49220,
    };
    for (uintptr_t base : kFlagArrays)
        *reinterpret_cast<int*>(base + idx4) = 0;
}

}  // namespace

// =============================================================================
// hook_PanelRefresh — replacement for FUN_00b08b40.
// Called as FUN_00b08b40(subObj); subObj[0] is the panel pointer.
// =============================================================================
uint32_t __cdecl hook_PanelRefresh(void** subObj) {
    if (!subObj) return 0;
    uint32_t* panel = reinterpret_cast<uint32_t*>(subObj[0]);
    if (!panel) return 0;

    // First refresh: build the logo/visual slots (goes through our own
    // BuildLeaguePanelVisualSlots replacement, exactly as the detoured
    // stock `call 0x00b083b0` would).
    if (PDW(panel, 0x94) == 0)
        hook_BuildLeaguePanelVisualSlots_C(panel);

    if (PIN(panel, 0x48) == 1) {
        if (PIN(panel, 0x190C) == 0)
            RebuildLeagueSelection(panel);
        else
            RebuildTeamSelection(panel);

        // The stock rebuilds above only show/hide the 20 stock logo nodes at
        // panel[+0x98..]. The extra logo quads (slots >= 20) live in a sidecar
        // array, so mirror the same show/hide here — visible while browsing
        // leagues, hidden in team mode — otherwise they draw in every state
        // (including after a league is chosen).
        UpdateExtraLeagueVisualSlotVisibility(panel);
    }

    PIN(panel, 0x48) = 0;
    ClearInputFlags(panel);
    return 0;
}
