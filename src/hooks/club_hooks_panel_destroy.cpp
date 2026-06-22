// club_hooks_panel_destroy.cpp — hooked destructor for FUN_00b0ac50
//
// The original destructor reads the subObj pointer from the FIXED offset
// param_1[0x1D87] (byte offset 0x761C).  When MAX_PANEL_SLOTS > 20,
// the slot region grows and the true trailer shifts by
// (MAX_PANEL_SLOTS - 20) * 0x4A4 bytes.
//
// This hook reads the subObj from the CORRECT dynamic offset using
// PanelTrailerB_DwordIdx() from club_hooks_common.h.
//
// All other operations are reproduced exactly from the Ghidra pseudocode
// of FUN_00b0ac50.  The 20-iteration resource loop at param_1+0x26
// operates on header-region data (byte offsets 0x94..0xE4) and is
// unrelated to MAX_PANEL_SLOTS — it stays at 20.

#include "club_hooks_common.h"
#include "../utils/logger.h"

// Trampoline is defined in club_hooks_register.cpp,
// declared extern in club_hooks_common.h.

// ---------------------------------------------------------------------------
// hook_DestroyPanel — replacement for FUN_00b0ac50
// ---------------------------------------------------------------------------
void __cdecl hook_DestroyPanel(uint32_t* param_1)
{
    if (!param_1)
        return;

    Logger::Log("[PanelDestroy] Destroying panel at 0x%08X", (uintptr_t)param_1);
    ClearExtraLeagueVisualSlotNodes(param_1);

    // ── Step 1: Pre-destruction cleanup ──────────────────────────────────
    // Ghidra: FUN_00b09b20(param_1)
    if (fn_PreDestroyCleanup)
        fn_PreDestroyCleanup(param_1);

    // ── Step 2: Destroy scroll controller ────────────────────────────────
    // Ghidra: FUN_00b0f400((undefined4*)param_1[0x15])
    // param_1[0x15] = byte offset 0x54
    if (fn_DestroyScrollCtrl)
        fn_DestroyScrollCtrl(reinterpret_cast<uint32_t*>(param_1[0x15]));

    // ── Step 3: Destroy scroll list ──────────────────────────────────────
    // Ghidra: FUN_00b00d10((undefined4*)param_1[0x19])
    // param_1[0x19] = byte offset 0x64
    if (fn_DestroyScrollList)
        fn_DestroyScrollList(reinterpret_cast<uint32_t*>(param_1[0x19]));

    // ── Step 4: Destroy 2 display items at [0x58] and [0x5C] ────────────
    // Ghidra: puVar2 = param_1 + 0x16; iVar1 = 2;
    //   do { FUN_00b00400(*puVar2); puVar2++; iVar1--; } while (iVar1 != 0);
    {
        uint32_t* puVar2 = param_1 + 0x16;
        for (int i = 0; i < 2; i++, puVar2++)
        {
            if (fn_DestroyDisplayItem)
                fn_DestroyDisplayItem(*puVar2);
        }
    }

    // ── Step 5: Destroy display item at [0x60] ──────────────────────────
    // Ghidra: FUN_00b00400(param_1[0x18])
    if (fn_DestroyDisplayItem)
        fn_DestroyDisplayItem(param_1[0x18]);

    // ── Step 6: Destroy 3 node entries at [0x6C], [0x70], [0x74] ────────
    // Ghidra: puVar2 = param_1 + 0x1b; iVar1 = 3;
    //   do {
    //     if (puVar2[3] != 0) FUN_009553f0(puVar2[3]);  // bone attachment
    //     if (*puVar2 != 0)   FUN_009502b0(*puVar2);     // node child
    //     puVar2++; iVar1--;
    //   } while (iVar1 != 0);
    //
    // puVar2 walks: [0x1b],[0x1c],[0x1d] → byte offsets 0x6C,0x70,0x74
    // puVar2[3] reads: [0x1e],[0x1f],[0x20] → byte offsets 0x78,0x7C,0x80
    {
        uint32_t* puVar2 = param_1 + 0x1B;
        for (int i = 0; i < 3; i++, puVar2++)
        {
            if (puVar2[3] != 0)
            {
                if (fn_DestroyBoneAttach)
                    fn_DestroyBoneAttach(reinterpret_cast<uint32_t*>(puVar2[3]));
            }
            if (*puVar2 != 0)
            {
                if (fn_DestroyNodeChild)
                    fn_DestroyNodeChild(reinterpret_cast<int*>(*puVar2));
            }
        }
    }

    // ── Step 7: Destroy primary child node at [0x84] ────────────────────
    // Ghidra: if (param_1[0x21] != 0) FUN_009502b0(param_1[0x21])
    if (param_1[0x21] != 0)
    {
        if (fn_DestroyNodeChild)
            fn_DestroyNodeChild(reinterpret_cast<int*>(param_1[0x21]));
    }

    // ── Step 8: Destroy extra bone attachment at [0x90] ─────────────────
    // Ghidra: if (param_1[0x24] != 0) FUN_009553f0(param_1[0x24])
    // Only populated when side==0 during creation
    if (param_1[0x24] != 0)
    {
        if (fn_DestroyBoneAttach)
            fn_DestroyBoneAttach(reinterpret_cast<uint32_t*>(param_1[0x24]));
    }

    // ── Step 9: Destroy bone attachment at [0x8C] ───────────────────────
    // Ghidra: FUN_009553f0(param_1[0x23])
    // Note: called unconditionally (no null check in original)
    if (fn_DestroyBoneAttach)
        fn_DestroyBoneAttach(reinterpret_cast<uint32_t*>(param_1[0x23]));

    // ── Step 10: Destroy secondary render node at [0x88] ────────────────
    // Ghidra: FUN_009502b0(param_1[0x22])
    // Note: called unconditionally (no null check in original)
    if (fn_DestroyNodeChild)
        fn_DestroyNodeChild(reinterpret_cast<int*>(param_1[0x22]));

    // ── Step 11: Resource release loop (header area, always 20) ─────────
    // Ghidra: puVar2 = param_1 + 0x26; iVar1 = 0x14;
    //   do {
    //     if (*puVar2 != 0) {
    //       FUN_0094f420(param_1[0x25], *puVar2);
    //       *puVar2 = 0;
    //     }
    //     if (param_1[0x25] != 0) {
    //       FUN_0094f780(param_1[0x25]);
    //       param_1[0x25] = 0;
    //     }
    //     puVar2++; iVar1--;
    //   } while (iVar1 != 0);
    //
    // This walks 20 dwords at byte offsets 0x98..0xE4 plus the owner
    // at 0x94.  This is a HEADER array — NOT the slot array.
    // The count 0x14 is unrelated to MAX_PANEL_SLOTS.
    {
        uint32_t* puVar2 = param_1 + 0x26;
        for (int i = 0; i < 0x14; i++, puVar2++)
        {
            if (*puVar2 != 0)
            {
                if (fn_ReleaseResource)
                    fn_ReleaseResource(reinterpret_cast<int*>(param_1[0x25]),
                                       reinterpret_cast<uint32_t*>(*puVar2));
                *puVar2 = 0;
            }
            if (param_1[0x25] != 0)
            {
                if (fn_FinalizeResOwner)
                    fn_FinalizeResOwner(reinterpret_cast<int*>(param_1[0x25]));
                param_1[0x25] = 0;
            }
        }
    }

    // ── Step 12: Free sub-object (DYNAMIC trailer offset) ────────────────
    // Original: FUN_00b0bb60(param_1[0x1D87]);
    //           FUN_00b0bb80((undefined4*)param_1[0x1D87]);
    //
    // 0x1D87 = byte offset 0x761C = original trailer B for 20 slots.
    // When MAX_PANEL_SLOTS > 20, the true subObj pointer is at the
    // shifted offset.  We use the dynamic index from the header helper.
    {
        int subObjIdx = PanelTrailerB_DwordIdx(MAX_PANEL_SLOTS);
        uint32_t subObjVal = param_1[subObjIdx];

        Logger::Log("[PanelDestroy] SubObj at dword[0x%X] (byte 0x%X) = 0x%08X",
                    subObjIdx, subObjIdx * 4, subObjVal);

        // FUN_00b0bb60(subObjVal) — pre-free (passes the value, not pointer)
        if (fn_PreFreeSubObj)
            fn_PreFreeSubObj(subObjVal);

        // FUN_00b0bb80((undefined4*)subObjVal) — free (dereferences as pointer)
        if (fn_FreeSubObj)
            fn_FreeSubObj(reinterpret_cast<uint32_t*>(subObjVal));
    }

    // ── Step 13: Free the panel allocation itself ────────────────────────
    // Ghidra: FUN_0045bc50(param_1)
    if (fn_GameFree)
        fn_GameFree(param_1);

    Logger::Log("[PanelDestroy] Panel freed.");
}
