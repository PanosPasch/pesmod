// d3d8_min.h
//
// A minimal, self-contained declaration of the Direct3D 8 API surface that
// PESMod needs. The DirectX 8 SDK has not shipped with the Windows SDK for
// well over a decade, so rather than depend on a headers package that is no
// longer obtainable, the interfaces are vendored here — the same approach the
// repo already takes with MinHook.
//
// Only what the interception layer actually touches is fully fleshed out.
// Everything else is declared faithfully enough to keep the vtables correct,
// which is the part that matters: our proxy objects must present a vtable
// that is byte-for-byte compatible with the real one, because pes6.exe calls
// every method by hard-coded offset (e.g. `call [ecx+0xC8]` for
// SetRenderState).
//
// The layout below was cross-checked against five independent call sites in
// pes6.exe (md5 678e9ac0e741207dddb9aa33caed47a0):
//
//   IDirect3DDevice8 vtable     game evidence
//   ─────────────────────────   ───────────────────────────────────────────
//   +0x0C  TestCooperativeLevel 0x00403e15: ff 51 0c, cmp eax,0x88760868
//   +0x3C  Present              0x00405850: (this,0,0,0,0)
//   +0x90  Clear                0x00405300: (this,0,0,3,0,1.0f,0)
//   +0xC8  SetRenderState       0x0087be70: (this,0x1b,1) → ALPHABLENDENABLE
//   +0x120 DrawPrimitiveUP      0x0087be70: (this,5,2,ptr,0x14) → tri-strip
//
//   IDirect3D8 vtable           game evidence
//   ─────────────────────────   ───────────────────────────────────────────
//   +0x20  GetAdapterDisplayMode 0x00405300
//   +0x24  CheckDeviceType       0x004051d0
//   +0x34  GetDeviceCaps         0x00404c60
//   +0x3C  CreateDevice          0x00405300 → writes DAT_00f83e68
//
#pragma once

#include <windows.h>
#include <unknwn.h>

// ── Handles and simple typedefs ──────────────────────────────────────────
typedef DWORD D3DCOLOR;

#define D3D_SDK_VERSION 220   // pes6.exe calls Direct3DCreate8(0xdc)

// ── Result codes ─────────────────────────────────────────────────────────
// Confirmed against the literal comparisons in FUN_00403e10 / FUN_00405850.
#define D3D_OK                      S_OK
#define D3DERR_DEVICELOST           ((HRESULT)0x88760868L)
#define D3DERR_DEVICENOTRESET       ((HRESULT)0x88760869L)
#define D3DERR_NOTAVAILABLE         ((HRESULT)0x8876086AL)
#define D3DERR_INVALIDCALL          ((HRESULT)0x8876086CL)
#define D3DERR_OUTOFVIDEOMEMORY     ((HRESULT)0x8876017CL)

// ── Enumerations ─────────────────────────────────────────────────────────
typedef enum _D3DDEVTYPE {
    D3DDEVTYPE_HAL = 1, D3DDEVTYPE_REF = 2, D3DDEVTYPE_SW = 3,
    D3DDEVTYPE_FORCE_DWORD = 0x7fffffff
} D3DDEVTYPE;

typedef enum _D3DFORMAT {
    D3DFMT_UNKNOWN = 0,
    D3DFMT_R8G8B8 = 20, D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22,
    D3DFMT_R5G6B5 = 23, D3DFMT_X1R5G5B5 = 24, D3DFMT_A1R5G5B5 = 25,
    D3DFMT_A4R4G4B4 = 26, D3DFMT_R3G3B2 = 27, D3DFMT_A8 = 28,
    D3DFMT_A8R3G3B2 = 29, D3DFMT_X4R4G4B4 = 30,
    D3DFMT_A8P8 = 40, D3DFMT_P8 = 41,
    D3DFMT_L8 = 50, D3DFMT_A8L8 = 51, D3DFMT_A4L4 = 52,
    D3DFMT_V8U8 = 60, D3DFMT_L6V5U5 = 61, D3DFMT_X8L8V8U8 = 62,
    D3DFMT_Q8W8V8U8 = 63, D3DFMT_V16U16 = 64, D3DFMT_W11V11U10 = 65,
    D3DFMT_UYVY = 0x59565955, D3DFMT_YUY2 = 0x32595559,
    D3DFMT_DXT1 = 0x31545844, D3DFMT_DXT2 = 0x32545844,
    D3DFMT_DXT3 = 0x33545844, D3DFMT_DXT4 = 0x34545844,
    D3DFMT_DXT5 = 0x35545844,
    D3DFMT_D16_LOCKABLE = 70, D3DFMT_D32 = 71, D3DFMT_D15S1 = 73,
    D3DFMT_D24S8 = 75, D3DFMT_D24X8 = 77, D3DFMT_D24X4S4 = 79,
    D3DFMT_D16 = 80,
    D3DFMT_VERTEXDATA = 100, D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102,
    D3DFMT_FORCE_DWORD = 0x7fffffff
} D3DFORMAT;

typedef enum _D3DRESOURCETYPE {
    D3DRTYPE_SURFACE = 1, D3DRTYPE_VOLUME = 2, D3DRTYPE_TEXTURE = 3,
    D3DRTYPE_VOLUMETEXTURE = 4, D3DRTYPE_CUBETEXTURE = 5,
    D3DRTYPE_VERTEXBUFFER = 6, D3DRTYPE_INDEXBUFFER = 7,
    D3DRTYPE_FORCE_DWORD = 0x7fffffff
} D3DRESOURCETYPE;

typedef enum _D3DPOOL {
    D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2,
    D3DPOOL_SCRATCH = 3, D3DPOOL_FORCE_DWORD = 0x7fffffff
} D3DPOOL;

typedef enum _D3DPRIMITIVETYPE {
    D3DPT_POINTLIST = 1, D3DPT_LINELIST = 2, D3DPT_LINESTRIP = 3,
    D3DPT_TRIANGLELIST = 4, D3DPT_TRIANGLESTRIP = 5, D3DPT_TRIANGLEFAN = 6,
    D3DPT_FORCE_DWORD = 0x7fffffff
} D3DPRIMITIVETYPE;

// D3DTS_WORLD is 256; the WORLDn matrices run 256..511. VIEW/PROJECTION are
// the low indices, which is why the state tracker stores them separately.
typedef enum _D3DTRANSFORMSTATETYPE {
    D3DTS_VIEW = 2, D3DTS_PROJECTION = 3,
    D3DTS_TEXTURE0 = 16, D3DTS_TEXTURE1 = 17, D3DTS_TEXTURE2 = 18,
    D3DTS_TEXTURE3 = 19, D3DTS_TEXTURE4 = 20, D3DTS_TEXTURE5 = 21,
    D3DTS_TEXTURE6 = 22, D3DTS_TEXTURE7 = 23,
    D3DTS_WORLD = 256, D3DTS_WORLD1 = 257, D3DTS_WORLD2 = 258,
    D3DTS_WORLD3 = 259,
    D3DTS_FORCE_DWORD = 0x7fffffff
} D3DTRANSFORMSTATETYPE;

typedef enum _D3DRENDERSTATETYPE {
    D3DRS_ZENABLE = 7, D3DRS_FILLMODE = 8, D3DRS_SHADEMODE = 9,
    D3DRS_LINEPATTERN = 10, D3DRS_ZWRITEENABLE = 14,
    D3DRS_ALPHATESTENABLE = 15, D3DRS_LASTPIXEL = 16,
    D3DRS_SRCBLEND = 19, D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22,
    D3DRS_ZFUNC = 23, D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25,
    D3DRS_DITHERENABLE = 26, D3DRS_ALPHABLENDENABLE = 27,
    D3DRS_FOGENABLE = 28, D3DRS_SPECULARENABLE = 29, D3DRS_FOGCOLOR = 34,
    D3DRS_FOGTABLEMODE = 35, D3DRS_FOGSTART = 36, D3DRS_FOGEND = 37,
    D3DRS_FOGDENSITY = 38, D3DRS_ZBIAS = 47, D3DRS_RANGEFOGENABLE = 48,
    D3DRS_STENCILENABLE = 52, D3DRS_STENCILFAIL = 53,
    D3DRS_STENCILZFAIL = 54, D3DRS_STENCILPASS = 55, D3DRS_STENCILFUNC = 56,
    D3DRS_STENCILREF = 57, D3DRS_STENCILMASK = 58,
    D3DRS_STENCILWRITEMASK = 59, D3DRS_TEXTUREFACTOR = 60,
    D3DRS_WRAP0 = 128, D3DRS_CLIPPING = 136, D3DRS_LIGHTING = 137,
    D3DRS_AMBIENT = 139, D3DRS_FOGVERTEXMODE = 140,
    D3DRS_COLORVERTEX = 141, D3DRS_LOCALVIEWER = 142,
    D3DRS_NORMALIZENORMALS = 143, D3DRS_DIFFUSEMATERIALSOURCE = 145,
    D3DRS_SPECULARMATERIALSOURCE = 146, D3DRS_AMBIENTMATERIALSOURCE = 147,
    D3DRS_EMISSIVEMATERIALSOURCE = 148, D3DRS_VERTEXBLEND = 151,
    D3DRS_CLIPPLANEENABLE = 152, D3DRS_SOFTWAREVERTEXPROCESSING = 153,
    D3DRS_POINTSIZE = 154, D3DRS_POINTSIZE_MIN = 155,
    D3DRS_POINTSPRITEENABLE = 156, D3DRS_POINTSCALEENABLE = 157,
    D3DRS_MULTISAMPLEANTIALIAS = 161, D3DRS_MULTISAMPLEMASK = 162,
    D3DRS_PATCHEDGESTYLE = 163, D3DRS_COLORWRITEENABLE = 168,
    D3DRS_TWEENFACTOR = 170, D3DRS_BLENDOP = 171,
    D3DRS_POSITIONORDER = 172, D3DRS_NORMALORDER = 173,
    D3DRS_FORCE_DWORD = 0x7fffffff
} D3DRENDERSTATETYPE;

// Depth and alpha comparison functions, the values D3DRS_ZFUNC and
// D3DRS_ALPHAFUNC carry.
typedef enum _D3DCMPFUNC {
    D3DCMP_NEVER = 1, D3DCMP_LESS = 2, D3DCMP_EQUAL = 3,
    D3DCMP_LESSEQUAL = 4, D3DCMP_GREATER = 5, D3DCMP_NOTEQUAL = 6,
    D3DCMP_GREATEREQUAL = 7, D3DCMP_ALWAYS = 8,
    D3DCMP_FORCE_DWORD = 0x7fffffff
} D3DCMPFUNC;

typedef enum _D3DTEXTURESTAGESTATETYPE {
    D3DTSS_COLOROP = 1, D3DTSS_COLORARG1 = 2, D3DTSS_COLORARG2 = 3,
    D3DTSS_ALPHAOP = 4, D3DTSS_ALPHAARG1 = 5, D3DTSS_ALPHAARG2 = 6,
    D3DTSS_BUMPENVMAT00 = 7, D3DTSS_TEXCOORDINDEX = 11,
    D3DTSS_ADDRESSU = 13, D3DTSS_ADDRESSV = 14, D3DTSS_BORDERCOLOR = 15,
    D3DTSS_MAGFILTER = 16, D3DTSS_MINFILTER = 17, D3DTSS_MIPFILTER = 18,
    D3DTSS_MIPMAPLODBIAS = 19, D3DTSS_MAXMIPLEVEL = 20,
    D3DTSS_MAXANISOTROPY = 21, D3DTSS_ADDRESSW = 25,
    D3DTSS_COLORARG0 = 26, D3DTSS_ALPHAARG0 = 27, D3DTSS_RESULTARG = 28,
    D3DTSS_FORCE_DWORD = 0x7fffffff
} D3DTEXTURESTAGESTATETYPE;

// Shadow-array sizes used by the state tracker.
#define D3DRS_SHADOW_COUNT   256   // render-state indices are all < 256
#define D3DTSS_SHADOW_COUNT   32   // stage-state indices are all < 32
#define D3D8_TEXTURE_STAGES    8   // fixed-function stage count
#define D3D8_MAX_STREAMS      16

typedef enum _D3DSWAPEFFECT {
    D3DSWAPEFFECT_DISCARD = 1, D3DSWAPEFFECT_FLIP = 2, D3DSWAPEFFECT_COPY = 3,
    D3DSWAPEFFECT_COPY_VSYNC = 4, D3DSWAPEFFECT_FORCE_DWORD = 0x7fffffff
} D3DSWAPEFFECT;

typedef enum _D3DMULTISAMPLE_TYPE {
    D3DMULTISAMPLE_NONE = 0, D3DMULTISAMPLE_2_SAMPLES = 2,
    D3DMULTISAMPLE_FORCE_DWORD = 0x7fffffff
} D3DMULTISAMPLE_TYPE;

typedef enum _D3DBACKBUFFER_TYPE {
    D3DBACKBUFFER_TYPE_MONO = 0, D3DBACKBUFFER_TYPE_LEFT = 1,
    D3DBACKBUFFER_TYPE_RIGHT = 2, D3DBACKBUFFER_TYPE_FORCE_DWORD = 0x7fffffff
} D3DBACKBUFFER_TYPE;

typedef enum _D3DCUBEMAP_FACES {
    D3DCUBEMAP_FACE_POSITIVE_X = 0, D3DCUBEMAP_FACE_NEGATIVE_X = 1,
    D3DCUBEMAP_FACE_POSITIVE_Y = 2, D3DCUBEMAP_FACE_NEGATIVE_Y = 3,
    D3DCUBEMAP_FACE_POSITIVE_Z = 4, D3DCUBEMAP_FACE_NEGATIVE_Z = 5,
    D3DCUBEMAP_FACE_FORCE_DWORD = 0x7fffffff
} D3DCUBEMAP_FACES;

typedef enum _D3DSTATEBLOCKTYPE {
    D3DSBT_ALL = 1, D3DSBT_PIXELSTATE = 2, D3DSBT_VERTEXSTATE = 3,
    D3DSBT_FORCE_DWORD = 0x7fffffff
} D3DSTATEBLOCKTYPE;

// ── Flexible vertex format ───────────────────────────────────────────────
// SetVertexShader is called with these codes rather than shader handles —
// pes6.exe contains no vertex shader tokens at all, so every draw is
// fixed-function and its vertex layout is fully described by the FVF.
#define D3DFVF_RESERVED0        0x001
#define D3DFVF_POSITION_MASK    0x00E
#define D3DFVF_XYZ              0x002
#define D3DFVF_XYZRHW           0x004
#define D3DFVF_XYZB1            0x006
#define D3DFVF_XYZB2            0x008
#define D3DFVF_XYZB3            0x00a
#define D3DFVF_XYZB4            0x00c
#define D3DFVF_XYZB5            0x00e
#define D3DFVF_NORMAL           0x010
#define D3DFVF_PSIZE            0x020
#define D3DFVF_DIFFUSE          0x040
#define D3DFVF_SPECULAR         0x080
#define D3DFVF_TEXCOUNT_MASK    0xf00
#define D3DFVF_TEXCOUNT_SHIFT   8

// Lock flags
#define D3DLOCK_READONLY        0x00000010L
#define D3DLOCK_NOSYSLOCK       0x00000800L
#define D3DLOCK_NOOVERWRITE     0x00001000L
#define D3DLOCK_DISCARD         0x00002000L

// Usage flags
#define D3DUSAGE_RENDERTARGET   0x00000001L
#define D3DUSAGE_DEPTHSTENCIL   0x00000002L
#define D3DUSAGE_WRITEONLY      0x00000008L
#define D3DUSAGE_DYNAMIC        0x00000200L

// Clear flags — FUN_00405300 clears with 3 == TARGET|ZBUFFER.
#define D3DCLEAR_TARGET         0x00000001L
#define D3DCLEAR_ZBUFFER        0x00000002L
#define D3DCLEAR_STENCIL        0x00000004L

// ── Structures ───────────────────────────────────────────────────────────
typedef struct _D3DVECTOR { float x, y, z; } D3DVECTOR;

typedef struct _D3DCOLORVALUE { float r, g, b, a; } D3DCOLORVALUE;

typedef struct _D3DRECT { LONG x1, y1, x2, y2; } D3DRECT;

// The named-field / m[4][4] overlay is exactly how the real d3d8.h declares
// this, and callers use both spellings, so the nameless union stays and the
// /W4 warning about it is suppressed here rather than project-wide.
#pragma warning(push)
#pragma warning(disable: 4201)   // nonstandard extension: nameless struct/union
typedef struct _D3DMATRIX {
    union {
        struct {
            float _11, _12, _13, _14;
            float _21, _22, _23, _24;
            float _31, _32, _33, _34;
            float _41, _42, _43, _44;
        };
        float m[4][4];
    };
} D3DMATRIX;
#pragma warning(pop)

typedef struct _D3DVIEWPORT8 {
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
} D3DVIEWPORT8;

typedef struct _D3DMATERIAL8 {
    D3DCOLORVALUE Diffuse, Ambient, Specular, Emissive;
    float Power;
} D3DMATERIAL8;

typedef struct _D3DLIGHT8 {
    DWORD Type;
    D3DCOLORVALUE Diffuse, Specular, Ambient;
    D3DVECTOR Position, Direction;
    float Range, Falloff, Attenuation0, Attenuation1, Attenuation2;
    float Theta, Phi;
} D3DLIGHT8;

typedef struct _D3DCLIPSTATUS8 { DWORD ClipUnion, ClipIntersection; } D3DCLIPSTATUS8;

typedef struct _D3DDISPLAYMODE {
    UINT Width, Height, RefreshRate;
    D3DFORMAT Format;
} D3DDISPLAYMODE;

typedef struct _D3DPRESENT_PARAMETERS_ {
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
} D3DPRESENT_PARAMETERS;

typedef struct _D3DDEVICE_CREATION_PARAMETERS {
    UINT AdapterOrdinal;
    D3DDEVTYPE DeviceType;
    HWND hFocusWindow;
    DWORD BehaviorFlags;
} D3DDEVICE_CREATION_PARAMETERS;

typedef struct _D3DSURFACE_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    UINT Width, Height;
} D3DSURFACE_DESC;

typedef struct _D3DVOLUME_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    UINT Width, Height, Depth;
} D3DVOLUME_DESC;

typedef struct _D3DVERTEXBUFFER_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    DWORD FVF;
} D3DVERTEXBUFFER_DESC;

typedef struct _D3DINDEXBUFFER_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
} D3DINDEXBUFFER_DESC;

typedef struct _D3DLOCKED_RECT { INT Pitch; void* pBits; } D3DLOCKED_RECT;

typedef struct _D3DLOCKED_BOX { INT RowPitch, SlicePitch; void* pBits; } D3DLOCKED_BOX;

typedef struct _D3DBOX { UINT Left, Top, Right, Bottom, Front, Back; } D3DBOX;

typedef struct _D3DRASTER_STATUS { BOOL InVBlank; UINT ScanLine; } D3DRASTER_STATUS;

typedef struct _D3DGAMMARAMP { WORD red[256], green[256], blue[256]; } D3DGAMMARAMP;

#define MAX_DEVICE_IDENTIFIER_STRING 512
typedef struct _D3DADAPTER_IDENTIFIER8 {
    char Driver[MAX_DEVICE_IDENTIFIER_STRING];
    char Description[MAX_DEVICE_IDENTIFIER_STRING];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId, DeviceId, SubSysId, Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
} D3DADAPTER_IDENTIFIER8;

// D3DCAPS8 is passed straight through, but the interception layer reports a
// few fields (MaxTextureWidth, MaxPrimitiveCount, TextureCaps) so the field
// order has to be right. pes6.exe keeps its copy at DAT_00f83ef0 and tests
// bit 0x80000 of DevCaps in FUN_004051d0.
typedef struct _D3DCAPS8 {
    D3DDEVTYPE DeviceType;
    UINT AdapterOrdinal;
    DWORD Caps, Caps2, Caps3, PresentationIntervals;
    DWORD CursorCaps;
    DWORD DevCaps;
    DWORD PrimitiveMiscCaps, RasterCaps, ZCmpCaps, SrcBlendCaps, DestBlendCaps;
    DWORD AlphaCmpCaps, ShadeCaps, TextureCaps;
    DWORD TextureFilterCaps, CubeTextureFilterCaps, VolumeTextureFilterCaps;
    DWORD TextureAddressCaps, VolumeTextureAddressCaps;
    DWORD LineCaps;
    DWORD MaxTextureWidth, MaxTextureHeight;
    DWORD MaxVolumeExtent;
    DWORD MaxTextureRepeat, MaxTextureAspectRatio, MaxAnisotropy;
    float MaxVertexW;
    float GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom;
    float ExtentsAdjust;
    DWORD StencilCaps, FVFCaps, TextureOpCaps;
    DWORD MaxTextureBlendStages, MaxSimultaneousTextures;
    DWORD VertexProcessingCaps;
    DWORD MaxActiveLights, MaxUserClipPlanes, MaxVertexBlendMatrices;
    DWORD MaxVertexBlendMatrixIndex;
    float MaxPointSize;
    DWORD MaxPrimitiveCount, MaxVertexIndex;
    DWORD MaxStreams, MaxStreamStride;
    DWORD VertexShaderVersion, MaxVertexShaderConst;
    DWORD PixelShaderVersion;
    float MaxPixelShaderValue;
} D3DCAPS8;

// ── Interface forward declarations ───────────────────────────────────────
struct IDirect3D8;
struct IDirect3DDevice8;
struct IDirect3DResource8;
struct IDirect3DBaseTexture8;
struct IDirect3DTexture8;
struct IDirect3DVolumeTexture8;
struct IDirect3DCubeTexture8;
struct IDirect3DVertexBuffer8;
struct IDirect3DIndexBuffer8;
struct IDirect3DSurface8;
struct IDirect3DVolume8;
struct IDirect3DSwapChain8;

// ── Interface IIDs ───────────────────────────────────────────────────────
// QueryInterface must recognise these or wrappers such as d3d8to9 and
// ReShade — both present in the game folder — will fail to attach.
extern const GUID IID_IDirect3D8_PESMod;
extern const GUID IID_IDirect3DDevice8_PESMod;

// ── IDirect3DResource8 ───────────────────────────────────────────────────
struct IDirect3DResource8 : public IUnknown
{
    virtual HRESULT __stdcall GetDevice(IDirect3DDevice8** ppDevice) = 0;
    virtual HRESULT __stdcall SetPrivateData(REFGUID refguid, const void* pData, DWORD SizeOfData, DWORD Flags) = 0;
    virtual HRESULT __stdcall GetPrivateData(REFGUID refguid, void* pData, DWORD* pSizeOfData) = 0;
    virtual HRESULT __stdcall FreePrivateData(REFGUID refguid) = 0;
    virtual DWORD   __stdcall SetPriority(DWORD PriorityNew) = 0;
    virtual DWORD   __stdcall GetPriority() = 0;
    virtual void    __stdcall PreLoad() = 0;
    virtual D3DRESOURCETYPE __stdcall GetType() = 0;
};

// ── IDirect3DBaseTexture8 ────────────────────────────────────────────────
struct IDirect3DBaseTexture8 : public IDirect3DResource8
{
    virtual DWORD __stdcall SetLOD(DWORD LODNew) = 0;
    virtual DWORD __stdcall GetLOD() = 0;
    virtual DWORD __stdcall GetLevelCount() = 0;
};

// ── IDirect3DTexture8 ────────────────────────────────────────────────────
struct IDirect3DTexture8 : public IDirect3DBaseTexture8
{
    virtual HRESULT __stdcall GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) = 0;
    virtual HRESULT __stdcall GetSurfaceLevel(UINT Level, IDirect3DSurface8** ppSurfaceLevel) = 0;
    virtual HRESULT __stdcall LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect, const RECT* pRect, DWORD Flags) = 0;
    virtual HRESULT __stdcall UnlockRect(UINT Level) = 0;
    virtual HRESULT __stdcall AddDirtyRect(const RECT* pDirtyRect) = 0;
};

// ── IDirect3DVolumeTexture8 ──────────────────────────────────────────────
struct IDirect3DVolumeTexture8 : public IDirect3DBaseTexture8
{
    virtual HRESULT __stdcall GetLevelDesc(UINT Level, D3DVOLUME_DESC* pDesc) = 0;
    virtual HRESULT __stdcall GetVolumeLevel(UINT Level, IDirect3DVolume8** ppVolumeLevel) = 0;
    virtual HRESULT __stdcall LockBox(UINT Level, D3DLOCKED_BOX* pLockedVolume, const D3DBOX* pBox, DWORD Flags) = 0;
    virtual HRESULT __stdcall UnlockBox(UINT Level) = 0;
    virtual HRESULT __stdcall AddDirtyBox(const D3DBOX* pDirtyBox) = 0;
};

// ── IDirect3DCubeTexture8 ────────────────────────────────────────────────
struct IDirect3DCubeTexture8 : public IDirect3DBaseTexture8
{
    virtual HRESULT __stdcall GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) = 0;
    virtual HRESULT __stdcall GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface8** ppCubeMapSurface) = 0;
    virtual HRESULT __stdcall LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT* pLockedRect, const RECT* pRect, DWORD Flags) = 0;
    virtual HRESULT __stdcall UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) = 0;
    virtual HRESULT __stdcall AddDirtyRect(D3DCUBEMAP_FACES FaceType, const RECT* pDirtyRect) = 0;
};

// ── IDirect3DVertexBuffer8 ───────────────────────────────────────────────
struct IDirect3DVertexBuffer8 : public IDirect3DResource8
{
    virtual HRESULT __stdcall Lock(UINT OffsetToLock, UINT SizeToLock, BYTE** ppbData, DWORD Flags) = 0;
    virtual HRESULT __stdcall Unlock() = 0;
    virtual HRESULT __stdcall GetDesc(D3DVERTEXBUFFER_DESC* pDesc) = 0;
};

// ── IDirect3DIndexBuffer8 ────────────────────────────────────────────────
struct IDirect3DIndexBuffer8 : public IDirect3DResource8
{
    virtual HRESULT __stdcall Lock(UINT OffsetToLock, UINT SizeToLock, BYTE** ppbData, DWORD Flags) = 0;
    virtual HRESULT __stdcall Unlock() = 0;
    virtual HRESULT __stdcall GetDesc(D3DINDEXBUFFER_DESC* pDesc) = 0;
};

// ── IDirect3DSurface8 ────────────────────────────────────────────────────
// Surface and Volume derive from IUnknown directly, not from Resource8.
struct IDirect3DSurface8 : public IUnknown
{
    virtual HRESULT __stdcall GetDevice(IDirect3DDevice8** ppDevice) = 0;
    virtual HRESULT __stdcall SetPrivateData(REFGUID refguid, const void* pData, DWORD SizeOfData, DWORD Flags) = 0;
    virtual HRESULT __stdcall GetPrivateData(REFGUID refguid, void* pData, DWORD* pSizeOfData) = 0;
    virtual HRESULT __stdcall FreePrivateData(REFGUID refguid) = 0;
    virtual HRESULT __stdcall GetContainer(REFIID riid, void** ppContainer) = 0;
    virtual HRESULT __stdcall GetDesc(D3DSURFACE_DESC* pDesc) = 0;
    virtual HRESULT __stdcall LockRect(D3DLOCKED_RECT* pLockedRect, const RECT* pRect, DWORD Flags) = 0;
    virtual HRESULT __stdcall UnlockRect() = 0;
};

// ── IDirect3DVolume8 ─────────────────────────────────────────────────────
struct IDirect3DVolume8 : public IUnknown
{
    virtual HRESULT __stdcall GetDevice(IDirect3DDevice8** ppDevice) = 0;
    virtual HRESULT __stdcall SetPrivateData(REFGUID refguid, const void* pData, DWORD SizeOfData, DWORD Flags) = 0;
    virtual HRESULT __stdcall GetPrivateData(REFGUID refguid, void* pData, DWORD* pSizeOfData) = 0;
    virtual HRESULT __stdcall FreePrivateData(REFGUID refguid) = 0;
    virtual HRESULT __stdcall GetContainer(REFIID riid, void** ppContainer) = 0;
    virtual HRESULT __stdcall GetDesc(D3DVOLUME_DESC* pDesc) = 0;
    virtual HRESULT __stdcall LockBox(D3DLOCKED_BOX* pLockedVolume, const D3DBOX* pBox, DWORD Flags) = 0;
    virtual HRESULT __stdcall UnlockBox() = 0;
};

// ── IDirect3DSwapChain8 ──────────────────────────────────────────────────
struct IDirect3DSwapChain8 : public IUnknown
{
    virtual HRESULT __stdcall Present(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) = 0;
    virtual HRESULT __stdcall GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8** ppBackBuffer) = 0;
};

// ── IDirect3DDevice8 ─────────────────────────────────────────────────────
// 97 methods, indices 0..96. Order is authoritative — see the header comment.
struct IDirect3DDevice8 : public IUnknown
{
    /* 03 */ virtual HRESULT __stdcall TestCooperativeLevel() = 0;
    /* 04 */ virtual UINT    __stdcall GetAvailableTextureMem() = 0;
    /* 05 */ virtual HRESULT __stdcall ResourceManagerDiscardBytes(DWORD Bytes) = 0;
    /* 06 */ virtual HRESULT __stdcall GetDirect3D(IDirect3D8** ppD3D8) = 0;
    /* 07 */ virtual HRESULT __stdcall GetDeviceCaps(D3DCAPS8* pCaps) = 0;
    /* 08 */ virtual HRESULT __stdcall GetDisplayMode(D3DDISPLAYMODE* pMode) = 0;
    /* 09 */ virtual HRESULT __stdcall GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters) = 0;
    /* 10 */ virtual HRESULT __stdcall SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8* pCursorBitmap) = 0;
    /* 11 */ virtual void    __stdcall SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace, DWORD Flags) = 0;
    /* 12 */ virtual BOOL    __stdcall ShowCursor(BOOL bShow) = 0;
    /* 13 */ virtual HRESULT __stdcall CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain8** pSwapChain) = 0;
    /* 14 */ virtual HRESULT __stdcall Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) = 0;
    /* 15 */ virtual HRESULT __stdcall Present(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) = 0;
    /* 16 */ virtual HRESULT __stdcall GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8** ppBackBuffer) = 0;
    /* 17 */ virtual HRESULT __stdcall GetRasterStatus(D3DRASTER_STATUS* pRasterStatus) = 0;
    /* 18 */ virtual void    __stdcall SetGammaRamp(DWORD Flags, const D3DGAMMARAMP* pRamp) = 0;
    /* 19 */ virtual void    __stdcall GetGammaRamp(D3DGAMMARAMP* pRamp) = 0;
    /* 20 */ virtual HRESULT __stdcall CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8** ppTexture) = 0;
    /* 21 */ virtual HRESULT __stdcall CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8** ppVolumeTexture) = 0;
    /* 22 */ virtual HRESULT __stdcall CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8** ppCubeTexture) = 0;
    /* 23 */ virtual HRESULT __stdcall CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8** ppVertexBuffer) = 0;
    /* 24 */ virtual HRESULT __stdcall CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8** ppIndexBuffer) = 0;
    /* 25 */ virtual HRESULT __stdcall CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8** ppSurface) = 0;
    /* 26 */ virtual HRESULT __stdcall CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8** ppSurface) = 0;
    /* 27 */ virtual HRESULT __stdcall CreateImageSurface(UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8** ppSurface) = 0;
    /* 28 */ virtual HRESULT __stdcall CopyRects(IDirect3DSurface8* pSourceSurface, const RECT* pSourceRectsArray, UINT cRects, IDirect3DSurface8* pDestinationSurface, const POINT* pDestPointsArray) = 0;
    /* 29 */ virtual HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture8* pSourceTexture, IDirect3DBaseTexture8* pDestinationTexture) = 0;
    /* 30 */ virtual HRESULT __stdcall GetFrontBuffer(IDirect3DSurface8* pDestSurface) = 0;
    /* 31 */ virtual HRESULT __stdcall SetRenderTarget(IDirect3DSurface8* pRenderTarget, IDirect3DSurface8* pNewZStencil) = 0;
    /* 32 */ virtual HRESULT __stdcall GetRenderTarget(IDirect3DSurface8** ppRenderTarget) = 0;
    /* 33 */ virtual HRESULT __stdcall GetDepthStencilSurface(IDirect3DSurface8** ppZStencilSurface) = 0;
    /* 34 */ virtual HRESULT __stdcall BeginScene() = 0;
    /* 35 */ virtual HRESULT __stdcall EndScene() = 0;
    /* 36 */ virtual HRESULT __stdcall Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) = 0;
    /* 37 */ virtual HRESULT __stdcall SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) = 0;
    /* 38 */ virtual HRESULT __stdcall GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) = 0;
    /* 39 */ virtual HRESULT __stdcall MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) = 0;
    /* 40 */ virtual HRESULT __stdcall SetViewport(const D3DVIEWPORT8* pViewport) = 0;
    /* 41 */ virtual HRESULT __stdcall GetViewport(D3DVIEWPORT8* pViewport) = 0;
    /* 42 */ virtual HRESULT __stdcall SetMaterial(const D3DMATERIAL8* pMaterial) = 0;
    /* 43 */ virtual HRESULT __stdcall GetMaterial(D3DMATERIAL8* pMaterial) = 0;
    /* 44 */ virtual HRESULT __stdcall SetLight(DWORD Index, const D3DLIGHT8* pLight) = 0;
    /* 45 */ virtual HRESULT __stdcall GetLight(DWORD Index, D3DLIGHT8* pLight) = 0;
    /* 46 */ virtual HRESULT __stdcall LightEnable(DWORD Index, BOOL Enable) = 0;
    /* 47 */ virtual HRESULT __stdcall GetLightEnable(DWORD Index, BOOL* pEnable) = 0;
    /* 48 */ virtual HRESULT __stdcall SetClipPlane(DWORD Index, const float* pPlane) = 0;
    /* 49 */ virtual HRESULT __stdcall GetClipPlane(DWORD Index, float* pPlane) = 0;
    /* 50 */ virtual HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) = 0;
    /* 51 */ virtual HRESULT __stdcall GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) = 0;
    /* 52 */ virtual HRESULT __stdcall BeginStateBlock() = 0;
    /* 53 */ virtual HRESULT __stdcall EndStateBlock(DWORD* pToken) = 0;
    /* 54 */ virtual HRESULT __stdcall ApplyStateBlock(DWORD Token) = 0;
    /* 55 */ virtual HRESULT __stdcall CaptureStateBlock(DWORD Token) = 0;
    /* 56 */ virtual HRESULT __stdcall DeleteStateBlock(DWORD Token) = 0;
    /* 57 */ virtual HRESULT __stdcall CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD* pToken) = 0;
    /* 58 */ virtual HRESULT __stdcall SetClipStatus(const D3DCLIPSTATUS8* pClipStatus) = 0;
    /* 59 */ virtual HRESULT __stdcall GetClipStatus(D3DCLIPSTATUS8* pClipStatus) = 0;
    /* 60 */ virtual HRESULT __stdcall GetTexture(DWORD Stage, IDirect3DBaseTexture8** ppTexture) = 0;
    /* 61 */ virtual HRESULT __stdcall SetTexture(DWORD Stage, IDirect3DBaseTexture8* pTexture) = 0;
    /* 62 */ virtual HRESULT __stdcall GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) = 0;
    /* 63 */ virtual HRESULT __stdcall SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) = 0;
    /* 64 */ virtual HRESULT __stdcall ValidateDevice(DWORD* pNumPasses) = 0;
    /* 65 */ virtual HRESULT __stdcall GetInfo(DWORD DevInfoID, void* pDevInfoStruct, DWORD DevInfoStructSize) = 0;
    /* 66 */ virtual HRESULT __stdcall SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) = 0;
    /* 67 */ virtual HRESULT __stdcall GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) = 0;
    /* 68 */ virtual HRESULT __stdcall SetCurrentTexturePalette(UINT PaletteNumber) = 0;
    /* 69 */ virtual HRESULT __stdcall GetCurrentTexturePalette(UINT* PaletteNumber) = 0;
    /* 70 */ virtual HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) = 0;
    /* 71 */ virtual HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount) = 0;
    /* 72 */ virtual HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) = 0;
    /* 73 */ virtual HRESULT __stdcall DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertexIndices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) = 0;
    /* 74 */ virtual HRESULT __stdcall ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer8* pDestBuffer, DWORD Flags) = 0;
    /* 75 */ virtual HRESULT __stdcall CreateVertexShader(const DWORD* pDeclaration, const DWORD* pFunction, DWORD* pHandle, DWORD Usage) = 0;
    /* 76 */ virtual HRESULT __stdcall SetVertexShader(DWORD Handle) = 0;
    /* 77 */ virtual HRESULT __stdcall GetVertexShader(DWORD* pHandle) = 0;
    /* 78 */ virtual HRESULT __stdcall DeleteVertexShader(DWORD Handle) = 0;
    /* 79 */ virtual HRESULT __stdcall SetVertexShaderConstant(DWORD Register, const void* pConstantData, DWORD ConstantCount) = 0;
    /* 80 */ virtual HRESULT __stdcall GetVertexShaderConstant(DWORD Register, void* pConstantData, DWORD ConstantCount) = 0;
    /* 81 */ virtual HRESULT __stdcall GetVertexShaderDeclaration(DWORD Handle, void* pData, DWORD* pSizeOfData) = 0;
    /* 82 */ virtual HRESULT __stdcall GetVertexShaderFunction(DWORD Handle, void* pData, DWORD* pSizeOfData) = 0;
    /* 83 */ virtual HRESULT __stdcall SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8* pStreamData, UINT Stride) = 0;
    /* 84 */ virtual HRESULT __stdcall GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8** ppStreamData, UINT* pStride) = 0;
    /* 85 */ virtual HRESULT __stdcall SetIndices(IDirect3DIndexBuffer8* pIndexData, UINT BaseVertexIndex) = 0;
    /* 86 */ virtual HRESULT __stdcall GetIndices(IDirect3DIndexBuffer8** ppIndexData, UINT* pBaseVertexIndex) = 0;
    /* 87 */ virtual HRESULT __stdcall CreatePixelShader(const DWORD* pFunction, DWORD* pHandle) = 0;
    /* 88 */ virtual HRESULT __stdcall SetPixelShader(DWORD Handle) = 0;
    /* 89 */ virtual HRESULT __stdcall GetPixelShader(DWORD* pHandle) = 0;
    /* 90 */ virtual HRESULT __stdcall DeletePixelShader(DWORD Handle) = 0;
    /* 91 */ virtual HRESULT __stdcall SetPixelShaderConstant(DWORD Register, const void* pConstantData, DWORD ConstantCount) = 0;
    /* 92 */ virtual HRESULT __stdcall GetPixelShaderConstant(DWORD Register, void* pConstantData, DWORD ConstantCount) = 0;
    /* 93 */ virtual HRESULT __stdcall GetPixelShaderFunction(DWORD Handle, void* pData, DWORD* pSizeOfData) = 0;
    /* 94 */ virtual HRESULT __stdcall DrawRectPatch(UINT Handle, const float* pNumSegs, const void* pRectPatchInfo) = 0;
    /* 95 */ virtual HRESULT __stdcall DrawTriPatch(UINT Handle, const float* pNumSegs, const void* pTriPatchInfo) = 0;
    /* 96 */ virtual HRESULT __stdcall DeletePatch(UINT Handle) = 0;
};

// ── IDirect3D8 ───────────────────────────────────────────────────────────
// 16 methods, indices 0..15.
struct IDirect3D8 : public IUnknown
{
    /* 03 */ virtual HRESULT __stdcall RegisterSoftwareDevice(void* pInitializeFunction) = 0;
    /* 04 */ virtual UINT    __stdcall GetAdapterCount() = 0;
    /* 05 */ virtual HRESULT __stdcall GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER8* pIdentifier) = 0;
    /* 06 */ virtual UINT    __stdcall GetAdapterModeCount(UINT Adapter) = 0;
    /* 07 */ virtual HRESULT __stdcall EnumAdapterModes(UINT Adapter, UINT Mode, D3DDISPLAYMODE* pMode) = 0;
    /* 08 */ virtual HRESULT __stdcall GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) = 0;
    /* 09 */ virtual HRESULT __stdcall CheckDeviceType(UINT Adapter, D3DDEVTYPE CheckType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL Windowed) = 0;
    /* 10 */ virtual HRESULT __stdcall CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) = 0;
    /* 11 */ virtual HRESULT __stdcall CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType) = 0;
    /* 12 */ virtual HRESULT __stdcall CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) = 0;
    /* 13 */ virtual HRESULT __stdcall GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS8* pCaps) = 0;
    /* 14 */ virtual HMONITOR __stdcall GetAdapterMonitor(UINT Adapter) = 0;
    /* 15 */ virtual HRESULT __stdcall CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice8** ppReturnedDeviceInterface) = 0;
};

typedef IDirect3D8* (__stdcall* PFN_Direct3DCreate8)(UINT SDKVersion);
