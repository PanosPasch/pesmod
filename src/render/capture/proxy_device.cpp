// proxy_device.cpp
#include "proxy_device.h"
#include "proxy_d3d8.h"
#include "resource_registry.h"
#include "../d3d8/d3d8_util.h"
#include "../render_config.h"
#include "../../utils/logger.h"

#include <cstring>

namespace Capture
{

ProxyDevice8::ProxyDevice8(IDirect3DDevice8* real, ProxyD3D8* parent,
                           const D3DPRESENT_PARAMETERS& pp, bool captureEnabled)
    : m_real(real)
    , m_parent(parent)
    , m_refCount(1)
    , m_captureEnabled(captureEnabled)
    , m_hotkeyDown(false)
{
    m_state.SetDefaults();

    // The default viewport is the whole back buffer; only the present
    // parameters know how big that is.
    m_state.viewport.X      = 0;
    m_state.viewport.Y      = 0;
    m_state.viewport.Width  = pp.BackBufferWidth;
    m_state.viewport.Height = pp.BackBufferHeight;

    // No depth buffer means ZENABLE defaults to FALSE rather than TRUE.
    if (!pp.EnableAutoDepthStencil)
        m_state.renderState[D3DRS_ZENABLE] = 0;

    Logger::Log("[Render] Device proxy attached: %ux%u %s, %s, swap=%s, "
                "depth=%s, capture=%s",
                pp.BackBufferWidth, pp.BackBufferHeight,
                D3D8Util::FormatName(pp.BackBufferFormat),
                pp.Windowed ? "windowed" : "fullscreen",
                D3D8Util::SwapEffectName(pp.SwapEffect),
                pp.EnableAutoDepthStencil
                    ? D3D8Util::FormatName(pp.AutoDepthStencilFormat) : "none",
                captureEnabled ? "on" : "off");

    if (m_captureEnabled)
        Frame::BeginFrame();
}

// ── IUnknown ─────────────────────────────────────────────────────────────
HRESULT __stdcall ProxyDevice8::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_IDirect3DDevice8_PESMod)
    {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }

    // Anything else is refused rather than forwarded: handing back the real
    // device would let the caller bypass the proxy entirely, and the capture
    // would silently go blind from that point on.
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG __stdcall ProxyDevice8::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_refCount);
}

ULONG __stdcall ProxyDevice8::Release()
{
    const LONG remaining = InterlockedDecrement(&m_refCount);
    if (remaining == 0)
    {
        if (m_captureEnabled)
        {
            Frame::WriteSessionReport(RenderConfig::SessionReportPath());
            Frame::Shutdown();
        }
        Logger::Log("[Render] Device proxy released after %u frames.",
                    Frame::CurrentFrameIndex());
        m_real->Release();
        delete this;
        return 0;
    }
    return (ULONG)remaining;
}

// ── Frame lifecycle ──────────────────────────────────────────────────────
void ProxyDevice8::PollHotkeys()
{
    const int key = RenderConfig::CaptureHotkey();
    if (key == 0) return;

    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    if (down && !m_hotkeyDown)
        Frame::ArmSingleFrame();     // edge-triggered: one capture per press
    m_hotkeyDown = down;
}

HRESULT __stdcall ProxyDevice8::Present(const RECT* pSrc, const RECT* pDst,
                                        HWND hOverride, const RGNDATA* pDirty)
{
    if (m_captureEnabled)
    {
        PollHotkeys();
        Frame::EndFrame(m_real);
    }

    const HRESULT hr = m_real->Present(pSrc, pDst, hOverride, pDirty);

    if (m_captureEnabled)
        Frame::BeginFrame();

    return hr;
}

HRESULT __stdcall ProxyDevice8::Reset(D3DPRESENT_PARAMETERS* pp)
{
    const HRESULT hr = m_real->Reset(pp);
    if (SUCCEEDED(hr))
    {
        // A successful Reset restores every render state to its default, so
        // the shadow has to follow. The resource registry deliberately is not
        // cleared: managed resources survive a reset, and entries for released
        // default-pool ones are recycled by pointer on the next creation.
        m_state.SetDefaults();
        if (pp)
        {
            m_state.viewport.Width  = pp->BackBufferWidth;
            m_state.viewport.Height = pp->BackBufferHeight;
            if (!pp->EnableAutoDepthStencil)
                m_state.renderState[D3DRS_ZENABLE] = 0;
        }
        Logger::Log("[Render] Device Reset; shadow state re-defaulted.");
    }
    return hr;
}

HRESULT __stdcall ProxyDevice8::BeginScene()
{
    if (m_captureEnabled) Frame::OnBeginScene();
    return m_real->BeginScene();
}

HRESULT __stdcall ProxyDevice8::EndScene()
{
    return m_real->EndScene();
}

HRESULT __stdcall ProxyDevice8::Clear(DWORD Count, const D3DRECT* pRects,
                                      DWORD Flags, D3DCOLOR Color, float Z,
                                      DWORD Stencil)
{
    if (m_captureEnabled) Frame::OnClear(Flags, Color, Z);
    return m_real->Clear(Count, pRects, Flags, Color, Z, Stencil);
}

// ── Resource creation ────────────────────────────────────────────────────
HRESULT __stdcall ProxyDevice8::CreateVertexBuffer(UINT Length, DWORD Usage,
                                                   DWORD FVF, D3DPOOL Pool,
                                                   IDirect3DVertexBuffer8** ppVB)
{
    // Stripping WRITEONLY is what makes buffer contents readable at capture
    // time without wrapping every buffer object. It costs upload bandwidth,
    // so it only happens while capture is enabled.
    DWORD effectiveUsage = Usage;
    bool  stripped = false;
    if (m_captureEnabled && (Usage & D3DUSAGE_WRITEONLY))
    {
        effectiveUsage = Usage & ~(DWORD)D3DUSAGE_WRITEONLY;
        stripped = true;
    }

    const HRESULT hr = m_real->CreateVertexBuffer(Length, effectiveUsage, FVF,
                                                  Pool, ppVB);
    if (SUCCEEDED(hr) && ppVB && *ppVB && m_captureEnabled)
        Registry::AddVertexBuffer(*ppVB, Length, Usage, FVF, Pool, stripped,
                                  Frame::CurrentFrameIndex());
    return hr;
}

HRESULT __stdcall ProxyDevice8::CreateIndexBuffer(UINT Length, DWORD Usage,
                                                  D3DFORMAT Fmt, D3DPOOL Pool,
                                                  IDirect3DIndexBuffer8** ppIB)
{
    DWORD effectiveUsage = Usage;
    bool  stripped = false;
    if (m_captureEnabled && (Usage & D3DUSAGE_WRITEONLY))
    {
        effectiveUsage = Usage & ~(DWORD)D3DUSAGE_WRITEONLY;
        stripped = true;
    }

    const HRESULT hr = m_real->CreateIndexBuffer(Length, effectiveUsage, Fmt,
                                                 Pool, ppIB);
    if (SUCCEEDED(hr) && ppIB && *ppIB && m_captureEnabled)
        Registry::AddIndexBuffer(*ppIB, Length, Usage, Fmt, Pool, stripped,
                                 Frame::CurrentFrameIndex());
    return hr;
}

HRESULT __stdcall ProxyDevice8::CreateTexture(UINT W, UINT H, UINT Levels,
                                              DWORD Usage, D3DFORMAT Fmt,
                                              D3DPOOL Pool,
                                              IDirect3DTexture8** ppTexture)
{
    const HRESULT hr = m_real->CreateTexture(W, H, Levels, Usage, Fmt, Pool,
                                             ppTexture);
    if (SUCCEEDED(hr) && ppTexture && *ppTexture && m_captureEnabled)
    {
        // Levels == 0 asks D3D to generate a full mip chain, so the actual
        // count has to come from the created object.
        const UINT actualLevels = (*ppTexture)->GetLevelCount();
        Registry::AddTexture(*ppTexture, W, H, actualLevels, Usage, Fmt, Pool,
                             Frame::CurrentFrameIndex());
    }
    return hr;
}

// ── State setters that feed the shadow ───────────────────────────────────
HRESULT __stdcall ProxyDevice8::SetTransform(D3DTRANSFORMSTATETYPE State,
                                             const D3DMATRIX* pMatrix)
{
    if (pMatrix)
    {
        const uint32_t s = (uint32_t)State;
        if      (s == D3DTS_VIEW)       m_state.view       = *pMatrix;
        else if (s == D3DTS_PROJECTION) m_state.projection = *pMatrix;
        else if (s == D3DTS_WORLD)      m_state.world      = *pMatrix;
        else if (s > D3DTS_WORLD && s <= D3DTS_WORLD3)
            m_state.worldBlend[s - D3DTS_WORLD - 1] = *pMatrix;
        else if (s >= D3DTS_TEXTURE0 && s <= D3DTS_TEXTURE7)
            m_state.textureMatrix[s - D3DTS_TEXTURE0] = *pMatrix;
    }
    if (m_captureEnabled) Frame::OnSetTransform((uint32_t)State);
    return m_real->SetTransform(State, pMatrix);
}

HRESULT __stdcall ProxyDevice8::SetRenderState(D3DRENDERSTATETYPE State,
                                               DWORD Value)
{
    bool redundant = false;
    const uint32_t s = (uint32_t)State;
    if (s < D3DRS_SHADOW_COUNT)
    {
        redundant = (m_state.renderState[s] == Value);
        m_state.renderState[s] = Value;
    }
    if (m_captureEnabled) Frame::OnSetRenderState(s, Value, redundant);
    return m_real->SetRenderState(State, Value);
}

HRESULT __stdcall ProxyDevice8::SetTextureStageState(DWORD Stage,
                                                     D3DTEXTURESTAGESTATETYPE Type,
                                                     DWORD Value)
{
    if (Stage < D3D8_TEXTURE_STAGES && (uint32_t)Type < D3DTSS_SHADOW_COUNT)
        m_state.stageState[Stage][Type] = Value;
    if (m_captureEnabled) Frame::OnSetTextureStageState();
    return m_real->SetTextureStageState(Stage, Type, Value);
}

HRESULT __stdcall ProxyDevice8::SetTexture(DWORD Stage,
                                           IDirect3DBaseTexture8* pTexture)
{
    if (Stage < D3D8_TEXTURE_STAGES)
        m_state.texture[Stage] = pTexture;
    if (m_captureEnabled) Frame::OnSetTexture(pTexture);
    return m_real->SetTexture(Stage, pTexture);
}

HRESULT __stdcall ProxyDevice8::SetStreamSource(UINT Stream,
                                                IDirect3DVertexBuffer8* pData,
                                                UINT Stride)
{
    if (Stream < D3D8_MAX_STREAMS)
    {
        m_state.stream[Stream].buffer = pData;
        m_state.stream[Stream].stride = Stride;
    }
    return m_real->SetStreamSource(Stream, pData, Stride);
}

HRESULT __stdcall ProxyDevice8::SetIndices(IDirect3DIndexBuffer8* pIndexData,
                                           UINT BaseVertexIndex)
{
    m_state.indexBuffer     = pIndexData;
    m_state.baseVertexIndex = BaseVertexIndex;
    return m_real->SetIndices(pIndexData, BaseVertexIndex);
}

HRESULT __stdcall ProxyDevice8::SetVertexShader(DWORD Handle)
{
    m_state.vertexShader = Handle;
    return m_real->SetVertexShader(Handle);
}

HRESULT __stdcall ProxyDevice8::SetPixelShader(DWORD Handle)
{
    m_state.pixelShader = Handle;
    return m_real->SetPixelShader(Handle);
}

HRESULT __stdcall ProxyDevice8::SetViewport(const D3DVIEWPORT8* pViewport)
{
    if (pViewport) m_state.viewport = *pViewport;
    return m_real->SetViewport(pViewport);
}

HRESULT __stdcall ProxyDevice8::SetMaterial(const D3DMATERIAL8* pMaterial)
{
    if (pMaterial) m_state.material = *pMaterial;
    return m_real->SetMaterial(pMaterial);
}

HRESULT __stdcall ProxyDevice8::SetLight(DWORD Index, const D3DLIGHT8* pLight)
{
    if (Index < kMaxTrackedLights && pLight)
    {
        m_state.lights[Index].light   = *pLight;
        m_state.lights[Index].everSet = true;
    }
    return m_real->SetLight(Index, pLight);
}

HRESULT __stdcall ProxyDevice8::LightEnable(DWORD Index, BOOL Enable)
{
    if (Index < kMaxTrackedLights)
        m_state.lights[Index].enabled = (Enable != FALSE);
    return m_real->LightEnable(Index, Enable);
}

// ── Draw calls ───────────────────────────────────────────────────────────
void ProxyDevice8::RecordDraw(const DrawCallInfo& info)
{
    Frame::OnDraw(m_real, m_state, info);
}

HRESULT __stdcall ProxyDevice8::DrawPrimitive(D3DPRIMITIVETYPE Type,
                                              UINT StartVertex,
                                              UINT PrimitiveCount)
{
    if (m_captureEnabled)
    {
        DrawCallInfo info = DrawCallInfo();
        info.apiName        = "DrawPrimitive";
        info.primitiveType  = Type;
        info.primitiveCount = PrimitiveCount;
        info.startVertex    = StartVertex;
        RecordDraw(info);
    }
    return m_real->DrawPrimitive(Type, StartVertex, PrimitiveCount);
}

HRESULT __stdcall ProxyDevice8::DrawIndexedPrimitive(D3DPRIMITIVETYPE Type,
                                                     UINT MinIndex,
                                                     UINT NumVertices,
                                                     UINT StartIndex,
                                                     UINT PrimitiveCount)
{
    if (m_captureEnabled)
    {
        DrawCallInfo info = DrawCallInfo();
        info.apiName        = "DrawIndexedPrimitive";
        info.primitiveType  = Type;
        info.primitiveCount = PrimitiveCount;
        info.indexed        = true;
        info.minIndex       = MinIndex;
        info.numVertices    = NumVertices;
        info.startIndex     = StartIndex;
        RecordDraw(info);
    }
    return m_real->DrawIndexedPrimitive(Type, MinIndex, NumVertices,
                                        StartIndex, PrimitiveCount);
}

HRESULT __stdcall ProxyDevice8::DrawPrimitiveUP(D3DPRIMITIVETYPE Type,
                                                UINT PrimitiveCount,
                                                const void* pVertexData,
                                                UINT Stride)
{
    if (m_captureEnabled)
    {
        DrawCallInfo info = DrawCallInfo();
        info.apiName        = "DrawPrimitiveUP";
        info.primitiveType  = Type;
        info.primitiveCount = PrimitiveCount;
        info.userPointer    = true;
        info.upVertexData   = pVertexData;
        info.upVertexStride = Stride;
        RecordDraw(info);
    }
    return m_real->DrawPrimitiveUP(Type, PrimitiveCount, pVertexData, Stride);
}

HRESULT __stdcall ProxyDevice8::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE Type, UINT MinVertexIndex, UINT NumVertexIndices,
    UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexFormat,
    const void* pVertexData, UINT Stride)
{
    if (m_captureEnabled)
    {
        DrawCallInfo info = DrawCallInfo();
        info.apiName        = "DrawIndexedPrimitiveUP";
        info.primitiveType  = Type;
        info.primitiveCount = PrimitiveCount;
        info.indexed        = true;
        info.userPointer    = true;
        info.minIndex       = MinVertexIndex;
        info.numVertices    = NumVertexIndices;
        info.upIndexData    = pIndexData;
        info.upIndexFormat  = IndexFormat;
        info.upVertexData   = pVertexData;
        info.upVertexStride = Stride;
        RecordDraw(info);
    }
    return m_real->DrawIndexedPrimitiveUP(Type, MinVertexIndex, NumVertexIndices,
                                          PrimitiveCount, pIndexData, IndexFormat,
                                          pVertexData, Stride);
}

// ── Everything else: straight pass-through ───────────────────────────────
HRESULT __stdcall ProxyDevice8::TestCooperativeLevel()
    { return m_real->TestCooperativeLevel(); }
UINT __stdcall ProxyDevice8::GetAvailableTextureMem()
    { return m_real->GetAvailableTextureMem(); }
HRESULT __stdcall ProxyDevice8::ResourceManagerDiscardBytes(DWORD Bytes)
    { return m_real->ResourceManagerDiscardBytes(Bytes); }

HRESULT __stdcall ProxyDevice8::GetDirect3D(IDirect3D8** ppD3D8)
{
    // Hand back our own IDirect3D8 wrapper, not the real one — otherwise the
    // caller could create a second, unwrapped device behind our back.
    if (!ppD3D8) return D3DERR_INVALIDCALL;
    if (!m_parent) return m_real->GetDirect3D(ppD3D8);
    m_parent->AddRef();
    *ppD3D8 = m_parent;
    return D3D_OK;
}

HRESULT __stdcall ProxyDevice8::GetDeviceCaps(D3DCAPS8* pCaps)
    { return m_real->GetDeviceCaps(pCaps); }
HRESULT __stdcall ProxyDevice8::GetDisplayMode(D3DDISPLAYMODE* pMode)
    { return m_real->GetDisplayMode(pMode); }
HRESULT __stdcall ProxyDevice8::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p)
    { return m_real->GetCreationParameters(p); }
HRESULT __stdcall ProxyDevice8::SetCursorProperties(UINT X, UINT Y, IDirect3DSurface8* pBitmap)
    { return m_real->SetCursorProperties(X, Y, pBitmap); }
void __stdcall ProxyDevice8::SetCursorPosition(UINT X, UINT Y, DWORD Flags)
    { m_real->SetCursorPosition(X, Y, Flags); }
BOOL __stdcall ProxyDevice8::ShowCursor(BOOL bShow)
    { return m_real->ShowCursor(bShow); }
HRESULT __stdcall ProxyDevice8::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain8** ppSwapChain)
    { return m_real->CreateAdditionalSwapChain(pp, ppSwapChain); }
HRESULT __stdcall ProxyDevice8::GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8** ppBackBuffer)
    { return m_real->GetBackBuffer(BackBuffer, Type, ppBackBuffer); }
HRESULT __stdcall ProxyDevice8::GetRasterStatus(D3DRASTER_STATUS* pStatus)
    { return m_real->GetRasterStatus(pStatus); }
void __stdcall ProxyDevice8::SetGammaRamp(DWORD Flags, const D3DGAMMARAMP* pRamp)
    { m_real->SetGammaRamp(Flags, pRamp); }
void __stdcall ProxyDevice8::GetGammaRamp(D3DGAMMARAMP* pRamp)
    { m_real->GetGammaRamp(pRamp); }
HRESULT __stdcall ProxyDevice8::CreateVolumeTexture(UINT W, UINT H, UINT D, UINT L, DWORD U, D3DFORMAT F, D3DPOOL P, IDirect3DVolumeTexture8** pp)
    { return m_real->CreateVolumeTexture(W, H, D, L, U, F, P, pp); }
HRESULT __stdcall ProxyDevice8::CreateCubeTexture(UINT E, UINT L, DWORD U, D3DFORMAT F, D3DPOOL P, IDirect3DCubeTexture8** pp)
    { return m_real->CreateCubeTexture(E, L, U, F, P, pp); }
HRESULT __stdcall ProxyDevice8::CreateRenderTarget(UINT W, UINT H, D3DFORMAT F, D3DMULTISAMPLE_TYPE MS, BOOL Lockable, IDirect3DSurface8** pp)
    { return m_real->CreateRenderTarget(W, H, F, MS, Lockable, pp); }
HRESULT __stdcall ProxyDevice8::CreateDepthStencilSurface(UINT W, UINT H, D3DFORMAT F, D3DMULTISAMPLE_TYPE MS, IDirect3DSurface8** pp)
    { return m_real->CreateDepthStencilSurface(W, H, F, MS, pp); }
HRESULT __stdcall ProxyDevice8::CreateImageSurface(UINT W, UINT H, D3DFORMAT F, IDirect3DSurface8** pp)
    { return m_real->CreateImageSurface(W, H, F, pp); }
HRESULT __stdcall ProxyDevice8::CopyRects(IDirect3DSurface8* pSrc, const RECT* pSrcRects, UINT cRects, IDirect3DSurface8* pDst, const POINT* pDstPoints)
    { return m_real->CopyRects(pSrc, pSrcRects, cRects, pDst, pDstPoints); }
HRESULT __stdcall ProxyDevice8::UpdateTexture(IDirect3DBaseTexture8* pSrc, IDirect3DBaseTexture8* pDst)
    { return m_real->UpdateTexture(pSrc, pDst); }
HRESULT __stdcall ProxyDevice8::GetFrontBuffer(IDirect3DSurface8* pDestSurface)
    { return m_real->GetFrontBuffer(pDestSurface); }
HRESULT __stdcall ProxyDevice8::SetRenderTarget(IDirect3DSurface8* pRT, IDirect3DSurface8* pZ)
    { return m_real->SetRenderTarget(pRT, pZ); }
HRESULT __stdcall ProxyDevice8::GetRenderTarget(IDirect3DSurface8** ppRT)
    { return m_real->GetRenderTarget(ppRT); }
HRESULT __stdcall ProxyDevice8::GetDepthStencilSurface(IDirect3DSurface8** ppZ)
    { return m_real->GetDepthStencilSurface(ppZ); }
HRESULT __stdcall ProxyDevice8::GetTransform(D3DTRANSFORMSTATETYPE S, D3DMATRIX* pM)
    { return m_real->GetTransform(S, pM); }
HRESULT __stdcall ProxyDevice8::MultiplyTransform(D3DTRANSFORMSTATETYPE S, const D3DMATRIX* pM)
    { return m_real->MultiplyTransform(S, pM); }
HRESULT __stdcall ProxyDevice8::GetViewport(D3DVIEWPORT8* pViewport)
    { return m_real->GetViewport(pViewport); }
HRESULT __stdcall ProxyDevice8::GetMaterial(D3DMATERIAL8* pMaterial)
    { return m_real->GetMaterial(pMaterial); }
HRESULT __stdcall ProxyDevice8::GetLight(DWORD Index, D3DLIGHT8* pLight)
    { return m_real->GetLight(Index, pLight); }
HRESULT __stdcall ProxyDevice8::GetLightEnable(DWORD Index, BOOL* pEnable)
    { return m_real->GetLightEnable(Index, pEnable); }
HRESULT __stdcall ProxyDevice8::SetClipPlane(DWORD Index, const float* pPlane)
    { return m_real->SetClipPlane(Index, pPlane); }
HRESULT __stdcall ProxyDevice8::GetClipPlane(DWORD Index, float* pPlane)
    { return m_real->GetClipPlane(Index, pPlane); }
HRESULT __stdcall ProxyDevice8::GetRenderState(D3DRENDERSTATETYPE S, DWORD* pValue)
    { return m_real->GetRenderState(S, pValue); }
HRESULT __stdcall ProxyDevice8::BeginStateBlock()
    { return m_real->BeginStateBlock(); }
HRESULT __stdcall ProxyDevice8::EndStateBlock(DWORD* pToken)
    { return m_real->EndStateBlock(pToken); }
HRESULT __stdcall ProxyDevice8::ApplyStateBlock(DWORD Token)
{
    // A state block replays arbitrary state changes inside the runtime, which
    // our shadow never sees. The game does not use them (no CreateStateBlock
    // call site exists in pes6.exe), but if that ever changes the shadow would
    // silently drift, so it is worth a one-time warning.
    static bool warned = false;
    if (!warned) { warned = true;
        Logger::Log("[Render] WARNING: ApplyStateBlock used - shadow state "
                    "may drift from the real device."); }
    return m_real->ApplyStateBlock(Token);
}
HRESULT __stdcall ProxyDevice8::CaptureStateBlock(DWORD Token)
    { return m_real->CaptureStateBlock(Token); }
HRESULT __stdcall ProxyDevice8::DeleteStateBlock(DWORD Token)
    { return m_real->DeleteStateBlock(Token); }
HRESULT __stdcall ProxyDevice8::CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD* pToken)
    { return m_real->CreateStateBlock(Type, pToken); }
HRESULT __stdcall ProxyDevice8::SetClipStatus(const D3DCLIPSTATUS8* pClipStatus)
    { return m_real->SetClipStatus(pClipStatus); }
HRESULT __stdcall ProxyDevice8::GetClipStatus(D3DCLIPSTATUS8* pClipStatus)
    { return m_real->GetClipStatus(pClipStatus); }
HRESULT __stdcall ProxyDevice8::GetTexture(DWORD Stage, IDirect3DBaseTexture8** ppTexture)
    { return m_real->GetTexture(Stage, ppTexture); }
HRESULT __stdcall ProxyDevice8::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
    { return m_real->GetTextureStageState(Stage, Type, pValue); }
HRESULT __stdcall ProxyDevice8::ValidateDevice(DWORD* pNumPasses)
    { return m_real->ValidateDevice(pNumPasses); }
HRESULT __stdcall ProxyDevice8::GetInfo(DWORD DevInfoID, void* pStruct, DWORD Size)
    { return m_real->GetInfo(DevInfoID, pStruct, Size); }
HRESULT __stdcall ProxyDevice8::SetPaletteEntries(UINT n, const PALETTEENTRY* pEntries)
    { return m_real->SetPaletteEntries(n, pEntries); }
HRESULT __stdcall ProxyDevice8::GetPaletteEntries(UINT n, PALETTEENTRY* pEntries)
    { return m_real->GetPaletteEntries(n, pEntries); }
HRESULT __stdcall ProxyDevice8::SetCurrentTexturePalette(UINT n)
    { return m_real->SetCurrentTexturePalette(n); }
HRESULT __stdcall ProxyDevice8::GetCurrentTexturePalette(UINT* pn)
    { return m_real->GetCurrentTexturePalette(pn); }
HRESULT __stdcall ProxyDevice8::ProcessVertices(UINT S, UINT D, UINT C, IDirect3DVertexBuffer8* pDest, DWORD Flags)
    { return m_real->ProcessVertices(S, D, C, pDest, Flags); }
HRESULT __stdcall ProxyDevice8::CreateVertexShader(const DWORD* pDecl, const DWORD* pFunc, DWORD* pHandle, DWORD Usage)
{
    const HRESULT hr = m_real->CreateVertexShader(pDecl, pFunc, pHandle, Usage);

    // A declaration with pFunction == NULL is not a shader at all — it is a
    // vertex *declaration* driving the fixed-function pipeline. That is how
    // this game describes its 3D geometry, so the declaration has to be kept
    // or the vertex layout of every draw using it is unknowable.
    if (SUCCEEDED(hr) && pHandle && m_captureEnabled)
    {
        Registry::AddVertexShader((uint32_t)*pHandle, (const uint32_t*)pDecl,
                                  (const uint32_t*)pFunc);

        const Registry::VertexShaderInfo* info =
            Registry::FindVertexShader((uint32_t)*pHandle);
        char desc[256];
        Logger::Log("[Render] CreateVertexShader handle=0x%X %s (%u tokens): %s",
                    (unsigned)*pHandle,
                    pFunc ? "vs shader" : "declaration only",
                    info ? (unsigned)info->function.size() : 0u,
                    info ? D3D8Util::VertexDeclDescribe(
                               info->layout, pFunc == nullptr, desc,
                               sizeof(desc))
                         : "<undecodable>");
    }
    return hr;
}
HRESULT __stdcall ProxyDevice8::GetVertexShader(DWORD* pHandle)
    { return m_real->GetVertexShader(pHandle); }
HRESULT __stdcall ProxyDevice8::DeleteVertexShader(DWORD Handle)
{
    if (m_captureEnabled) Registry::RemoveVertexShader((uint32_t)Handle);
    return m_real->DeleteVertexShader(Handle);
}
HRESULT __stdcall ProxyDevice8::SetVertexShaderConstant(DWORD R, const void* pData, DWORD C)
{
    // For a shader-driven draw this is where the object and camera transforms
    // actually live; SetTransform is ignored by the vertex shader entirely.
    if (pData && C && R < kMaxVsConstants)
    {
        const DWORD count = (R + C > kMaxVsConstants) ? (kMaxVsConstants - R) : C;
        memcpy(&m_state.vsConstants[R][0], pData, (size_t)count * 4u * sizeof(float));
        if (R + count > m_state.vsConstantsHighWater)
            m_state.vsConstantsHighWater = R + count;
    }
    return m_real->SetVertexShaderConstant(R, pData, C);
}
HRESULT __stdcall ProxyDevice8::GetVertexShaderConstant(DWORD R, void* pData, DWORD C)
    { return m_real->GetVertexShaderConstant(R, pData, C); }
HRESULT __stdcall ProxyDevice8::GetVertexShaderDeclaration(DWORD H, void* pData, DWORD* pSize)
    { return m_real->GetVertexShaderDeclaration(H, pData, pSize); }
HRESULT __stdcall ProxyDevice8::GetVertexShaderFunction(DWORD H, void* pData, DWORD* pSize)
    { return m_real->GetVertexShaderFunction(H, pData, pSize); }
HRESULT __stdcall ProxyDevice8::GetStreamSource(UINT S, IDirect3DVertexBuffer8** pp, UINT* pStride)
    { return m_real->GetStreamSource(S, pp, pStride); }
HRESULT __stdcall ProxyDevice8::GetIndices(IDirect3DIndexBuffer8** pp, UINT* pBase)
    { return m_real->GetIndices(pp, pBase); }
HRESULT __stdcall ProxyDevice8::CreatePixelShader(const DWORD* pFunction, DWORD* pHandle)
{
    const HRESULT hr = m_real->CreatePixelShader(pFunction, pHandle);

    // Worth capturing for the same reason as the vertex shaders — the game
    // assembles these at runtime too, so the bytecode exists nowhere on disk.
    // It also decides how a draw is shaded: with a pixel shader bound, the
    // fixed-function texture stage states are ignored entirely.
    if (SUCCEEDED(hr) && pHandle && m_captureEnabled)
    {
        Registry::AddPixelShader((uint32_t)*pHandle, (const uint32_t*)pFunction);
        const Registry::PixelShaderInfo* info =
            Registry::FindPixelShader((uint32_t)*pHandle);
        Logger::Log("[Render] CreatePixelShader handle=0x%X (%u tokens).",
                    (unsigned)*pHandle,
                    info ? (unsigned)info->function.size() : 0u);
    }
    return hr;
}
HRESULT __stdcall ProxyDevice8::GetPixelShader(DWORD* pHandle)
    { return m_real->GetPixelShader(pHandle); }
HRESULT __stdcall ProxyDevice8::DeletePixelShader(DWORD Handle)
{
    if (m_captureEnabled) Registry::RemovePixelShader((uint32_t)Handle);
    return m_real->DeletePixelShader(Handle);
}
HRESULT __stdcall ProxyDevice8::SetPixelShaderConstant(DWORD R, const void* pData, DWORD C)
{
    if (pData && C && R < kMaxPsConstants)
    {
        const DWORD count = (R + C > kMaxPsConstants) ? (kMaxPsConstants - R) : C;
        memcpy(&m_state.psConstants[R][0], pData, (size_t)count * 4u * sizeof(float));
        if (R + count > m_state.psConstantsHighWater)
            m_state.psConstantsHighWater = R + count;
    }
    return m_real->SetPixelShaderConstant(R, pData, C);
}
HRESULT __stdcall ProxyDevice8::GetPixelShaderConstant(DWORD R, void* pData, DWORD C)
    { return m_real->GetPixelShaderConstant(R, pData, C); }
HRESULT __stdcall ProxyDevice8::GetPixelShaderFunction(DWORD H, void* pData, DWORD* pSize)
    { return m_real->GetPixelShaderFunction(H, pData, pSize); }
HRESULT __stdcall ProxyDevice8::DrawRectPatch(UINT H, const float* pSegs, const void* pInfo)
    { return m_real->DrawRectPatch(H, pSegs, pInfo); }
HRESULT __stdcall ProxyDevice8::DrawTriPatch(UINT H, const float* pSegs, const void* pInfo)
    { return m_real->DrawTriPatch(H, pSegs, pInfo); }
HRESULT __stdcall ProxyDevice8::DeletePatch(UINT Handle)
    { return m_real->DeletePatch(Handle); }

} // namespace Capture
