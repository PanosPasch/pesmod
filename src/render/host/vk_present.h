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

// vk_present.h
//
// A window, a swapchain, and getting the traced image onto the screen.
//
// ── Why the host owns the window ──────────────────────────────────────────
//
// Everything up to here has been verified by writing PNGs, which is the right
// way to test a renderer but not a way to use one. The ray traced image is
// the output now, so it is the one that should be on screen; the game's own
// window keeps running its Direct3D 8 path and becomes, in effect, the input
// device.
//
// ── Why a blit rather than tracing into the swapchain ─────────────────────
//
// The tracer writes a storage image at its own resolution, chosen to keep the
// game's aspect ratio rather than the window's. Blitting decouples the two:
// the window can be resized, or be a different shape, without changing what
// is traced or reintroducing the stretch that fixing the aspect ratio solved.
#pragma once

#include "vk_device.h"

// NOMINMAX before windows.h: it defines min/max as macros, which breaks
// std::max wherever this header is pulled in transitively.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>

namespace Host
{
    class Presenter
    {
    public:
        Presenter();
        ~Presenter();

        bool Init(VulkanDevice* device, uint32_t width, uint32_t height,
                  const char* title);
        void Shutdown();

        // False once the user has closed the window, which is how the host
        // knows to stop.
        bool IsOpen() const { return m_open; }

        // Drains the window's message queue. Must be called every frame or
        // the window stops responding, which Windows reports to the user as
        // the application having hung.
        void PumpMessages();

        // Copies `source` onto the next swapchain image and presents it.
        // `source` is expected in VK_IMAGE_LAYOUT_GENERAL and is left that
        // way, since that is what the tracer wants it in.
        bool Present(VkImage source, uint32_t srcWidth, uint32_t srcHeight);

        const std::string& LastError() const { return m_lastError; }

    private:
        bool CreateWindowAndSurface(uint32_t width, uint32_t height,
                                    const char* title);
        bool CreateSwapchain(uint32_t width, uint32_t height);
        void DestroySwapchain();

        static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

        VulkanDevice* m_device;
        HWND          m_window;
        HINSTANCE     m_instanceHandle;
        bool          m_open;

        VkSurfaceKHR   m_surface;
        VkSwapchainKHR m_swapchain;
        VkFormat       m_format;
        VkExtent2D     m_extent;
        std::vector<VkImage> m_images;

        VkCommandPool   m_commandPool;
        VkCommandBuffer m_commandBuffer;
        VkSemaphore     m_acquired;
        VkSemaphore     m_presented;
        VkFence         m_inFlight;

        // Set when the swapchain no longer matches the window, so the next
        // frame rebuilds it rather than presenting a stretched or failed one.
        bool m_needsRebuild;

        std::string m_lastError;
    };
}
