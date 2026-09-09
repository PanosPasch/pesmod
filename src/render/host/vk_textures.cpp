// vk_textures.cpp
#include "vk_textures.h"

#include <cstring>

namespace Host
{
namespace
{
    // Enough for a match with room to spare: the busiest capture held 361
    // live textures. The array is fully populated with the white texture, so
    // this is a real memory cost of one descriptor each, not of one image.
    const uint32_t kCapacity = 2048;

    uint32_t MipBytes(uint32_t width, uint32_t height, uint32_t level)
    {
        const uint32_t w = width  >> level ? width  >> level : 1u;
        const uint32_t h = height >> level ? height >> level : 1u;
        return w * h * 4u;
    }
}

TextureCache::TextureCache()
    : m_device(nullptr), m_alloc(nullptr), m_commandPool(VK_NULL_HANDLE)
    , m_sampler(VK_NULL_HANDLE)
{
    memset(&m_stats, 0, sizeof(m_stats));
}

TextureCache::~TextureCache() { Shutdown(); }

bool TextureCache::Init(VulkanDevice* device, GpuAllocator* allocator)
{
    m_device = device;
    m_alloc  = allocator;

    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                           VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_device->GraphicsQueueFamily();
    if (vkCreateCommandPool(m_device->Device(), &pci, nullptr,
                            &m_commandPool) != VK_SUCCESS)
    {
        m_lastError = "texture command pool creation failed";
        return false;
    }

    // Anisotropy matters here more than usual: the pitch is a single plane
    // seen at a grazing angle across most of the frame.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_device->PhysicalDevice(), &props);

    VkSamplerCreateInfo si{};
    si.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter        = VK_FILTER_LINEAR;
    si.minFilter        = VK_FILTER_LINEAR;
    si.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.anisotropyEnable = VK_TRUE;
    si.maxAnisotropy    = props.limits.maxSamplerAnisotropy;
    si.maxLod           = VK_LOD_CLAMP_NONE;
    si.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (vkCreateSampler(m_device->Device(), &si, nullptr, &m_sampler) != VK_SUCCESS)
    {
        m_lastError = "sampler creation failed";
        return false;
    }

    m_images.resize(kCapacity);
    m_descriptors.resize(kCapacity);

    if (!CreateWhiteTexture()) return false;

    // Every slot starts pointing at white, so the array is valid before a
    // single game texture has arrived.
    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        m_descriptors[i].sampler     = m_sampler;
        m_descriptors[i].imageView   = m_images[kWhiteTextureSlot].view;
        m_descriptors[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    m_stats.resident = 1;
    return true;
}

bool TextureCache::CreateWhiteTexture()
{
    SceneIPC::TextureDesc desc{};
    desc.format    = SceneIPC::kTexBGRA8;
    desc.width     = 1;
    desc.height    = 1;
    desc.mipLevels = 1;

    const uint8_t white[4] = { 255, 255, 255, 255 };
    if (!UploadImage(desc, white, m_images[kWhiteTextureSlot]))
    {
        m_lastError = "white texture upload failed: " + m_lastError;
        return false;
    }
    return true;
}

bool TextureCache::CreateImage(uint32_t width, uint32_t height, uint32_t mips,
                               Image& out)
{
    VkImageCreateInfo ii{};
    ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.format        = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent        = { width, height, 1 };
    ii.mipLevels     = mips;
    ii.arrayLayers   = 1;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device->Device(), &ii, nullptr, &out.image) != VK_SUCCESS)
    {
        m_lastError = "vkCreateImage failed";
        return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device->Device(), out.image, &req);

    const int typeIndex = m_alloc->FindMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = (uint32_t)(typeIndex < 0 ? 0 : typeIndex);
    if (typeIndex < 0 ||
        vkAllocateMemory(m_device->Device(), &ai, nullptr, &out.memory) != VK_SUCCESS)
    {
        m_lastError = "image memory allocation failed";
        vkDestroyImage(m_device->Device(), out.image, nullptr);
        out.image = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(m_device->Device(), out.image, out.memory, 0);
    out.bytes = req.size;

    VkImageViewCreateInfo vi{};
    vi.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image            = out.image;
    vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vi.format           = VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
    if (vkCreateImageView(m_device->Device(), &vi, nullptr, &out.view) != VK_SUCCESS)
    {
        m_lastError = "vkCreateImageView failed";
        return false;
    }
    return true;
}

bool TextureCache::UploadImage(const SceneIPC::TextureDesc& desc,
                               const uint8_t* pixels, Image& out)
{
    const uint32_t mips = desc.mipLevels ? desc.mipLevels : 1u;

    uint64_t total = 0;
    for (uint32_t m = 0; m < mips; ++m) total += MipBytes(desc.width, desc.height, m);

    if (!CreateImage(desc.width, desc.height, mips, out)) return false;

    GpuBuffer staging;
    if (!m_alloc->CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging))
    {
        m_lastError = "texture staging buffer allocation failed";
        DestroyImage(out);
        return false;
    }
    memcpy(staging.mapped, pixels, (size_t)total);

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

    VkImageMemoryBarrier toDst{};
    toDst.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image            = out.image;
    toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
    toDst.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toDst);

    // The producer packs mip levels tightly, largest first.
    std::vector<VkBufferImageCopy> copies(mips);
    uint64_t offset = 0;
    for (uint32_t m = 0; m < mips; ++m)
    {
        const uint32_t w = desc.width  >> m ? desc.width  >> m : 1u;
        const uint32_t h = desc.height >> m ? desc.height >> m : 1u;

        copies[m] = VkBufferImageCopy{};
        copies[m].bufferOffset      = offset;
        copies[m].imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 };
        copies[m].imageExtent       = { w, h, 1 };
        offset += MipBytes(desc.width, desc.height, m);
    }
    vkCmdCopyBufferToImage(cmd, staging.buffer, out.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)copies.size(), copies.data());

    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_device->GraphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_device->GraphicsQueue());

    vkFreeCommandBuffers(m_device->Device(), m_commandPool, 1, &cmd);
    m_alloc->DestroyBuffer(staging);
    return true;
}

void TextureCache::Sync(SceneReceiver& scene, uint32_t budget)
{
    m_stats.uploadedThisFrame = 0;
    if (!budget) return;

    std::vector<uint64_t> dirty;
    scene.CollectDirtyTextures(dirty, budget);

    for (size_t i = 0; i < dirty.size(); ++i)
    {
        const Texture* tex = scene.FindTexture(dirty[i]);
        if (!tex) continue;

        if (tex->desc.format != SceneIPC::kTexBGRA8)
        {
            // Counted, not guessed at. Every texture in every capture of this
            // game is A8R8G8B8; anything else means a build that differs and
            // should be looked at rather than silently mis-decoded.
            ++m_stats.skippedFormat;
            scene.MarkTextureClean(dirty[i]);
            continue;
        }

        uint32_t slot;
        auto existing = m_slotOf.find(dirty[i]);
        if (existing != m_slotOf.end())
        {
            slot = existing->second;
            DestroyImage(m_images[slot]);       // re-upload: the game changed it
        }
        else
        {
            if (m_stats.resident >= Capacity())
            {
                ++m_stats.skippedFull;
                scene.MarkTextureClean(dirty[i]);
                continue;
            }
            slot = m_stats.resident++;
            m_slotOf[dirty[i]] = slot;
        }

        if (!UploadImage(tex->desc, tex->pixels.data(), m_images[slot]))
        {
            // Left dirty deliberately, so a transient failure is retried.
            m_slotOf.erase(dirty[i]);
            if (slot + 1 == m_stats.resident) --m_stats.resident;
            continue;
        }

        m_descriptors[slot].sampler     = m_sampler;
        m_descriptors[slot].imageView   = m_images[slot].view;
        m_descriptors[slot].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m_stats.bytesResident += m_images[slot].bytes;
        ++m_stats.uploadedThisFrame;
        scene.MarkTextureClean(dirty[i]);
    }
}

uint32_t TextureCache::Slot(uint64_t textureId) const
{
    auto it = m_slotOf.find(textureId);
    return (it == m_slotOf.end()) ? kWhiteTextureSlot : it->second;
}

void TextureCache::DestroyImage(Image& img)
{
    if (img.view)   vkDestroyImageView(m_device->Device(), img.view, nullptr);
    if (img.image)  vkDestroyImage(m_device->Device(), img.image, nullptr);
    if (img.memory) vkFreeMemory(m_device->Device(), img.memory, nullptr);
    if (m_stats.bytesResident >= img.bytes) m_stats.bytesResident -= img.bytes;
    img = Image();
}

void TextureCache::Shutdown()
{
    if (!m_device || !m_device->IsValid()) return;
    vkDeviceWaitIdle(m_device->Device());

    for (size_t i = 0; i < m_images.size(); ++i) DestroyImage(m_images[i]);
    m_images.clear();
    m_descriptors.clear();
    m_slotOf.clear();

    if (m_sampler)
    {
        vkDestroySampler(m_device->Device(), m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_commandPool)
    {
        vkDestroyCommandPool(m_device->Device(), m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
    m_device = nullptr;
}
}
