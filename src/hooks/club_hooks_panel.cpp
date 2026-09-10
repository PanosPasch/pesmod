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

// club_hooks_panel.cpp — precise rewrite from x32dbg disassembly of FUN_00b09050
//
// Every offset, branch, and call order verified against the raw ASM.
// The assembly is the single source of truth; Ghidra pseudocode used only
// as a cross-reference where it agrees with the ASM.
//
// SLOT SCALING NOTES (MAX_PANEL_SLOTS > 20):
// -------------------------------------------
// The original panel struct layout:
//   [0x0000 .. 0x1943]  header fields
//   [0x1944 .. 0x7617]  20 slot entries, each 0x4A4 bytes
//                        slot i starts at 0x1944 + i * 0x4A4
//                        slot i name field at 0x19E4 + i * 0x4A4
//   [0x7618]            trailer field A (zeroed)
//   [0x761C]            trailer field B (subObj pointer)
//
// When MAX_PANEL_SLOTS > 20, the slot region grows and pushes the true
// trailer to the end of the larger allocation.  However, the game's
// destructor and other unhooked functions read the trailer at the FIXED
// offsets 0x7618 / 0x761C.
//
// Strategy: write the subObj pointer at BOTH the true (shifted) offset
// AND the original fixed offset 0x761C.  The overlap with slot 20's
// data region is managed by writing the mirror as the LAST operation,
// after all slot initialisation.  The overlapping bytes in slot 20's
// name field (offset +0x88 within the name, well past any realistic
// league name length) are sacrificed.  This is safe because:
//   - Slot 20's name area at that offset holds sentinel/zero data
//   - The game destructor only reads 0x761C as a pointer, not as a name
//   - Name population of slot 20 starts at offset 0 of the name and
//     typical league names are <64 chars, nowhere near byte 0x88

#include "club_hooks_common.h"
#include "../utils/logger.h"

FN_CreateLeagueSelectionPanel_t orig_CreateLeagueSelectionPanel = nullptr;

// ---------------------------------------------------------------------------
// Runtime globals read by the original ASM from fixed addresses.
// ---------------------------------------------------------------------------
static inline uint32_t ReadSlotNameSentinel() {
    return *reinterpret_cast<uint32_t*>(0x00BB5DA8);
}
static inline uint8_t ReadSlotFlagByte() {
    return *reinterpret_cast<uint8_t*>(0x00BB5DAC);
}
static inline float ReadFloatConst_B7D6BC() {
    return *reinterpret_cast<float*>(0x00B7D6BC);
}

static const int DISPLAY_SUB_OBJ_VTABLE = 0x00B08B40;
static const int DISPLAY_SUB_OBJ_STRING = 0x00BB5DB0;

// ---------------------------------------------------------------------------
// Trailer offset A computation (not in header — only needed by creator).
// Trailer B uses PanelTrailerB_ByteOff() from club_hooks_common.h.
// ---------------------------------------------------------------------------
static inline int TrailerOffsetA(int slotCount) {
    return PANEL_ORIG_TRAILER_A + (slotCount - 20) * PANEL_SLOT_STRIDE;
}

uint32_t* __cdecl hook_CreateLeagueSelectionPanel(uint32_t* param_1,
                                                    uint32_t* param_2,
                                                    float*    param_3)
{
    float local_20 = 0.0f;
    float local_1c = 0.0f;
    uint32_t local_18_raw = 0;

    float localBoundsA = 0.0f;
    float localBoundsB = 0.0f;

    // ── Resolve side flag (ebp) ──────────────────────────────────────────
    uint32_t* sidePtr = param_1;
    if (sidePtr == nullptr)
        sidePtr = reinterpret_cast<uint32_t*>(2);

    int side = static_cast<int>(reinterpret_cast<uintptr_t>(sidePtr));

    if (side < 0 || side > 2)
    {
        Logger::Log("[PanelCreate] Unexpected side=%d, falling through to original.", side);
        return orig_CreateLeagueSelectionPanel
            ? orig_CreateLeagueSelectionPanel(param_1, param_2, param_3)
            : nullptr;
    }

    Logger::Log("[PanelCreate] side=%d slots=%d", side, MAX_PANEL_SLOTS);

    // ── Step 1: Allocate panel memory ────────────────────────────────────
    int allocSize = PANEL_BASE_ALLOC + (MAX_PANEL_SLOTS - 20) * PANEL_SLOT_STRIDE;

    uint32_t* panel = fn_GameAlloc ? fn_GameAlloc(1, allocSize) : nullptr;
    if (!panel)
    {
        Logger::Log("[PanelCreate] Allocation failed (%d bytes).", allocSize);
        return nullptr;
    }
    memset(panel, 0, allocSize);
    Logger::Log("[PanelCreate] Allocated %d bytes at 0x%08X", allocSize, (uintptr_t)panel);

    uint8_t* base = reinterpret_cast<uint8_t*>(panel);

    auto DW = [&](int off) -> uint32_t& {
        return *reinterpret_cast<uint32_t*>(base + off);
    };
    auto F = [&](int off) -> float& {
        return *reinterpret_cast<float*>(base + off);
    };
    auto BY = [&](int off) -> uint8_t& {
        return *reinterpret_cast<uint8_t*>(base + off);
    };

    // Compute trailer offsets for this slot count
    int trailerA = TrailerOffsetA(MAX_PANEL_SLOTS);
    int trailerB = PanelTrailerB_ByteOff(MAX_PANEL_SLOTS);

    // ── Step 2: Create display sub-object ────────────────────────────────
    // ASM pushes 6 arguments; the typedef has only 5.  Cast to 6-param.
    typedef uint32_t* (__cdecl *FN_CreateDisplaySubObject6)(
        int, int, int, int, int, int);
    FN_CreateDisplaySubObject6 fn_CreateSubObj6 =
        reinterpret_cast<FN_CreateDisplaySubObject6>(
            reinterpret_cast<void*>(fn_CreateDisplaySubObject));

    uint32_t* subObj = fn_CreateSubObj6
        ? fn_CreateSubObj6(0, DISPLAY_SUB_OBJ_VTABLE, 0, 4, 0, DISPLAY_SUB_OBJ_STRING)
        : nullptr;

    // Write subObj at the true trailer offset
    DW(trailerB) = reinterpret_cast<uint32_t>(subObj);

    if (fn_InitDisplaySubObject && subObj)
        fn_InitDisplaySubObject(reinterpret_cast<int>(subObj));

    // ── Step 3: Slot initialisation loop ─────────────────────────────────
    {
        uint32_t nameSentinel = ReadSlotNameSentinel();
        uint8_t  flagByte    = ReadSlotFlagByte();

        uint8_t* namePtr = base + 0x19E4;

        for (int i = 0; i < MAX_PANEL_SLOTS; i++)
        {
            uint32_t* teamIDs = reinterpret_cast<uint32_t*>(namePtr - 0xA0);
            for (int j = 0; j < TEAM_SLOT_COUNT; j++)
                teamIDs[j] = 0xFFFF;

            *reinterpret_cast<uint32_t*>(namePtr) = nameSentinel;
            namePtr[4] = flagByte;
            *reinterpret_cast<uint32_t*>(namePtr + 0x400) = 0xFFFFFFFF;

            namePtr += PANEL_SLOT_STRIDE;
        }
    }
    Logger::Log("[PanelCreate] %d slots initialised.", MAX_PANEL_SLOTS);

    // ── Step 4: Header fields ────────────────────────────────────────────
    DW(0x0000) = reinterpret_cast<uint32_t>(param_2);
    DW(0x0040) = 0;
    DW(0x0044) = 1;
    DW(0x00E8) = 1;
    DW(0x1900) = 1;
    DW(0x1904) = 1;
    DW(0x1908) = 0;
    DW(0x190C) = 0;
    DW(0x1918) = 1;
    DW(0x1930) = 1;
    DW(0x1940) = 0;

    // DW(0x0040) = 0;
    // DW(0x0044) = 1;
    // DW(0x00E8) = 1;
    // DW(0x1900) = 1;
    // DW(0x1904) = 1;
    // DW(0x1908) = 0;
    // DW(0x190C) = 0;
    // DW(0x1918) = 1;
    // DW(0x1930) = 1;
    // DW(0x1940) = 0

    // Trailer field A at true offset
    DW(trailerA) = 0;

    // ── Step 5: Scroll direction switch ──────────────────────────────────
    // Ghidra (authoritative for branch structure):
    //   side==0: F0=5, F4=4   (default — overridable via leagues/setup.ini)
    //   side==1 or 2: F0=4, F4=5
    // These are the panel column / row counts for the team grid. They're
    // exposed as globals so leagues/setup.ini can override them — the
    // defaults match the stock binary so a missing setup.ini changes
    // nothing.
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

    localBoundsA = 36.0f;
    localBoundsB = 36.0f;

    // ── Step 6: Store side and z-depth ───────────────────────────────────
    DW(0x00EC) = static_cast<uint32_t>(side);
    DW(0x003C) = *reinterpret_cast<uint32_t*>(&param_3[2]);

    // ── Step 7: Create render node ───────────────────────────────────────
    uint32_t renderNode = fn_CreateRenderNode ? fn_CreateRenderNode(0x10385) : 0;
    DW(0x0068) = renderNode;

    // ── Step 8: Primary texture child (childA at [esi+0x84]) ─────────────
    {
        uint32_t texA = *reinterpret_cast<uint32_t*>(TABLE_TEXTURE_A + side * 4);
        uint32_t childA = fn_GetChildNode ? fn_GetChildNode(renderNode) : 0;
        if (fn_SetNodeTexture) fn_SetNodeTexture(childA, texA);
        if (fn_NodePropA)      fn_NodePropA(childA, 0);
        if (fn_NodePropC)      fn_NodePropC(childA, 2);
        if (fn_NodePropB)      fn_NodePropB(childA, 1);
        if (fn_NodePropD)      fn_NodePropD(childA, 0);
        DW(0x0084) = childA;
    }

    // ── Step 9: Three-node loop ([esi+0x6C], [0x70], [0x74]) ─────────────
    // TABLE_NODE_DATA indexed by side*12:
    //   ASM: lea edx,[ebp+ebp*2] / lea eax,[edx*4+E860E8]
    {
        uint32_t* tablePtr   = reinterpret_cast<uint32_t*>(TABLE_NODE_DATA + side * 12);
        uint32_t* nodeSlotDW = reinterpret_cast<uint32_t*>(base + 0x6C);

        for (int i = 0; i < 3; i++)
        {
            uint32_t texID = tablePtr[i];

            if (texID != 0xFFFFFFFF)
            {
                uint32_t child = fn_GetChildNode ? fn_GetChildNode(DW(0x0068)) : 0;
                if (fn_SetNodeTexture) fn_SetNodeTexture(child, texID);
                if (fn_NodePropA)      fn_NodePropA(child, 0);
                if (fn_NodePropC)      fn_NodePropC(child, 0);
                if (fn_NodePropB)      fn_NodePropB(child, 0);
                if (fn_NodePropD)      fn_NodePropD(child, 0);
                nodeSlotDW[i] = child;
                Logger::Log("[CUSTOM PANEL C] TexID %d", i);
            }

            float posVec[4];
            posVec[0] = param_3[0];
            posVec[1] = param_3[1];
            *reinterpret_cast<uint32_t*>(&posVec[2]) = DW(0x003C);
            *reinterpret_cast<uint32_t*>(&posVec[3]) = 0x3F800000;

            Logger::Log("[CUSTOM PANEL C] POS VEC %f %f %f %f", posVec[0], posVec[1], posVec[2], posVec[3]);
            if (fn_SetNodePosition)
                fn_SetNodePosition(nodeSlotDW[i], posVec);
            
        }
    }

    // ── Step 10: Secondary render node (childB at [esi+0x88]) ───Team Container Panel Position────
    {
        uint32_t texB = *reinterpret_cast<uint32_t*>(TABLE_TEXTURE_B + side * 4);
        uint32_t childB = fn_GetChildNode ? fn_GetChildNode(DW(0x0068)) : 0;
        if (fn_SetNodeTexture) fn_SetNodeTexture(childB, texB);
        if (fn_NodePropA)      fn_NodePropA(childB, 0);
        if (fn_NodePropC)      fn_NodePropC(childB, 2);
        if (fn_NodePropB)      fn_NodePropB(childB, 0);
        if (fn_NodePropD)      fn_NodePropD(childB, 0);
        DW(0x0088) = childB;

        //ASM pushes param_3 directly (ebx = param_3)
        if (fn_SetNodePosition)
            fn_SetNodePosition(childB, param_3);
    }

    // ── Step 11: Bone attachment (TABLE_TEXTURE_C[side]) ─────────────────
    {
        int32_t animID = *reinterpret_cast<int32_t*>(TABLE_TEXTURE_C + side * 4);
        uint32_t* boneAttach = nullptr;

        if (animID >= 0)
        {
            uint32_t bh = fn_LookupBone ? fn_LookupBone(DW(0x0088), animID) : 0;
            int* bp      = fn_ResolveBone ? fn_ResolveBone(bh) : nullptr;
            if (bp)
                boneAttach = fn_AttachBone
                    ? fn_AttachBone(DW(0x0088), bp, animID) : nullptr;
        }
        DW(0x008C) = reinterpret_cast<uint32_t>(boneAttach);

        if (fn_SetBoneVisibility && boneAttach)
            fn_SetBoneVisibility(boneAttach[3], 0);
    }

    // ── Step 12: Scroll list from bone A ───L1 - R1 side buttons───────────
    {
        float localVec[4] = {};

        int16_t boneIDA = *reinterpret_cast<int16_t*>(TABLE_BONE_ID_A + side * 4);
        if (fn_GetBoneWorldPos)
            fn_GetBoneWorldPos(static_cast<int>(DW(0x0088)), localVec, boneIDA);
        // if (fn_GetBoneWorldPos)
        //     orig_GetBoneWorldPosition(static_cast<int>(DW(0x0088)), localVec, boneIDA);

        uint32_t scrollCount = (side == 0) ? 10 : 2;
        
        uint32_t* scrollList = fn_CreateScrollList
            ? fn_CreateScrollList(scrollCount, reinterpret_cast<int>(localVec))
            : nullptr;
        DW(0x0064) = reinterpret_cast<uint32_t>(scrollList);

        float floatConst = ReadFloatConst_B7D6BC();
        

        if (fn_SetScrollScale && scrollList)
            fn_SetScrollScale(reinterpret_cast<int>(scrollList),
                              F(0x003C) - floatConst);
        if (fn_SetScrollFlag && scrollList)
            fn_SetScrollFlag(reinterpret_cast<int>(scrollList), '\0');
    }

    // ── Step 13: Side==0 extra bone attachment ───────────────────────────
    if (side == 0)
    {
        uint32_t* ba2 = nullptr;
        uint32_t bh2  = fn_LookupBone ? fn_LookupBone(DW(0x0088), 9) : 0;
        int* bp2       = fn_ResolveBone ? fn_ResolveBone(bh2) : nullptr;
        if (bp2)
            ba2 = fn_AttachBone ? fn_AttachBone(DW(0x0088), bp2, 9) : nullptr;
        DW(0x0090) = reinterpret_cast<uint32_t>(ba2);
        if (fn_SetBoneVisibility && ba2)
            fn_SetBoneVisibility(ba2[3], 0);
    }

    // ── Step 14: Finalise panel layout ───────────────────────────────────
    if (fn_FinalisePanelLayout) fn_FinalisePanelLayout(panel);

    // ── Step 15: Pagination scroll controller ────  Team Panel Location  ─────────
    {
        float localVec[4] = {};
        int16_t boneIDB = *reinterpret_cast<int16_t*>(TABLE_BONE_ID_B + side * 4);
        if (fn_GetBoneWorldPos)
            fn_GetBoneWorldPos(static_cast<int>(DW(0x0088)), localVec, boneIDB);
        // if (fn_GetBoneWorldPos)
        //     orig_GetBoneWorldPosition(static_cast<int>(DW(0x0088)), localVec, boneIDB);
        
        uint32_t* scrollCtrl = fn_CreateScrollController
            ? fn_CreateScrollController(2, localVec, DW(0x00F0), DW(0x00F4))
            : nullptr;
        DW(0x0054) = reinterpret_cast<uint32_t>(scrollCtrl);

        if (fn_SetScrollCtrlScale && scrollCtrl)
            fn_SetScrollCtrlScale(reinterpret_cast<int*>(scrollCtrl), F(0x003C));

        if (fn_LinkScrollCtrlToSlots && scrollCtrl)
        {
            fn_LinkScrollCtrlToSlots(
                reinterpret_cast<int*>(scrollCtrl),
                reinterpret_cast<int>(base + 0x1944),
                MAX_PANEL_SLOTS);
            Logger::Log("[PanelCreate] Scroll ctrl linked: slotBase=0x%08X count=%d",
                (uintptr_t)(base + 0x1944), MAX_PANEL_SLOTS);
        }

        // NOTE: ApplyPanelLayoutOverrides() was previously called here.
        // It's been disabled because THIS hook builds the TEAM-SELECTION
        // panel (Screen 3 — the per-league team grid), NOT the league-
        // selection panel (Screen 1/2) where the [panel] keys in
        // leagues/<n>.ini should take effect. The team panel always has
        // exactly TEAM_SLOT_COUNT (20) cells driven by g_PanelF*_Side*.
        // The correct league-panel creation site is still being identified
        // — see docs/INTERNALS.md.

        if (fn_SetScrollBounds && scrollCtrl)
            fn_SetScrollBounds(reinterpret_cast<int*>(scrollCtrl),
                               localBoundsB, localBoundsA);
    }

    // ── Step 16: Colour / alpha bytes ────────────────────────────────────
    BY(0x4C) = 0x20;
    BY(0x4D) = 0x20;
    BY(0x4E) = 0x20;
    BY(0x4F) = 0xFF;
    BY(0x50) = 0xFF;
    BY(0x51) = 0xFF;
    BY(0x52) = 0xFF;
    BY(0x53) = 0xFF;

    // ── Step 17: Display items ───────────────────────────────────────────
    {
        float floatConst = ReadFloatConst_B7D6BC();
        uint32_t* itemSlotPtr = reinterpret_cast<uint32_t*>(base + 0x58);

        for (int i = 0; i < 2; i++)
        {
            uint32_t* item = fn_CreateDisplayItem ? fn_CreateDisplayItem(8) : nullptr;
            itemSlotPtr[i] = reinterpret_cast<uint32_t>(item);
            if (fn_InitDisplayItem && item) fn_InitDisplayItem(item, 0);
        }

        uint32_t* item3 = fn_CreateDisplayItem ? fn_CreateDisplayItem(8) : nullptr;
        DW(0x0060) = reinterpret_cast<uint32_t>(item3);
        if (fn_SetItemScale && item3)
            fn_SetItemScale(reinterpret_cast<int>(item3), F(0x003C) + floatConst);
        if (fn_InitDisplayItem && item3) fn_InitDisplayItem(item3, 0);
    }

    // ── Step 18: Register in global panel registry ───────────────────────
    // Pass subObj directly (not re-reading from panel memory) to avoid
    // any aliasing issues with the trailer offset.
    {
        uint32_t* regSlot = fn_GetPanelRegistrySlot
            ? fn_GetPanelRegistrySlot(subObj) : nullptr;
        if (regSlot) *regSlot = reinterpret_cast<uint32_t>(panel);
    }

    // ── Final trailer mirror (MUST be last) ──────────────────────────────
    // When MAX_PANEL_SLOTS > 20, slot 20's data region overlaps the
    // original trailer offsets.  We write the subObj pointer at the
    // ORIGINAL fixed offset 0x761C so that unpatched game functions
    // (destructor at ~B0C250, display refresh, etc.) can find it.
    //
    // This overwrites bytes within slot 20's name field at relative
    // offset +0x88, which is well past any realistic league name length.
    // The mirror must be written AFTER all slot initialisation to ensure
    // it isn't clobbered by sentinel writes.
    if (MAX_PANEL_SLOTS != 20)
    {
        DW(PANEL_ORIG_TRAILER_B) = reinterpret_cast<uint32_t>(subObj);
        DW(PANEL_ORIG_TRAILER_A) = 0;
        Logger::Log("[PanelCreate] Mirror trailer written at original offsets "
                    "0x%04X/0x%04X (true at 0x%04X/0x%04X)",
                    PANEL_ORIG_TRAILER_A, PANEL_ORIG_TRAILER_B, trailerA, trailerB);
    }

    Logger::Log("[PanelCreate] Complete. handle=0x%08X", (uintptr_t)panel);
    return panel;
}
