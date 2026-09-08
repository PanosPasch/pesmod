// vk_raytracer.cpp
#include "vk_raytracer.h"
#include "image_write.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace Host
{
namespace
{
    inline uint32_t AlignUp(uint32_t v, uint32_t a)
    {
        return a ? ((v + a - 1) / a) * a : v;
    }

    double NowMs()
    {
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
    }
}

RayTracer::RayTracer()
    : m_device(nullptr), m_alloc(nullptr), m_commandPool(VK_NULL_HANDLE)
    , m_setLayout(VK_NULL_HANDLE), m_descriptorPool(VK_NULL_HANDLE)
    , m_descriptorSet(VK_NULL_HANDLE), m_pipelineLayout(VK_NULL_HANDLE)
    , m_pipeline(VK_NULL_HANDLE)
    , m_image(VK_NULL_HANDLE), m_imageMemory(VK_NULL_HANDLE)
    , m_imageView(VK_NULL_HANDLE), m_width(0), m_height(0)
{
    memset(&m_rgenRegion, 0, sizeof(m_rgenRegion));
    memset(&m_missRegion, 0, sizeof(m_missRegion));
    memset(&m_hitRegion, 0, sizeof(m_hitRegion));
    memset(&m_callableRegion, 0, sizeof(m_callableRegion));
    memset(&m_stats, 0, sizeof(m_stats));
}

RayTracer::~RayTracer() { Shutdown(); }

bool RayTracer::LoadShaderModule(const char* path, VkShaderModule& out)
{
    FILE* f = nullptr;
    fopen_s(&f, path, "rb");
    if (!f)
    {
        m_lastError = std::string("cannot open shader: ") + path;
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint32_t> code((bytes + 3) / 4, 0);
    fread(code.data(), 1, bytes, f);
    fclose(f);

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = (size_t)bytes;
    ci.pCode    = code.data();

    if (vkCreateShaderModule(m_device->Device(), &ci, nullptr, &out) != VK_SUCCESS)
    {
        m_lastError = std::string("vkCreateShaderModule failed for ") + path;
        return false;
    }
    return true;
}

bool RayTracer::CreateDescriptors()
{
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                  VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                  VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                  VK_SHADER_STAGE_MISS_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(m_device->Device(), &li, nullptr,
                                    &m_setLayout) != VK_SUCCESS)
    {
        m_lastError = "vkCreateDescriptorSetLayout failed";
        return false;
    }

    VkDescriptorPoolSize sizes[3]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;              sizes[1].descriptorCount = 1;
    sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;             sizes[2].descriptorCount = 1;

    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets       = 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes    = sizes;
    if (vkCreateDescriptorPool(m_device->Device(), &pi, nullptr,
                               &m_descriptorPool) != VK_SUCCESS)
    {
        m_lastError = "vkCreateDescriptorPool failed";
        return false;
    }

    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = m_descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &m_setLayout;
    if (vkAllocateDescriptorSets(m_device->Device(), &ai, &m_descriptorSet) != VK_SUCCESS)
    {
        m_lastError = "vkAllocateDescriptorSets failed";
        return false;
    }

    return m_alloc->CreateBuffer(sizeof(SceneUniforms),
                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 true, m_uniforms);
}

bool RayTracer::CreatePipeline(const char* shaderDir)
{
    const std::string dir(shaderDir);

    VkShaderModule rgen = VK_NULL_HANDLE, skyMiss = VK_NULL_HANDLE;
    VkShaderModule shadowMiss = VK_NULL_HANDLE, chit = VK_NULL_HANDLE;

    if (!LoadShaderModule((dir + "/primary.rgen.spv").c_str(),   rgen))       return false;
    if (!LoadShaderModule((dir + "/sky.rmiss.spv").c_str(),      skyMiss))    return false;
    if (!LoadShaderModule((dir + "/shadow.rmiss.spv").c_str(),   shadowMiss)) return false;
    if (!LoadShaderModule((dir + "/primary.rchit.spv").c_str(),  chit))       return false;

    VkPipelineShaderStageCreateInfo stages[4]{};
    for (int i = 0; i < 4; ++i)
    {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;      stages[0].module = rgen;
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;        stages[1].module = skyMiss;
    stages[2].stage = VK_SHADER_STAGE_MISS_BIT_KHR;        stages[2].module = shadowMiss;
    stages[3].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR; stages[3].module = chit;

    // Group order defines the SBT layout: raygen, then both miss shaders in
    // the order the shaders index them (0 = sky, 1 = shadow), then hit.
    VkRayTracingShaderGroupCreateInfoKHR groups[4]{};
    for (int i = 0; i < 4; ++i)
    {
        groups[i].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[i].generalShader      = VK_SHADER_UNUSED_KHR;
        groups[i].closestHitShader   = VK_SHADER_UNUSED_KHR;
        groups[i].anyHitShader       = VK_SHADER_UNUSED_KHR;
        groups[i].intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR; groups[0].generalShader = 0;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR; groups[1].generalShader = 1;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR; groups[2].generalShader = 2;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[3].closestHitShader = 3;

    VkPipelineLayoutCreateInfo pli{};
    pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts    = &m_setLayout;
    if (vkCreatePipelineLayout(m_device->Device(), &pli, nullptr,
                               &m_pipelineLayout) != VK_SUCCESS)
    {
        m_lastError = "vkCreatePipelineLayout failed";
        return false;
    }

    VkRayTracingPipelineCreateInfoKHR ci{};
    ci.sType                        = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    ci.stageCount                   = 4;
    ci.pStages                      = stages;
    ci.groupCount                   = 4;
    ci.pGroups                      = groups;
    // The primary ray spawns one shadow ray, so two levels are needed and no
    // more. Asking for more than the device supports is a validation error.
    ci.maxPipelineRayRecursionDepth = 2;
    ci.layout                       = m_pipelineLayout;

    const VkResult r = m_device->RayTracingApi_().CreateRayTracingPipelines(
        m_device->Device(), VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci, nullptr,
        &m_pipeline);

    vkDestroyShaderModule(m_device->Device(), rgen, nullptr);
    vkDestroyShaderModule(m_device->Device(), skyMiss, nullptr);
    vkDestroyShaderModule(m_device->Device(), shadowMiss, nullptr);
    vkDestroyShaderModule(m_device->Device(), chit, nullptr);

    if (r != VK_SUCCESS)
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "vkCreateRayTracingPipelinesKHR failed (%d)", (int)r);
        m_lastError = buf;
        return false;
    }
    return true;
}

bool RayTracer::CreateShaderBindingTable()
{
    const RayTracingProperties& rt = m_device->RayTracing();

    // Handle size and alignment are separate limits. Packing records at
    // handle size alone happens to work when they are equal and mis-addresses
    // every record after the first when they are not.
    const uint32_t handleSize    = rt.shaderGroupHandleSize;
    const uint32_t handleStride  = AlignUp(handleSize, rt.shaderGroupHandleAlignment);
    const uint32_t baseAlignment = rt.shaderGroupBaseAlignment;
    const uint32_t groupCount    = 4;

    std::vector<uint8_t> handles((size_t)handleSize * groupCount);
    if (m_device->RayTracingApi_().GetRayTracingShaderGroupHandles(
            m_device->Device(), m_pipeline, 0, groupCount,
            handles.size(), handles.data()) != VK_SUCCESS)
    {
        m_lastError = "vkGetRayTracingShaderGroupHandlesKHR failed";
        return false;
    }

    // Each region starts at shaderGroupBaseAlignment; records within a region
    // are spaced by the handle stride.
    const uint32_t rgenSize = AlignUp(handleStride, baseAlignment);
    const uint32_t missSize = AlignUp(handleStride * 2, baseAlignment);
    const uint32_t hitSize  = AlignUp(handleStride, baseAlignment);
    const uint32_t total    = rgenSize + missSize + hitSize;

    if (!m_alloc->CreateBuffer(total,
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, m_sbt))
    {
        m_lastError = "shader binding table allocation failed";
        return false;
    }

    uint8_t* dst = (uint8_t*)m_sbt.mapped;
    memset(dst, 0, total);

    memcpy(dst, handles.data(), handleSize);                                   // raygen
    memcpy(dst + rgenSize,                handles.data() + handleSize,     handleSize);  // miss: sky
    memcpy(dst + rgenSize + handleStride, handles.data() + handleSize * 2, handleSize);  // miss: shadow
    memcpy(dst + rgenSize + missSize,     handles.data() + handleSize * 3, handleSize);  // hit

    const VkDeviceAddress base = m_sbt.address;

    // A raygen region must have size == stride; the spec is explicit and
    // validation will say so if it does not.
    m_rgenRegion.deviceAddress = base;
    m_rgenRegion.stride        = rgenSize;
    m_rgenRegion.size          = rgenSize;

    m_missRegion.deviceAddress = base + rgenSize;
    m_missRegion.stride        = handleStride;
    m_missRegion.size          = missSize;

    m_hitRegion.deviceAddress  = base + rgenSize + missSize;
    m_hitRegion.stride         = handleStride;
    m_hitRegion.size           = hitSize;

    m_stats.sbtBytes = total;
    return true;
}

bool RayTracer::CreateOutputImage(uint32_t width, uint32_t height)
{
    VkImageCreateInfo ii{};
    ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.format        = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent        = { width, height, 1 };
    ii.mipLevels     = 1;
    ii.arrayLayers   = 1;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device->Device(), &ii, nullptr, &m_image) != VK_SUCCESS)
    {
        m_lastError = "vkCreateImage failed";
        return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device->Device(), m_image, &req);

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(m_device->PhysicalDevice(), &mem);
    uint32_t typeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            { typeIndex = i; break; }
    if (typeIndex == UINT32_MAX) { m_lastError = "no device-local memory type"; return false; }

    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = typeIndex;
    if (vkAllocateMemory(m_device->Device(), &ai, nullptr, &m_imageMemory) != VK_SUCCESS)
    {
        m_lastError = "output image allocation failed";
        return false;
    }
    vkBindImageMemory(m_device->Device(), m_image, m_imageMemory, 0);

    VkImageViewCreateInfo vi{};
    vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image    = m_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(m_device->Device(), &vi, nullptr, &m_imageView) != VK_SUCCESS)
    {
        m_lastError = "vkCreateImageView failed";
        return false;
    }

    m_width  = width;
    m_height = height;
    m_stats.width  = width;
    m_stats.height = height;
    return true;
}

void RayTracer::DestroyOutputImage()
{
    if (m_imageView)   { vkDestroyImageView(m_device->Device(), m_imageView, nullptr); m_imageView = VK_NULL_HANDLE; }
    if (m_image)       { vkDestroyImage(m_device->Device(), m_image, nullptr);         m_image = VK_NULL_HANDLE; }
    if (m_imageMemory) { vkFreeMemory(m_device->Device(), m_imageMemory, nullptr);     m_imageMemory = VK_NULL_HANDLE; }
}

bool RayTracer::Init(VulkanDevice* device, GpuAllocator* allocator,
                     const char* shaderDir, uint32_t width, uint32_t height)
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

    if (!CreateDescriptors())              return false;
    if (!CreatePipeline(shaderDir))        return false;
    if (!CreateShaderBindingTable())       return false;
    if (!CreateOutputImage(width, height)) return false;
    return true;
}

bool RayTracer::Resize(uint32_t width, uint32_t height)
{
    if (width == m_width && height == m_height) return true;
    vkDeviceWaitIdle(m_device->Device());
    DestroyOutputImage();
    return CreateOutputImage(width, height);
}

void RayTracer::UpdateDescriptors(VkAccelerationStructureKHR tlas)
{
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
    asInfo.sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures    = &tlas;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView   = m_imageView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_uniforms.buffer;
    bufferInfo.range  = sizeof(SceneUniforms);

    VkWriteDescriptorSet writes[3]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext           = &asInfo;
    writes[0].dstSet          = m_descriptorSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_descriptorSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo      = &imageInfo;

    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = m_descriptorSet;
    writes[2].dstBinding      = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo     = &bufferInfo;

    vkUpdateDescriptorSets(m_device->Device(), 3, writes, 0, nullptr);
}

bool RayTracer::Trace(VkAccelerationStructureKHR tlas, const SceneUniforms& uniforms)
{
    if (tlas == VK_NULL_HANDLE) { m_lastError = "no TLAS to trace against"; return false; }

    const double started = NowMs();

    memcpy(m_uniforms.mapped, &uniforms, sizeof(uniforms));
    UpdateDescriptors(tlas);

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

    // The storage image must be in GENERAL for imageStore, and the previous
    // frame left it in TRANSFER_SRC if it was read back.
    VkImageMemoryBarrier toGeneral{};
    toGeneral.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.image            = m_image;
    toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toGeneral.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGeneral);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    m_device->RayTracingApi_().CmdTraceRays(cmd, &m_rgenRegion, &m_missRegion,
                                            &m_hitRegion, &m_callableRegion,
                                            m_width, m_height, 1);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_device->GraphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_device->GraphicsQueue());
    vkFreeCommandBuffers(m_device->Device(), m_commandPool, 1, &cmd);

    m_stats.traceMilliseconds = NowMs() - started;
    return true;
}

bool RayTracer::SaveImage(const char* path)
{
    const VkDeviceSize bytes = (VkDeviceSize)m_width * m_height * 4;

    GpuBuffer readback;
    if (!m_alloc->CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback))
    {
        m_lastError = "readback buffer allocation failed";
        return false;
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

    VkImageMemoryBarrier toSrc{};
    toSrc.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image            = m_image;
    toSrc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toSrc.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent      = { m_width, m_height, 1 };
    vkCmdCopyImageToBuffer(cmd, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &copy);

    // Back to GENERAL. The descriptor written at init names GENERAL, so an
    // image left in TRANSFER_SRC_OPTIMAL makes every subsequent trace write
    // through a descriptor whose layout no longer matches the image — which
    // is exactly what happens in the live loop, where a save is followed by
    // more frames rather than by shutdown.
    VkImageMemoryBarrier toGeneral = toSrc;
    toGeneral.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGeneral.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toGeneral);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_device->GraphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_device->GraphicsQueue());
    vkFreeCommandBuffers(m_device->Device(), m_commandPool, 1, &cmd);

    // The extension picks the encoder; see image_write.h for why there are
    // two of them.
    const bool ok = WriteImage(path, (const uint8_t*)readback.mapped,
                               m_width, m_height);
    if (!ok) m_lastError = std::string("cannot write ") + path;

    m_alloc->DestroyBuffer(readback);
    return ok;
}

void RayTracer::Shutdown()
{
    if (!m_device || !m_device->IsValid()) return;
    vkDeviceWaitIdle(m_device->Device());

    DestroyOutputImage();
    if (m_sbt.IsValid())      m_alloc->DestroyBuffer(m_sbt);
    if (m_uniforms.IsValid()) m_alloc->DestroyBuffer(m_uniforms);

    if (m_pipeline)       { vkDestroyPipeline(m_device->Device(), m_pipeline, nullptr); m_pipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device->Device(), m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device->Device(), m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_setLayout)      { vkDestroyDescriptorSetLayout(m_device->Device(), m_setLayout, nullptr); m_setLayout = VK_NULL_HANDLE; }
    if (m_commandPool)    { vkDestroyCommandPool(m_device->Device(), m_commandPool, nullptr); m_commandPool = VK_NULL_HANDLE; }
}

} // namespace Host
