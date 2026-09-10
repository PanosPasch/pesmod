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

// proxy_d3d8.h
//
// A pass-through IDirect3D8 whose only real job is to intercept CreateDevice
// and hand the game a ProxyDevice8 instead of the real device.
//
// pes6.exe's FUN_00405300 calls CreateDevice through vtable slot 15 and writes
// the result straight into DAT_00f83e68, the global every render call site
// dispatches from. Substituting here is therefore enough to capture the whole
// engine — no code patching, no vtable rewriting of a shared runtime object.
#pragma once

#include "../d3d8/d3d8_min.h"

namespace Capture
{
    class ProxyD3D8 : public IDirect3D8
    {
    public:
        explicit ProxyD3D8(IDirect3D8* real);

        IDirect3D8* Real() const { return m_real; }

        // ── IUnknown ─────────────────────────────────────────────────────
        HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObj) override;
        ULONG   __stdcall AddRef() override;
        ULONG   __stdcall Release() override;

        // ── IDirect3D8 ───────────────────────────────────────────────────
        HRESULT  __stdcall RegisterSoftwareDevice(void* pInitializeFunction) override;
        UINT     __stdcall GetAdapterCount() override;
        HRESULT  __stdcall GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER8* pIdentifier) override;
        UINT     __stdcall GetAdapterModeCount(UINT Adapter) override;
        HRESULT  __stdcall EnumAdapterModes(UINT Adapter, UINT Mode, D3DDISPLAYMODE* pMode) override;
        HRESULT  __stdcall GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override;
        HRESULT  __stdcall CheckDeviceType(UINT Adapter, D3DDEVTYPE CheckType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL Windowed) override;
        HRESULT  __stdcall CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override;
        HRESULT  __stdcall CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType) override;
        HRESULT  __stdcall CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override;
        HRESULT  __stdcall GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS8* pCaps) override;
        HMONITOR __stdcall GetAdapterMonitor(UINT Adapter) override;
        HRESULT  __stdcall CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice8** ppReturnedDeviceInterface) override;

    private:
        IDirect3D8* m_real;
        LONG        m_refCount;
    };
}
