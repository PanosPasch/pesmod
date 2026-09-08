// d3d8_util.h
//
// Helpers for making sense of the raw D3D8 call stream: decoding FVF codes
// into a concrete vertex layout, sizing surfaces, and turning enum values
// into readable names for the capture log and the generated report.
//
// pes6.exe uses both of D3D8's vertex-format mechanisms: FVF codes for its 2D
// menu and HUD draws, and vertex declarations bound to runtime-assembled
// vs.1.1 shaders for the 3D scene. Both paths have to be decodable here for
// geometry extraction to work.
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

    // ── Vertex declarations ──────────────────────────────────────────────
    // D3D8 overloads SetVertexShader: it takes either an FVF code or a handle
    // from CreateVertexShader. A declaration passed with pFunction == NULL
    // creates a pure *vertex declaration* driving the fixed-function pipeline;
    // passed with a function it describes the inputs of a real vertex shader.
    //
    // pes6.exe does the latter for its 3D scene. The binary contains no shader
    // bytecode because it links the D3DX8 shader assembler and assembles
    // vs.1.1 sources at runtime — which is why scanning for compiled shader
    // version tokens finds nothing.
    //
    // The declaration is a DWORD token stream terminated by 0xFFFFFFFF.

    enum VertexDeclType   // D3DVSDT_*
    {
        kVsdtFloat1 = 0, kVsdtFloat2 = 1, kVsdtFloat3 = 2, kVsdtFloat4 = 3,
        kVsdtD3DColor = 4, kVsdtUByte4 = 5, kVsdtShort2 = 6, kVsdtShort4 = 7
    };

    struct VertexDeclElement
    {
        uint32_t stream;
        uint32_t reg;        // vertex register (D3DVSDE_POSITION == 0, etc.)
        uint32_t type;       // VertexDeclType
        uint32_t offset;     // byte offset within the stream's vertex
        uint32_t size;       // byte size of this element
    };

    struct VertexDeclLayout
    {
        VertexDeclElement elements[32];
        uint32_t          elementCount;
        uint32_t          streamStride[D3D8_MAX_STREAMS];
        uint32_t          tokenCount;
        bool              valid;
    };

    // Decodes a declaration token stream. `maxTokens` bounds the scan so a
    // malformed or non-terminated stream cannot run away.
    bool VertexDeclDecode(const uint32_t* decl, uint32_t maxTokens,
                          VertexDeclLayout& out);

    // A one-line summary of the layout.
    //
    // `semanticNames` selects how registers are labelled. A declaration used
    // with the *fixed-function* pipeline binds registers by fixed semantics
    // (D3DVSDE_POSITION == 0, D3DVSDE_NORMAL == 3, ...), so those names are
    // meaningful. A declaration feeding a real vertex shader binds plain
    // input registers v0..v15 that the shader may use for anything, so
    // labelling v1 "BLENDWEIGHT" there is actively misleading — pass false.
    const char* VertexDeclDescribe(const VertexDeclLayout& layout,
                                   bool semanticNames,
                                   char* buf, size_t bufSize);

    const char* VertexDeclTypeName(uint32_t type);
    const char* VertexRegisterName(uint32_t reg);

    // True when a SetVertexShader argument is an FVF code rather than a
    // handle. FVF codes always have bit 0 (D3DFVF_RESERVED0) clear.
    inline bool VertexShaderArgIsFvf(uint32_t arg)
    {
        return (arg & D3DFVF_RESERVED0) == 0;
    }

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
