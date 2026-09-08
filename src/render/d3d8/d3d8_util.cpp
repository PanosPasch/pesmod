// d3d8_util.cpp
#include "d3d8_util.h"
#include <cstdio>
#include <cstring>

namespace
{
    // Unknown enum values are rendered as hex into a small rotating pool so
    // that two of them can appear in the same printf without clobbering each
    // other. Log formatting is single-threaded (the render thread), so a
    // plain rotating index is sufficient.
    const char* UnknownName(const char* prefix, uint32_t value)
    {
        static char pool[4][40];
        static int  next = 0;
        char* slot = pool[next];
        next = (next + 1) & 3;
        _snprintf_s(slot, sizeof(pool[0]), _TRUNCATE, "%s(0x%X)", prefix, value);
        return slot;
    }
}

namespace D3D8Util
{

// ── Vertex layout ────────────────────────────────────────────────────────
bool FvfDecode(uint32_t fvf, FvfLayout& out)
{
    memset(&out, 0, sizeof(out));
    out.fvf = fvf;
    out.posOffset = out.blendWeightOffset = out.normalOffset = -1;
    out.pointSizeOffset = out.diffuseOffset = out.specularOffset = -1;
    for (int i = 0; i < 8; ++i) out.texCoordOffset[i] = -1;

    uint32_t offset = 0;

    // Position. The D3D8 component order in memory is fixed:
    //   position → blend weights → normal → psize → diffuse → specular → uv
    const uint32_t posBits = fvf & D3DFVF_POSITION_MASK;
    if (posBits == D3DFVF_XYZ)
    {
        out.posOffset = 0;
        offset = 12;
    }
    else if (posBits == D3DFVF_XYZRHW)
    {
        out.posOffset = 0;
        offset = 16;
        out.positionIsTransformed = true;
    }
    else if (posBits >= D3DFVF_XYZB1 && posBits <= D3DFVF_XYZB5)
    {
        // XYZB1..XYZB5 are 0x006, 0x008, 0x00a, 0x00c, 0x00e — two apart.
        out.blendWeightCount  = (int)((posBits - D3DFVF_XYZB1) / 2) + 1;
        out.posOffset         = 0;
        out.blendWeightOffset = 12;
        offset = 12 + 4u * (uint32_t)out.blendWeightCount;
    }
    else
    {
        // No usable position bits — not a layout we can interpret.
        return false;
    }

    if (fvf & D3DFVF_NORMAL)
    {
        // XYZRHW is already transformed and lit; a normal alongside it is
        // illegal in D3D8 and means we have misread the code.
        if (out.positionIsTransformed) return false;
        out.normalOffset = (int)offset; offset += 12;
    }
    if (fvf & D3DFVF_PSIZE)    { out.pointSizeOffset = (int)offset; offset += 4; }
    if (fvf & D3DFVF_DIFFUSE)  { out.diffuseOffset   = (int)offset; offset += 4; }
    if (fvf & D3DFVF_SPECULAR) { out.specularOffset  = (int)offset; offset += 4; }

    out.texCoordCount = (int)((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
    if (out.texCoordCount > 8) return false;

    for (int i = 0; i < out.texCoordCount; ++i)
    {
        // Two bits per set, starting at bit 16, encode the coordinate width.
        // The encoding is deliberately not in numeric order: 0→2f is the
        // common case so that a plain "TEX1" FVF needs no extra bits.
        const uint32_t bits = (fvf >> (16 + i * 2)) & 0x3;
        static const int kFloatsForBits[4] = { 2, 3, 4, 1 };
        const int floats = kFloatsForBits[bits];
        out.texCoordFloats[i] = floats;
        out.texCoordOffset[i] = (int)offset;
        offset += 4u * (uint32_t)floats;
    }

    out.stride = offset;
    return true;
}

uint32_t FvfStride(uint32_t fvf)
{
    FvfLayout layout;
    return FvfDecode(fvf, layout) ? layout.stride : 0u;
}

bool FvfIsScreenSpace(uint32_t fvf)
{
    return (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
}

const char* FvfDescribe(uint32_t fvf, char* buf, size_t bufSize)
{
    FvfLayout layout;
    if (!FvfDecode(fvf, layout))
    {
        _snprintf_s(buf, bufSize, _TRUNCATE, "UNDECODABLE(0x%X)", fvf);
        return buf;
    }

    char parts[192];
    parts[0] = '\0';
    size_t used = 0;

    // Small local append that silently stops at the end of the buffer.
    #define APPEND(...)                                                        \
        do {                                                                   \
            if (used < sizeof(parts) - 1) {                                    \
                int n = _snprintf_s(parts + used, sizeof(parts) - used,        \
                                    _TRUNCATE, __VA_ARGS__);                   \
                if (n > 0) used += (size_t)n;                                  \
            }                                                                  \
        } while (0)

    if (layout.positionIsTransformed)      APPEND("XYZRHW");
    else if (layout.blendWeightCount > 0)  APPEND("XYZB%d", layout.blendWeightCount);
    else                                   APPEND("XYZ");

    if (layout.normalOffset    >= 0) APPEND("|NORMAL");
    if (layout.pointSizeOffset >= 0) APPEND("|PSIZE");
    if (layout.diffuseOffset   >= 0) APPEND("|DIFFUSE");
    if (layout.specularOffset  >= 0) APPEND("|SPECULAR");

    for (int i = 0; i < layout.texCoordCount; ++i)
        APPEND("|TEX%d(%df)", i, layout.texCoordFloats[i]);

    #undef APPEND

    _snprintf_s(buf, bufSize, _TRUNCATE, "%s %uB", parts, layout.stride);
    return buf;
}

// ── Primitive maths ──────────────────────────────────────────────────────
uint32_t PrimitiveVertexCount(D3DPRIMITIVETYPE type, uint32_t n)
{
    switch (type)
    {
    case D3DPT_POINTLIST:     return n;
    case D3DPT_LINELIST:      return n * 2;
    case D3DPT_LINESTRIP:     return n + 1;
    case D3DPT_TRIANGLELIST:  return n * 3;
    case D3DPT_TRIANGLESTRIP: return n + 2;
    case D3DPT_TRIANGLEFAN:   return n + 2;
    default:                  return 0;
    }
}

uint32_t PrimitiveTriangleCount(D3DPRIMITIVETYPE type, uint32_t n)
{
    switch (type)
    {
    case D3DPT_TRIANGLELIST:
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:  return n;
    default:                 return 0;
    }
}

// ── Surface sizing ───────────────────────────────────────────────────────
uint32_t FormatBitsPerPixel(D3DFORMAT fmt)
{
    switch (fmt)
    {
    case D3DFMT_R8G8B8:                              return 24;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
    case D3DFMT_X8L8V8U8: case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16:   case D3DFMT_W11V11U10:
    case D3DFMT_D32:      case D3DFMT_D24S8:
    case D3DFMT_D24X8:    case D3DFMT_D24X4S4:       return 32;
    case D3DFMT_R5G6B5:   case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_A8R3G3B2: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8P8:     case D3DFMT_A8L8:
    case D3DFMT_V8U8:     case D3DFMT_L6V5U5:
    case D3DFMT_D16:      case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D15S1:    case D3DFMT_INDEX16:
    case D3DFMT_UYVY:     case D3DFMT_YUY2:          return 16;
    case D3DFMT_R3G3B2:   case D3DFMT_A8:
    case D3DFMT_P8:       case D3DFMT_L8:
    case D3DFMT_A4L4:                                return 8;
    case D3DFMT_INDEX32:                             return 32;
    default:                                         return 0;   // compressed / unknown
    }
}

uint32_t SurfaceBytes(D3DFORMAT fmt, uint32_t width, uint32_t height)
{
    switch (fmt)
    {
    case D3DFMT_DXT1:
    {
        const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
        return (bw ? bw : 1) * (bh ? bh : 1) * 8;
    }
    case D3DFMT_DXT2: case D3DFMT_DXT3:
    case D3DFMT_DXT4: case D3DFMT_DXT5:
    {
        const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
        return (bw ? bw : 1) * (bh ? bh : 1) * 16;
    }
    default:
        break;
    }

    const uint32_t bpp = FormatBitsPerPixel(fmt);
    if (bpp == 0) return 0;
    // Rows are byte-aligned; D3D8 pitch may be larger, but for a tightly
    // packed dump this is the payload size.
    return ((width * bpp + 7) / 8) * height;
}

// ── Enum names ───────────────────────────────────────────────────────────
const char* FormatName(D3DFORMAT fmt)
{
    switch (fmt)
    {
    case D3DFMT_UNKNOWN:       return "UNKNOWN";
    case D3DFMT_R8G8B8:        return "R8G8B8";
    case D3DFMT_A8R8G8B8:      return "A8R8G8B8";
    case D3DFMT_X8R8G8B8:      return "X8R8G8B8";
    case D3DFMT_R5G6B5:        return "R5G6B5";
    case D3DFMT_X1R5G5B5:      return "X1R5G5B5";
    case D3DFMT_A1R5G5B5:      return "A1R5G5B5";
    case D3DFMT_A4R4G4B4:      return "A4R4G4B4";
    case D3DFMT_R3G3B2:        return "R3G3B2";
    case D3DFMT_A8:            return "A8";
    case D3DFMT_A8R3G3B2:      return "A8R3G3B2";
    case D3DFMT_X4R4G4B4:      return "X4R4G4B4";
    case D3DFMT_A8P8:          return "A8P8";
    case D3DFMT_P8:            return "P8";
    case D3DFMT_L8:            return "L8";
    case D3DFMT_A8L8:          return "A8L8";
    case D3DFMT_A4L4:          return "A4L4";
    case D3DFMT_V8U8:          return "V8U8";
    case D3DFMT_L6V5U5:        return "L6V5U5";
    case D3DFMT_X8L8V8U8:      return "X8L8V8U8";
    case D3DFMT_Q8W8V8U8:      return "Q8W8V8U8";
    case D3DFMT_V16U16:        return "V16U16";
    case D3DFMT_W11V11U10:     return "W11V11U10";
    case D3DFMT_UYVY:          return "UYVY";
    case D3DFMT_YUY2:          return "YUY2";
    case D3DFMT_DXT1:          return "DXT1";
    case D3DFMT_DXT2:          return "DXT2";
    case D3DFMT_DXT3:          return "DXT3";
    case D3DFMT_DXT4:          return "DXT4";
    case D3DFMT_DXT5:          return "DXT5";
    case D3DFMT_D16_LOCKABLE:  return "D16_LOCKABLE";
    case D3DFMT_D32:           return "D32";
    case D3DFMT_D15S1:         return "D15S1";
    case D3DFMT_D24S8:         return "D24S8";
    case D3DFMT_D24X8:         return "D24X8";
    case D3DFMT_D24X4S4:       return "D24X4S4";
    case D3DFMT_D16:           return "D16";
    case D3DFMT_VERTEXDATA:    return "VERTEXDATA";
    case D3DFMT_INDEX16:       return "INDEX16";
    case D3DFMT_INDEX32:       return "INDEX32";
    default:                   return UnknownName("FMT", (uint32_t)fmt);
    }
}

const char* PoolName(D3DPOOL pool)
{
    switch (pool)
    {
    case D3DPOOL_DEFAULT:   return "DEFAULT";
    case D3DPOOL_MANAGED:   return "MANAGED";
    case D3DPOOL_SYSTEMMEM: return "SYSTEMMEM";
    case D3DPOOL_SCRATCH:   return "SCRATCH";
    default:                return UnknownName("POOL", (uint32_t)pool);
    }
}

const char* PrimitiveTypeName(D3DPRIMITIVETYPE type)
{
    switch (type)
    {
    case D3DPT_POINTLIST:     return "POINTLIST";
    case D3DPT_LINELIST:      return "LINELIST";
    case D3DPT_LINESTRIP:     return "LINESTRIP";
    case D3DPT_TRIANGLELIST:  return "TRIANGLELIST";
    case D3DPT_TRIANGLESTRIP: return "TRIANGLESTRIP";
    case D3DPT_TRIANGLEFAN:   return "TRIANGLEFAN";
    default:                  return UnknownName("PRIM", (uint32_t)type);
    }
}

const char* RenderStateName(uint32_t state)
{
    switch (state)
    {
    case D3DRS_ZENABLE:                  return "ZENABLE";
    case D3DRS_FILLMODE:                 return "FILLMODE";
    case D3DRS_SHADEMODE:                return "SHADEMODE";
    case D3DRS_LINEPATTERN:              return "LINEPATTERN";
    case D3DRS_ZWRITEENABLE:             return "ZWRITEENABLE";
    case D3DRS_ALPHATESTENABLE:          return "ALPHATESTENABLE";
    case D3DRS_LASTPIXEL:                return "LASTPIXEL";
    case D3DRS_SRCBLEND:                 return "SRCBLEND";
    case D3DRS_DESTBLEND:                return "DESTBLEND";
    case D3DRS_CULLMODE:                 return "CULLMODE";
    case D3DRS_ZFUNC:                    return "ZFUNC";
    case D3DRS_ALPHAREF:                 return "ALPHAREF";
    case D3DRS_ALPHAFUNC:                return "ALPHAFUNC";
    case D3DRS_DITHERENABLE:             return "DITHERENABLE";
    case D3DRS_ALPHABLENDENABLE:         return "ALPHABLENDENABLE";
    case D3DRS_FOGENABLE:                return "FOGENABLE";
    case D3DRS_SPECULARENABLE:           return "SPECULARENABLE";
    case D3DRS_FOGCOLOR:                 return "FOGCOLOR";
    case D3DRS_FOGTABLEMODE:             return "FOGTABLEMODE";
    case D3DRS_FOGSTART:                 return "FOGSTART";
    case D3DRS_FOGEND:                   return "FOGEND";
    case D3DRS_FOGDENSITY:               return "FOGDENSITY";
    case D3DRS_ZBIAS:                    return "ZBIAS";
    case D3DRS_RANGEFOGENABLE:           return "RANGEFOGENABLE";
    case D3DRS_STENCILENABLE:            return "STENCILENABLE";
    case D3DRS_STENCILFAIL:              return "STENCILFAIL";
    case D3DRS_STENCILZFAIL:             return "STENCILZFAIL";
    case D3DRS_STENCILPASS:              return "STENCILPASS";
    case D3DRS_STENCILFUNC:              return "STENCILFUNC";
    case D3DRS_STENCILREF:               return "STENCILREF";
    case D3DRS_STENCILMASK:              return "STENCILMASK";
    case D3DRS_STENCILWRITEMASK:         return "STENCILWRITEMASK";
    case D3DRS_TEXTUREFACTOR:            return "TEXTUREFACTOR";
    case D3DRS_CLIPPING:                 return "CLIPPING";
    case D3DRS_LIGHTING:                 return "LIGHTING";
    case D3DRS_AMBIENT:                  return "AMBIENT";
    case D3DRS_FOGVERTEXMODE:            return "FOGVERTEXMODE";
    case D3DRS_COLORVERTEX:              return "COLORVERTEX";
    case D3DRS_LOCALVIEWER:              return "LOCALVIEWER";
    case D3DRS_NORMALIZENORMALS:         return "NORMALIZENORMALS";
    case D3DRS_DIFFUSEMATERIALSOURCE:    return "DIFFUSEMATERIALSOURCE";
    case D3DRS_SPECULARMATERIALSOURCE:   return "SPECULARMATERIALSOURCE";
    case D3DRS_AMBIENTMATERIALSOURCE:    return "AMBIENTMATERIALSOURCE";
    case D3DRS_EMISSIVEMATERIALSOURCE:   return "EMISSIVEMATERIALSOURCE";
    case D3DRS_VERTEXBLEND:              return "VERTEXBLEND";
    case D3DRS_CLIPPLANEENABLE:          return "CLIPPLANEENABLE";
    case D3DRS_SOFTWAREVERTEXPROCESSING: return "SOFTWAREVERTEXPROCESSING";
    case D3DRS_POINTSIZE:                return "POINTSIZE";
    case D3DRS_POINTSIZE_MIN:            return "POINTSIZE_MIN";
    case D3DRS_POINTSPRITEENABLE:        return "POINTSPRITEENABLE";
    case D3DRS_POINTSCALEENABLE:         return "POINTSCALEENABLE";
    case D3DRS_MULTISAMPLEANTIALIAS:     return "MULTISAMPLEANTIALIAS";
    case D3DRS_MULTISAMPLEMASK:          return "MULTISAMPLEMASK";
    case D3DRS_PATCHEDGESTYLE:           return "PATCHEDGESTYLE";
    case D3DRS_COLORWRITEENABLE:         return "COLORWRITEENABLE";
    case D3DRS_TWEENFACTOR:              return "TWEENFACTOR";
    case D3DRS_BLENDOP:                  return "BLENDOP";
    case D3DRS_POSITIONORDER:            return "POSITIONORDER";
    case D3DRS_NORMALORDER:              return "NORMALORDER";
    default:
        if (state >= D3DRS_WRAP0 && state < D3DRS_WRAP0 + 8)
            return UnknownName("WRAP", state - D3DRS_WRAP0);
        return UnknownName("RS", state);
    }
}

const char* TextureStageStateName(uint32_t state)
{
    switch (state)
    {
    case D3DTSS_COLOROP:       return "COLOROP";
    case D3DTSS_COLORARG1:     return "COLORARG1";
    case D3DTSS_COLORARG2:     return "COLORARG2";
    case D3DTSS_ALPHAOP:       return "ALPHAOP";
    case D3DTSS_ALPHAARG1:     return "ALPHAARG1";
    case D3DTSS_ALPHAARG2:     return "ALPHAARG2";
    case D3DTSS_BUMPENVMAT00:  return "BUMPENVMAT00";
    case D3DTSS_TEXCOORDINDEX: return "TEXCOORDINDEX";
    case D3DTSS_ADDRESSU:      return "ADDRESSU";
    case D3DTSS_ADDRESSV:      return "ADDRESSV";
    case D3DTSS_ADDRESSW:      return "ADDRESSW";
    case D3DTSS_BORDERCOLOR:   return "BORDERCOLOR";
    case D3DTSS_MAGFILTER:     return "MAGFILTER";
    case D3DTSS_MINFILTER:     return "MINFILTER";
    case D3DTSS_MIPFILTER:     return "MIPFILTER";
    case D3DTSS_MIPMAPLODBIAS: return "MIPMAPLODBIAS";
    case D3DTSS_MAXMIPLEVEL:   return "MAXMIPLEVEL";
    case D3DTSS_MAXANISOTROPY: return "MAXANISOTROPY";
    case D3DTSS_COLORARG0:     return "COLORARG0";
    case D3DTSS_ALPHAARG0:     return "ALPHAARG0";
    case D3DTSS_RESULTARG:     return "RESULTARG";
    default:                   return UnknownName("TSS", state);
    }
}

const char* TransformStateName(uint32_t state)
{
    switch (state)
    {
    case D3DTS_VIEW:       return "VIEW";
    case D3DTS_PROJECTION: return "PROJECTION";
    default:
        if (state >= D3DTS_TEXTURE0 && state <= D3DTS_TEXTURE7)
            return UnknownName("TEXTURE", state - D3DTS_TEXTURE0);
        if (state >= D3DTS_WORLD)
            return UnknownName("WORLD", state - D3DTS_WORLD);
        return UnknownName("TS", state);
    }
}

const char* DeviceTypeName(D3DDEVTYPE type)
{
    switch (type)
    {
    case D3DDEVTYPE_HAL: return "HAL";
    case D3DDEVTYPE_REF: return "REF";
    case D3DDEVTYPE_SW:  return "SW";
    default:             return UnknownName("DEVTYPE", (uint32_t)type);
    }
}

const char* SwapEffectName(D3DSWAPEFFECT effect)
{
    switch (effect)
    {
    case D3DSWAPEFFECT_DISCARD:    return "DISCARD";
    case D3DSWAPEFFECT_FLIP:       return "FLIP";
    case D3DSWAPEFFECT_COPY:       return "COPY";
    case D3DSWAPEFFECT_COPY_VSYNC: return "COPY_VSYNC";
    default:                       return UnknownName("SWAPEFFECT", (uint32_t)effect);
    }
}

} // namespace D3D8Util
