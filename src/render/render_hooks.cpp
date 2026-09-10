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

// render_hooks.cpp
//
// Entry point for the renderer subsystem.
//
// Hooking Direct3DCreate8 rather than patching the game's own D3D wrapper
// keeps this independent of the executable's addresses: it works on any build
// of the game, and it also sits correctly underneath d3d8to9 / ReShade if
// those are ever re-enabled in the game folder, because whatever d3d8.dll is
// actually loaded is the thing we hook.
#include "render_hooks.h"
#include "render_config.h"
#include "capture/proxy_d3d8.h"
#include "capture/frame_capture.h"
#include "../utils/logger.h"
#include "MinHook/include/MinHook.h"

#include <windows.h>

namespace
{
    PFN_Direct3DCreate8 orig_Direct3DCreate8 = nullptr;
    bool                g_installed          = false;

    IDirect3D8* __stdcall hook_Direct3DCreate8(UINT SDKVersion)
    {
        IDirect3D8* real = orig_Direct3DCreate8
                         ? orig_Direct3DCreate8(SDKVersion)
                         : nullptr;
        if (!real)
        {
            Logger::Log("[Render] Direct3DCreate8(%u) returned NULL.", SDKVersion);
            return nullptr;
        }

        Logger::Log("[Render] Direct3DCreate8(%u) intercepted; wrapping IDirect3D8.",
                    SDKVersion);
        return new Capture::ProxyD3D8(real);
    }
}

void RenderHooks::Register()
{
    RenderConfig::Load();

    if (!RenderConfig::Enabled())
    {
        Logger::Log("[Render] Disabled ([render] enabled=0); no D3D8 hooks.");
        return;
    }

    // d3d8.dll is a static import of pes6.exe, so it is already mapped by the
    // time our DllMain runs. LoadLibrary is only a fallback.
    HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
    if (!d3d8) d3d8 = LoadLibraryA("d3d8.dll");
    if (!d3d8)
    {
        Logger::Log("[Render] FATAL: d3d8.dll not present; renderer disabled.");
        return;
    }

    void* target = (void*)GetProcAddress(d3d8, "Direct3DCreate8");
    if (!target)
    {
        Logger::Log("[Render] FATAL: Direct3DCreate8 not exported by d3d8.dll.");
        return;
    }

    const MH_STATUS created = MH_CreateHook(target, (void*)&hook_Direct3DCreate8,
                                            (void**)&orig_Direct3DCreate8);
    if (created != MH_OK)
    {
        Logger::Log("[Render] FATAL: MH_CreateHook(Direct3DCreate8) failed (%d).",
                    created);
        return;
    }

    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK)
    {
        Logger::Log("[Render] FATAL: MH_EnableHook(Direct3DCreate8) failed (%d).",
                    enabled);
        return;
    }

    g_installed = true;
    Logger::Log("[Render] Direct3DCreate8 hook installed at %p.", target);

    if (RenderConfig::CaptureEnabled())
        Logger::Log("[Render] Capture armed on key 0x%02X; output '%s'.",
                    RenderConfig::CaptureHotkey(), RenderConfig::CaptureDir());
}

void RenderHooks::Unregister()
{
    if (!g_installed) return;

    // Normally the session report is written when the device proxy is
    // released. A hard exit can skip that, so write it here too — the
    // capture layer no-ops if there is nothing to report.
    if (RenderConfig::CaptureEnabled())
        Capture::Frame::WriteSessionReport(RenderConfig::SessionReportPath());

    g_installed = false;
}
