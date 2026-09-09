// vk_present.cpp
#include "vk_present.h"

#include <cstdio>
#include <cstring>

namespace Host
{
namespace
{
    const wchar_t* kWindowClass = L"PESModHostWindow";

    // A window closed by the user is not an error, so it is signalled by a
    // flag on the Presenter rather than by failing a call. The pointer is
    // stashed at creation so the static WndProc can reach it.
    Presenter* FromWindow(HWND hwnd)
    {
        return (Presenter*)(LONG_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
}

Presenter::Presenter()
    : m_device(nullptr), m_window(nullptr), m_instanceHandle(nullptr)
    , m_open(false), m_surface(VK_NULL_HANDLE), m_swapchain(VK_NULL_HANDLE)
    , m_format(VK_FORMAT_UNDEFINED), m_commandPool(VK_NULL_HANDLE)
    , m_commandBuffer(VK_NULL_HANDLE), m_acquired(VK_NULL_HANDLE)
    , m_presented(VK_NULL_HANDLE), m_inFlight(VK_NULL_HANDLE)
    , m_needsRebuild(false)
{
    m_extent.width = m_extent.height = 0;
}

Presenter::~Presenter() { Shutdown(); }

LRESULT CALLBACK Presenter::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }

    Presenter* self = FromWindow(hwnd);
    switch (msg)
    {
    case WM_CLOSE:
        if (self) self->m_open = false;
        return 0;
    case WM_DESTROY:
        if (self) self->m_open = false;
        return 0;
    case WM_SIZE:
        // Rebuilt lazily on the next present: resizing fires this many times
        // per drag, and each rebuild waits for the device to go idle.
        if (self) self->m_needsRebuild = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Presenter::CreateWindowAndSurface(uint32_t width, uint32_t height,
                                       const char* title)
{
    m_instanceHandle = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &Presenter::WndProc;
    wc.hInstance     = m_instanceHandle;
    // IDC_ARROW is an integer-encoded resource id that the headers hand
    // out as LPSTR unless UNICODE is defined; the cast keeps the W call
    // without making the whole target Unicode.
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);   // harmless if already registered

    wchar_t wide[128] = {0};
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wide, 127);

    // The requested size is the client area, not the frame.
    RECT r = { 0, 0, (LONG)width, (LONG)height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    m_window = CreateWindowExW(
        0, kWindowClass, wide, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, m_instanceHandle, this);

    if (!m_window)
    {
        m_lastError = "CreateWindowEx failed";
        return false;
    }
    ShowWindow(m_window, SW_SHOW);
    m_open = true;

    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = m_instanceHandle;
    sci.hwnd      = m_window;

    if (vkCreateWin32SurfaceKHR(m_device->Instance(), &sci, nullptr,
                                &m_surface) != VK_SUCCESS)
    {
        m_lastError = "vkCreateWin32SurfaceKHR failed";
        return false;
    }

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(m_device->PhysicalDevice(),
                                         m_device->GraphicsQueueFamily(),
                                         m_surface, &supported);
    if (!supported)
    {
        // The queue chosen for ray tracing cannot present. Nothing here can
        // recover from that, and rendering to a window it cannot show would
        // be worse than saying so.
        m_lastError = "the graphics queue does not support presenting";
        return false;
    }
    return true;
}

bool Presenter::CreateSwapchain(uint32_t width, uint32_t height)
{
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_device->PhysicalDevice(),
                                              m_surface, &caps);

    m_extent = caps.currentExtent;
    if (m_extent.width == 0xFFFFFFFFu)   // the surface defers to us
    {
        m_extent.width  = width;
        m_extent.height = height;
    }
    if (m_extent.width == 0 || m_extent.height == 0)
        return false;                     // minimised; try again later

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_device->PhysicalDevice(), m_surface,
                                         &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_device->PhysicalDevice(), m_surface,
                                         &formatCount, formats.data());
    if (formats.empty())
    {
        m_lastError = "the surface reports no formats";
        return false;
    }

    // B8G8R8A8_UNORM to match what the tracer produces, so the blit is a
    // copy rather than a conversion; the first format otherwise.
    VkSurfaceFormatKHR chosen = formats[0];
    for (size_t i = 0; i < formats.size(); ++i)
    {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM)
        {
            chosen = formats[i];
            break;
        }
    }
    m_format = chosen.format;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = m_surface;
    ci.minImageCount    = imageCount;
    ci.imageFormat      = chosen.format;
    ci.imageColorSpace  = chosen.colorSpace;
    ci.imageExtent      = m_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    // FIFO is the only mode guaranteed present, and it is the right one here:
    // the host is paced by the game's frames, not by how fast it can spin.
    ci.presentMode  = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped      = VK_TRUE;
    ci.oldSwapchain = m_swapchain;

    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(m_device->Device(), &ci, nullptr, &created) != VK_SUCCESS)
    {
        m_lastError = "vkCreateSwapchainKHR failed";
        return false;
    }

    if (m_swapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(m_device->Device(), m_swapchain, nullptr);
    m_swapchain = created;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(m_device->Device(), m_swapchain, &count, nullptr);
    m_images.resize(count);
    vkGetSwapchainImagesKHR(m_device->Device(), m_swapchain, &count, m_images.data());

    m_needsRebuild = false;
    return true;
}

bool Presenter::Init(VulkanDevice* device, uint32_t width, uint32_t height,
                     const char* title)
{
    m_device = device;
    if (!device || !device->IsValid()) return false;

    if (!CreateWindowAndSurface(width, height, title)) return false;
    if (!CreateSwapchain(width, height)) return false;

    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_device->GraphicsQueueFamily();
    if (vkCreateCommandPool(m_device->Device(), &pci, nullptr,
                            &m_commandPool) != VK_SUCCESS)
    {
        m_lastError = "present command pool creation failed";
        return false;
    }

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = m_commandPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device->Device(), &cbai, &m_commandBuffer);

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(m_device->Device(), &sci, nullptr, &m_acquired);
    vkCreateSemaphore(m_device->Device(), &sci, nullptr, &m_presented);

    // Created signalled so the first frame does not wait on a fence nothing
    // has submitted yet.
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(m_device->Device(), &fci, nullptr, &m_inFlight);

    return true;
}

void Presenter::PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool Presenter::Present(VkImage source, uint32_t srcWidth, uint32_t srcHeight)
{
    if (!m_open || m_swapchain == VK_NULL_HANDLE) return false;

    if (m_needsRebuild)
    {
        vkDeviceWaitIdle(m_device->Device());
        if (!CreateSwapchain(m_extent.width, m_extent.height))
            return true;      // minimised, most likely; try again next frame
    }

    vkWaitForFences(m_device->Device(), 1, &m_inFlight, VK_TRUE, UINT64_MAX);

    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(m_device->Device(), m_swapchain,
                                       UINT64_MAX, m_acquired,
                                       VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        m_needsRebuild = true;
        return true;
    }
    if (r != VK_SUCCESS)
    {
        m_lastError = "vkAcquireNextImageKHR failed";
        return false;
    }

    vkResetFences(m_device->Device(), 1, &m_inFlight);
    vkResetCommandBuffer(m_commandBuffer, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_commandBuffer, &bi);

    VkImageMemoryBarrier toSrc{};
    toSrc.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image            = source;
    toSrc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toSrc.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;

    VkImageMemoryBarrier toDst = toSrc;
    toDst.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image            = m_images[index];
    toDst.srcAccessMask    = 0;
    toDst.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;

    VkImageMemoryBarrier before[2] = { toSrc, toDst };
    vkCmdPipelineBarrier(m_commandBuffer,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, before);

    // Blit rather than copy, so the window need not match the traced size.
    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1]  = { (int32_t)srcWidth, (int32_t)srcHeight, 1 };
    blit.dstOffsets[1]  = { (int32_t)m_extent.width, (int32_t)m_extent.height, 1 };

    vkCmdBlitImage(m_commandBuffer,
                   source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);

    // The source goes back to GENERAL because that is what the tracer's
    // descriptor says it is in; leaving it in TRANSFER_SRC would make every
    // later trace write through a stale layout.
    VkImageMemoryBarrier backToGeneral = toSrc;
    backToGeneral.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    backToGeneral.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    backToGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    backToGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    VkImageMemoryBarrier toPresent = toDst;
    toPresent.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;

    VkImageMemoryBarrier after[2] = { backToGeneral, toPresent };
    vkCmdPipelineBarrier(m_commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 2, after);

    vkEndCommandBuffer(m_commandBuffer);

    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &m_acquired;
    si.pWaitDstStageMask    = &wait;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &m_commandBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &m_presented;

    if (vkQueueSubmit(m_device->GraphicsQueue(), 1, &si, m_inFlight) != VK_SUCCESS)
    {
        m_lastError = "present submit failed";
        return false;
    }

    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &m_presented;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &m_swapchain;
    pi.pImageIndices      = &index;

    r = vkQueuePresentKHR(m_device->GraphicsQueue(), &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        m_needsRebuild = true;
    else if (r != VK_SUCCESS)
    {
        m_lastError = "vkQueuePresentKHR failed";
        return false;
    }
    return true;
}

void Presenter::DestroySwapchain()
{
    if (m_swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_device->Device(), m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    m_images.clear();
}

void Presenter::Shutdown()
{
    if (!m_device || !m_device->IsValid()) return;
    vkDeviceWaitIdle(m_device->Device());

    if (m_inFlight)  { vkDestroyFence(m_device->Device(), m_inFlight, nullptr);  m_inFlight = VK_NULL_HANDLE; }
    if (m_acquired)  { vkDestroySemaphore(m_device->Device(), m_acquired, nullptr);  m_acquired = VK_NULL_HANDLE; }
    if (m_presented) { vkDestroySemaphore(m_device->Device(), m_presented, nullptr); m_presented = VK_NULL_HANDLE; }
    if (m_commandPool)
    {
        vkDestroyCommandPool(m_device->Device(), m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }

    DestroySwapchain();
    if (m_surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(m_device->Instance(), m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    if (m_window) { DestroyWindow(m_window); m_window = nullptr; }
    m_open   = false;
    m_device = nullptr;
}
}
