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
// The game creates 32-bit BGRA textures, which is exactly
// VK_FORMAT_B8G8R8A8_UNORM, so no conversion happens. Anything else is
// counted and skipped rather than guessed at; if a build turns out to use
// DXT or 16-bit formats, the counter says so instead of the screen filling
// with garbage.
//
// A8R8G8B8 and X8R8G8B8 are byte-identical and must still be told apart. In
// X8 the high byte is not alpha, just whatever the game left there — usually
// zero. Treated as alpha it makes every such surface fail the any-hit test
// and vanish, which is what happened to the pitch, so those are forced
// opaque on upload.
//
// An A8R8G8B8 texture whose alpha happens to be zero everywhere is *not* the
// same thing and is left alone. The game draws those - unused decal slots,
// contributing nothing - and forcing them opaque paints black patches.
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

    // Texture addressing is a property of the draw, not of the texture: the
    // pitch grass is tiled and wraps, while a projected shadow samples one
    // blob out of a mostly-empty atlas with UVs running past 5 and must
    // clamp. Wrapping that tiles the atlas over the quad.
    //
    // So images and samplers are separate descriptors and the shader pairs
    // them, rather than one combined sampler baked to a single mode. Four
    // covers every combination the game uses; the index is
    // (U clamps ? 1 : 0) | (V clamps ? 2 : 0).
    const uint32_t kSamplerCount = 4;

    // Maps a packed D3DTSS_ADDRESSU/V pair, as InstanceDesc carries it, to
    // one of those. Zero - the producer not having reported it - means the
    // Direct3D default, which is wrap.
    // Whether a blended draw takes coverage from the vertex colour as
    // well as the texture. Reads InstanceDesc::stageState, whose alpha op
    // is the only thing that actually knows.
    bool VertexAlphaContributes(uint32_t packedStage);

    uint32_t SamplerIndexForAddress(uint32_t packedAddress);

    // Decodes one mip of one texture into B8G8R8A8, and how many source
    // bytes that level occupies.
    //
    // Exposed for the self-test. The decoders are pure functions with
    // exactly known answers, and no recording made so far contains a
    // single texture in any format but B8G8R8A8 - so replaying one cannot
    // exercise them at all, and only a test with hand-written blocks can.
    bool DecodeTextureMip(uint32_t format, const uint8_t* src, size_t srcBytes,
                          uint32_t width, uint32_t height, uint8_t* dst);
    uint32_t TextureSourceMipBytes(uint32_t format, uint32_t width,
                                   uint32_t height, uint32_t level);

    struct TextureStats
    {
        uint32_t resident;          // slots in use, including white
        uint32_t uploadedThisFrame;
        // A texture that never becomes resident is a surface that renders
        // white, and every reason for it looks identical on screen. So each
        // one is counted separately and reported even when zero: silence
        // here is what made the live pitch's missing texture take a screen
        // recording to notice.
        uint32_t skippedFormat;     // nothing here can decode it
        uint32_t skippedFull;       // full, and nothing older to reclaim
        uint32_t slotsReclaimed;    // taken from the least recently used
        uint32_t opaqueAlpha;       // alpha is 255 everywhere: no coverage
        uint32_t opaqueForced;      // X8R8G8B8: alpha byte is not alpha
        uint32_t fullyTransparent;  // alpha zero everywhere; drawn as nothing
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

        // Whether this texture's alpha channel is 255 everywhere, so it
        // says nothing about coverage. Reported, not acted on: which of the
        // texture and the vertex colour supplies alpha is the draw's alpha
        // op, and InstanceDesc::stageState carries it.
        bool TextureCarriesAlpha(uint64_t textureId) const;

        // Sized to capacity, not to what is resident: a descriptor array must
        // be fully populated, so unused entries point at the white texture.
        const std::vector<VkDescriptorImageInfo>& Descriptors() const
        { return m_descriptors; }

        uint32_t Capacity() const { return (uint32_t)m_descriptors.size(); }

        // Sampler descriptors, indexed as SamplerIndexForAddress returns.
        const std::vector<VkDescriptorImageInfo>& Samplers() const
        { return m_samplerInfos; }
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
                         const uint8_t* pixels, size_t pixelBytes, Image& out);
        bool CreateWhiteTexture();
        void DestroyImage(Image& img);

        VulkanDevice* m_device;
        GpuAllocator* m_alloc;
        VkCommandPool m_commandPool;
        VkSampler     m_samplers[kSamplerCount];
        std::vector<VkDescriptorImageInfo> m_samplerInfos;

        std::vector<Image>                     m_images;
        std::unordered_map<uint64_t, uint32_t> m_slotOf;

        // Which texture each slot holds, and when it was last asked for.
        //
        // Slots used to be handed out and never taken back, which was fine
        // while a session sent three hundred textures. It stopped being fine
        // when the producer started giving a recreated texture a new id
        // rather than a dead one's: a session now sends around fifteen
        // hundred, and the two thousand slots would run out partway through
        // a third match. What that looks like on screen is surfaces sampling
        // white - which is indistinguishable from the bug that made the
        // producer send them in the first place.
        std::vector<uint64_t>                  m_slotTexture;
        mutable std::vector<uint64_t>          m_slotLastUsed;
        uint64_t                               m_useClock;

        // The clock value for the frame being assembled, so a slot touched
        // this frame is never chosen as a victim.
        uint64_t                               m_frameClock;

        // Texture ids whose alpha channel is 255 everywhere.
        std::unordered_map<uint64_t, bool> m_carriesAlpha;

        // Set by the last UploadImage, read by Sync once it succeeds.
        bool m_lastUploadCarriesAlpha;
        std::vector<VkDescriptorImageInfo>     m_descriptors;

        TextureStats m_stats;
        std::string  m_lastError;
    };
}
