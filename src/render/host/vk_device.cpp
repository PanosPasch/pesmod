// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 Panagiotis Paschalis
//
// This file is part of PESMod.
//
// PESMod is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// PESMod is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with PESMod.  If not, see <https://www.gnu.org/licenses/>.

// vk_device.cpp
#include "vk_device.h"

#include <cstdio>
#include <cstring>
#include <set>

namespace Host
{
namespace
{
    // Required device extensions. Every one of these is load-bearing for a
    // ray traced renderer, so a missing entry is a hard failure rather than
    // a feature to switch off.
    const char* const kRequiredDeviceExtensions[] =
    {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,   // AS build dependency
        VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME,
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    const char* const kValidationLayer = "VK_LAYER_KHRONOS_validation";

    VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void*)
    {
        // Warnings and errors only. Validation is verbose enough at info
        // level to bury the messages that matter during RT bring-up.
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            printf("[vulkan] %s\n", data->pMessage ? data->pMessage : "(no message)");
        return VK_FALSE;
    }

    bool HasLayer(const char* name)
    {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());
        for (uint32_t i = 0; i < count; ++i)
            if (!strcmp(layers[i].layerName, name)) return true;
        return false;
    }

    const char* DeviceTypeName(VkPhysicalDeviceType t)
    {
        switch (t)
        {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
        default:                                     return "other";
        }
    }
}

VulkanDevice::VulkanDevice()
    : m_instance(VK_NULL_HANDLE)
    , m_debugMessenger(VK_NULL_HANDLE)
    , m_physicalDevice(VK_NULL_HANDLE)
    , m_device(VK_NULL_HANDLE)
    , m_graphicsQueue(VK_NULL_HANDLE)
    , m_graphicsFamily(UINT32_MAX)
    , m_apiVersion(0)
    , m_deviceLocalBytes(0)
    , m_validationEnabled(false)
{
    memset(&m_rtProperties, 0, sizeof(m_rtProperties));
    memset(&m_rt, 0, sizeof(m_rt));
}

VulkanDevice::~VulkanDevice()
{
    Destroy();
}

bool VulkanDevice::Create(const VulkanDeviceOptions& options)
{
    if (!CreateInstance(options))      return false;
    if (!SelectPhysicalDevice(options)) return false;
    if (!CreateLogicalDevice(options))  return false;
    if (!LoadRayTracingApi())           return false;
    return true;
}

bool VulkanDevice::CreateInstance(const VulkanDeviceOptions& options)
{
    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "PESMod Render Host";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName        = "PESMod";
    app.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion         = VK_API_VERSION_1_3;

    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    extensions.push_back("VK_KHR_win32_surface");

    if (options.enableValidation)
    {
        if (HasLayer(kValidationLayer))
        {
            layers.push_back(kValidationLayer);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            m_validationEnabled = true;
        }
        else if (options.verbose)
        {
            // Not fatal, but worth saying out loud: without validation, a
            // malformed acceleration structure is usually a silent hang.
            printf("[vulkan] validation layers requested but not installed; "
                   "continuing without them\n");
        }
    }

    VkInstanceCreateInfo ici{};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &app;
    ici.enabledLayerCount       = (uint32_t)layers.size();
    ici.ppEnabledLayerNames     = layers.empty() ? nullptr : layers.data();
    ici.enabledExtensionCount   = (uint32_t)extensions.size();
    ici.ppEnabledExtensionNames = extensions.data();

    const VkResult r = vkCreateInstance(&ici, nullptr, &m_instance);
    if (r != VK_SUCCESS)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "vkCreateInstance failed (VkResult %d)", (int)r);
        m_lastError = buf;
        return false;
    }

    if (m_validationEnabled)
    {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            m_instance, "vkCreateDebugUtilsMessengerEXT");
        if (create)
        {
            VkDebugUtilsMessengerCreateInfoEXT dbg{};
            dbg.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbg.pfnUserCallback = DebugCallback;
            create(m_instance, &dbg, nullptr, &m_debugMessenger);
        }
    }
    return true;
}

bool VulkanDevice::AdapterSupportsRayTracing(
    VkPhysicalDevice adapter, std::vector<std::string>& outMissing) const
{
    outMissing.clear();

    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(adapter, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(adapter, nullptr, &count, available.data());

    std::set<std::string> have;
    for (uint32_t i = 0; i < count; ++i) have.insert(available[i].extensionName);

    for (size_t i = 0; i < sizeof(kRequiredDeviceExtensions) /
                           sizeof(kRequiredDeviceExtensions[0]); ++i)
    {
        if (!have.count(kRequiredDeviceExtensions[i]))
            outMissing.push_back(kRequiredDeviceExtensions[i]);
    }

    // Extension presence is necessary but not sufficient — the features have
    // to be actually enabled-able, which is a separate query.
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{};
    rtFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
    asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeatures.pNext = &rtFeatures;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &asFeatures;
    vkGetPhysicalDeviceFeatures2(adapter, &features);

    if (!asFeatures.accelerationStructure)
        outMissing.push_back("feature: accelerationStructure");
    if (!rtFeatures.rayTracingPipeline)
        outMissing.push_back("feature: rayTracingPipeline");

    return outMissing.empty();
}

bool VulkanDevice::SelectPhysicalDevice(const VulkanDeviceOptions& options)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0)
    {
        m_lastError = "no Vulkan physical devices found";
        return false;
    }
    std::vector<VkPhysicalDevice> adapters(count);
    vkEnumeratePhysicalDevices(m_instance, &count, adapters.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    int bestScore = -1;
    std::string rejectionReport;

    for (uint32_t i = 0; i < count; ++i)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(adapters[i], &props);

        std::vector<std::string> missing;
        if (!AdapterSupportsRayTracing(adapters[i], missing))
        {
            rejectionReport += std::string("  ") + props.deviceName +
                               " (" + DeviceTypeName(props.deviceType) + "): missing ";
            for (size_t m = 0; m < missing.size(); ++m)
                rejectionReport += (m ? ", " : "") + missing[m];
            rejectionReport += "\n";
            continue;
        }

        int score = 1;
        if (options.preferDiscrete &&
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score += 1000;

        if (options.verbose)
            printf("[vulkan] candidate: %s (%s) - ray tracing OK\n",
                   props.deviceName, DeviceTypeName(props.deviceType));

        if (score > bestScore) { bestScore = score; best = adapters[i]; }
    }

    if (best == VK_NULL_HANDLE)
    {
        m_lastError = "no adapter supports ray tracing.\n" + rejectionReport +
            "The render host exists precisely because ray tracing is "
            "unavailable in the 32-bit game process, so a device without it "
            "has nothing to offer.";
        return false;
    }

    m_physicalDevice = best;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    m_deviceName = props.deviceName;
    m_apiVersion = props.apiVersion;

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &mem);
    for (uint32_t h = 0; h < mem.memoryHeapCount; ++h)
        if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            m_deviceLocalBytes += mem.memoryHeaps[h].size;

    // Ray tracing limits, which constrain the shader binding table layout.
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    rtProps.pNext = &asProps;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &props2);

    m_rtProperties.shaderGroupHandleSize      = rtProps.shaderGroupHandleSize;
    m_rtProperties.shaderGroupBaseAlignment   = rtProps.shaderGroupBaseAlignment;
    m_rtProperties.shaderGroupHandleAlignment = rtProps.shaderGroupHandleAlignment;
    m_rtProperties.maxRayRecursionDepth       = rtProps.maxRayRecursionDepth;
    m_rtProperties.maxShaderGroupStride       = rtProps.maxShaderGroupStride;
    m_rtProperties.maxRayDispatchInvocationCount = rtProps.maxRayDispatchInvocationCount;
    m_rtProperties.maxGeometryCount           = asProps.maxGeometryCount;
    m_rtProperties.maxInstanceCount           = asProps.maxInstanceCount;
    m_rtProperties.maxPrimitiveCount          = asProps.maxPrimitiveCount;
    m_rtProperties.minScratchOffsetAlignment  =
        asProps.minAccelerationStructureScratchOffsetAlignment;

    // Queue family: one that can do graphics and compute. RT dispatch and AS
    // builds both go on a graphics-capable queue here; splitting them across
    // queues is an optimisation that can come later.
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &qCount, families.data());
    for (uint32_t i = 0; i < qCount; ++i)
    {
        const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((families[i].queueFlags & need) == need) { m_graphicsFamily = i; break; }
    }
    if (m_graphicsFamily == UINT32_MAX)
    {
        m_lastError = "no graphics+compute queue family on the selected adapter";
        return false;
    }
    return true;
}

bool VulkanDevice::CreateLogicalDevice(const VulkanDeviceOptions& options)
{
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = m_graphicsFamily;
    queue.queueCount       = 1;
    queue.pQueuePriorities = &priority;

    // Feature chain. bufferDeviceAddress is not optional: acceleration
    // structure builds address their vertex and index data by device address,
    // not by descriptor.
    // Position fetch lets the hit shader read the triangle vertices it hit
    // without binding every mesh's buffers. That is what makes deriving a
    // geometric normal cheap, which this game needs: its 24-byte vertex
    // layout carries no normals at all.
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR posFetch{};
    posFetch.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR;
    posFetch.rayTracingPositionFetch = VK_TRUE;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{};
    rtFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtFeatures.rayTracingPipeline = VK_TRUE;
    rtFeatures.pNext = &posFetch;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
    asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeatures.accelerationStructure = VK_TRUE;
    asFeatures.pNext = &rtFeatures;

    VkPhysicalDeviceVulkan13Features v13{};
    v13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    v13.dynamicRendering = VK_TRUE;
    v13.synchronization2 = VK_TRUE;
    v13.pNext            = &asFeatures;

    VkPhysicalDeviceVulkan12Features v12{};
    v12.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    v12.bufferDeviceAddress  = VK_TRUE;
    v12.descriptorIndexing   = VK_TRUE;
    v12.runtimeDescriptorArray = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    v12.descriptorBindingPartiallyBound = VK_TRUE;
    v12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    v12.scalarBlockLayout    = VK_TRUE;
    v12.timelineSemaphore    = VK_TRUE;
    v12.pNext                = &v13;

    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &v12;
    features.features.samplerAnisotropy = VK_TRUE;
    features.features.shaderInt64       = VK_TRUE;

    std::vector<const char*> deviceExtensions;
    for (size_t i = 0; i < sizeof(kRequiredDeviceExtensions) /
                           sizeof(kRequiredDeviceExtensions[0]); ++i)
        deviceExtensions.push_back(kRequiredDeviceExtensions[i]);

    VkDeviceCreateInfo dci{};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext                   = &features;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &queue;
    dci.enabledExtensionCount   = (uint32_t)deviceExtensions.size();
    dci.ppEnabledExtensionNames = deviceExtensions.data();

    const VkResult r = vkCreateDevice(m_physicalDevice, &dci, nullptr, &m_device);
    if (r != VK_SUCCESS)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "vkCreateDevice failed (VkResult %d)", (int)r);
        m_lastError = buf;
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
    if (options.verbose)
        printf("[vulkan] device created on queue family %u\n", m_graphicsFamily);
    return true;
}

bool VulkanDevice::LoadRayTracingApi()
{
    // Every one of these is an extension entry point, so none is exported by
    // vulkan-1.lib and all must come through vkGetDeviceProcAddr.
    #define LOAD_RT(member, name)                                              \
        m_rt.member = (PFN_##name)vkGetDeviceProcAddr(m_device, #name);        \
        if (!m_rt.member) { m_lastError = "missing entry point: " #name; return false; }

    LOAD_RT(CreateAccelerationStructure,              vkCreateAccelerationStructureKHR);
    LOAD_RT(DestroyAccelerationStructure,             vkDestroyAccelerationStructureKHR);
    LOAD_RT(GetAccelerationStructureBuildSizes,       vkGetAccelerationStructureBuildSizesKHR);
    LOAD_RT(GetAccelerationStructureDeviceAddress,    vkGetAccelerationStructureDeviceAddressKHR);
    LOAD_RT(CmdBuildAccelerationStructures,           vkCmdBuildAccelerationStructuresKHR);
    LOAD_RT(CmdWriteAccelerationStructuresProperties, vkCmdWriteAccelerationStructuresPropertiesKHR);
    LOAD_RT(CmdCopyAccelerationStructure,             vkCmdCopyAccelerationStructureKHR);
    LOAD_RT(CreateRayTracingPipelines,                vkCreateRayTracingPipelinesKHR);
    LOAD_RT(GetRayTracingShaderGroupHandles,          vkGetRayTracingShaderGroupHandlesKHR);
    LOAD_RT(CmdTraceRays,                             vkCmdTraceRaysKHR);

    #undef LOAD_RT
    return true;
}

void VulkanDevice::PrintCapabilities() const
{
    printf("  device            %s\n", m_deviceName.c_str());
    printf("  api version       %u.%u.%u\n",
           VK_API_VERSION_MAJOR(m_apiVersion),
           VK_API_VERSION_MINOR(m_apiVersion),
           VK_API_VERSION_PATCH(m_apiVersion));
    printf("  device-local mem  %.1f GB\n", m_deviceLocalBytes / (1024.0*1024.0*1024.0));
    printf("  validation        %s\n", m_validationEnabled ? "on" : "off");
    printf("\n  ray tracing limits:\n");
    printf("    shader group handle size / alignment   %u / %u\n",
           m_rtProperties.shaderGroupHandleSize,
           m_rtProperties.shaderGroupHandleAlignment);
    printf("    shader group base alignment            %u\n",
           m_rtProperties.shaderGroupBaseAlignment);
    printf("    max ray recursion depth                %u\n",
           m_rtProperties.maxRayRecursionDepth);
    printf("    max ray dispatch invocations           %u\n",
           m_rtProperties.maxRayDispatchInvocationCount);
    printf("    max geometry / instance / primitive    %llu / %llu / %llu\n",
           (unsigned long long)m_rtProperties.maxGeometryCount,
           (unsigned long long)m_rtProperties.maxInstanceCount,
           (unsigned long long)m_rtProperties.maxPrimitiveCount);
}

void VulkanDevice::Destroy()
{
    if (m_device)
    {
        vkDeviceWaitIdle(m_device);
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_debugMessenger)
    {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            m_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }
    if (m_instance)
    {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

} // namespace Host

namespace Host
{
bool SubmitAndWait(VkQueue queue, VkCommandBuffer cmd, const char* what,
                   std::string& outError)
{
    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;

    VkResult r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) r = vkQueueWaitIdle(queue);
    if (r == VK_SUCCESS) return true;

    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %s (VkResult %d)", what,
             r == VK_ERROR_DEVICE_LOST      ? "device lost" :
             r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? "out of device memory" :
             r == VK_ERROR_OUT_OF_HOST_MEMORY   ? "out of host memory" :
                                              "submit failed", (int)r);
    outError = buf;
    return false;
}
}
