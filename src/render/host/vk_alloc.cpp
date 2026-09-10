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

// vk_alloc.cpp
#include "vk_alloc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Host
{
namespace
{
    // Blocks are large so that a scene of a few hundred meshes stays well
    // inside maxMemoryAllocationCount. A block still grows to fit a single
    // oversized request.
    const VkDeviceSize kBlockSize = 64ull * 1024ull * 1024ull;

    inline VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a)
    {
        return a ? ((v + a - 1) / a) * a : v;
    }
}

GpuAllocator::GpuAllocator()
    : m_device(nullptr), m_bytesReserved(0), m_bytesInUse(0)
    , m_liveAllocations(0)
{
}

GpuAllocator::~GpuAllocator()
{
    Shutdown();
}

bool GpuAllocator::Init(VulkanDevice* device)
{
    m_device = device;
    return device && device->IsValid();
}

void GpuAllocator::Shutdown()
{
    if (!m_device || !m_device->IsValid()) { m_blocks.clear(); return; }

    for (size_t i = 0; i < m_blocks.size(); ++i)
    {
        if (m_blocks[i].mapped) vkUnmapMemory(m_device->Device(), m_blocks[i].memory);
        vkFreeMemory(m_device->Device(), m_blocks[i].memory, nullptr);
    }
    m_blocks.clear();
    m_bytesReserved = m_bytesInUse = 0;
    m_liveAllocations = 0;
}

int GpuAllocator::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(m_device->PhysicalDevice(), &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props)
            return (int)i;
    }
    return -1;
}

bool GpuAllocator::AllocateFrom(uint32_t memoryTypeIndex, VkDeviceSize size,
                                VkDeviceSize alignment, bool hostVisible,
                                uint32_t& outBlock, VkDeviceSize& outOffset,
                                VkDeviceSize& outPadded)
{
    // First fit across existing blocks of the right memory type.
    for (size_t b = 0; b < m_blocks.size(); ++b)
    {
        Block& blk = m_blocks[b];
        if (blk.memoryTypeIndex != memoryTypeIndex) continue;

        for (size_t r = 0; r < blk.freeList.size(); ++r)
        {
            const VkDeviceSize alignedOffset = AlignUp(blk.freeList[r].offset, alignment);
            const VkDeviceSize padding = alignedOffset - blk.freeList[r].offset;
            if (blk.freeList[r].size < padding + size) continue;

            outBlock  = (uint32_t)b;
            outOffset = alignedOffset;
            outPadded = padding + size;

            // Consume from the front of the range; the alignment padding is
            // charged to this allocation so the range stays contiguous.
            blk.freeList[r].offset += outPadded;
            blk.freeList[r].size   -= outPadded;
            if (blk.freeList[r].size == 0)
                blk.freeList.erase(blk.freeList.begin() + r);
            return true;
        }
    }

    // Nothing fits; take a new block, growing it if the request is larger.
    const VkDeviceSize blockSize = std::max(kBlockSize, AlignUp(size + alignment, kBlockSize));

    VkMemoryAllocateFlagsInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext           = &flagsInfo;   // required for any buffer we take an address of
    ai.allocationSize  = blockSize;
    ai.memoryTypeIndex = memoryTypeIndex;

    Block blk{};
    blk.size            = blockSize;
    blk.memoryTypeIndex = memoryTypeIndex;

    if (vkAllocateMemory(m_device->Device(), &ai, nullptr, &blk.memory) != VK_SUCCESS)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "vkAllocateMemory failed for %llu bytes",
                 (unsigned long long)blockSize);
        m_lastError = buf;
        return false;
    }

    if (hostVisible)
        vkMapMemory(m_device->Device(), blk.memory, 0, blockSize, 0, &blk.mapped);

    FreeRange whole{};
    whole.offset = 0;
    whole.size   = blockSize;
    blk.freeList.push_back(whole);

    m_blocks.push_back(blk);
    m_bytesReserved += blockSize;

    return AllocateFrom(memoryTypeIndex, size, alignment, hostVisible,
                        outBlock, outOffset, outPadded);
}

bool GpuAllocator::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                bool hostVisible, GpuBuffer& out)
{
    out = GpuBuffer();
    if (size == 0) return false;

    // Anything an acceleration structure build reads is addressed by device
    // address rather than through a descriptor, so the flag is added
    // unconditionally; it costs nothing for buffers that never take one.
    usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = size;
    bi.usage       = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device->Device(), &bi, nullptr, &out.buffer) != VK_SUCCESS)
    {
        m_lastError = "vkCreateBuffer failed";
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device->Device(), out.buffer, &req);

    const VkMemoryPropertyFlags props = hostVisible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    const int typeIndex = FindMemoryType(req.memoryTypeBits, props);
    if (typeIndex < 0)
    {
        vkDestroyBuffer(m_device->Device(), out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        m_lastError = "no memory type satisfies the requested properties";
        return false;
    }

    if (!AllocateFrom((uint32_t)typeIndex, req.size, req.alignment, hostVisible,
                      out.blockIndex, out.blockOffset, out.blockSize))
    {
        vkDestroyBuffer(m_device->Device(), out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }

    const Block& blk = m_blocks[out.blockIndex];
    if (vkBindBufferMemory(m_device->Device(), out.buffer, blk.memory,
                           out.blockOffset) != VK_SUCCESS)
    {
        vkDestroyBuffer(m_device->Device(), out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        m_lastError = "vkBindBufferMemory failed";
        return false;
    }

    out.size = size;
    if (blk.mapped) out.mapped = (uint8_t*)blk.mapped + out.blockOffset;

    VkBufferDeviceAddressInfo ai{};
    ai.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    ai.buffer = out.buffer;
    out.address = vkGetBufferDeviceAddress(m_device->Device(), &ai);

    m_bytesInUse += out.blockSize;
    ++m_liveAllocations;
    return true;
}

void GpuAllocator::DestroyBuffer(GpuBuffer& buffer)
{
    if (!buffer.IsValid()) return;

    vkDestroyBuffer(m_device->Device(), buffer.buffer, nullptr);

    if (buffer.blockIndex < m_blocks.size())
    {
        Block& blk = m_blocks[buffer.blockIndex];

        FreeRange r{};
        r.offset = buffer.blockOffset;
        r.size   = buffer.blockSize;

        // Insert in order, then coalesce with either neighbour. Without this
        // the free list fragments into unusable slivers over a long session.
        auto pos = std::lower_bound(blk.freeList.begin(), blk.freeList.end(), r,
            [](const FreeRange& a, const FreeRange& b) { return a.offset < b.offset; });
        pos = blk.freeList.insert(pos, r);

        if (pos + 1 != blk.freeList.end() &&
            pos->offset + pos->size == (pos + 1)->offset)
        {
            pos->size += (pos + 1)->size;
            blk.freeList.erase(pos + 1);
        }
        if (pos != blk.freeList.begin())
        {
            auto prev = pos - 1;
            if (prev->offset + prev->size == pos->offset)
            {
                prev->size += pos->size;
                blk.freeList.erase(pos);
            }
        }

        m_bytesInUse -= buffer.blockSize;
        --m_liveAllocations;
    }

    buffer = GpuBuffer();
}

bool GpuAllocator::UploadBuffer(const GpuBuffer& dst, const void* data,
                                VkDeviceSize bytes, VkCommandPool pool)
{
    if (!dst.IsValid() || bytes == 0) return false;

    // A host-visible destination needs no staging step at all.
    if (dst.mapped)
    {
        memcpy(dst.mapped, data, (size_t)bytes);
        return true;
    }

    GpuBuffer staging;
    if (!CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging))
        return false;
    memcpy(staging.mapped, data, (size_t)bytes);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device->Device(), &cbai, &cmd);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkBufferCopy copy{};
    copy.size = bytes;
    vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &copy);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_device->GraphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_device->GraphicsQueue());

    vkFreeCommandBuffers(m_device->Device(), pool, 1, &cmd);
    DestroyBuffer(staging);
    return true;
}

} // namespace Host
