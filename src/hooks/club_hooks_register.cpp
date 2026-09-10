// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 Panagiotis Paschalis
//
// This file is part of PESMod.
//
// PESMod is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// PESMod is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along
// with PESMod.  If not, see <https://www.gnu.org/licenses/>.

// =============================================================================
// club_hooks_register.cpp
//
// Defines all function pointer variables (declared extern in club_hooks_common.h)
// and implements ClubHooks::Register() which resolves all pointers and
// installs all MinHook hooks.
//
// To add a new hook:
//   1. Declare the typedef and extern in club_hooks_common.h
//   2. Define the pointer here (initialised to nullptr)
//   3. Assign the address in the "Resolve pointers" block below
//   4. Add MH_CreateHook / MH_EnableHook in the "Install hooks" block
// =============================================================================

#include "club_hooks.h"
#include "club_hooks_common.h"
#include "MinHook/include/MinHook.h"
#include "../utils/logger.h"

// =============================================================================
// Function pointer definitions — one per extern in club_hooks_common.h
// =============================================================================

FN_GetLeagueSelector    fn_GetLeagueSelector    = nullptr;
FN_GetSelectedCupIdx    fn_GetSelectedCupIdx    = nullptr;
FN_GetLeagueString      fn_GetLeagueString      = nullptr;
FN_GetStringByHandle    fn_GetStringByHandle    = nullptr;
FN_SetTeamList          fn_SetTeamList          = nullptr;
FN_SetSelectedTeam      fn_SetSelectedTeam      = nullptr;
FN_SetNavState          fn_SetNavState          = nullptr;
FN_PostLoadUpdate       fn_PostLoadUpdate       = nullptr;
FN_DisplayReset         fn_DisplayReset         = nullptr;
FN_SetLabel             fn_SetLabel             = nullptr;
FN_SetPage              fn_SetPage              = nullptr;
FN_GetModeFlag          fn_GetModeFlag          = nullptr;
FN_IsTeamUnlocked       fn_IsTeamUnlocked       = nullptr;
FN_IsTeamAvail          fn_AvailSide0           = nullptr;
FN_IsTeamAvail          fn_AvailSide1           = nullptr;
FN_LoadLeagueLogo       fn_LoadLeagueLogo       = nullptr;
FN_LoadUIResource       fn_LoadUIResource       = nullptr;
FN_InitClubDisplayState fn_InitClubDisplayState = nullptr;
FN_CreateDisplayPanel   fn_CreateDisplayPanel   = nullptr;
FN_ShowPanel            fn_ShowPanel            = nullptr;
FN_TransformNode        fn_TransformNode        = nullptr;
FN_GetBoneWorldPos      fn_GetBoneWorldPos      = nullptr;
FN_GetBoneScale         fn_GetBoneScale         = nullptr;
FN_GetPlayerState       fn_GetPlayerState       = nullptr;
FN_InitPlayerState      fn_InitPlayerState      = nullptr;
FN_ResetPlayerSelection fn_ResetPlayerSelection = nullptr;
FN_SetPlayerActive      fn_SetPlayerActive      = nullptr;
FN_SetPlayerInputMode   fn_SetPlayerInputMode   = nullptr;
FN_SetPlayerCursorType  fn_SetPlayerCursorType  = nullptr;
FN_SetNavContext         fn_SetNavContext        = nullptr;
FN_LinkDisplayObjects   fn_LinkDisplayObjects   = nullptr;
FN_MLClubScreenInit     fn_MLClubScreenInit     = nullptr;
FN_SetupNavMapping      fn_SetupNavMapping      = nullptr;
FN_TimeGetTime          fn_TimeGetTime          = nullptr;
FN_GameAlloc            fn_GameAlloc            = nullptr;
FN_CreateDisplaySubObject fn_CreateDisplaySubObject = nullptr;
FN_InitDisplaySubObject fn_InitDisplaySubObject = nullptr;
FN_CreateRenderNode     fn_CreateRenderNode     = nullptr;
FN_GetChildNode         fn_GetChildNode         = nullptr;
FN_SetNodeTexture       fn_SetNodeTexture       = nullptr;
FN_NodePropA            fn_NodePropA            = nullptr;
FN_NodePropB            fn_NodePropB            = nullptr;
FN_NodePropC            fn_NodePropC            = nullptr;
FN_NodePropD            fn_NodePropD            = nullptr;
FN_SetNodePosition      fn_SetNodePosition      = nullptr;
FN_LookupBone           fn_LookupBone           = nullptr;
FN_ResolveBone          fn_ResolveBone          = nullptr;
FN_AttachBone           fn_AttachBone           = nullptr;
FN_SetBoneVisibility    fn_SetBoneVisibility    = nullptr;
FN_CreateScrollList     fn_CreateScrollList     = nullptr;
FN_SetScrollScale       fn_SetScrollScale       = nullptr;
FN_SetScrollFlag        fn_SetScrollFlag        = nullptr;
FN_FinalisePanelLayout  fn_FinalisePanelLayout  = nullptr;
FN_CreateScrollController fn_CreateScrollController = nullptr;
FN_SetScrollCtrlScale   fn_SetScrollCtrlScale   = nullptr;
FN_LinkScrollCtrlToSlots fn_LinkScrollCtrlToSlots = nullptr;
FN_SetScrollBounds      fn_SetScrollBounds      = nullptr;
FN_SetCellPosition      fn_SetCellPosition      = nullptr;
FN_CreateDisplayItem    fn_CreateDisplayItem    = nullptr;
FN_InitDisplayItem      fn_InitDisplayItem      = nullptr;
FN_SetItemScale         fn_SetItemScale         = nullptr;
FN_GetPanelRegistrySlot fn_GetPanelRegistrySlot = nullptr;

// Panel destructor sub-functions
FN_PreDestroyCleanup     fn_PreDestroyCleanup   = nullptr;
FN_DestroyScrollCtrl     fn_DestroyScrollCtrl   = nullptr;
FN_DestroyScrollList     fn_DestroyScrollList   = nullptr;
FN_DestroyDisplayItem    fn_DestroyDisplayItem  = nullptr;
FN_DestroyBoneAttach     fn_DestroyBoneAttach   = nullptr;
FN_DestroyNodeChild      fn_DestroyNodeChild    = nullptr;
FN_ReleaseResource       fn_ReleaseResource     = nullptr;
FN_FinalizeResourceOwner fn_FinalizeResOwner    = nullptr;
FN_PreFreeSubObj         fn_PreFreeSubObj       = nullptr;
FN_FreeSubObj            fn_FreeSubObj          = nullptr;
FN_GameFree              fn_GameFree            = nullptr;

// Panel navigation sub-functions
FN_InputEvent            fn_InputEvent          = nullptr;
FN_ConfirmCheck          fn_ConfirmCheck        = nullptr;

// Kit data loading sub-functions
FN_ResolveTeamID         fn_ResolveTeamID       = nullptr;
FN_GetTeamDataType       fn_GetTeamDataType     = nullptr;
FN_GetClubTeamKitBase    fn_GetClubTeamKitBase  = nullptr;
FN_GetNationalTeamKitBase fn_GetNationalTeamKitBase = nullptr;
FN_GetTeamKitDataPtr     fn_GetTeamKitData      = nullptr;
FN_GetWeatherCondition   fn_GetWeatherCondition = nullptr;
FN_HasCustomFormation    fn_HasCustomFormation   = nullptr;
FN_IsOnlineMode          fn_IsOnlineMode        = nullptr;
FN_LoadTeamData_MLDefault fn_LoadTeamData_MLDefault = nullptr;
FN_LoadTeamData_CustomFormation fn_LoadTeamData_CustomFormation = nullptr;
FN_LoadTeamData_Normal   fn_LoadTeamData_Normal = nullptr;
FN_LoadTeamData_EditedKit fn_LoadTeamData_EditedKit = nullptr;

// Original trampolines
FN_DestroyPanel_t orig_DestroyPanel = nullptr;
FN_PanelNavUpdate_t orig_PanelNavUpdate = nullptr;
// Trampoline for FUN_00b08b40. hook_PanelRefresh fully reimplements the
// original and never calls back through this pointer — it is declared only
// because MH_CreateHook needs a slot to write the trampoline address into.
FN_PanelRefresh_t orig_PanelRefresh = nullptr;
// Trampoline for FUN_00863570 (GetTeamName) — hook_GetTeamName forwards
// non-custom ids through it.
FN_GetTeamName_t orig_GetTeamName = nullptr;
// Trampoline for FUN_00b3beb0 (CrestResolve) — hook_CrestResolve forwards
// teams without a teams/<ID>.png through it.
FN_CrestResolve_t orig_CrestResolve = nullptr;
// Trampoline for FUN_00b0f990 (club badge render). The naked thunk tail-jmps
// here for non-custom teams.
FN_BadgeRender_t orig_BadgeRender = nullptr;
// Trampoline for FUN_00b0fa80 (clean __cdecl sibling of the badge render).
FN_BadgeRenderCdecl_t orig_BadgeRenderCdecl = nullptr;
// Trampolines for the shared badge-render choke points (FUN_009b0390 team->slot,
// FUN_00b0f8b0 shared texture setter) that cover the rest of the render family.
FN_ComputeBadgeSlot_t orig_ComputeBadgeSlot = nullptr;
FN_BadgeSet_t orig_BadgeSet = nullptr;
FN_SetTeamList_t orig_SetTeamList = nullptr;
FN_BuildLeaguePanelVisualSlots_t orig_BuildLeaguePanelVisualSlots = nullptr;
// Trampolines for FUN_00950580 / FUN_00b0fcd0. Both hooks fully
// reimplement the originals (with NULL guards added) and never call
// back through the trampoline — they're declared only because
// MH_CreateHook needs a slot to write the trampoline address into.
FN_GetBoneScale_t  orig_GetBoneScale  = nullptr;
FN_GetCellPosition_t orig_GetCellPosition = nullptr;
// Trampoline for the FUN_00950350 instrumentation hook (Layer C
// investigation — locating the LEAGUE-slot renderer). The hook is
// installed by default; remove its INSTALL_HOOK line below once the
// renderer has been identified, since SetNodePosition fires every
// frame for every UI element.
FN_SetNodePosition_t orig_SetNodePosition = nullptr;
FN_LoadBothTeamKitData_t orig_LoadBothTeamKitData = nullptr;
FN_ExtractKitColor_t orig_ExtractKitColor = nullptr;
FN_IsTeamKitEdited_t orig_IsTeamKitEdited = nullptr;
// Declared only because MH_CreateHook needs somewhere to write the
// trampoline pointer — hook_LoadTeamKitDataBySlot fully reimplements
// FUN_00969610 and never calls back through this pointer.
FN_LoadTeamKitDataBySlot_t orig_LoadTeamKitDataBySlot = nullptr;
// Used by hook_Match_SetupKitData to forward standard-range team IDs
// straight through to the original FUN_00968700 implementation.
FN_Match_SetupKitData_t    orig_Match_SetupKitData    = nullptr;
// Same role for the FUN_00968960 sibling dispatcher.
FN_Match_SetupKitData2_t   orig_Match_SetupKitData2   = nullptr;
// Trampoline for FUN_00865240 — declared only because MH_CreateHook needs
// somewhere to write it. hook_GetTeamKitData fully reimplements the
// function and never calls back through this pointer.
FN_GetTeamKitDataPtr       orig_GetTeamKitData        = nullptr;
// Trampolines for FUN_00865380 / FUN_00865430 — same rationale as
// orig_GetTeamKitData: hook_GetTeamKitDataB/C reimplement the originals
// fully (RE_GetTeamKitDataB / RE_GetTeamKitDataC) and never call back.
FN_GetTeamKitDataPtr       orig_GetTeamKitDataB       = nullptr;
FN_GetTeamKitDataPtr       orig_GetTeamKitDataC       = nullptr;

// Trampolines for FUN_00967170 / FUN_00966f70 — these DO get called
// through (the hooks only short-circuit on NULL src; non-NULL forwards
// to the original logic via these pointers).
extern "C" FN_FillKitVariantSlot_t orig_FillKitVariantSlot = nullptr;
extern "C" FN_FillKitTeamSlot_t    orig_FillKitTeamSlot    = nullptr;

// Squad / player-record accessors (see club_hooks_squad_data.cpp).
// All five trampolines are forwarded to from the hooks for stock-range
// teamIDs / playerIDs; INI-driven overrides short-circuit before the
// trampoline call.
FN_GetTeamPlayerID_t   orig_GetTeamPlayerID   = nullptr;
extern "C" FN_GetPlayerRecord_t   orig_GetPlayerRecord   = nullptr;
FN_GetTeamPlayerAttr_t orig_GetTeamPlayerAttr = nullptr;
FN_SetTeamPlayerAttr_t orig_SetTeamPlayerAttr = nullptr;
FN_SetTeamPlayerID_t   orig_SetTeamPlayerID   = nullptr;

// =============================================================================
// Helper macro to create and enable a hook with logging
// =============================================================================

#define INSTALL_HOOK(addr, hookFn, origPtr, name)                               \
    do {                                                                         \
        MH_STATUS _s = MH_CreateHook(reinterpret_cast<LPVOID>(addr),            \
                                      reinterpret_cast<LPVOID>(&hookFn),         \
                                      reinterpret_cast<LPVOID*>(&origPtr));      \
        if (_s != MH_OK) {                                                       \
            Logger::Log("[ClubHooks] ERROR: " name " CreateHook failed (%d)",_s);\
        } else {                                                                 \
            _s = MH_EnableHook(reinterpret_cast<LPVOID>(addr));                  \
            if (_s != MH_OK)                                                     \
                Logger::Log("[ClubHooks] ERROR: " name " EnableHook failed (%d)",_s);\
            else                                                                 \
                Logger::Log("[ClubHooks] " name " installed at 0x%08X", (addr));\
        }                                                                        \
    } while(0)

// =============================================================================
// ClubHooks::Register
// =============================================================================

void ClubHooks::Register()
{
    Logger::Log("[ClubHooks] Registering...");

    // ── Load leagues/setup.ini and probe leagues/<n>.ini count ────────────
    // Must run before CreateLeagueSelectionPanel allocates the panel, since
    // it reads MAX_PANEL_SLOTS at allocation time. Sets the runtime panel
    // dimensions (g_PanelF0_Side0/1, g_PanelF4_Side0/1) and the per-league
    // team-slot parse cap (g_LeagueTeamSlotMax).
    LoadLeaguesSetup();

    // ── Resolve all function pointers ─────────────────────────────────────
    // All addresses are from Ghidra analysis of PES6 retail (copy-protection removed).
    // Replace any address with 0 to disable that feature gracefully.

    fn_GetLeagueSelector    = reinterpret_cast<FN_GetLeagueSelector>   (0x009d2880);
    fn_GetSelectedCupIdx    = reinterpret_cast<FN_GetSelectedCupIdx>   (0x009d3bc0);
    fn_GetLeagueString      = reinterpret_cast<FN_GetLeagueString>     (0x00861640);
    fn_GetStringByHandle    = reinterpret_cast<FN_GetStringByHandle>   (0x0094f2c0);
    fn_SetTeamList          = reinterpret_cast<FN_SetTeamList>         (0x00b07b00);
    fn_SetSelectedTeam      = reinterpret_cast<FN_SetSelectedTeam>     (0x00b09590);
    fn_SetNavState          = reinterpret_cast<FN_SetNavState>         (0x00b0ad80);
    fn_PostLoadUpdate       = reinterpret_cast<FN_PostLoadUpdate>      (0x009dcb00);
    fn_DisplayReset         = reinterpret_cast<FN_DisplayReset>        (0x00b07ea0);
    fn_SetLabel             = reinterpret_cast<FN_SetLabel>            (0x00b07bc0);
    fn_SetPage              = reinterpret_cast<FN_SetPage>             (0x00b07c40);
    fn_GetModeFlag          = reinterpret_cast<FN_GetModeFlag>         (0x008c0180);
    fn_IsTeamUnlocked       = reinterpret_cast<FN_IsTeamUnlocked>      (0x009cc4b0);
    fn_AvailSide0           = reinterpret_cast<FN_IsTeamAvail>         (0x00a65af0);
    fn_AvailSide1           = reinterpret_cast<FN_IsTeamAvail>         (0x00a65b50);
    fn_LoadLeagueLogo       = reinterpret_cast<FN_LoadLeagueLogo>      (0x009dab20);
    fn_LoadUIResource       = reinterpret_cast<FN_LoadUIResource>      (0x009c2780);
    fn_InitClubDisplayState = reinterpret_cast<FN_InitClubDisplayState>(0x009edc40);
    fn_CreateDisplayPanel   = reinterpret_cast<FN_CreateDisplayPanel>  (0x00b09050);
    fn_ShowPanel            = reinterpret_cast<FN_ShowPanel>           (0x00b07f70);
    fn_TransformNode        = reinterpret_cast<FN_TransformNode>       (0x00955320);
    fn_GetBoneWorldPos      = reinterpret_cast<FN_GetBoneWorldPos>     (0x00950490);
    fn_GetBoneScale         = reinterpret_cast<FN_GetBoneScale>        (0x00950580);
    fn_GetPlayerState       = reinterpret_cast<FN_GetPlayerState>      (0x00a4f840);
    fn_InitPlayerState      = reinterpret_cast<FN_InitPlayerState>     (0x00a4f0a0);
    fn_ResetPlayerSelection = reinterpret_cast<FN_ResetPlayerSelection>(0x00a4f3b0);
    fn_SetPlayerActive      = reinterpret_cast<FN_SetPlayerActive>     (0x00a4f120);
    fn_SetPlayerInputMode   = reinterpret_cast<FN_SetPlayerInputMode>  (0x00a4f520);
    fn_SetPlayerCursorType  = reinterpret_cast<FN_SetPlayerCursorType> (0x00a4f150);
    fn_SetNavContext         = reinterpret_cast<FN_SetNavContext>       (0x009c39c0);
    fn_LinkDisplayObjects   = reinterpret_cast<FN_LinkDisplayObjects>  (0x00b0f5a0);
    fn_MLClubScreenInit     = reinterpret_cast<FN_MLClubScreenInit>    (0x009edfd0);
    fn_SetupNavMapping      = reinterpret_cast<FN_SetupNavMapping>     (0x009c2720);

    // timeGetTime is a Windows API — resolve from winmm.dll at runtime
    fn_TimeGetTime = reinterpret_cast<FN_TimeGetTime>(
        GetProcAddress(GetModuleHandleA("winmm.dll"), "timeGetTime"));
    if (!fn_TimeGetTime)
        Logger::Log("[ClubHooks] WARNING: timeGetTime not resolved (winmm not loaded?)");

    // Panel creation internals (used by hook_CreateLeagueSelectionPanel)
    fn_GameAlloc              = reinterpret_cast<FN_GameAlloc>             (0x00877070);
    fn_CreateDisplaySubObject = reinterpret_cast<FN_CreateDisplaySubObject>(0x00b0bbd0);
    fn_InitDisplaySubObject   = reinterpret_cast<FN_InitDisplaySubObject>  (0x00af9e00);
    fn_CreateRenderNode       = reinterpret_cast<FN_CreateRenderNode>      (0x00952180);
    fn_GetChildNode           = reinterpret_cast<FN_GetChildNode>          (0x00950b40);
    fn_SetNodeTexture         = reinterpret_cast<FN_SetNodeTexture>        (0x009509b0);
    fn_NodePropA              = reinterpret_cast<FN_NodePropA>             (0x00950300);
    fn_NodePropB              = reinterpret_cast<FN_NodePropB>             (0x00950330);
    fn_NodePropC              = reinterpret_cast<FN_NodePropC>             (0x009503c0);
    fn_NodePropD              = reinterpret_cast<FN_NodePropD>             (0x009503e0);
    fn_SetNodePosition        = reinterpret_cast<FN_SetNodePosition>       (0x00950350);
    fn_LookupBone             = reinterpret_cast<FN_LookupBone>            (0x009554a0);
    fn_ResolveBone            = reinterpret_cast<FN_ResolveBone>           (0x0094e380);
    fn_AttachBone             = reinterpret_cast<FN_AttachBone>            (0x009557e0);
    fn_SetBoneVisibility      = reinterpret_cast<FN_SetBoneVisibility>     (0x00955db0);
    fn_CreateScrollList       = reinterpret_cast<FN_CreateScrollList>      (0x00b01230);
    fn_SetScrollScale         = reinterpret_cast<FN_SetScrollScale>        (0x00b00d70);
    fn_SetScrollFlag          = reinterpret_cast<FN_SetScrollFlag>         (0x00b011c0);
    fn_FinalisePanelLayout    = reinterpret_cast<FN_FinalisePanelLayout>   (0x00b081d0);
    fn_CreateScrollController = reinterpret_cast<FN_CreateScrollController>(0x00b0f270);
    fn_SetScrollCtrlScale     = reinterpret_cast<FN_SetScrollCtrlScale>    (0x00b0f540);
    fn_LinkScrollCtrlToSlots  = reinterpret_cast<FN_LinkScrollCtrlToSlots> (0x00b0f5a0);
    fn_SetScrollBounds        = reinterpret_cast<FN_SetScrollBounds>       (0x00b0f4b0);
    fn_SetCellPosition        = reinterpret_cast<FN_SetCellPosition>       (0x00b0fc60);
    fn_CreateDisplayItem      = reinterpret_cast<FN_CreateDisplayItem>     (0x00b00920);
    fn_InitDisplayItem        = reinterpret_cast<FN_InitDisplayItem>       (0x00b00ce0);
    fn_SetItemScale           = reinterpret_cast<FN_SetItemScale>          (0x00b004a0);
    fn_GetPanelRegistrySlot   = reinterpret_cast<FN_GetPanelRegistrySlot>  (0x00b0bb50);

    // Panel destructor sub-functions (used by hook_DestroyPanel)
    fn_PreDestroyCleanup = reinterpret_cast<FN_PreDestroyCleanup>    (0x00b09b20);
    fn_DestroyScrollCtrl = reinterpret_cast<FN_DestroyScrollCtrl>    (0x00b0f400);
    fn_DestroyScrollList = reinterpret_cast<FN_DestroyScrollList>    (0x00b00d10);
    fn_DestroyDisplayItem= reinterpret_cast<FN_DestroyDisplayItem>   (0x00b00400);
    fn_DestroyBoneAttach = reinterpret_cast<FN_DestroyBoneAttach>    (0x009553f0);
    fn_DestroyNodeChild  = reinterpret_cast<FN_DestroyNodeChild>     (0x009502b0);
    fn_ReleaseResource   = reinterpret_cast<FN_ReleaseResource>      (0x0094f420);
    fn_FinalizeResOwner  = reinterpret_cast<FN_FinalizeResourceOwner>(0x0094f780);
    fn_PreFreeSubObj     = reinterpret_cast<FN_PreFreeSubObj>        (0x00b0bb60);
    fn_FreeSubObj        = reinterpret_cast<FN_FreeSubObj>           (0x00b0bb80);
    fn_GameFree          = reinterpret_cast<FN_GameFree>             (0x0045bc50);

    // Panel navigation sub-functions (used by hook_PanelNavUpdate)
    fn_InputEvent        = reinterpret_cast<FN_InputEvent>           (0x009b8980);
    fn_ConfirmCheck      = reinterpret_cast<FN_ConfirmCheck>         (0x00b08370);

    // Kit data loading sub-functions (used by kit hooks)
    fn_ResolveTeamID         = reinterpret_cast<FN_ResolveTeamID>        (0x008646b0);
    fn_GetTeamDataType       = reinterpret_cast<FN_GetTeamDataType>      (0x00866d80);
    fn_GetClubTeamKitBase    = reinterpret_cast<FN_GetClubTeamKitBase>   (0x00864ef0);
    fn_GetNationalTeamKitBase= reinterpret_cast<FN_GetNationalTeamKitBase>(0x00865100);
    fn_GetTeamKitData        = reinterpret_cast<FN_GetTeamKitDataPtr>    (0x00865240);
    fn_GetWeatherCondition   = reinterpret_cast<FN_GetWeatherCondition>  (0x005c18b0);
    fn_HasCustomFormation    = reinterpret_cast<FN_HasCustomFormation>    (0x00966c70);
    fn_IsOnlineMode          = reinterpret_cast<FN_IsOnlineMode>         (0x00b15700);
    fn_LoadTeamData_MLDefault= reinterpret_cast<FN_LoadTeamData_MLDefault>(0x00969490);
    fn_LoadTeamData_CustomFormation = reinterpret_cast<FN_LoadTeamData_CustomFormation>(0x00969530);
    fn_LoadTeamData_Normal   = reinterpret_cast<FN_LoadTeamData_Normal>  (0x009692d0);
    fn_LoadTeamData_EditedKit= reinterpret_cast<FN_LoadTeamData_EditedKit>(0x009691d0);

    Logger::Log("[ClubHooks] All function pointers resolved.");

    // ── Install hooks ─────────────────────────────────────────────────────

    // FUN_009ed730 — LoadClubTeamSlots
    INSTALL_HOOK(0x009ed730,
                 hook_LoadClubTeamSlots,
                 orig_LoadClubTeamSlots,
                 "LoadClubTeamSlots");

    // FUN_009e25a0 — LoadCompetitionMenuTeams
    INSTALL_HOOK(0x009e25a0,
                 hook_LoadCompetitionMenuTeams,
                 orig_LoadCompetitionMenuTeams,
                 "LoadCompetitionMenuTeams");

    // FUN_00b07b00 — SetTeamList
    // Hooked to allow configurable team slot count per league.
    INSTALL_HOOK(0x00b07b00,
                 hook_SetTeamList,
                 orig_SetTeamList,
                 "SetTeamList");

    // FUN_009ee940 — InitClubSelectionScreen
    INSTALL_HOOK(0x009ee940,
                 hook_InitClubSelectionScreen,
                 orig_InitClubSelectionScreen,
                 "InitClubSelectionScreen");

    // FUN_00b09050 — CreateLeagueSelectionPanel.
    // Two implementations exist: the original v1 in club_hooks_panel.cpp
    // (kept as reference) and a clean rewrite v2 in club_hooks_panel_v2.cpp
    // with a configurable Step 9 (league-panel container loop) driven by
    // `g_LeaguePanelCount` and per-page [panel] overrides. INSTALL ONLY ONE.
    //
    // To revert to v1: comment the v2 line below and uncomment the v1 line.
    //
    // INSTALL_HOOK(0x00b09050,
    //              hook_CreateLeagueSelectionPanel,
    //              orig_CreateLeagueSelectionPanel,
    //              "CreateLeagueSelectionPanel[v1]");
    INSTALL_HOOK(0x00b09050,
                 hook_CreateLeagueSelectionPanel_v2,
                 orig_CreateLeagueSelectionPanel,
                 "CreateLeagueSelectionPanel[v2]");

    // FUN_00b083b0 — BuildLeaguePanelVisualSlots.
    // Fully replaced: the stock tail writes League selectable-count to
    // fixed 0x7614, which overlaps slot 20 when MAX_PANEL_SLOTS grows.
    INSTALL_HOOK(0x00b083b0,
                 hook_BuildLeaguePanelVisualSlots_Naked,
                 orig_BuildLeaguePanelVisualSlots,
                 "BuildLeaguePanelVisualSlots");

    // FUN_00863570 — GetTeamName. Hooked to supply custom team names/short
    // names from teams/<ID>.ini for ids not in the stock list (else NULL).
    INSTALL_HOOK(0x00863570,
                 hook_GetTeamName,
                 orig_GetTeamName,
                 "GetTeamName");

    // FUN_00b3beb0 — CrestResolve. Loads a team's crest into a scratch texture
    // id for the caller to copy; hooked to supply custom crests from
    // teams/<ID>.png for ids with no stock crest data (else the white flag).
    INSTALL_HOOK(0x00b3beb0,
                 hook_CrestResolve,
                 orig_CrestResolve,
                 "CrestResolve");

    // FUN_00b0f990 — club-selection badge render. Naked thunk draws
    // teams/<ID>.png for custom teams (else the white flag); stock falls through.
    INSTALL_HOOK(0x00b0f990,
                 hook_BadgeRender_Naked,
                 orig_BadgeRender,
                 "BadgeRender");

    // FUN_00b0fa80 — clean __cdecl sibling of the badge render.
    INSTALL_HOOK(0x00b0fa80,
                 hook_BadgeRenderCdecl,
                 orig_BadgeRenderCdecl,
                 "BadgeRenderCdecl");

    // FUN_009b0390 — records the team id for the shared badge setter below.
    INSTALL_HOOK(0x009b0390,
                 hook_ComputeBadgeSlot,
                 orig_ComputeBadgeSlot,
                 "ComputeBadgeSlot");

    // FUN_00b0f8b0 — shared badge-texture setter; draws teams/<id>.png for the
    // custom slot 0x1ca. Covers the render family the two hooks above don't.
    INSTALL_HOOK(0x00b0f8b0,
                 hook_BadgeSet_Naked,
                 orig_BadgeSet,
                 "BadgeSet");

    // FUN_00b08b40 — PanelRefresh (display sub-object refresh callback).
    // Full replacement: the LEAGUE branch positions the selection cursor from
    // the fixed 20/side seat control-id table (0x00E86130), which has no entry
    // for slots >= 20, so the cursor collapsed to screen centre over extra
    // league slots. We resolve the same control id the logo uses and clamp the
    // page-node index. See club_hooks_panel_refresh.cpp.
    INSTALL_HOOK(0x00b08b40,
                 hook_PanelRefresh,
                 orig_PanelRefresh,
                 "PanelRefresh");

    // FUN_00b0ac50 — DestroyPanel
    // Hooked to read subObj from the correct dynamic trailer offset
    // when MAX_PANEL_SLOTS > 20.
    INSTALL_HOOK(0x00b0ac50,
                 hook_DestroyPanel,
                 orig_DestroyPanel,
                 "DestroyPanel");

    // FUN_00b0ac40 — PanelNavUpdate (wrapper around FUN_00b09b80)
    // Hooked to remove hardcoded 20-slot navigation bounds.
    INSTALL_HOOK(0x00b0ac40,
                 hook_PanelNavUpdate,
                 orig_PanelNavUpdate,
                 "PanelNavUpdate");

    // FUN_00969790 — LoadBothTeamKitData
    // Hooked to add safety checks for unrecognized team IDs.
    INSTALL_HOOK(0x00969790,
                 hook_LoadBothTeamKitData,
                 orig_LoadBothTeamKitData,
                 "LoadBothTeamKitData");

    // FUN_00968e70 — ExtractKitColor
    // Hooked to add NULL-safety and allow modding of color extraction.
    INSTALL_HOOK(0x00968e70,
                 hook_ExtractKitColor,
                 orig_ExtractKitColor,
                 "ExtractKitColor");

    // FUN_008654d0 — IsTeamKitEdited
    // Hooked to prevent NULL dereference crashes for unknown team IDs.
    INSTALL_HOOK(0x008654d0,
                 hook_IsTeamKitEdited,
                 orig_IsTeamKitEdited,
                 "IsTeamKitEdited");

    // FUN_00969610 — LoadTeamKitDataBySlot (inner per-side kit loader)
    // We install the __declspec(naked) thunk because the function takes
    // an implicit EAX side-index parameter that cdecl can't express.
    // The thunk forwards everything to hook_LoadTeamKitDataBySlot, which
    // fully reimplements the original with NULL-safety added.
    INSTALL_HOOK(0x00969610,
                 hook_LoadTeamKitDataBySlot_Naked,
                 orig_LoadTeamKitDataBySlot,
                 "LoadTeamKitDataBySlot");

    // FUN_00968700 — Match_SetupKitData (per-frame kit-setup dispatcher)
    // Hooked to add the same teamID > 200 guard the rest of the kit
    // pipeline uses. Standard-range teams forward to the trampoline;
    // custom-range teams short-circuit so we don't deref garbage kit
    // data. Future custom kit loader will land in the >200 branch.
    INSTALL_HOOK(0x00968700,
                 hook_Match_SetupKitData,
                 orig_Match_SetupKitData,
                 "Match_SetupKitData");

    // FUN_00968960 — Match_SetupKitData2 (sibling kit-setup dispatcher)
    // Same crash, same fix as FUN_00968700; different downstream callees
    // (FUN_00968410 / FUN_009688e0). Crash is at 0x00968971.
    INSTALL_HOOK(0x00968960,
                 hook_Match_SetupKitData2,
                 orig_Match_SetupKitData2,
                 "Match_SetupKitData2");

    // FUN_00865240 — GetTeamKitData (THE central kit-data chokepoint).
    // Replaces the game's resolver with a custom-INI-aware reimplementation.
    // Tries kits/<teamID>.ini first, then falls through to a faithful
    // re-creation of the original (RE_GetTeamKitData) which returns NULL
    // for unknown IDs instead of the garbage fallback buffer that causes
    // the kitData[0x3a] crash family. With this in place the teamID > 200
    // short-circuits in the other kit hooks become redundant.
    INSTALL_HOOK(0x00865240,
                 hook_GetTeamKitData,
                 orig_GetTeamKitData,
                 "GetTeamKitData");

    // FUN_00865380 — GetTeamKitDataB (extra-B sub-record at base+0x100).
    // Sibling chokepoint into the per-team 0x220-byte kit buffer.
    // Crash family: PreloadTeamKitGraphics walks GetClubTeamKitBase()'s
    // unconditional table-stride math for custom IDs (>= 0xC8) and ends
    // up reading [ESI+0x3] from a wild pointer. INI-first / RE-fallback
    // shape mirrors hook_GetTeamKitData.
    INSTALL_HOOK(0x00865380,
                 hook_GetTeamKitDataB,
                 orig_GetTeamKitDataB,
                 "GetTeamKitDataB");

    // FUN_00865430 — GetTeamKitDataC (extra-C sub-record at base+0x160).
    // Direct provider of the wild pointer ESI in PreloadTeamKitGraphics
    // (FUN_00967B40); the +0x03 byte read at 0x00967BC9 is the kit-graphic
    // resource ID. Returning a zeroed buffer for custom IDs makes the
    // preloader treat them as "no graphic to load" instead of dereffing
    // garbage. Stock IDs route through faithful RE_GetTeamKitDataC.
    INSTALL_HOOK(0x00865430,
                 hook_GetTeamKitDataC,
                 orig_GetTeamKitDataC,
                 "GetTeamKitDataC");

    // FUN_00967170 — FillKitVariantSlot (per-slot inner of
    // BuildMatchKitDescriptors). Crash family #3: NULL deref at
    // 0x00967188 when InitMatchKitContext leaves +8..+0x14 NULL because
    // hook_GetTeamKitDataB returned NULL (custom team, no INI). Hook
    // adds a top-of-function NULL guard and forwards to the trampoline
    // for legitimate cases.
    INSTALL_HOOK(0x00967170,
                 hook_FillKitVariantSlot,
                 orig_FillKitVariantSlot,
                 "FillKitVariantSlot");

    // FUN_00966f70 — FillKitTeamSlot (sibling of FillKitVariantSlot for
    // the +0/+4 team-data slot range). Same NULL-guard rationale; uses
    // the __declspec(naked) thunk to bridge the original __thiscall
    // (ECX=slotIdx) into our cdecl C handler.
    INSTALL_HOOK(0x00966f70,
                 hook_FillKitTeamSlot_Naked,
                 orig_FillKitTeamSlot,
                 "FillKitTeamSlot");

    // FUN_00950490 — GetBoneWorldPosition
    // Installed for the missing NULL-guard fix: the original crashes at
    // 0x009553B0 (inside fn_TransformNode) when *(objectPtr+0x1c) is NULL
    // because it unconditionally does fn_TransformNode(skeletonNode + 4)
    // and the transform reads short[] from the +4 offset of NULL. This
    // fires when scrolling the extended (>20-slot) league panel onto
    // slots whose per-slot bone table was never registered. Our hook is
    // a faithful reimplementation that adds the NULL guard and returns
    // the same nullptr the original returns on no-bone-match. See
    // hook_GetBoneWorldPosition above for full discussion.
    INSTALL_HOOK(0x00950490,
                 hook_GetBoneWorldPosition,
                 orig_GetBoneWorldPosition,
                 "GetBoneWorldPosition");

    // FUN_00950580 — GetBoneScale.
    // Sibling of FUN_00950490 with the same missing-NULL-guard bug.
    // Without this hook, scrolling the league panel onto a slot whose
    // render-node has no skeleton (typically slots 20+ in extended-
    // panel scenarios) crashes at 0x00950596 reading [esi+2] with ESI=0.
    INSTALL_HOOK(0x00950580,
                 hook_GetBoneScale,
                 orig_GetBoneScale,
                 "GetBoneScale");

    // FUN_00b0fcd0 — GetCellPosition (scroll-controller per-frame nav).
    // Adds the missing `*param_1 != NULL` guard so that team-panel
    // navigation past the cell array end (e.g. slotIdx=21 when only
    // 20 cells are allocated) returns NULL silently instead of AVing
    // at 0x00B0FD02 reading [ecx+0x14] with ECX=0.
    INSTALL_HOOK(0x00b0fcd0,
                 hook_GetCellPosition,
                 orig_GetCellPosition,
                 "GetCellPosition");

    // FUN_00951330 — node-position setter (INSTRUMENTATION ONLY).
    // RETARGETED 2026-04-30 from FUN_00950350. The first log run showed
    // 950350 only fired for panel-decoration nodes (6 calls), never for
    // per-league-slot positioning. FUN_00951330 is the sibling position-
    // setter (same signature, writes to node[+0x0C..+0x18]) used by
    // FUN_00b0fc60 for cell label/icon nodes, with ~80 callers across
    // the binary; the league renderer almost certainly routes through
    // here. Filter (panel UI range 0x00b00000..0x00b1ffff) +
    // 800-line cap keep the log file usable.
    // REMOVE once the renderer is identified. See hook body in
    // club_hooks_screen.cpp for the full procedure.
    // INSTALL_HOOK(0x00951330,
    //              hook_SetNodePosition,
    //              orig_SetNodePosition,
    //              "SetNodePosition[INSTRUMENTATION]");

    // FUN_00950a00 — CreateChildNode (THE underlying child-node creator).
    // `fn_GetChildNode` (= FUN_00950b40 / CreateChildNode_Default) is a
    // 1-line wrapper that calls this with attachDefaultBehavior=1; its
    // unused sibling FUN_00950b50 / CreateChildNode_Bare passes 0.
    //
    // INSTRUMENTATION ONLY: logs caller/parent/behavior/result inside the
    // panel-UI range (0x00b00000..0x00b1ffff) so we can map the v2 Step 9
    // child-node usage during Layer D investigation. Forwards to the
    // trampoline; production behaviour unchanged. Comment out to disable.
    //
    // Hook body + procedure: see club_hooks_panel_v2.cpp.
    INSTALL_HOOK(0x00950a00,
                 hook_CreateChildNode,
                 orig_CreateChildNode,
                 "CreateChildNode[INSTRUMENTATION]");

    // ── Squad / player-record accessors ───────────────────────────────────
    // These five hooks let custom team IDs (those outside the stock player
    // table coverage) declare squads and player records via INI files.
    // For stock IDs the hooks fall straight through to the trampoline.
    // See club_hooks_squad_data.cpp for the INI format and cache details.

    // FUN_00862710 — GetTeamPlayerID. Master squad-slot → playerID chokepoint.
    INSTALL_HOOK(0x00862710,
                 hook_GetTeamPlayerID,
                 orig_GetTeamPlayerID,
                 "GetTeamPlayerID");

    // FUN_00861b90 — GetPlayerRecord (worker). The thin wrapper at 0x00861d20
    // is barely used; the real chokepoint is the worker, called directly by
    // the per-field accessors (jersey, name, stats, ...) at match time.
    // Hooking the wrapper alone meant our hook never fired during a match.
    // The naked thunk bridges the original's register-only convention
    // (ECX=playerID, EAX=bank, no stack args) into our cdecl C handler.
    INSTALL_HOOK(0x00861b90,
                 hook_GetPlayerRecord_Naked,
                 orig_GetPlayerRecord,
                 "GetPlayerRecord");

    // FUN_00862c10 — GetTeamPlayerAttr. Per-slot squad-number / position byte.
    INSTALL_HOOK(0x00862c10,
                 hook_GetTeamPlayerAttr,
                 orig_GetTeamPlayerAttr,
                 "GetTeamPlayerAttr");

    // FUN_00862200 — SetTeamPlayerAttr. Companion writer for the above.
    INSTALL_HOOK(0x00862200,
                 hook_SetTeamPlayerAttr,
                 orig_SetTeamPlayerAttr,
                 "SetTeamPlayerAttr");

    // FUN_00861ad0 — SetTeamPlayerID. Squad-slot writer (used by alias copy).
    INSTALL_HOOK(0x00861ad0,
                 hook_SetTeamPlayerID,
                 orig_SetTeamPlayerID,
                 "SetTeamPlayerID");

    Logger::Log("[ClubHooks] Registration complete. MAX_PANEL_SLOTS=%d",
        MAX_PANEL_SLOTS);
}
