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

// vk_alloc.h
//
// A small sub-allocating GPU memory allocator.
//
// Vulkan caps the number of live VkDeviceMemory objects
// (maxMemoryAllocationCount, commonly 4096), and this renderer needs several
// buffers per mesh — vertices, indices, acceleration structure storage,
// scratch. One allocation per buffer would exhaust that budget on a scene of
// a few hundred meshes, so memory is taken in large blocks and sub-allocated.
//
// The free list is first-fit with coalescing, which is more than adequate
// here: allocations are long-lived (a mesh's buffers live as long as the mesh)
// and the per-frame churn goes through a separate arena that is reset wholesale
// rather than freed piecemeal.
#pragma once

#include "vk_device.h"

#include <vector>

namespace Host
{
    struct GpuBuffer
    {
        VkBuffer        buffer;
        VkDeviceAddress address;    // 0 unless created with SHADER_DEVICE_ADDRESS
        VkDeviceSize    size;
        void*           mapped;     // non-null only for host-visible memory

        // Where this came from, so it can be returned.
        uint32_t        blockIndex;
        VkDeviceSize    blockOffset;
        VkDeviceSize    blockSize;  // includes alignment padding

        GpuBuffer()
            : buffer(VK_NULL_HANDLE), address(0), size(0), mapped(nullptr)
            , blockIndex(UINT32_MAX), blockOffset(0), blockSize(0) {}

        bool IsValid() const { return buffer != VK_NULL_HANDLE; }
    };

    class GpuAllocator
    {
    public:
        GpuAllocator();
        ~GpuAllocator();

        bool Init(VulkanDevice* device);
        void Shutdown();

        // `hostVisible` buffers are persistently mapped; use them for staging
        // and for data rewritten every frame. Device-local buffers are the
        // default for anything the GPU reads repeatedly.
        bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          bool hostVisible, GpuBuffer& out);
        void DestroyBuffer(GpuBuffer& buffer);

        // Copies through a temporary host-visible staging buffer. Submits and
        // waits, so it is for setup rather than per-frame traffic.
        bool UploadBuffer(const GpuBuffer& dst, const void* data,
                          VkDeviceSize bytes, VkCommandPool pool);

        uint64_t BytesReserved()  const { return m_bytesReserved; }
        uint64_t BytesInUse()     const { return m_bytesInUse; }
        uint32_t BlockCount()     const { return (uint32_t)m_blocks.size(); }
        uint32_t LiveAllocations() const { return m_liveAllocations; }

        // Public because images allocate their own memory - the block
        // sub-allocator here is for buffers - and still need this lookup.
        // Returns -1 if no memory type satisfies `props`.
        int FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

        const std::string& LastError() const { return m_lastError; }

    private:
        struct FreeRange { VkDeviceSize offset, size; };

        struct Block
        {
            VkDeviceMemory         memory;
            VkDeviceSize           size;
            uint32_t               memoryTypeIndex;
            void*                  mapped;
            std::vector<FreeRange> freeList;
        };


        bool AllocateFrom(uint32_t memoryTypeIndex, VkDeviceSize size,
                          VkDeviceSize alignment, bool hostVisible,
                          uint32_t& outBlock, VkDeviceSize& outOffset,
                          VkDeviceSize& outPadded);

        VulkanDevice*      m_device;
        std::vector<Block> m_blocks;
        uint64_t           m_bytesReserved;
        uint64_t           m_bytesInUse;
        uint32_t           m_liveAllocations;
        std::string        m_lastError;
    };
}
