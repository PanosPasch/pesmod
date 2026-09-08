// vk_accel.cpp
#include "vk_accel.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

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

    uint32_t TriangleCountOf(const Geometry& g)
    {
        const uint32_t indices = g.desc.indexCount ? g.desc.indexCount
                                                   : g.desc.vertexCount;
        return indices / 3;
    }
}

AccelBuilder::AccelBuilder()
    : m_device(nullptr), m_alloc(nullptr), m_commandPool(VK_NULL_HANDLE)
    , m_spriteCapacityBytes(0), m_tlasCapacityBytes(0), m_scratchCapacity(0)
    , m_haveVpHint(false), m_frameCounter(0)
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

bool AccelBuilder::PrepareMeshBlas(const Geometry& geo, MeshBlas& out,
                                   PendingBuild& job)
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

    memcpy(out.vertices.mapped, geo.vertices.data(), geo.vertices.size());
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
    job.geometry.flags              = VK_GEOMETRY_OPAQUE_BIT_KHR;

    job.build.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    job.build.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    job.build.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    job.build.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    job.build.geometryCount = 1;
    job.build.pGeometries   = &job.geometry;   // repointed after the vector settles

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    rt.GetAccelerationStructureBuildSizes(
        m_device->Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &job.build, &triangles, &sizes);

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
    job.geometry.flags              = VK_GEOMETRY_OPAQUE_BIT_KHR;

    job.build.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    job.build.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    // Rebuilt from scratch every frame, so build speed matters far more than
    // the trace quality of a structure that lives for a single frame.
    job.build.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    job.build.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    job.build.geometryCount = 1;
    job.build.pGeometries   = &job.geometry;

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    rt.GetAccelerationStructureBuildSizes(
        m_device->Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &job.build, &triangles, &sizes);

    if (m_spriteBlas.handle == VK_NULL_HANDLE ||
        m_spriteBlas.storage.size < sizes.accelerationStructureSize)
    {
        DestroyAccel(m_spriteBlas);
        if (!m_alloc->CreateBuffer(sizes.accelerationStructureSize +
                                   sizes.accelerationStructureSize / 2,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                false, m_spriteBlas.storage))
            return false;

        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = m_spriteBlas.storage.buffer;
        ci.size   = sizes.accelerationStructureSize;
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

    if (m_tlas.handle == VK_NULL_HANDLE ||
        m_tlas.storage.size < sizes.accelerationStructureSize)
    {
        DestroyAccel(m_tlas);
        if (!m_alloc->CreateBuffer(sizes.accelerationStructureSize +
                                   sizes.accelerationStructureSize / 2,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                false, m_tlas.storage))
            return false;

        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = m_tlas.storage.buffer;
        ci.size   = sizes.accelerationStructureSize;
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

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_device->GraphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_device->GraphicsQueue());
    vkFreeCommandBuffers(m_device->Device(), m_commandPool, 1, &cmd);
    return true;
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

    outInverse       = candidates[bestIndex].inverse;
    m_vpHint         = candidates[bestIndex].vp;
    m_haveVpHint     = true;
    m_stats.vpSource = candidates[bestIndex].source;
    return true;
}

bool AccelBuilder::RecoverWorld(const InstanceDesc& inst,
                                const Math::Mat4& inverseViewProj,
                                bool haveInverse, Math::Mat4& outWorld) const
{
    // A fixed-function draw already told us its world matrix outright; no
    // factorisation, and no chance of getting it wrong.
    if (inst.flags & kInstanceWorldValid)
    {
        outWorld = inst.worldTransform;
        return true;
    }
    if (!haveInverse) return false;

    outWorld = Math::Multiply(inst.clipTransform, inverseViewProj);

    // If the view-projection we divided by was not the one the shader used,
    // the result will not be affine. Rejecting those keeps a wrong camera
    // from silently scattering geometry across the scene.
    return Math::IsAffine(outWorld, 1e-2f);
}

bool AccelBuilder::BuildFrame(const SceneReceiver& scene)
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
    std::vector<float> spritePositions;

    for (size_t i = 0; i < frame.instances.size(); ++i)
    {
        const InstanceDesc& inst = frame.instances[i];

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

        if (triangles <= kSpriteTriangleLimit)
        {
            // Expand the indices and transform to world on the CPU. At a
            // couple of triangles apiece this is far cheaper than an
            // acceleration structure per sprite.
            const uint8_t* vb = geo->vertices.data();
            const uint32_t stride = geo->desc.vertexStride;
            const uint32_t indexCount = geo->desc.indexCount;

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
                // become an out-of-bounds read.
                if ((uint64_t)vertexIndex * stride + 12 > geo->vertices.size())
                    continue;

                const float* p = (const float*)(vb + (size_t)vertexIndex * stride);
                const float x = p[0], y = p[1], z = p[2];

                // Row-vector transform, matching the game's convention.
                spritePositions.push_back(x*world.m[0] + y*world.m[4] + z*world.m[8]  + world.m[12]);
                spritePositions.push_back(x*world.m[1] + y*world.m[5] + z*world.m[9]  + world.m[13]);
                spritePositions.push_back(x*world.m[2] + y*world.m[6] + z*world.m[10] + world.m[14]);
            }
            ++m_stats.spriteInstances;
            continue;
        }

        MeshBlas& blas = m_meshBlas[inst.geometryId];
        const bool needsBuild = (blas.accel.handle == VK_NULL_HANDLE) ||
                                (blas.contentHash != geo->desc.contentHash);
        if (needsBuild)
        {
            PendingBuild job;
            if (!PrepareMeshBlas(*geo, blas, job))
            {
                m_meshBlas.erase(inst.geometryId);
                continue;
            }
            blasJobs.push_back(job);
        }
        blas.lastUsedFrame = m_frameCounter;

        VkAccelerationStructureInstanceKHR out{};
        Math::ToVkTransform(world, &out.transform.matrix[0][0]);
        out.instanceCustomIndex                    = (uint32_t)i & 0xFFFFFF;
        out.mask                                   = 0xFF;
        out.instanceShaderBindingTableRecordOffset = 0;
        out.flags = (inst.flags & kInstanceTwoSided)
                  ? VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR : 0;
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
        VkAccelerationStructureInstanceKHR out{};
        Math::ToVkTransform(Math::Identity(), &out.transform.matrix[0][0]);
        out.mask  = 0xFF;
        out.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        out.accelerationStructureReference = m_spriteBlas.address;
        tlasInstances.push_back(out);
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
    for (auto it = m_meshBlas.begin(); it != m_meshBlas.end(); )
    {
        if (!scene.FindGeometry(it->first))
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
