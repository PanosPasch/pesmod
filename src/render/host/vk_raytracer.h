// vk_raytracer.h
//
// The ray tracing pipeline: shader modules, descriptors, the shader binding
// table, and the trace itself.
//
// ── Shader binding table ─────────────────────────────────────────────────
//
// Four shaders in three groups: one raygen, two miss (sky and shadow), one
// closest-hit. The SBT is the part of Vulkan RT most easily got subtly
// wrong, because handle size and alignment are separate device limits that
// happen to be equal on some hardware and not others. On this GPU they are
// 32 and 64, so a table packed at handle size alone would work for the
// raygen record and then mis-address the miss records. Both are respected
// explicitly here rather than assumed equal.
//
// ── Output ───────────────────────────────────────────────────────────────
//
// Traces into a storage image which can be read back to a file. There is no
// swapchain yet: proving the trace produces correct pixels does not need a
// window, and a file can be diffed by a test where a window cannot.
#pragma once

#include "vk_accel.h"
#include "vk_alloc.h"
#include "vk_device.h"

#include <string>
#include <vector>

namespace Host
{
    // Mirrors SceneUniforms in shaders/common.glsl. The matrix is stored
    // row-major row-vector exactly as the game produces it; see the note in
    // that file about why no transpose is needed.
    struct SceneUniforms
    {
        float invViewProj[16];
        float lightDirection[4];
        float lightColor[4];
        float hemisphereAxis[4];
        float skyColor[4];
        float groundColor[4];
        float ambient[4];
        float params[4];        // x = shadow ray length, y = exposure
    };

    struct TraceStats
    {
        uint32_t width;
        uint32_t height;
        double   traceMilliseconds;
        uint64_t sbtBytes;
    };

    class RayTracer
    {
    public:
        RayTracer();
        ~RayTracer();

        // `shaderDir` holds the compiled .spv files produced by the build.
        bool Init(VulkanDevice* device, GpuAllocator* allocator,
                  const char* shaderDir, uint32_t width, uint32_t height);
        void Shutdown();

        bool Resize(uint32_t width, uint32_t height);

        // Traces one frame against `tlas`. Returns false on a Vulkan error.
        bool Trace(VkAccelerationStructureKHR tlas, const SceneUniforms& uniforms);

        // Reads the output image back and writes a binary PPM. Slow — it
        // stalls on a queue wait — so it is for inspection and tests, not
        // for every frame.
        bool SaveImage(const char* path);

        // The pipeline is the last thing Init creates, so its presence
        // is what distinguishes a usable tracer from one whose shaders
        // were missing.
        bool IsReady() const { return m_pipeline != VK_NULL_HANDLE; }

        uint32_t Width()  const { return m_width; }
        uint32_t Height() const { return m_height; }

        const TraceStats& Stats() const { return m_stats; }
        const std::string& LastError() const { return m_lastError; }

    private:
        bool LoadShaderModule(const char* path, VkShaderModule& out);
        bool CreatePipeline(const char* shaderDir);
        bool CreateShaderBindingTable();
        bool CreateOutputImage(uint32_t width, uint32_t height);
        void DestroyOutputImage();
        bool CreateDescriptors();
        void UpdateDescriptors(VkAccelerationStructureKHR tlas);

        VulkanDevice* m_device;
        GpuAllocator* m_alloc;
        VkCommandPool m_commandPool;

        VkDescriptorSetLayout m_setLayout;
        VkDescriptorPool      m_descriptorPool;
        VkDescriptorSet       m_descriptorSet;
        VkPipelineLayout      m_pipelineLayout;
        VkPipeline            m_pipeline;

        GpuBuffer m_sbt;
        VkStridedDeviceAddressRegionKHR m_rgenRegion;
        VkStridedDeviceAddressRegionKHR m_missRegion;
        VkStridedDeviceAddressRegionKHR m_hitRegion;
        VkStridedDeviceAddressRegionKHR m_callableRegion;

        VkImage        m_image;
        VkDeviceMemory m_imageMemory;
        VkImageView    m_imageView;
        uint32_t       m_width, m_height;

        GpuBuffer   m_uniforms;
        TraceStats  m_stats;
        std::string m_lastError;
    };
}
