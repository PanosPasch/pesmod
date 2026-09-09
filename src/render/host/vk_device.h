// vk_device.h
//
// Vulkan instance and device setup for the render host, with ray tracing
// mandatory rather than optional.
//
// The whole reason this process exists is that RT is unreachable from inside
// the 32-bit game (docs/RENDERER.md §1), so a device without the ray tracing
// extensions is not a degraded mode worth supporting — it means something is
// wrong with the driver or the adapter selection, and saying so plainly beats
// falling back to a raster path that was never the goal.
#pragma once

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <string>
#include <vector>

namespace Host
{
    // Submits and waits, reporting what went wrong.
    //
    // Every submit in this renderer used to ignore its result. A lost device
    // then produced an unwritten output image and kept going, which looks
    // like corrupted rendering and says nothing about the cause - the whole
    // point of a device loss is that everything after it is garbage. Naming
    // it once is worth more than any amount of staring at the output.
    //
    // Returns false on any error; `what` names the caller in the message.
    bool SubmitAndWait(VkQueue queue, VkCommandBuffer cmd, const char* what,
                       std::string& outError);
}

namespace Host
{
    struct VulkanDeviceOptions
    {
        bool enableValidation;   // validation layers, if installed
        bool preferDiscrete;     // pick a discrete GPU over an integrated one
        bool verbose;
    };

    // The ray tracing limits that actually constrain how the renderer is
    // built — shader binding table stride and alignment, recursion budget,
    // and geometry capacity for the acceleration structures.
    struct RayTracingProperties
    {
        uint32_t shaderGroupHandleSize;
        uint32_t shaderGroupBaseAlignment;
        uint32_t shaderGroupHandleAlignment;
        uint32_t maxRayRecursionDepth;
        uint32_t maxShaderGroupStride;
        uint64_t maxGeometryCount;
        uint64_t maxInstanceCount;
        uint64_t maxPrimitiveCount;
        uint32_t maxRayDispatchInvocationCount;
        uint32_t minScratchOffsetAlignment;   // for packing builds together
    };

    // Extension entry points are not exported by the loader library, so each
    // one has to be fetched through vkGetDeviceProcAddr after device creation.
    struct RayTracingApi
    {
        PFN_vkCreateAccelerationStructureKHR                 CreateAccelerationStructure;
        PFN_vkDestroyAccelerationStructureKHR                DestroyAccelerationStructure;
        PFN_vkGetAccelerationStructureBuildSizesKHR          GetAccelerationStructureBuildSizes;
        PFN_vkGetAccelerationStructureDeviceAddressKHR       GetAccelerationStructureDeviceAddress;
        PFN_vkCmdBuildAccelerationStructuresKHR              CmdBuildAccelerationStructures;
        PFN_vkCmdWriteAccelerationStructuresPropertiesKHR    CmdWriteAccelerationStructuresProperties;
        PFN_vkCmdCopyAccelerationStructureKHR                CmdCopyAccelerationStructure;
        PFN_vkCreateRayTracingPipelinesKHR                   CreateRayTracingPipelines;
        PFN_vkGetRayTracingShaderGroupHandlesKHR             GetRayTracingShaderGroupHandles;
        PFN_vkCmdTraceRaysKHR                                CmdTraceRays;
    };

    class VulkanDevice
    {
    public:
        VulkanDevice();
        ~VulkanDevice();

        // Returns false and leaves `LastError()` set on any failure, including
        // "this adapter has no ray tracing support".
        bool Create(const VulkanDeviceOptions& options);
        void Destroy();

        bool IsValid() const { return m_device != VK_NULL_HANDLE; }
        const std::string& LastError() const { return m_lastError; }

        VkInstance       Instance()       const { return m_instance; }
        VkPhysicalDevice PhysicalDevice() const { return m_physicalDevice; }
        VkDevice         Device()         const { return m_device; }
        VkQueue          GraphicsQueue()  const { return m_graphicsQueue; }
        uint32_t         GraphicsQueueFamily() const { return m_graphicsFamily; }

        const RayTracingProperties& RayTracing()    const { return m_rtProperties; }
        const RayTracingApi&        RayTracingApi_() const { return m_rt; }
        const std::string&          DeviceName()    const { return m_deviceName; }
        uint32_t                    ApiVersion()    const { return m_apiVersion; }
        uint64_t                    DeviceLocalBytes() const { return m_deviceLocalBytes; }

        // Human-readable capability dump, used by the host's --probe mode.
        void PrintCapabilities() const;

    private:
        bool CreateInstance(const VulkanDeviceOptions& options);
        bool SelectPhysicalDevice(const VulkanDeviceOptions& options);
        bool CreateLogicalDevice(const VulkanDeviceOptions& options);
        bool LoadRayTracingApi();

        // Reports which required extensions a candidate adapter is missing,
        // so a rejection explains itself instead of just saying "no device".
        bool AdapterSupportsRayTracing(VkPhysicalDevice adapter,
                                       std::vector<std::string>& outMissing) const;

        VkInstance               m_instance;
        VkDebugUtilsMessengerEXT m_debugMessenger;
        VkPhysicalDevice         m_physicalDevice;
        VkDevice                 m_device;
        VkQueue                  m_graphicsQueue;
        uint32_t                 m_graphicsFamily;
        uint32_t                 m_apiVersion;
        uint64_t                 m_deviceLocalBytes;

        RayTracingProperties m_rtProperties;
        RayTracingApi        m_rt;
        std::string          m_deviceName;
        std::string          m_lastError;
        bool                 m_validationEnabled;
    };
}
