// vk_textures.h
//
// The game's textures, on the GPU, addressable from a shader by index.
//
// ── Why an array rather than one binding per draw ────────────────────────
//
// A ray tracer does not know which surface it is shading until the ray hits
// it, so there is no "current texture" to bind. Every texture the scene might
// hit has to be reachable at once, and the hit shader picks one with an index
// carried by the instance. That is what the descriptor array here is for.
//
// ── Format ───────────────────────────────────────────────────────────────
//
// Every texture this game creates is D3DFMT_A8R8G8B8 — 1768 of 1768 across
// every capture taken. That is exactly VK_FORMAT_B8G8R8A8_UNORM, so no
// conversion happens. Anything else is counted and skipped rather than
// guessed at; if a build of the game turns out to use DXT or 16-bit formats,
// the counter says so instead of the screen filling with garbage.
//
// ── Memory ───────────────────────────────────────────────────────────────
//
// One VkDeviceMemory per image. Vulkan caps live allocations
// (maxMemoryAllocationCount, ~4096 here) which is why buffers are
// sub-allocated, but a match uses a few hundred textures and the cap is
// enforced below, so images do not need the same machinery yet.
#pragma once

#include "scene_receiver.h"
#include "vk_alloc.h"
#include "vk_device.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace Host
{
    // Slot 0 is always a 1x1 white texture, so an untextured surface can use
    // the same shader path with no branch and no invalid descriptor.
    const uint32_t kWhiteTextureSlot = 0;

    struct TextureStats
    {
        uint32_t resident;          // slots in use, including white
        uint32_t uploadedThisFrame;
        uint32_t skippedFormat;     // not BGRA8
        uint32_t skippedFull;       // cache is at capacity
        uint64_t bytesResident;
    };

    class TextureCache
    {
    public:
        TextureCache();
        ~TextureCache();

        bool Init(VulkanDevice* device, GpuAllocator* allocator);
        void Shutdown();

        // Uploads textures the receiver has marked dirty, up to `budget` per
        // call. Uploads stall on a queue wait, so a frame that brings in a
        // hundred new textures would otherwise hitch badly; the rest arrive
        // over the following frames and surfaces briefly sample white.
        void Sync(SceneReceiver& scene, uint32_t budget);

        // The array index for a texture id, or kWhiteTextureSlot if it is not
        // resident (yet, or ever).
        uint32_t Slot(uint64_t textureId) const;

        // Sized to capacity, not to what is resident: a descriptor array must
        // be fully populated, so unused entries point at the white texture.
        const std::vector<VkDescriptorImageInfo>& Descriptors() const
        { return m_descriptors; }

        uint32_t Capacity() const { return (uint32_t)m_descriptors.size(); }
        const TextureStats& Stats() const { return m_stats; }
        const std::string& LastError() const { return m_lastError; }

    private:
        struct Image
        {
            VkImage        image;
            VkDeviceMemory memory;
            VkImageView    view;
            uint64_t       bytes;

            Image() : image(VK_NULL_HANDLE), memory(VK_NULL_HANDLE)
                    , view(VK_NULL_HANDLE), bytes(0) {}
        };

        bool CreateImage(uint32_t width, uint32_t height, uint32_t mips,
                         Image& out);
        bool UploadImage(const SceneIPC::TextureDesc& desc,
                         const uint8_t* pixels, Image& out);
        bool CreateWhiteTexture();
        void DestroyImage(Image& img);

        VulkanDevice* m_device;
        GpuAllocator* m_alloc;
        VkCommandPool m_commandPool;
        VkSampler     m_sampler;

        std::vector<Image>                     m_images;
        std::unordered_map<uint64_t, uint32_t> m_slotOf;
        std::vector<VkDescriptorImageInfo>     m_descriptors;

        TextureStats m_stats;
        std::string  m_lastError;
    };
}
