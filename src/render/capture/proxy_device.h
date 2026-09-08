// proxy_device.h
//
// A pass-through IDirect3DDevice8 that sits between pes6.exe and the real
// D3D8 runtime.
//
// The game stores the device pointer once (DAT_00f83e68, written by
// CreateDevice inside FUN_00405300) and every one of the 200+ call sites in
// the binary dispatches through it by hard-coded vtable offset. Substituting
// this object at creation time therefore captures the entire render stream
// with no code patching at all — nothing in the game is modified, which is
// why this can be left enabled without affecting the existing gameplay hooks.
//
// Every method forwards to the real device. The ones that carry information
// worth keeping also update the shadow state and notify the capture layer.
#pragma once

#include "../d3d8/d3d8_min.h"
#include "state_tracker.h"
#include "frame_capture.h"

namespace Capture
{
    class ProxyD3D8;

    class ProxyDevice8 : public IDirect3DDevice8
    {
    public:
        ProxyDevice8(IDirect3DDevice8* real, ProxyD3D8* parent,
                     const D3DPRESENT_PARAMETERS& pp, bool captureEnabled);

        IDirect3DDevice8*  Real()  const { return m_real; }
        const DeviceState& State() const { return m_state; }

        // ── IUnknown ─────────────────────────────────────────────────────
        HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObj) override;
        ULONG   __stdcall AddRef() override;
        ULONG   __stdcall Release() override;

        // ── IDirect3DDevice8 ─────────────────────────────────────────────
        HRESULT __stdcall TestCooperativeLevel() override;
        UINT    __stdcall GetAvailableTextureMem() override;
        HRESULT __stdcall ResourceManagerDiscardBytes(DWORD Bytes) override;
        HRESULT __stdcall GetDirect3D(IDirect3D8** ppD3D8) override;
        HRESULT __stdcall GetDeviceCaps(D3DCAPS8* pCaps) override;
        HRESULT __stdcall GetDisplayMode(D3DDISPLAYMODE* pMode) override;
        HRESULT __stdcall GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) override;
        HRESULT __stdcall SetCursorProperties(UINT X, UINT Y, IDirect3DSurface8* pBitmap) override;
        void    __stdcall SetCursorPosition(UINT X, UINT Y, DWORD Flags) override;
        BOOL    __stdcall ShowCursor(BOOL bShow) override;
        HRESULT __stdcall CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain8** ppSwapChain) override;
        HRESULT __stdcall Reset(D3DPRESENT_PARAMETERS* pp) override;
        HRESULT __stdcall Present(const RECT* pSrc, const RECT* pDst, HWND hOverride, const RGNDATA* pDirty) override;
        HRESULT __stdcall GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8** ppBackBuffer) override;
        HRESULT __stdcall GetRasterStatus(D3DRASTER_STATUS* pStatus) override;
        void    __stdcall SetGammaRamp(DWORD Flags, const D3DGAMMARAMP* pRamp) override;
        void    __stdcall GetGammaRamp(D3DGAMMARAMP* pRamp) override;
        HRESULT __stdcall CreateTexture(UINT W, UINT H, UINT Levels, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DTexture8** ppTexture) override;
        HRESULT __stdcall CreateVolumeTexture(UINT W, UINT H, UINT D, UINT Levels, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DVolumeTexture8** ppVolumeTexture) override;
        HRESULT __stdcall CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DCubeTexture8** ppCubeTexture) override;
        HRESULT __stdcall CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8** ppVertexBuffer) override;
        HRESULT __stdcall CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DIndexBuffer8** ppIndexBuffer) override;
        HRESULT __stdcall CreateRenderTarget(UINT W, UINT H, D3DFORMAT Fmt, D3DMULTISAMPLE_TYPE MS, BOOL Lockable, IDirect3DSurface8** ppSurface) override;
        HRESULT __stdcall CreateDepthStencilSurface(UINT W, UINT H, D3DFORMAT Fmt, D3DMULTISAMPLE_TYPE MS, IDirect3DSurface8** ppSurface) override;
        HRESULT __stdcall CreateImageSurface(UINT W, UINT H, D3DFORMAT Fmt, IDirect3DSurface8** ppSurface) override;
        HRESULT __stdcall CopyRects(IDirect3DSurface8* pSrc, const RECT* pSrcRects, UINT cRects, IDirect3DSurface8* pDst, const POINT* pDstPoints) override;
        HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture8* pSrc, IDirect3DBaseTexture8* pDst) override;
        HRESULT __stdcall GetFrontBuffer(IDirect3DSurface8* pDestSurface) override;
        HRESULT __stdcall SetRenderTarget(IDirect3DSurface8* pRT, IDirect3DSurface8* pZ) override;
        HRESULT __stdcall GetRenderTarget(IDirect3DSurface8** ppRT) override;
        HRESULT __stdcall GetDepthStencilSurface(IDirect3DSurface8** ppZ) override;
        HRESULT __stdcall BeginScene() override;
        HRESULT __stdcall EndScene() override;
        HRESULT __stdcall Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override;
        HRESULT __stdcall SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) override;
        HRESULT __stdcall GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override;
        HRESULT __stdcall MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) override;
        HRESULT __stdcall SetViewport(const D3DVIEWPORT8* pViewport) override;
        HRESULT __stdcall GetViewport(D3DVIEWPORT8* pViewport) override;
        HRESULT __stdcall SetMaterial(const D3DMATERIAL8* pMaterial) override;
        HRESULT __stdcall GetMaterial(D3DMATERIAL8* pMaterial) override;
        HRESULT __stdcall SetLight(DWORD Index, const D3DLIGHT8* pLight) override;
        HRESULT __stdcall GetLight(DWORD Index, D3DLIGHT8* pLight) override;
        HRESULT __stdcall LightEnable(DWORD Index, BOOL Enable) override;
        HRESULT __stdcall GetLightEnable(DWORD Index, BOOL* pEnable) override;
        HRESULT __stdcall SetClipPlane(DWORD Index, const float* pPlane) override;
        HRESULT __stdcall GetClipPlane(DWORD Index, float* pPlane) override;
        HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override;
        HRESULT __stdcall GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) override;
        HRESULT __stdcall BeginStateBlock() override;
        HRESULT __stdcall EndStateBlock(DWORD* pToken) override;
        HRESULT __stdcall ApplyStateBlock(DWORD Token) override;
        HRESULT __stdcall CaptureStateBlock(DWORD Token) override;
        HRESULT __stdcall DeleteStateBlock(DWORD Token) override;
        HRESULT __stdcall CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD* pToken) override;
        HRESULT __stdcall SetClipStatus(const D3DCLIPSTATUS8* pClipStatus) override;
        HRESULT __stdcall GetClipStatus(D3DCLIPSTATUS8* pClipStatus) override;
        HRESULT __stdcall GetTexture(DWORD Stage, IDirect3DBaseTexture8** ppTexture) override;
        HRESULT __stdcall SetTexture(DWORD Stage, IDirect3DBaseTexture8* pTexture) override;
        HRESULT __stdcall GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override;
        HRESULT __stdcall SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
        HRESULT __stdcall ValidateDevice(DWORD* pNumPasses) override;
        HRESULT __stdcall GetInfo(DWORD DevInfoID, void* pDevInfoStruct, DWORD Size) override;
        HRESULT __stdcall SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) override;
        HRESULT __stdcall GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) override;
        HRESULT __stdcall SetCurrentTexturePalette(UINT PaletteNumber) override;
        HRESULT __stdcall GetCurrentTexturePalette(UINT* PaletteNumber) override;
        HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE Type, UINT StartVertex, UINT PrimitiveCount) override;
        HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE Type, UINT MinIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount) override;
        HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE Type, UINT PrimitiveCount, const void* pVertexData, UINT Stride) override;
        HRESULT __stdcall DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE Type, UINT MinVertexIndex, UINT NumVertexIndices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexFormat, const void* pVertexData, UINT Stride) override;
        HRESULT __stdcall ProcessVertices(UINT SrcStart, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer8* pDest, DWORD Flags) override;
        HRESULT __stdcall CreateVertexShader(const DWORD* pDecl, const DWORD* pFunc, DWORD* pHandle, DWORD Usage) override;
        HRESULT __stdcall SetVertexShader(DWORD Handle) override;
        HRESULT __stdcall GetVertexShader(DWORD* pHandle) override;
        HRESULT __stdcall DeleteVertexShader(DWORD Handle) override;
        HRESULT __stdcall SetVertexShaderConstant(DWORD Reg, const void* pData, DWORD Count) override;
        HRESULT __stdcall GetVertexShaderConstant(DWORD Reg, void* pData, DWORD Count) override;
        HRESULT __stdcall GetVertexShaderDeclaration(DWORD Handle, void* pData, DWORD* pSize) override;
        HRESULT __stdcall GetVertexShaderFunction(DWORD Handle, void* pData, DWORD* pSize) override;
        HRESULT __stdcall SetStreamSource(UINT Stream, IDirect3DVertexBuffer8* pData, UINT Stride) override;
        HRESULT __stdcall GetStreamSource(UINT Stream, IDirect3DVertexBuffer8** ppData, UINT* pStride) override;
        HRESULT __stdcall SetIndices(IDirect3DIndexBuffer8* pIndexData, UINT BaseVertexIndex) override;
        HRESULT __stdcall GetIndices(IDirect3DIndexBuffer8** ppIndexData, UINT* pBaseVertexIndex) override;
        HRESULT __stdcall CreatePixelShader(const DWORD* pFunction, DWORD* pHandle) override;
        HRESULT __stdcall SetPixelShader(DWORD Handle) override;
        HRESULT __stdcall GetPixelShader(DWORD* pHandle) override;
        HRESULT __stdcall DeletePixelShader(DWORD Handle) override;
        HRESULT __stdcall SetPixelShaderConstant(DWORD Reg, const void* pData, DWORD Count) override;
        HRESULT __stdcall GetPixelShaderConstant(DWORD Reg, void* pData, DWORD Count) override;
        HRESULT __stdcall GetPixelShaderFunction(DWORD Handle, void* pData, DWORD* pSize) override;
        HRESULT __stdcall DrawRectPatch(UINT Handle, const float* pNumSegs, const void* pRectPatchInfo) override;
        HRESULT __stdcall DrawTriPatch(UINT Handle, const float* pNumSegs, const void* pTriPatchInfo) override;
        HRESULT __stdcall DeletePatch(UINT Handle) override;

    private:
        // Shared tail of all four draw entry points.
        void RecordDraw(const DrawCallInfo& info);

        // Polls the capture hotkey. Called once per Present.
        void PollHotkeys();

        IDirect3DDevice8* m_real;
        ProxyD3D8*        m_parent;
        LONG              m_refCount;
        DeviceState       m_state;
        bool              m_captureEnabled;
        bool              m_hotkeyDown;
    };
}
