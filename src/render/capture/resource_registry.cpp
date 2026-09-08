// resource_registry.cpp
#include "resource_registry.h"
#include "../d3d8/d3d8_util.h"

#include <vector>
#include <unordered_map>
#include <map>
#include <iterator>
#include <cstring>

namespace
{
    std::vector<Capture::ResourceInfo>            g_resources;
    std::unordered_map<const void*, uint32_t>     g_byPointer;   // object → index
    uint32_t                                      g_nextId = 1;

    // Keyed by SetVertexShader handle. Ordered so the report lists them
    // deterministically.
    std::map<uint32_t, Capture::Registry::VertexShaderInfo> g_vertexShaders;

    Capture::ResourceInfo& Append(Capture::ResourceKind kind, void* object)
    {
        // A pointer can legitimately be reused after the previous owner was
        // released, so an existing entry is overwritten rather than doubled up.
        auto it = g_byPointer.find(object);
        if (it != g_byPointer.end())
        {
            Capture::ResourceInfo& existing = g_resources[it->second];
            const uint32_t reusedId = existing.id;
            existing = Capture::ResourceInfo();
            existing.id     = reusedId;
            existing.kind   = kind;
            existing.object = object;
            return existing;
        }

        g_resources.push_back(Capture::ResourceInfo());
        Capture::ResourceInfo& info = g_resources.back();
        info.id     = g_nextId++;
        info.kind   = kind;
        info.object = object;
        g_byPointer[object] = (uint32_t)(g_resources.size() - 1);
        return info;
    }
}

namespace Capture { namespace Registry {

void Reset()
{
    g_resources.clear();
    g_byPointer.clear();
    g_vertexShaders.clear();
    g_nextId = 1;
}

void AddVertexBuffer(IDirect3DVertexBuffer8* vb, uint32_t length,
                     uint32_t usage, uint32_t fvf, D3DPOOL pool,
                     bool writeOnlyStripped, uint32_t frameIndex)
{
    if (!vb) return;
    ResourceInfo& info = Append(kResourceVertexBuffer, vb);
    info.byteLength        = length;
    info.usage             = usage;
    info.fvf               = fvf;
    info.pool              = pool;
    info.format            = D3DFMT_VERTEXDATA;
    info.writeOnlyStripped = writeOnlyStripped;
    info.firstSeenFrame    = frameIndex;
}

void AddIndexBuffer(IDirect3DIndexBuffer8* ib, uint32_t length,
                    uint32_t usage, D3DFORMAT format, D3DPOOL pool,
                    bool writeOnlyStripped, uint32_t frameIndex)
{
    if (!ib) return;
    ResourceInfo& info = Append(kResourceIndexBuffer, ib);
    info.byteLength        = length;
    info.usage             = usage;
    info.format            = format;
    info.pool              = pool;
    info.writeOnlyStripped = writeOnlyStripped;
    info.firstSeenFrame    = frameIndex;
}

void AddTexture(IDirect3DTexture8* tex, uint32_t width, uint32_t height,
                uint32_t levels, uint32_t usage, D3DFORMAT format,
                D3DPOOL pool, uint32_t frameIndex)
{
    if (!tex) return;
    ResourceInfo& info = Append(kResourceTexture, tex);
    info.width          = width;
    info.height         = height;
    info.levels         = levels;
    info.usage          = usage;
    info.format         = format;
    info.pool           = pool;
    info.firstSeenFrame = frameIndex;
    // Level 0 only; mip chain size is reported by the capture writer, which
    // queries the real level descriptors.
    info.byteLength     = D3D8Util::SurfaceBytes(format, width, height);
}

ResourceInfo* Find(const void* object)
{
    if (!object) return nullptr;
    auto it = g_byPointer.find(object);
    if (it == g_byPointer.end()) return nullptr;
    return &g_resources[it->second];
}

ResourceInfo* FindForDraw(const void* object)
{
    ResourceInfo* info = Find(object);
    if (info) ++info->drawRefCount;
    return info;
}

void Remove(const void* object)
{
    if (!object) return;
    auto it = g_byPointer.find(object);
    if (it == g_byPointer.end()) return;
    // The entry is left in place so ids stay stable for the report; only the
    // pointer mapping is dropped, so a later allocation at the same address
    // creates a fresh record rather than inheriting this one.
    g_resources[it->second].object = nullptr;
    g_byPointer.erase(it);
}

uint32_t Count()
{
    return (uint32_t)g_resources.size();
}

const ResourceInfo* At(uint32_t index)
{
    return (index < g_resources.size()) ? &g_resources[index] : nullptr;
}

// ── Vertex declarations ──────────────────────────────────────────────────
void AddVertexShader(uint32_t handle, const uint32_t* declaration,
                     bool hasFunction)
{
    VertexShaderInfo info;
    memset(&info, 0, sizeof(info));
    info.handle      = handle;
    info.hasFunction = hasFunction;

    // 512 tokens is far beyond any real fixed-function declaration and keeps
    // a malformed or non-terminated stream from running off into free memory.
    if (declaration)
        D3D8Util::VertexDeclDecode(declaration, 512, info.layout);

    g_vertexShaders[handle] = info;
}

void RemoveVertexShader(uint32_t handle)
{
    g_vertexShaders.erase(handle);
}

const VertexShaderInfo* FindVertexShader(uint32_t handle)
{
    std::map<uint32_t, VertexShaderInfo>::const_iterator it =
        g_vertexShaders.find(handle);
    return (it == g_vertexShaders.end()) ? nullptr : &it->second;
}

uint32_t VertexShaderCount()
{
    return (uint32_t)g_vertexShaders.size();
}

const VertexShaderInfo* VertexShaderAt(uint32_t index)
{
    if (index >= g_vertexShaders.size()) return nullptr;
    std::map<uint32_t, VertexShaderInfo>::const_iterator it =
        g_vertexShaders.begin();
    std::advance(it, index);
    return &it->second;
}

Totals GetTotals()
{
    Totals t = Totals();
    for (size_t i = 0; i < g_resources.size(); ++i)
    {
        const ResourceInfo& r = g_resources[i];
        switch (r.kind)
        {
        case kResourceVertexBuffer:
            ++t.vertexBuffers; t.vertexBufferBytes += r.byteLength; break;
        case kResourceIndexBuffer:
            ++t.indexBuffers;  t.indexBufferBytes  += r.byteLength; break;
        case kResourceTexture:
            ++t.textures;      t.textureBytes      += r.byteLength; break;
        }
        if (r.writeOnlyStripped) ++t.writeOnlyStrippedCount;
    }
    return t;
}

}} // namespace Capture::Registry
