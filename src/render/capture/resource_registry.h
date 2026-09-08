// resource_registry.h
//
// Tracks every vertex buffer, index buffer and texture the game creates, so
// that a draw call's bindings can be resolved back to something describable:
// a stable id, its dimensions/format, and where its bytes can be read from.
//
// Design note — why there are no resource proxy objects here:
//
// The obvious way to see buffer contents is to wrap every IDirect3DVertexBuffer8
// in a proxy and shadow it on Unlock. That means three more COM wrapper classes
// and unwrapping at every binding site (SetStreamSource, SetIndices, SetTexture,
// ProcessVertices...), each an opportunity for a pointer to leak through
// unwrapped and crash the game.
//
// Instead the registry records metadata at creation time and the capture layer
// simply Locks a buffer read-only when it needs the bytes. The one thing that
// blocks that is D3DUSAGE_WRITEONLY, so while capture is enabled the proxy
// device strips that flag at creation. That trades a little buffer-upload
// performance — only in capture mode, which is off by default — for a whole
// category of wrapper bugs that never get written.
#pragma once

#include "../d3d8/d3d8_min.h"
#include <cstdint>

namespace Capture
{
    enum ResourceKind : uint8_t
    {
        kResourceVertexBuffer = 0,
        kResourceIndexBuffer  = 1,
        kResourceTexture      = 2
    };

    struct ResourceInfo
    {
        uint32_t        id;          // stable, monotonically assigned
        ResourceKind    kind;
        void*           object;      // the real D3D8 interface pointer

        uint32_t        byteLength;  // buffers only
        uint32_t        usage;       // usage as the game requested it
        bool            writeOnlyStripped;

        D3DPOOL         pool;
        D3DFORMAT       format;      // index format, or texture format
        uint32_t        fvf;         // vertex buffers only

        uint32_t        width;       // textures only
        uint32_t        height;
        uint32_t        levels;

        uint32_t        firstSeenFrame;
        uint32_t        drawRefCount; // how many draws have referenced it
    };

    namespace Registry
    {
        // Clears everything. Called when a device is created or reset.
        void Reset();

        // Registration — called from the proxy device right after the real
        // resource is created. `writeOnlyStripped` records whether we removed
        // D3DUSAGE_WRITEONLY so the report can say the capture was intrusive.
        void AddVertexBuffer(IDirect3DVertexBuffer8* vb, uint32_t length,
                             uint32_t usage, uint32_t fvf, D3DPOOL pool,
                             bool writeOnlyStripped, uint32_t frameIndex);
        void AddIndexBuffer(IDirect3DIndexBuffer8* ib, uint32_t length,
                            uint32_t usage, D3DFORMAT format, D3DPOOL pool,
                            bool writeOnlyStripped, uint32_t frameIndex);
        void AddTexture(IDirect3DTexture8* tex, uint32_t width, uint32_t height,
                        uint32_t levels, uint32_t usage, D3DFORMAT format,
                        D3DPOOL pool, uint32_t frameIndex);

        // Lookup by the real interface pointer. Returns nullptr when unknown —
        // which happens legitimately for resources created before the hook was
        // installed, and for anything a wrapper DLL created behind our back.
        ResourceInfo* Find(const void* object);

        // Same, but also bumps drawRefCount. Used from the draw path.
        ResourceInfo* FindForDraw(const void* object);

        // Called from Release paths so ids are not reused for live objects.
        void Remove(const void* object);

        // Iteration for the report writer.
        uint32_t Count();
        const ResourceInfo* At(uint32_t index);

        // Aggregate counters for the running per-frame summary.
        struct Totals
        {
            uint32_t vertexBuffers;
            uint32_t indexBuffers;
            uint32_t textures;
            uint64_t vertexBufferBytes;
            uint64_t indexBufferBytes;
            uint64_t textureBytes;
            uint32_t writeOnlyStrippedCount;
        };
        Totals GetTotals();
    }
}
