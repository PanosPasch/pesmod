// proxy_d3d8.cpp
#include "proxy_d3d8.h"
#include "proxy_device.h"
#include "frame_capture.h"
#include "resource_registry.h"
#include "scene_export.h"
#include "../render_config.h"
#include "../d3d8/d3d8_util.h"
#include "../../utils/logger.h"

#include <cstring>

namespace Capture
{

ProxyD3D8::ProxyD3D8(IDirect3D8* real)
    : m_real(real)
    , m_refCount(1)
{
}

// ── IUnknown ─────────────────────────────────────────────────────────────
HRESULT __stdcall ProxyD3D8::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_IDirect3D8_PESMod)
    {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }

    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG __stdcall ProxyD3D8::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_refCount);
}

ULONG __stdcall ProxyD3D8::Release()
{
    const LONG remaining = InterlockedDecrement(&m_refCount);
    if (remaining == 0)
    {
        m_real->Release();
        delete this;
        return 0;
    }
    return (ULONG)remaining;
}

// ── The one method that matters ──────────────────────────────────────────
HRESULT __stdcall ProxyD3D8::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType,
                                          HWND hFocusWindow, DWORD BehaviorFlags,
                                          D3DPRESENT_PARAMETERS* pPresentationParameters,
                                          IDirect3DDevice8** ppReturnedDeviceInterface)
{
    IDirect3DDevice8* realDevice = nullptr;
    const HRESULT hr = m_real->CreateDevice(Adapter, DeviceType, hFocusWindow,
                                            BehaviorFlags,
                                            pPresentationParameters,
                                            &realDevice);
    if (FAILED(hr) || !realDevice)
    {
        Logger::Log("[Render] CreateDevice failed (hr=0x%08X); no proxy installed.",
                    (unsigned)hr);
        if (ppReturnedDeviceInterface) *ppReturnedDeviceInterface = nullptr;
        return hr;
    }

    if (!ppReturnedDeviceInterface)
    {
        realDevice->Release();
        return D3DERR_INVALIDCALL;
    }

    // A fresh device means every previously tracked resource is gone.
    Registry::Reset();

    // Streaming needs everything capture needs — the resource registry, the
    // readable-buffer workaround and the state shadow — so either one turns
    // the interception layer on. What differs is only whether frames are
    // written to disk, which the capture triggers gate separately.
    const bool capture   = RenderConfig::CaptureEnabled();
    const bool streaming = RenderConfig::StreamEnabled();
    const bool tracking  = capture || streaming;

    if (tracking)
        Frame::Init(RenderConfig::CaptureDir());
    if (streaming)
        SceneExport::Init(RenderConfig::StreamSection(),
                          (uint64_t)RenderConfig::StreamRingMB() * 1024ull * 1024ull);

    D3DPRESENT_PARAMETERS pp;
    if (pPresentationParameters) pp = *pPresentationParameters;
    else                         memset(&pp, 0, sizeof(pp));

    *ppReturnedDeviceInterface = new ProxyDevice8(realDevice, this, pp, tracking);

    // The device holds a reference to us for the lifetime of GetDirect3D.
    AddRef();
    return hr;
}

// ── Pass-through ─────────────────────────────────────────────────────────
HRESULT __stdcall ProxyD3D8::RegisterSoftwareDevice(void* pInitializeFunction)
    { return m_real->RegisterSoftwareDevice(pInitializeFunction); }
UINT __stdcall ProxyD3D8::GetAdapterCount()
    { return m_real->GetAdapterCount(); }
HRESULT __stdcall ProxyD3D8::GetAdapterIdentifier(UINT A, DWORD F, D3DADAPTER_IDENTIFIER8* p)
    { return m_real->GetAdapterIdentifier(A, F, p); }
UINT __stdcall ProxyD3D8::GetAdapterModeCount(UINT A)
    { return m_real->GetAdapterModeCount(A); }
HRESULT __stdcall ProxyD3D8::EnumAdapterModes(UINT A, UINT M, D3DDISPLAYMODE* p)
    { return m_real->EnumAdapterModes(A, M, p); }
HRESULT __stdcall ProxyD3D8::GetAdapterDisplayMode(UINT A, D3DDISPLAYMODE* p)
    { return m_real->GetAdapterDisplayMode(A, p); }
HRESULT __stdcall ProxyD3D8::CheckDeviceType(UINT A, D3DDEVTYPE T, D3DFORMAT DF, D3DFORMAT BF, BOOL W)
    { return m_real->CheckDeviceType(A, T, DF, BF, W); }
HRESULT __stdcall ProxyD3D8::CheckDeviceFormat(UINT A, D3DDEVTYPE T, D3DFORMAT AF, DWORD U, D3DRESOURCETYPE R, D3DFORMAT CF)
    { return m_real->CheckDeviceFormat(A, T, AF, U, R, CF); }
HRESULT __stdcall ProxyD3D8::CheckDeviceMultiSampleType(UINT A, D3DDEVTYPE T, D3DFORMAT SF, BOOL W, D3DMULTISAMPLE_TYPE MS)
    { return m_real->CheckDeviceMultiSampleType(A, T, SF, W, MS); }
HRESULT __stdcall ProxyD3D8::CheckDepthStencilMatch(UINT A, D3DDEVTYPE T, D3DFORMAT AF, D3DFORMAT RT, D3DFORMAT DS)
    { return m_real->CheckDepthStencilMatch(A, T, AF, RT, DS); }
HRESULT __stdcall ProxyD3D8::GetDeviceCaps(UINT A, D3DDEVTYPE T, D3DCAPS8* p)
    { return m_real->GetDeviceCaps(A, T, p); }
HMONITOR __stdcall ProxyD3D8::GetAdapterMonitor(UINT A)
    { return m_real->GetAdapterMonitor(A); }

} // namespace Capture
