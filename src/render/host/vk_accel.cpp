// vk_accel.cpp
#include "vk_accel.h"

#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <set>

using namespace SceneIPC;

namespace Host
{
namespace
{
    // Position is at offset 0 in both of the game's vertex layouts (24-byte
    // pre-lit and 32-byte lit), which is all an acceleration structure build
    // needs — the rest is material data the hit shaders read later.
    const VkDeviceSize kAccelScratchAlignment = 256;

    inline VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a)
    {
        return a ? ((v + a - 1) / a) * a : v;
    }

    double NowMilliseconds()
    {
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
    }

    // Applies the game's matrix palette to one mesh, writing world-relative
    // positions where the bone-local ones were.
    //
    // This mirrors the shader exactly:
    //
    //     mul   r10, v2, c57.z        ; palette row = index colour * scale
    //     mov   a0.x, r10.x
    //     m4x3  r11, v0, c0           ; c[row + 0..2], row-vector dot4s
    //     mul   r11.xyz, r11.xyzz, v1.x
    //
    // and runs here rather than in the producer because the bone-local
    // vertices never change: they upload once and are cached, while only the
    // pose crosses the process boundary. Skinning in the producer would
    // instead re-send 26,000 vertices every frame, about 48 MB/s.
    //
    // The normal is transformed by the same rows without translation, which
    // is what `m3x3 r8, v3, c0` does.
}

void SkinVertices(const Geometry& geo, const std::vector<float>& palette,
                      float indexScale, uint8_t* dst)
    {
        const SceneIPC::GeometryDesc& d = geo.desc;
        const uint32_t stride  = d.vertexStride;
        const uint32_t rows    = (uint32_t)(palette.size() / 4);
        const uint32_t weights = d.boneCount;

        for (uint32_t v = 0; v < d.vertexCount; ++v)
        {
            const uint8_t* src = geo.vertices.data() + (size_t)v * stride;
            uint8_t*       out = dst + (size_t)v * stride;

            const float* pos = (const float*)src;
            const float* nrm = (d.normalOffset != SceneIPC::kNoVertexAttribute)
                             ? (const float*)(src + d.normalOffset) : nullptr;

            // D3DCOLOR expands to (R,G,B,A) as x,y,z,w, each byte over 255.
            const uint8_t* idxBytes = src + d.boneIndexOffset;
            const uint8_t* wBytes   = (d.boneWeightOffset != SceneIPC::kNoVertexAttribute)
                                    ? src + d.boneWeightOffset : nullptr;

            float p[3] = { 0.0f, 0.0f, 0.0f };
            float n[3] = { 0.0f, 0.0f, 0.0f };
            float used = 0.0f;

            for (uint32_t b = 0; b < weights; ++b)
            {
                // A D3DCOLOR's bytes are stored BGRA, so component b is at
                // byte 2-b for the first three.
                const int comp = (b < 3) ? (2 - (int)b) : 3;
                const float rawIndex  = idxBytes[comp] / 255.0f;
                const float rawWeight = wBytes ? (wBytes[comp] / 255.0f) : 1.0f;

                const uint32_t row = (uint32_t)(rawIndex * indexScale + 0.5f);
                if (row + 2 >= rows) continue;
                if (rawWeight <= 0.0f) continue;

                const float* m0 = &palette[(size_t)(row + 0) * 4];
                const float* m1 = &palette[(size_t)(row + 1) * 4];
                const float* m2 = &palette[(size_t)(row + 2) * 4];

                p[0] += rawWeight * (pos[0]*m0[0] + pos[1]*m0[1] + pos[2]*m0[2] + m0[3]);
                p[1] += rawWeight * (pos[0]*m1[0] + pos[1]*m1[1] + pos[2]*m1[2] + m1[3]);
                p[2] += rawWeight * (pos[0]*m2[0] + pos[1]*m2[1] + pos[2]*m2[2] + m2[3]);

                if (nrm)
                {
                    n[0] += rawWeight * (nrm[0]*m0[0] + nrm[1]*m0[1] + nrm[2]*m0[2]);
                    n[1] += rawWeight * (nrm[0]*m1[0] + nrm[1]*m1[1] + nrm[2]*m1[2]);
                    n[2] += rawWeight * (nrm[0]*m2[0] + nrm[1]*m2[1] + nrm[2]*m2[2]);
                }
                used += rawWeight;
            }

            // Everything but the position and normal passes through, so UVs
            // and colours stay where the layout says they are.
            memcpy(out, src, stride);

            if (used > 0.0f)
            {
                float* op = (float*)out;
                op[0] = p[0]; op[1] = p[1]; op[2] = p[2];
                if (nrm)
                {
                    float* on = (float*)(out + d.normalOffset);
                    on[0] = n[0]; on[1] = n[1]; on[2] = n[2];
                }
            }
        }
    }

namespace
{
    // kDecalBias lives in the header now, because the shaders need it too.
    //
    // Its value: a step has to clear the depth resolution of the traversal,
    // which at 5,000 units of float32 is around 3e-4. At 1e-6 a step was
    // 0.005 units, only sixteen times that, and coplanar layers still tied
    // often enough to speckle the pitch. At 1e-5 a step is 0.05 units, 150
    // times the resolution, and still five parts in a million of a pitch
    // 10,500 units across. Measured on a recorded frame, the pitch's
    // neighbour-to-neighbour variation falls from 15.1 at 1e-6 to 8.1 at
    // 1e-5; at 1e-4 it rises again to 11.6, because by then the decals are
    // far enough off their base surface to be wrong in a new way.

    // How far the ordering runs before it starts over.
    //
    // A frame with hundreds of blended draws must not accumulate a visible
    // displacement, so the offset cannot grow without bound. It used to
    // saturate here, which was measurably wrong: a match frame has around
    // 700 blended draws out of 830, so every one of them past the 128th got
    // the same offset and tied with its neighbours exactly - no separation
    // at all for five draws in six, which is most of the pitch.
    //
    // Wrapping instead keeps the ordering correct inside any run of 128
    // consecutive blended draws while capping the displacement at the same
    // 128 steps. Coplanar draws are consecutive - the pitch's layers are
    // seven in a row - so a run is all that has to be ordered.
    //
    // The one case it does not separate is two coplanar draws exactly 128
    // apart in blended-draw order, where the later one lands behind the
    // earlier. That is what saturating did to every pair past the 128th, so
    // it is strictly the rarer failure, and no tuned constant moves.
    const uint32_t kMaxDecalSteps = 128;

    // The identity texture transform. A record is memset to zero before it
    // is filled, and an all-zero transform maps every UV onto one texel -
    // so this is not a nicety, it is what stops an untransformed draw
    // sampling a single pixel of its texture.
    inline void SetIdentityUv(Host::AccelBuilder::InstanceRecord& rec)
    {
        rec.uvTransform0[0] = 1.0f; rec.uvTransform0[1] = 0.0f;
        rec.uvTransform0[2] = 0.0f; rec.uvTransform0[3] = 1.0f;
        rec.uvTransform1[0] = 0.0f; rec.uvTransform1[1] = 0.0f;
        rec.uvTransform1[2] = 0.0f; rec.uvTransform1[3] = 0.0f;
    }

    inline bool IsBlended(const SceneIPC::InstanceDesc& inst)
    {
        return (inst.flags & (kInstanceAlphaBlend | kInstanceAlphaTest)) != 0;
    }

    inline uint64_t Mix64(uint64_t h, uint64_t v)
    {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h ? h : 1ull;
    }

    uint32_t TriangleCountOf(const Geometry& g)
    {
        const uint32_t indices = g.desc.indexCount ? g.desc.indexCount
                                                   : g.desc.vertexCount;
        return indices / 3;
    }
}

AccelBuilder::AccelBuilder()
    : m_device(nullptr), m_alloc(nullptr), m_commandPool(VK_NULL_HANDLE)
    , m_spriteCapacityBytes(0), m_tlasCapacityBytes(0)
    , m_instanceRecordCapacity(0), m_instanceRecordCount(0)
    , m_spriteTriangleCapacity(0), m_spriteTriangleCount(0), m_scratchCapacity(0)
    , m_haveVpHint(false), m_haveCameraPos(false), m_frameCounter(0)
{
    memset(&m_stats, 0, sizeof(m_stats));
}

AccelBuilder::~AccelBuilder()
{
    Shutdown();
}

bool AccelBuilder::Init(VulkanDevice* device, GpuAllocator* allocator)
{
    m_device = device;
    m_alloc  = allocator;
    if (!device || !device->IsValid() || !allocator) return false;

    VkCommandPoolCreateInfo pi{};
    pi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = device->GraphicsQueueFamily();

    if (vkCreateCommandPool(device->Device(), &pi, nullptr, &m_commandPool) != VK_SUCCESS)
    {
        m_lastError = "vkCreateCommandPool failed";
        return false;
    }
    return true;
}

void AccelBuilder::DestroyAccel(Accel& accel)
{
    if (accel.handle != VK_NULL_HANDLE)
    {
        m_device->RayTracingApi_().DestroyAccelerationStructure(
            m_device->Device(), accel.handle, nullptr);
        accel.handle = VK_NULL_HANDLE;
    }
    if (accel.storage.IsValid()) m_alloc->DestroyBuffer(accel.storage);
    accel.address = 0;
    accel.size    = 0;
}

void AccelBuilder::Shutdown()
{
    if (!m_device || !m_device->IsValid()) return;
    vkDeviceWaitIdle(m_device->Device());

    for (auto it = m_meshBlas.begin(); it != m_meshBlas.end(); ++it)
    {
        DestroyAccel(it->second.accel);
        m_alloc->DestroyBuffer(it->second.vertices);
        m_alloc->DestroyBuffer(it->second.indices);
    }
    m_meshBlas.clear();

    DestroyAccel(m_spriteBlas);
    DestroyAccel(m_tlas);
    if (m_spriteVertices.IsValid()) m_alloc->DestroyBuffer(m_spriteVertices);
    if (m_tlasInstances.IsValid())  m_alloc->DestroyBuffer(m_tlasInstances);
    if (m_scratch.IsValid())        m_alloc->DestroyBuffer(m_scratch);
    if (m_instanceRecords.IsValid()) m_alloc->DestroyBuffer(m_instanceRecords);
    if (m_spriteTriangles.IsValid()) m_alloc->DestroyBuffer(m_spriteTriangles);

    if (m_commandPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_device->Device(), m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
}

bool AccelBuilder::EnsureScratch(VkDeviceSize bytes)
{
    bytes = AlignUp(bytes, kAccelScratchAlignment);
    if (m_scratch.IsValid() && m_scratchCapacity >= bytes) return true;

    if (m_scratch.IsValid()) m_alloc->DestroyBuffer(m_scratch);

    // Grow generously; scratch is reused for every build in the frame and
    // reallocating it per build would dominate the cost.
    const VkDeviceSize capacity = bytes + bytes / 2 + (1u << 20);
    if (!m_alloc->CreateBuffer(capacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, m_scratch))
    {
        m_lastError = "scratch allocation failed: " + m_alloc->LastError();
        return false;
    }
    m_scratchCapacity = capacity;
    return true;
}

bool AccelBuilder::LooksLikeSky(const Geometry& geo, const Math::Mat4& world,
                                const float upAxis[3])
{
    if (!m_haveCameraPos) return false;

    const uint32_t stride = geo.desc.vertexStride;
    const uint32_t count  = geo.desc.vertexCount;
    if (stride < 12 || count == 0) return false;

    Bounds& b = m_objectBounds[geo.desc.geometryId];
    if (b.contentHash != geo.desc.contentHash || (b.lo[0] > b.hi[0]))
    {
        b.lo[0] = b.lo[1] = b.lo[2] =  3.4e38f;
        b.hi[0] = b.hi[1] = b.hi[2] = -3.4e38f;

        const uint8_t* vb = geo.vertices.data();
        const size_t   have = geo.vertices.size();
        for (uint32_t v = 0; v < count; ++v)
        {
            if ((size_t)v * stride + 12 > have) break;
            const float* p = (const float*)(vb + (size_t)v * stride);
            for (int k = 0; k < 3; ++k)
            {
                if (p[k] < b.lo[k]) b.lo[k] = p[k];
                if (p[k] > b.hi[k]) b.hi[k] = p[k];
            }
        }
        b.contentHash = geo.desc.contentHash;
        if (b.lo[0] > b.hi[0]) return false;   // nothing readable
    }

    // The eight corners of the object box, transformed, then bounded again.
    // A box of a rotated box is looser than the true bound, which is the safe
    // direction here: it can only make the test less willing to call
    // something the sky.
    float lo[3] = {  3.4e38f,  3.4e38f,  3.4e38f };
    float hi[3] = { -3.4e38f, -3.4e38f, -3.4e38f };
    for (int corner = 0; corner < 8; ++corner)
    {
        const float x = (corner & 1) ? b.hi[0] : b.lo[0];
        const float y = (corner & 2) ? b.hi[1] : b.lo[1];
        const float z = (corner & 4) ? b.hi[2] : b.lo[2];

        // Row-vector, as everywhere else on this path.
        const float w[3] = {
            x * world.m[0] + y * world.m[4] + z * world.m[8]  + world.m[12],
            x * world.m[1] + y * world.m[5] + z * world.m[9]  + world.m[13],
            x * world.m[2] + y * world.m[6] + z * world.m[10] + world.m[14]
        };
        for (int k = 0; k < 3; ++k)
        {
            if (w[k] < lo[k]) lo[k] = w[k];
            if (w[k] > hi[k]) hi[k] = w[k];
        }
    }

    const float eye[3] = { m_cameraPos.x, m_cameraPos.y, m_cameraPos.z };

    // A box around the camera: the simple case, and enough on its own.
    bool contains = true;
    for (int k = 0; k < 3; ++k)
        if (eye[k] < lo[k] || eye[k] > hi[k]) contains = false;
    if (contains) return true;

    // Otherwise: which axis is up, and which way along it. The game's own
    // hemisphere axis says, so this does not assume a handedness.
    int up = 1;
    for (int k = 0; k < 3; ++k)
        if (fabsf(upAxis[k]) > fabsf(upAxis[up])) up = k;
    if (fabsf(upAxis[up]) < 0.5f) return false;      // no usable up axis
    const float sign = upAxis[up] < 0.0f ? -1.0f : 1.0f;

    // Wholly overhead: every corner of the box further along up than the
    // camera is. A surface that dips below the viewer is part of the scene.
    const float nearEdge = (sign > 0.0f) ? lo[up] : hi[up];
    if ((nearEdge - eye[up]) * sign <= 0.0f) return false;

    // And surrounding: the camera inside the box on both other axes. This
    // is what separates a sky ring from a floodlight glow hanging in one
    // corner of it.
    for (int k = 0; k < 3; ++k)
    {
        if (k == up) continue;
        if (eye[k] < lo[k] || eye[k] > hi[k]) return false;
    }
    return true;
}

bool AccelBuilder::PrepareMeshBlas(const Geometry& geo, MeshBlas& out,
                                   PendingBuild& job,
                                   const std::vector<float>* palette,
                                   float indexScale)
{
    const RayTracingApi& rt = m_device->RayTracingApi_();

    const uint32_t vertexStride = geo.desc.vertexStride;
    const uint32_t vertexCount  = geo.desc.vertexCount;
    const uint32_t indexCount   = geo.desc.indexCount;
    const uint32_t triangles    = TriangleCountOf(geo);
    if (triangles == 0 || vertexCount == 0) return false;

    // Reuse the buffers when only the vertex contents moved, which is the
    // common case for a CPU-skinned mesh: its topology never changes.
    const bool sizesMatch = out.vertices.IsValid() &&
                            out.vertices.size == (VkDeviceSize)vertexCount * vertexStride &&
                            out.triangleCount == triangles;

    if (!sizesMatch)
    {
        if (out.vertices.IsValid()) m_alloc->DestroyBuffer(out.vertices);
        if (out.indices.IsValid())  m_alloc->DestroyBuffer(out.indices);
        DestroyAccel(out.accel);

        const VkBufferUsageFlags geomUsage =
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        // Host-visible so a re-skinned mesh refreshes with a memcpy rather
        // than a staging copy and a queue wait every frame.
        if (!m_alloc->CreateBuffer((VkDeviceSize)vertexCount * vertexStride,
                                   geomUsage, true, out.vertices))
            return false;

        if (indexCount &&
            !m_alloc->CreateBuffer((VkDeviceSize)geo.indices.size(),
                                   geomUsage, true, out.indices))
            return false;
    }

    // The copy into the BLAS buffer is where skinning happens: the same pass
    // that would memcpy the vertices applies the pose instead.
    if (geo.desc.boneCount && palette && !palette->empty() &&
        geo.desc.boneIndexOffset != SceneIPC::kNoVertexAttribute)
    {
        SkinVertices(geo, *palette, indexScale, (uint8_t*)out.vertices.mapped);
    }
    else
    {
        memcpy(out.vertices.mapped, geo.vertices.data(), geo.vertices.size());
    }
    if (indexCount && out.indices.IsValid())
        memcpy(out.indices.mapped, geo.indices.data(), geo.indices.size());

    VkAccelerationStructureGeometryTrianglesDataKHR tri{};
    tri.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;   // position is at offset 0
    tri.vertexData.deviceAddress = out.vertices.address;
    tri.vertexStride = vertexStride;
    tri.maxVertex    = vertexCount - 1;
    tri.indexType    = indexCount
                     ? (geo.desc.indexStride == 4 ? VK_INDEX_TYPE_UINT32
                                                  : VK_INDEX_TYPE_UINT16)
                     : VK_INDEX_TYPE_NONE_KHR;
    tri.indexData.deviceAddress = indexCount ? out.indices.address : 0;

    memset(&job, 0, sizeof(job));
    job.geometry.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    job.geometry.geometryType       = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    job.geometry.geometry.triangles = tri;

    // Deliberately not VK_GEOMETRY_OPAQUE_BIT_KHR. One BLAS can be referenced
    // by several instances, and whether a surface is opaque is a property of
    // the draw, not of the mesh - so opacity is decided per instance with
    // FORCE_OPAQUE and the any-hit shader handles the rest.
    job.geometry.flags              = 0;

    job.build.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    job.build.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    // ALLOW_DATA_ACCESS is what makes gl_HitTriangleVertexPositionsEXT work
    // in the hit shader; without it position fetch reads nothing.
    // A skinned mesh is rebuilt every frame - its pose changes even though
    // its vertices do not - so build time is what matters for it, while a
    // static mesh is built once and traced forever. Asking for a fast trace
    // on several hundred per-frame rebuilds spends the whole frame in the
    // builder.
    const bool rebuiltEveryFrame = geo.desc.boneCount != 0;
    job.build.flags = (rebuiltEveryFrame
                          ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                          : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_KHR;
    job.build.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    job.build.geometryCount = 1;
    job.build.pGeometries   = &job.geometry;   // repointed after the vector settles

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    rt.GetAccelerationStructureBuildSizes(
        m_device->Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &job.build, &triangles, &sizes);

    // The buffers can be reusable while the structure is not: the driver
    // is free to ask for more room for the same triangle count when the
    // vertex data moves, and building into a structure created for the
    // old, smaller answer corrupts memory past its end.
    if (out.accel.handle != VK_NULL_HANDLE &&
        out.accel.size < sizes.accelerationStructureSize)
    {
        DestroyAccel(out.accel);
        ++m_stats.blasResized;
    }

    if (out.accel.handle == VK_NULL_HANDLE)
    {
        if (!m_alloc->CreateBuffer(sizes.accelerationStructureSize,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                false, out.accel.storage))
            return false;

        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = out.accel.storage.buffer;
        ci.size   = sizes.accelerationStructureSize;
        ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

        if (rt.CreateAccelerationStructure(m_device->Device(), &ci, nullptr,
                                           &out.accel.handle) != VK_SUCCESS)
        {
            m_lastError = "vkCreateAccelerationStructureKHR failed (BLAS)";
            return false;
        }

        VkAccelerationStructureDeviceAddressInfoKHR ai{};
        ai.sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = out.accel.handle;
        out.accel.address = rt.GetAccelerationStructureDeviceAddress(m_device->Device(), &ai);
        out.accel.size    = ci.size;

        m_stats.blasBytes += sizes.accelerationStructureSize;
    }

    job.build.dstAccelerationStructure = out.accel.handle;
    job.range.primitiveCount           = triangles;
    job.scratchSize                    = sizes.buildScratchSize;

    out.triangleCount = triangles;
    out.contentHash   = geo.desc.contentHash;
    ++m_stats.blasBuiltThisFrame;
    return true;
}

bool AccelBuilder::PrepareSpriteBlas(const std::vector<float>& positions,
                                     PendingBuild& job)
{
    const RayTracingApi& rt = m_device->RayTracingApi_();
    const uint32_t vertexCount = (uint32_t)(positions.size() / 3);
    const uint32_t triangles   = vertexCount / 3;
    if (triangles == 0) return false;   // nothing to merge this frame

    const VkDeviceSize bytes = positions.size() * sizeof(float);
    if (!m_spriteVertices.IsValid() || m_spriteCapacityBytes < bytes)
    {
        if (m_spriteVertices.IsValid()) m_alloc->DestroyBuffer(m_spriteVertices);
        DestroyAccel(m_spriteBlas);

        const VkDeviceSize capacity = bytes + bytes / 2 + (1u << 16);
        if (!m_alloc->CreateBuffer(capacity,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                true, m_spriteVertices))
        {
            m_lastError = "sprite vertex buffer allocation failed";
            return false;
        }
        m_spriteCapacityBytes = capacity;
    }
    memcpy(m_spriteVertices.mapped, positions.data(), (size_t)bytes);

    VkAccelerationStructureGeometryTrianglesDataKHR tri{};
    tri.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexData.deviceAddress = m_spriteVertices.address;
    tri.vertexStride = sizeof(float) * 3;
    tri.maxVertex    = vertexCount - 1;
    tri.indexType    = VK_INDEX_TYPE_NONE_KHR;   // already expanded and merged

    memset(&job, 0, sizeof(job));
    job.geometry.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    job.geometry.geometryType       = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    job.geometry.geometry.triangles = tri;

    // Not opaque any more. The batch carries a per-triangle material now,
    // and some of those materials are mostly empty - the projected shadows
    // are a blob in a 128x128 texture whose mean alpha is a tenth - so the
    // any-hit shader has to get a chance to step over the empty parts.
    job.geometry.flags              = 0;

    job.build.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    job.build.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    // Rebuilt from scratch every frame, so build speed matters far more than
    // the trace quality of a structure that lives for a single frame.
    job.build.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR |
                              VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_KHR;
    job.build.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    job.build.geometryCount = 1;
    job.build.pGeometries   = &job.geometry;

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    rt.GetAccelerationStructureBuildSizes(
        m_device->Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &job.build, &triangles, &sizes);

    // Against the structure's own size, not the buffer's. The buffer is
    // half again as large so that a frame with a few more sprites does
    // not reallocate; that headroom is only useful if the structure is
    // created to span it, or every growth builds past the end of a
    // structure the buffer was merely big enough to hold.
    if (m_spriteBlas.handle == VK_NULL_HANDLE ||
        m_spriteBlas.size < sizes.accelerationStructureSize)
    {
        DestroyAccel(m_spriteBlas);
        const VkDeviceSize capacity = sizes.accelerationStructureSize +
                                      sizes.accelerationStructureSize / 2;
        if (!m_alloc->CreateBuffer(capacity,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                false, m_spriteBlas.storage))
            return false;

        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = m_spriteBlas.storage.buffer;
        ci.size   = capacity;
        ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (rt.CreateAccelerationStructure(m_device->Device(), &ci, nullptr,
                                           &m_spriteBlas.handle) != VK_SUCCESS)
        {
            m_lastError = "vkCreateAccelerationStructureKHR failed (sprite BLAS)";
            return false;
        }

        VkAccelerationStructureDeviceAddressInfoKHR ai{};
        ai.sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = m_spriteBlas.handle;
        m_spriteBlas.address = rt.GetAccelerationStructureDeviceAddress(
            m_device->Device(), &ai);
        m_spriteBlas.size = ci.size;
    }

    job.build.dstAccelerationStructure = m_spriteBlas.handle;
    job.range.primitiveCount           = triangles;
    job.scratchSize                    = sizes.buildScratchSize;

    m_stats.spriteTriangles = triangles;
    return true;
}

bool AccelBuilder::PrepareTlas(
    const std::vector<VkAccelerationStructureInstanceKHR>& instances,
    PendingBuild& job)
{
    const RayTracingApi& rt = m_device->RayTracingApi_();
    const uint32_t count = (uint32_t)instances.size();
    if (count == 0) return false;

    const VkDeviceSize bytes = sizeof(VkAccelerationStructureInstanceKHR) * count;
    if (!m_tlasInstances.IsValid() || m_tlasCapacityBytes < bytes)
    {
        if (m_tlasInstances.IsValid()) m_alloc->DestroyBuffer(m_tlasInstances);
        const VkDeviceSize capacity = bytes + bytes / 2 + 4096;
        if (!m_alloc->CreateBuffer(capacity,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                true, m_tlasInstances))
        {
            m_lastError = "TLAS instance buffer allocation failed";
            return false;
        }
        m_tlasCapacityBytes = capacity;
    }
    memcpy(m_tlasInstances.mapped, instances.data(), (size_t)bytes);

    VkAccelerationStructureGeometryInstancesDataKHR inst{};
    inst.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    inst.data.deviceAddress = m_tlasInstances.address;

    memset(&job, 0, sizeof(job));
    job.geometry.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    job.geometry.geometryType       = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    job.geometry.geometry.instances = inst;

    job.build.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    job.build.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    job.build.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    job.build.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    job.build.geometryCount = 1;
    job.build.pGeometries   = &job.geometry;

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    rt.GetAccelerationStructureBuildSizes(
        m_device->Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &job.build, &count, &sizes);

    // See PrepareSpriteBlas: the headroom belongs to the structure, and
    // the reuse test is against the structure rather than the buffer.
    if (m_tlas.handle == VK_NULL_HANDLE ||
        m_tlas.size < sizes.accelerationStructureSize)
    {
        DestroyAccel(m_tlas);
        const VkDeviceSize capacity = sizes.accelerationStructureSize +
                                      sizes.accelerationStructureSize / 2;
        if (!m_alloc->CreateBuffer(capacity,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                false, m_tlas.storage))
            return false;

        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = m_tlas.storage.buffer;
        ci.size   = capacity;
        ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (rt.CreateAccelerationStructure(m_device->Device(), &ci, nullptr,
                                           &m_tlas.handle) != VK_SUCCESS)
        {
            m_lastError = "vkCreateAccelerationStructureKHR failed (TLAS)";
            return false;
        }

        VkAccelerationStructureDeviceAddressInfoKHR ai{};
        ai.sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = m_tlas.handle;
        m_tlas.address = rt.GetAccelerationStructureDeviceAddress(m_device->Device(), &ai);
    }

    job.build.dstAccelerationStructure = m_tlas.handle;
    job.range.primitiveCount           = count;
    job.scratchSize                    = sizes.buildScratchSize;
    return true;
}

bool AccelBuilder::RecordAndSubmit(std::vector<PendingBuild>& blasJobs,
                                   PendingBuild* tlasJob)
{
    const RayTracingApi& rt = m_device->RayTracingApi_();
    if (blasJobs.empty() && !tlasJob) return true;

    // Builds submitted together each need their own scratch region, so the
    // total is summed first and every job takes a slice at the alignment the
    // device requires. Sharing one region would make the builds alias.
    const VkDeviceSize align =
        m_device->RayTracing().minScratchOffsetAlignment
            ? m_device->RayTracing().minScratchOffsetAlignment
            : kAccelScratchAlignment;

    VkDeviceSize cursor = 0;
    for (size_t i = 0; i < blasJobs.size(); ++i)
    {
        blasJobs[i].scratchOffset = cursor;
        cursor = AlignUp(cursor + blasJobs[i].scratchSize, align);
    }
    if (tlasJob)
    {
        tlasJob->scratchOffset = cursor;
        cursor = AlignUp(cursor + tlasJob->scratchSize, align);
    }
    if (!EnsureScratch(cursor)) return false;
    m_stats.scratchBytes = cursor;

    // pGeometries could only be pointed at its final address once the vector
    // had stopped reallocating.
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> builds;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
    builds.reserve(blasJobs.size());
    ranges.reserve(blasJobs.size());
    for (size_t i = 0; i < blasJobs.size(); ++i)
    {
        blasJobs[i].build.pGeometries = &blasJobs[i].geometry;
        blasJobs[i].build.scratchData.deviceAddress =
            m_scratch.address + blasJobs[i].scratchOffset;
        builds.push_back(blasJobs[i].build);
        ranges.push_back(&blasJobs[i].range);
    }

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = m_commandPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device->Device(), &cbai, &cmd);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    if (!builds.empty())
        rt.CmdBuildAccelerationStructures(cmd, (uint32_t)builds.size(),
                                          builds.data(), ranges.data());

    if (tlasJob)
    {
        // The TLAS reads the BLAS addresses written above, so it cannot begin
        // until those builds have completed.
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            0, 1, &barrier, 0, nullptr, 0, nullptr);

        tlasJob->build.pGeometries = &tlasJob->geometry;
        tlasJob->build.scratchData.deviceAddress =
            m_scratch.address + tlasJob->scratchOffset;
        const VkAccelerationStructureBuildRangeInfoKHR* r = &tlasJob->range;
        rt.CmdBuildAccelerationStructures(cmd, 1, &tlasJob->build, &r);
    }

    vkEndCommandBuffer(cmd);

    const bool submitted = SubmitAndWait(m_device->GraphicsQueue(), cmd,
                                         "acceleration structure build", m_lastError);
    vkFreeCommandBuffers(m_device->Device(), m_commandPool, 1, &cmd);
    return submitted;
}

bool AccelBuilder::ResolveViewProjection(const Frame& frame, Math::Mat4& outInverse)
{
    m_stats.vpCandidatesTried = 0;
    m_stats.vpBestScore       = 0;
    m_stats.vpSampleSize      = 0;
    m_stats.vpSource          = "none";

    if (frame.instances.empty()) return false;

    // Score a candidate by how many instances it turns into an affine world
    // matrix. A correct VP makes nearly all of them affine; a wrong one makes
    // almost none, so the two are separated by a wide margin rather than a
    // delicate threshold.
    const size_t sampleStride =
        frame.instances.size() > 128 ? frame.instances.size() / 128 : 1;

    struct Candidate { Math::Mat4 vp; Math::Mat4 inverse; const char* source; };
    std::vector<Candidate> candidates;

    auto addCandidate = [&](const Math::Mat4& vp, const char* source)
    {
        if (candidates.size() >= 24) return;
        Math::Mat4 inv;
        if (!Math::Inverse(vp, inv)) return;
        // Skip duplicates; the same clip transform recurs across instances.
        for (size_t i = 0; i < candidates.size(); ++i)
            if (memcmp(candidates[i].vp.m, vp.m, sizeof(vp.m)) == 0) return;
        Candidate c; c.vp = vp; c.inverse = inv; c.source = source;
        candidates.push_back(c);
    };

    // The VP that worked last frame is tried first: the camera moves, but the
    // *object* whose world matrix is identity stays the same, so its clip
    // transform remains the best candidate frame to frame.
    if (m_haveVpHint) addCandidate(m_vpHint, "previous frame hint");

    // What SetTransform claims. Correct for the fixed-function draws, wrong
    // for the shader ones, but cheap to test and it wins when it is right.
    addCandidate(Math::Multiply(frame.begin.view, frame.begin.projection),
                 "SetTransform view*projection");

    // Distinct clip transforms from the instances themselves. Any object with
    // an identity world matrix has clipTransform == VP exactly.
    for (size_t i = 0; i < frame.instances.size() && candidates.size() < 24; ++i)
        addCandidate(frame.instances[i].clipTransform, "instance clip transform");

    m_stats.vpCandidatesTried = (uint32_t)candidates.size();

    uint32_t sampled = 0;
    for (size_t i = 0; i < frame.instances.size(); i += sampleStride) ++sampled;
    m_stats.vpSampleSize = sampled;

    int bestIndex = -1;
    uint32_t bestScore = 0;
    for (size_t c = 0; c < candidates.size(); ++c)
    {
        uint32_t score = 0;
        for (size_t i = 0; i < frame.instances.size(); i += sampleStride)
        {
            const Math::Mat4 world =
                Math::Multiply(frame.instances[i].clipTransform, candidates[c].inverse);
            if (Math::IsAffine(world, 1e-2f)) ++score;
        }
        if (score > bestScore) { bestScore = score; bestIndex = (int)c; }
    }

    m_stats.vpBestScore = bestScore;

    // Require a clear majority. A candidate that only explains a handful of
    // instances is coincidence, and using it would misplace everything else.
    if (bestIndex < 0 || bestScore * 2 < sampled)
    {
        m_haveVpHint = false;
        return false;
    }

    // ── Where the camera is ──────────────────────────────────────────────
    // The eye is the world point that projects to clip x = y = w = 0: it is
    // on the view axis, and it is the point the projection is singular at.
    // Three linear equations, one per zeroed column.
    {
        const Math::Mat4& vp = candidates[bestIndex].vp;
        const float a[3][3] = {
            { vp.m[0], vp.m[1], vp.m[3] },
            { vp.m[4], vp.m[5], vp.m[7] },
            { vp.m[8], vp.m[9], vp.m[11] },
        };
        const float rhs[3] = { -vp.m[12], -vp.m[13], -vp.m[15] };

        // Cramer's rule: the matrix is 3x3 and inverting it by hand keeps
        // this self-contained.
        const float det =
            a[0][0]*(a[1][1]*a[2][2] - a[1][2]*a[2][1]) -
            a[0][1]*(a[1][0]*a[2][2] - a[1][2]*a[2][0]) +
            a[0][2]*(a[1][0]*a[2][1] - a[1][1]*a[2][0]);

        if (fabsf(det) > 1e-20f)
        {
            const float inv = 1.0f / det;
            m_cameraPos.x = inv * (rhs[0]*(a[1][1]*a[2][2] - a[1][2]*a[2][1]) -
                                   a[0][1]*(rhs[1]*a[2][2] - a[1][2]*rhs[2]) +
                                   a[0][2]*(rhs[1]*a[2][1] - a[1][1]*rhs[2]));
            m_cameraPos.y = inv * (a[0][0]*(rhs[1]*a[2][2] - a[1][2]*rhs[2]) -
                                   rhs[0]*(a[1][0]*a[2][2] - a[1][2]*a[2][0]) +
                                   a[0][2]*(a[1][0]*rhs[2] - rhs[1]*a[2][0]));
            m_cameraPos.z = inv * (a[0][0]*(a[1][1]*rhs[2] - rhs[1]*a[2][1]) -
                                   a[0][1]*(a[1][0]*rhs[2] - rhs[1]*a[2][0]) +
                                   rhs[0]*(a[1][0]*a[2][1] - a[1][1]*a[2][0]));
            m_haveCameraPos = true;
        }
        else m_haveCameraPos = false;
    }

    outInverse       = candidates[bestIndex].inverse;
    m_lastInverseVp  = candidates[bestIndex].inverse;
    m_vpHint         = candidates[bestIndex].vp;
    m_haveVpHint     = true;
    m_stats.vpSource = candidates[bestIndex].source;
    return true;
}

bool AccelBuilder::RecoverWorld(const InstanceDesc& inst,
                                const Math::Mat4& inverseViewProj,
                                bool haveInverse, Math::Mat4& outWorld) const
{
    // `worldTransform` is deliberately NOT used here, even when the producer
    // marked it valid.
    //
    // A fixed-function draw does know its own world matrix outright — but it
    // knows it in SetTransform's world space, and the shader draws (405 of
    // 466 in a measured match frame) live in the space their own c58 matrix
    // defines. Those two spaces differ: comparing the shader VP against
    // SetTransform's view*proj on a real frame shows rows 0 and 2 negated,
    // an axis flip between the fixed-function and shader paths.
    //
    // Since the reconstructed scene lives in whatever space the resolved
    // view-projection defines, placing fixed-function geometry by its own
    // world matrix would mirror it in X and Z relative to everything else.
    // Factorising every instance the same way keeps one space, whatever that
    // space happens to be. worldTransform is still sent, as a cross-check.
    (void)inst.worldTransform;

    if (!haveInverse) return false;

    outWorld = Math::Multiply(inst.clipTransform, inverseViewProj);

    // If the view-projection we divided by was not the one the shader used,
    // the result will not be affine. Rejecting those keeps a wrong camera
    // from silently scattering geometry across the scene.
    return Math::IsAffine(outWorld, 1e-2f);
}

bool AccelBuilder::BuildFrame(const SceneReceiver& scene,
                              const TextureCache* textures)
{
    const double started = NowMilliseconds();

    const uint32_t carriedBlas = (uint32_t)m_meshBlas.size();
    memset(&m_stats, 0, sizeof(m_stats));
    m_stats.persistentBlas = carriedBlas;
    ++m_frameCounter;

    const Frame& frame = scene.CurrentFrame();

    // The view-projection is shared by every draw in the frame, so it is
    // inverted once rather than per instance.
    Math::Mat4 inverseViewProj;
    const bool haveInverse = ResolveViewProjection(frame, inverseViewProj);

    std::vector<VkAccelerationStructureInstanceKHR> tlasInstances;
    tlasInstances.reserve(frame.instances.size() + 1);

    // Reserved up front because each job's build info points at the geometry
    // stored beside it; a reallocation would leave those pointers dangling.
    std::vector<PendingBuild> blasJobs;
    blasJobs.reserve(frame.instances.size() + 1);

    // Sprite triangles are expanded to world space here and merged into one
    // structure; see the header for why they do not get individual BLASes.
    // The parallel table carries the material the merge would otherwise
    // throw away, one entry per triangle.
    std::vector<float>          spritePositions;
    std::vector<SpriteTriangle> spriteTris;

    // One record per TLAS instance, in the same order, so
    // gl_InstanceCustomIndexEXT indexes straight into it.
    // How many times each skinned geometry has been drawn this frame, so
    // repeats get their own structure rather than fighting over one.
    std::unordered_map<uint64_t, uint32_t> skinnedUse;

    // Every destination already queued in this batch.
    std::set<VkAccelerationStructureKHR> scheduled;

    // How many blended instances have been emitted so far this frame. Their
    // order is the game's draw order, which is what decides which decal sits
    // on top of which.
    // Which way is up, for the sky test. Taken from the hemisphere axis the
    // game publishes in c93 rather than assumed: it is the game's own
    // statement of the vertical, and it is already being carried across
    // frames because the producer only resends the rig when it changes.
    //
    // Before the first lit draw of a session there is no rig, and the
    // fallback is the axis this game actually uses.
    float skyUp[3] = { 0.0f, -1.0f, 0.0f };
    if (frame.lightingValid)
    {
        skyUp[0] = frame.lighting.hemisphereAxis[0];
        skyUp[1] = frame.lighting.hemisphereAxis[1];
        skyUp[2] = frame.lighting.hemisphereAxis[2];
    }

    uint32_t decalOrder = 0;

    std::vector<InstanceRecord> records;
    records.reserve(frame.instances.size());

    for (size_t i = 0; i < frame.instances.size(); ++i)
    {
        const InstanceDesc& inst = frame.instances[i];

        const bool nonOccluding = (inst.flags & kInstanceNoDepthWrite) != 0;
        if (nonOccluding) ++m_stats.nonOccluding;

        const Geometry* geo = scene.FindGeometry(inst.geometryId);
        if (!geo) { ++m_stats.geometryUnresolved; continue; }

        Math::Mat4 world;
        if (!RecoverWorld(inst, inverseViewProj, haveInverse, world))
        {
            ++m_stats.transformsRejected;
            continue;
        }

        const uint32_t triangles = TriangleCountOf(*geo);
        if (triangles == 0) continue;

        // ── Backdrop, or decal? ──────────────────────────────────────────
        //
        // "Must not occlude" covers two quite different things, and letting
        // them in together painted the stadium out.
        //
        // A non-occluding draw that is *blended* is a decal: it composites
        // over what it decorates, which is what the pitch's wear and
        // markings and the projected shadows are, and it belongs in the
        // scene on kMaskNonOccluding.
        //
        // A non-occluding draw that is *opaque* cannot be a decal - a
        // surface that composites has to be blended - so it is a backdrop.
        // This game paints its sky first, with depth writes off, and then
        // draws the stadium over the top; those draws are the second and
        // third of the frame. The ray traced equivalent of "painted first
        // and never writes depth" is "behind everything", which is exactly
        // what kMaskSky is: consulted only once a primary ray has found
        // nothing else.
        //
        // The enclosure test stays as well, because a backdrop that wraps
        // the camera is a backdrop whatever its material - but on its own it
        // could only ever catch the sky drawn as a box. This stadium draws
        // it as flat planes, which enclose nothing and came out as a white
        // polygon over the roof.
        // Only the draw whose bounds contain the camera. Treating every
        // non-occluding *opaque* draw as a backdrop was tried and measured
        // wrong: it lightened the pitch from rgb(128,134,106) to
        // rgb(157,173,138) against the game's own rgb(76,95,51), because
        // some of what it swept out of the scene are opaque layers that
        // genuinely darken the pitch. "Opaque and told not to occlude" is
        // not the same statement as "backdrop", and this game makes both.
        // Opaque as well as overhead. The sky this game draws is opaque -
        // it is the backdrop everything else is painted over - while the
        // blended things that hang overhead are floodlight glow and haze,
        // which belong in the scene and tint what is under them. Moving
        // those to the sky mask lightened the pitch from rgb(129,134,106) to
        // rgb(157,173,138) against the game's own rgb(76,95,51).
        //
        // Opacity alone is not enough either, and was tried: it sweeps up
        // opaque non-occluding layers lying on the pitch, which is the same
        // failure from the other direction. It takes both.
        const bool isSky = nonOccluding && !IsBlended(inst) &&
                           LooksLikeSky(*geo, world, skyUp);
        if (isSky) ++m_stats.skyDraws;

        const uint32_t rayMask = isSky ? (uint32_t)kMaskSky
                               : nonOccluding ? (uint32_t)kMaskNonOccluding
                                              : (uint32_t)kMaskSolid;

        if (!isSky && triangles <= kSpriteTriangleLimit)
        {
            // Expand the indices and transform to world on the CPU. At a
            // couple of triangles apiece this is far cheaper than an
            // acceleration structure per sprite.
            const uint8_t* vb = geo->vertices.data();
            const uint32_t stride = geo->desc.vertexStride;
            const uint32_t indexCount = geo->desc.indexCount;
            const uint32_t uvOffset = geo->desc.uvOffset;

            // The material this draw would have had if it kept its own
            // structure. Every sprite in the stream is textured - the
            // hoardings, the stand panels, the projected shadows - so
            // dropping this is what left the batch rendering white.
            SpriteTriangle mat;
            memset(&mat, 0, sizeof(mat));
            mat.textureSlot  = textures ? textures->Slot(inst.baseTextureId)
                                        : kWhiteTextureSlot;
            mat.samplerIndex = SamplerIndexForAddress(inst.stageState);
            mat.flags = (inst.flags & (kInstanceAlphaBlend | kInstanceAlphaTest))
                      ? kRecordBlended : 0u;
            memcpy(mat.baseColor, inst.baseColorFactor, sizeof(mat.baseColor));

            for (uint32_t t = 0; t < triangles * 3; ++t)
            {
                uint32_t vertexIndex = t;
                if (indexCount)
                {
                    if (geo->desc.indexStride == 2)
                        vertexIndex = ((const uint16_t*)geo->indices.data())[t];
                    else
                        vertexIndex = ((const uint32_t*)geo->indices.data())[t];
                }

                // The index came from another process; a bad one must not
                // become an out-of-bounds read. A bad vertex is emitted
                // degenerate rather than skipped: dropping one would shift
                // every later triangle by a vertex, and now that
                // gl_PrimitiveID indexes the material table that would
                // mis-texture the rest of the batch rather than merely
                // deform one sprite.
                float x = 0.0f, y = 0.0f, z = 0.0f, u = 0.0f, v = 0.0f;
                if ((uint64_t)vertexIndex * stride + 12 <= geo->vertices.size())
                {
                    const float* p = (const float*)(vb + (size_t)vertexIndex * stride);
                    x = p[0]; y = p[1]; z = p[2];

                    if (uvOffset != SceneIPC::kNoVertexAttribute &&
                        (uint64_t)vertexIndex * stride + uvOffset + 8 <=
                            geo->vertices.size())
                    {
                        const float* uvp = (const float*)
                            (vb + (size_t)vertexIndex * stride + uvOffset);
                        u = uvp[0]; v = uvp[1];
                    }
                }

                // Row-vector transform, matching the game's convention.
                spritePositions.push_back(x*world.m[0] + y*world.m[4] + z*world.m[8]  + world.m[12]);
                spritePositions.push_back(x*world.m[1] + y*world.m[5] + z*world.m[9]  + world.m[13]);
                spritePositions.push_back(x*world.m[2] + y*world.m[6] + z*world.m[10] + world.m[14]);

                // Every third vertex opens a new triangle and its entry.
                const uint32_t corner = t % 3u;
                if (corner == 0u) spriteTris.push_back(mat);
                spriteTris.back().uv[corner * 2u]      = u;
                spriteTris.back().uv[corner * 2u + 1u] = v;
            }
            ++m_stats.spriteInstances;
            continue;
        }

        // A skinned mesh's vertices are bone-local and identical every frame,
        // so contentHash never changes - the pose does. Its structure has to
        // be rebuilt regardless.
        const std::vector<float>* palette =
            (i < frame.palettes.size() && !frame.palettes[i].empty())
                ? &frame.palettes[i] : nullptr;
        const bool skinned = geo->desc.boneCount != 0 && palette != nullptr;

        // A static mesh drawn twice shares one structure; a skinned one
        // cannot, because each instance carries its own pose. Keyed by
        // geometry id alone, two instances of the same skinned mesh both
        // rebuilt into the same destination in one command buffer - which is
        // undefined, and took the device with it.
        uint64_t blasKey = inst.geometryId;
        if (skinned)
            blasKey = Mix64(inst.geometryId, ++skinnedUse[inst.geometryId]);

        MeshBlas& blas = m_meshBlas[blasKey];
        blas.geometryId = inst.geometryId;

        const bool needsBuild = skinned ||
                                (blas.accel.handle == VK_NULL_HANDLE) ||
                                (blas.contentHash != geo->desc.contentHash);
        if (needsBuild)
        {
            PendingBuild job;
            if (!PrepareMeshBlas(*geo, blas, job, palette, inst.boneIndexScale))
            {
                m_meshBlas.erase(blasKey);
                continue;
            }

            // The keying above should make a repeated destination impossible.
            // This makes sure a future change cannot bring it back silently:
            // the symptom is a lost device, which says nothing about why.
            if (!scheduled.insert(job.build.dstAccelerationStructure).second)
            {
                ++m_stats.duplicateBuildsDropped;
            }
            else
            {
                if (skinned) ++m_stats.skinnedRebuilds;
                blasJobs.push_back(job);
            }
        }
        blas.lastUsedFrame = m_frameCounter;

        // The record must be pushed with the instance, not with the scene
        // draw: sprites and rejected instances leave gaps, so the scene index
        // is not the TLAS index.
        InstanceRecord rec;
        memset(&rec, 0, sizeof(rec));
        SetIdentityUv(rec);

        // The draw's own texture transform, when its shader had one. The
        // producer evaluates it, because which constant registers a shader
        // reads is a property of its bytecode - c75 is a UV basis in one
        // shader and a fog plane in another.
        if ((inst.flags & kInstanceUvTransform) != 0 &&
            i < frame.uvTransforms.size())
        {
            const std::array<float, 6>& t = frame.uvTransforms[i];
            rec.uvTransform0[0] = t[0]; rec.uvTransform0[1] = t[1];
            rec.uvTransform0[2] = t[2]; rec.uvTransform0[3] = t[3];
            rec.uvTransform1[0] = t[4]; rec.uvTransform1[1] = t[5];
            ++m_stats.uvTransformedDraws;
        }

        rec.vertexAddress = blas.vertices.address;
        rec.indexAddress  = blas.indices.IsValid() ? blas.indices.address : 0;
        rec.vertexStride  = geo->desc.vertexStride;
        rec.indexStride   = geo->desc.indexCount ? geo->desc.indexStride : 0;

        // Straight from the game's own vertex declaration. This was once
        // derived from vertexKind, which only worked because the producer
        // rejected every layout that did not match the guess - including the
        // most common one in the game. See DescribeDeclLayout.
        rec.uvOffset = geo->desc.uvOffset;

        // Skinned meshes have their normals rewritten by the palette during
        // the copy into the BLAS buffer, the same pass that moves the
        // positions, so this offset is valid for a posed mesh too.
        rec.normalOffset = geo->desc.normalOffset;

        // Untouched by the palette, which rewrites only position and normal
        // and copies the rest of the vertex through.
        rec.colorOffset = geo->desc.colorOffset;

        rec.textureSlot  = textures ? textures->Slot(inst.baseTextureId)
                                    : kWhiteTextureSlot;
        rec.samplerIndex = SamplerIndexForAddress(inst.stageState);
        rec.flags = (inst.flags & (kInstanceAlphaBlend | kInstanceAlphaTest))
                  ? kRecordBlended : 0u;
        if (isSky) rec.flags |= kRecordUnlit;

        // Where the coverage of a blended surface comes from: the draw's own
        // alpha op says, and nothing else can. Guessing it from whether the
        // texture had alpha of its own was measurably wrong - the pitch grass
        // is a blended draw with an opaque texture, so the guess folded its
        // vertex alpha in and lightened the pitch from rgb(129,134,106) to
        // rgb(157,173,138) against the game's own rgb(76,95,51).
        if ((rec.flags & kRecordBlended) != 0u &&
            VertexAlphaContributes(inst.stageState))
        {
            rec.flags |= kRecordVertexAlpha;
            ++m_stats.vertexAlphaDraws;
        }
        memcpy(rec.baseColor, inst.baseColorFactor, sizeof(rec.baseColor));

        // ── Coplanar decals: separate them by draw order ─────────────────
        //
        // Peeling layers front to back composites correctly, but it orders
        // by distance, and the game's decals are exactly coplanar with what
        // they decorate. Seven pitch layers sit on one plane; at equal depth
        // the traversal order is arbitrary and varies per ray, which is what
        // made the pitch speckle.
        //
        // The game resolves this by draw order, so that is what is
        // reproduced: each blended instance is nudged toward the viewer by an
        // amount that grows with its position in the frame, which makes the
        // later draw the nearer one - painter's order, expressed as depth.
        //
        // Scaled by distance because floating-point precision is: a fixed
        // offset is either lost in the noise far away or visible up close.
        // At 1e-6 a draw 400 places into a frame 5,000 units away moves two
        // units, which is four thousand times the depth resolution there and
        // invisible on a pitch 10,500 units across. Opaque geometry is left
        // exactly where it is, so the base surfaces never move.
        if (IsBlended(inst) && m_haveCameraPos)
        {
            const float dx = m_cameraPos.x - world.m[12];
            const float dy = m_cameraPos.y - world.m[13];
            const float dz = m_cameraPos.z - world.m[14];
            const float len = sqrtf(dx*dx + dy*dy + dz*dz);
            if (len > 1e-3f)
            {
                const uint32_t steps = decalOrder % kMaxDecalSteps;
                const float step = (float)steps * kDecalBias * len;
                world.m[12] += dx / len * step;
                world.m[13] += dy / len * step;
                world.m[14] += dz / len * step;
            }
            ++decalOrder;
        }

        VkAccelerationStructureInstanceKHR out{};
        Math::ToVkTransform(world, &out.transform.matrix[0][0]);
        out.instanceCustomIndex                    = (uint32_t)records.size() & 0xFFFFFF;
        records.push_back(rec);
        out.instanceShaderBindingTableRecordOffset = 0;
        out.flags = (inst.flags & kInstanceTwoSided)
                  ? VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR : 0;
        out.mask  = rayMask;

        // Anything the game drew without blending or alpha testing is a solid
        // surface, and forcing it opaque skips the any-hit shader entirely -
        // which is the fast path and covers most of the scene. The rest go
        // through the alpha test, which is what stops the pitch's six blended
        // overlays from fighting the grass they sit on.
        // Both bits are set explicitly rather than leaving one implied by the
        // geometry's own flags: opacity then depends only on what this loop
        // decides, and reads the same way at the call site.
        out.flags |= (inst.flags & (kInstanceAlphaBlend | kInstanceAlphaTest))
                   ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR
                   : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        out.accelerationStructureReference = blas.accel.address;
        tlasInstances.push_back(out);
    }

    // One TLAS instance covers every sprite, since they were already merged
    // in world space.
    bool haveSprites = false;
    if (!spritePositions.empty())
    {
        PendingBuild spriteJob;
        if (PrepareSpriteBlas(spritePositions, spriteJob))
        {
            blasJobs.push_back(spriteJob);
            haveSprites = true;
        }
    }
    if (haveSprites && m_spriteBlas.handle != VK_NULL_HANDLE)
    {
        // The merged sprite BLAS has no per-sprite identity of its own:
        // its triangles came from many draws and were baked into world
        // space together. The record says so, and the hit shaders read the
        // material out of the SpriteTriangle table by gl_PrimitiveID
        // instead of out of the record.
        InstanceRecord rec;
        memset(&rec, 0, sizeof(rec));
        SetIdentityUv(rec);
        rec.textureSlot  = kWhiteTextureSlot;
        rec.samplerIndex = 0;
        rec.uvOffset     = SceneIPC::kNoVertexAttribute;
        rec.normalOffset = SceneIPC::kNoVertexAttribute;
        rec.colorOffset  = SceneIPC::kNoVertexAttribute;
        rec.flags        = kRecordSpriteBatch;
        rec.baseColor[0] = rec.baseColor[1] = rec.baseColor[2] = rec.baseColor[3] = 1.0f;

        VkAccelerationStructureInstanceKHR out{};
        Math::ToVkTransform(Math::Identity(), &out.transform.matrix[0][0]);
        out.instanceCustomIndex = (uint32_t)records.size() & 0xFFFFFF;
        records.push_back(rec);

        // kMaskSolid, not kMaskPrimary: the batch mixes occluding and
        // non-occluding draws into one structure, so it cannot express
        // per-triangle shadow visibility. Casting shadows is what it did
        // before masks existed, so this changes nothing; splitting the
        // merge in two is what would fix it.
        out.mask  = kMaskSolid;

        // Deliberately not FORCE_OPAQUE: the batch has UVs now, and the
        // shadows and netting among it are mostly empty texture.
        out.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        out.accelerationStructureReference = m_spriteBlas.address;
        tlasInstances.push_back(out);
    }

    // The sprite material table goes up with the records, and for the same
    // reason: it describes the batch that is about to be built.
    m_spriteTriangleCount = haveSprites ? (uint32_t)spriteTris.size() : 0u;
    if (m_spriteTriangleCount)
    {
        const VkDeviceSize bytes = spriteTris.size() * sizeof(SpriteTriangle);
        if (!m_spriteTriangles.IsValid() || m_spriteTriangleCapacity < bytes)
        {
            if (m_spriteTriangles.IsValid())
                m_alloc->DestroyBuffer(m_spriteTriangles);

            const VkDeviceSize capacity = bytes + bytes / 2 + 4096;
            if (m_alloc->CreateBuffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      true, m_spriteTriangles))
                m_spriteTriangleCapacity = capacity;
            else
                m_spriteTriangleCount = 0;
        }
        if (m_spriteTriangleCount)
            memcpy(m_spriteTriangles.mapped, spriteTris.data(), (size_t)bytes);
    }

    // Records go up before the submit, so they describe the same frame the
    // structures do. Host-visible: this is rewritten every frame and is far
    // too small to be worth a staging copy and a queue wait.
    m_instanceRecordCount = (uint32_t)records.size();
    if (!records.empty())
    {
        const VkDeviceSize bytes = records.size() * sizeof(InstanceRecord);
        if (!m_instanceRecords.IsValid() || m_instanceRecordCapacity < bytes)
        {
            if (m_instanceRecords.IsValid()) m_alloc->DestroyBuffer(m_instanceRecords);
            const VkDeviceSize capacity = bytes + bytes / 2 + 4096;
            if (!m_alloc->CreateBuffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       true, m_instanceRecords))
            {
                m_lastError = "instance record buffer allocation failed: " +
                              m_alloc->LastError();
                return false;
            }
            m_instanceRecordCapacity = capacity;
        }
        memcpy(m_instanceRecords.mapped, records.data(), (size_t)bytes);
    }

    PendingBuild tlasJob;
    const bool haveTlas = PrepareTlas(tlasInstances, tlasJob);

    // Everything goes in one command buffer and one submit. Submitting and
    // waiting per structure measured about 7 ms of overhead each, which a
    // scene of a few hundred meshes cannot afford.
    if (!RecordAndSubmit(blasJobs, haveTlas ? &tlasJob : nullptr))
        return false;

    m_stats.persistentBlas    = (uint32_t)m_meshBlas.size();
    m_stats.tlasInstances     = (uint32_t)tlasInstances.size();
    m_stats.buildMilliseconds = NowMilliseconds() - started;
    return true;
}

void AccelBuilder::PruneOrphans(const SceneReceiver& scene)
{
    // Skinned meshes get one structure per occurrence, so a geometry drawn
    // eight times one frame and three the next leaves five behind. Those
    // still reference live geometry, so staleness has to be the other test.
    const uint64_t kUnusedFrames = 120;

    for (auto it = m_meshBlas.begin(); it != m_meshBlas.end(); )
    {
        const bool stale = m_frameCounter > kUnusedFrames &&
                           it->second.lastUsedFrame < m_frameCounter - kUnusedFrames;

        if (stale || !scene.FindGeometry(it->second.geometryId))
        {
            DestroyAccel(it->second.accel);
            m_alloc->DestroyBuffer(it->second.vertices);
            m_alloc->DestroyBuffer(it->second.indices);
            it = m_meshBlas.erase(it);
        }
        else ++it;
    }
}

} // namespace Host
