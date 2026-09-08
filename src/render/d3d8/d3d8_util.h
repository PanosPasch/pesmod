// d3d8_util.h
//
// Helpers for making sense of the raw D3D8 call stream: decoding FVF codes
// into a concrete vertex layout, sizing surfaces, and turning enum values
// into readable names for the capture log and the generated report.
//
// pes6.exe has no vertex shaders at all, so the FVF *is* the complete vertex
// declaration for every draw in the game. Getting FvfDecode right is what
// makes geometry extraction possible.
#pragma once

#include "d3d8_min.h"
#include <cstdint>

namespace D3D8Util
{
    // ── Vertex layout ────────────────────────────────────────────────────
    // Byte offsets are -1 when the component is absent from the FVF.
    struct FvfLayout
    {
        uint32_t fvf;
        uint32_t stride;         // bytes per vertex

        int      posOffset;      // XYZ / XYZRHW
        bool     positionIsTransformed;   // XYZRHW: already in screen space
        int      blendWeightCount;
        int      blendWeightOffset;
        int      normalOffset;
        int      pointSizeOffset;
        int      diffuseOffset;
        int      specularOffset;

        int      texCoordCount;
        int      texCoordOffset[8];
        int      texCoordFloats[8];   // 1..4 floats per set
    };

    // Decodes an FVF code into `out`. Returns false for an FVF whose position
    // bits are unrecognised, in which case the stride cannot be trusted.
    bool FvfDecode(uint32_t fvf, FvfLayout& out);

    // Convenience: stride only. Returns 0 when the FVF cannot be decoded.
    uint32_t FvfStride(uint32_t fvf);

    // A one-line human summary, e.g. "XYZ|NORMAL|DIFFUSE|TEX1(2f) 32B".
    // Writes into `buf` and returns it.
    const char* FvfDescribe(uint32_t fvf, char* buf, size_t bufSize);

    // True when the FVF says the vertices are pre-transformed screen-space
    // (D3DFVF_XYZRHW). This is the discriminator that separates the game's
    // 2D overlay draws from real 3D scene geometry, and therefore decides
    // what the RT renderer takes ownership of.
    bool FvfIsScreenSpace(uint32_t fvf);

    // ── Primitive maths ──────────────────────────────────────────────────
    // Number of vertices consumed / indices read for a given primitive count.
    uint32_t PrimitiveVertexCount(D3DPRIMITIVETYPE type, uint32_t primitiveCount);

    // Number of triangles a draw contributes (0 for point/line primitives).
    uint32_t PrimitiveTriangleCount(D3DPRIMITIVETYPE type, uint32_t primitiveCount);

    // ── Surface sizing ───────────────────────────────────────────────────
    // Bits per pixel for uncompressed formats; 0 for block-compressed and
    // unknown formats (use SurfaceBytes, which handles both).
    uint32_t FormatBitsPerPixel(D3DFORMAT fmt);

    // Total bytes for one mip level. Handles DXT block compression.
    uint32_t SurfaceBytes(D3DFORMAT fmt, uint32_t width, uint32_t height);

    // ── Enum names (never null; unknown values render as hex) ────────────
    const char* FormatName(D3DFORMAT fmt);
    const char* PoolName(D3DPOOL pool);
    const char* PrimitiveTypeName(D3DPRIMITIVETYPE type);
    const char* RenderStateName(uint32_t state);
    const char* TextureStageStateName(uint32_t state);
    const char* TransformStateName(uint32_t state);
    const char* DeviceTypeName(D3DDEVTYPE type);
    const char* SwapEffectName(D3DSWAPEFFECT effect);
}
