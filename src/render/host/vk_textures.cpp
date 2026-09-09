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

    // The size of one mip of the image the cache keeps, which is always
    // B8G8R8A8 whatever the game sent.
    uint32_t MipBytes(uint32_t width, uint32_t height, uint32_t level)
    {
        const uint32_t w = width  >> level ? width  >> level : 1u;
        const uint32_t h = height >> level ? height >> level : 1u;
        return w * h * 4u;
    }

    // ── Source layouts ───────────────────────────────────────────────────
    //
    // The image the cache keeps is always B8G8R8A8, so everything the game
    // can hand over is decoded into that on arrival. That keeps one Vulkan
    // format, one alpha analysis and one shader path, at the cost of memory
    // for the compressed formats - which is the right trade at 2048 slots on
    // a card with 16 GB.
    //
    // Before this, anything that was not already B8G8R8A8 or B8G8R8X8 was
    // counted and dropped, and the surfaces using it sampled the white slot.
    // That is eight of the ten formats the producer can send.
    uint32_t SourceMipBytes(uint32_t format, uint32_t width, uint32_t height,
                            uint32_t level)
    {
        const uint32_t w = width  >> level ? width  >> level : 1u;
        const uint32_t h = height >> level ? height >> level : 1u;

        switch (format)
        {
        case SceneIPC::kTexDXT1:
            return ((w + 3u) / 4u) * ((h + 3u) / 4u) * 8u;
        case SceneIPC::kTexDXT3:
        case SceneIPC::kTexDXT5:
            return ((w + 3u) / 4u) * ((h + 3u) / 4u) * 16u;
        case SceneIPC::kTexBGR565:
        case SceneIPC::kTexBGRA5551:
        case SceneIPC::kTexBGRA4444:
        case SceneIPC::kTexA8L8:
            return w * h * 2u;
        case SceneIPC::kTexL8:
            return w * h;
        default:
            return w * h * 4u;
        }
    }

    // 5, 6 and 4 bit channels to 8, by bit replication rather than a
    // multiply-and-shift: it maps the full range exactly onto 0..255, so
    // white stays white.
    inline uint8_t Expand5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
    inline uint8_t Expand6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
    inline uint8_t Expand4(uint32_t v) { return (uint8_t)((v << 4) | v); }

    struct Bgra { uint8_t b, g, r, a; };

    inline Bgra From565(uint16_t v)
    {
        Bgra c;
        c.b = Expand5(v & 0x1Fu);
        c.g = Expand6((v >> 5) & 0x3Fu);
        c.r = Expand5((v >> 11) & 0x1Fu);
        c.a = 255;
        return c;
    }

    // One 4x4 block of BC1 colour. `opaqueOnly` is set by BC2 and BC3, whose
    // alpha lives in their own half of the block: their colour endpoints are
    // always in four-colour mode regardless of which is numerically larger,
    // and reading them the BC1 way turns a third of the texels transparent.
    void DecodeBc1Colour(const uint8_t* src, bool opaqueOnly, Bgra out[16])
    {
        const uint16_t c0 = (uint16_t)(src[0] | (src[1] << 8));
        const uint16_t c1 = (uint16_t)(src[2] | (src[3] << 8));

        Bgra p[4];
        p[0] = From565(c0);
        p[1] = From565(c1);

        if (c0 > c1 || opaqueOnly)
        {
            p[2].b = (uint8_t)((2 * p[0].b + p[1].b) / 3);
            p[2].g = (uint8_t)((2 * p[0].g + p[1].g) / 3);
            p[2].r = (uint8_t)((2 * p[0].r + p[1].r) / 3);
            p[2].a = 255;
            p[3].b = (uint8_t)((p[0].b + 2 * p[1].b) / 3);
            p[3].g = (uint8_t)((p[0].g + 2 * p[1].g) / 3);
            p[3].r = (uint8_t)((p[0].r + 2 * p[1].r) / 3);
            p[3].a = 255;
        }
        else
        {
            // Three colours and a transparent slot. This is how DXT1 carries
            // one bit of alpha, and it is the only reason the mode bit exists.
            p[2].b = (uint8_t)((p[0].b + p[1].b) / 2);
            p[2].g = (uint8_t)((p[0].g + p[1].g) / 2);
            p[2].r = (uint8_t)((p[0].r + p[1].r) / 2);
            p[2].a = 255;
            p[3].b = p[3].g = p[3].r = 0;
            p[3].a = 0;
        }

        const uint32_t bits = (uint32_t)src[4] | ((uint32_t)src[5] << 8) |
                              ((uint32_t)src[6] << 16) | ((uint32_t)src[7] << 24);
        for (uint32_t i = 0; i < 16; ++i)
            out[i] = p[(bits >> (i * 2)) & 3u];
    }

    // BC3's alpha half: two endpoints and sixteen three-bit indices.
    void DecodeBc3Alpha(const uint8_t* src, uint8_t out[16])
    {
        const uint8_t a0 = src[0], a1 = src[1];

        uint8_t a[8];
        a[0] = a0;
        a[1] = a1;
        if (a0 > a1)
        {
            for (uint32_t i = 1; i < 7; ++i)
                a[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
        }
        else
        {
            for (uint32_t i = 1; i < 5; ++i)
                a[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
            a[6] = 0;
            a[7] = 255;
        }

        // Six bytes of packed 3-bit indices, little endian, read as two
        // 24-bit halves so the shifts stay inside 32 bits.
        for (uint32_t half = 0; half < 2; ++half)
        {
            const uint8_t* p = src + 2 + half * 3;
            const uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                  ((uint32_t)p[2] << 16);
            for (uint32_t i = 0; i < 8; ++i)
                out[half * 8 + i] = a[(bits >> (i * 3)) & 7u];
        }
    }

    // Decodes one mip level into `dst`, which holds w*h B8G8R8A8 texels.
    // False means the payload was too short for what it claimed to be, which
    // is a malformed message rather than an unsupported format.
    bool DecodeMip(uint32_t format, const uint8_t* src, size_t srcBytes,
                   uint32_t w, uint32_t h, uint8_t* dst)
    {
        const size_t need = SourceMipBytes(format, w, h, 0);
        if (srcBytes < need) return false;

        switch (format)
        {
        case SceneIPC::kTexBGRA8:
        case SceneIPC::kTexBGRX8:
            memcpy(dst, src, (size_t)w * h * 4);
            return true;

        case SceneIPC::kTexL8:
            for (uint32_t i = 0; i < w * h; ++i)
            {
                const uint8_t l = src[i];
                dst[i * 4 + 0] = l; dst[i * 4 + 1] = l;
                dst[i * 4 + 2] = l; dst[i * 4 + 3] = 255;
            }
            return true;

        case SceneIPC::kTexA8L8:
            // Luminance in the low byte, alpha in the high one.
            for (uint32_t i = 0; i < w * h; ++i)
            {
                const uint8_t l = src[i * 2 + 0];
                dst[i * 4 + 0] = l; dst[i * 4 + 1] = l;
                dst[i * 4 + 2] = l; dst[i * 4 + 3] = src[i * 2 + 1];
            }
            return true;

        case SceneIPC::kTexBGR565:
            for (uint32_t i = 0; i < w * h; ++i)
            {
                const Bgra c = From565((uint16_t)(src[i*2] | (src[i*2+1] << 8)));
                dst[i*4+0] = c.b; dst[i*4+1] = c.g;
                dst[i*4+2] = c.r; dst[i*4+3] = 255;
            }
            return true;

        case SceneIPC::kTexBGRA5551:
            for (uint32_t i = 0; i < w * h; ++i)
            {
                const uint16_t v = (uint16_t)(src[i*2] | (src[i*2+1] << 8));
                dst[i*4+0] = Expand5(v & 0x1Fu);
                dst[i*4+1] = Expand5((v >> 5) & 0x1Fu);
                dst[i*4+2] = Expand5((v >> 10) & 0x1Fu);
                // X1R5G5B5 arrives here too, and its top bit is undefined
                // rather than zero - but the producer only maps A1R5G5B5 and
                // X1R5G5B5 to this format together, so treating the bit as
                // alpha is the best available reading. A texture the game
                // drew opaque with the bit clear would vanish, which is why
                // an all-zero alpha channel is reported rather than assumed.
                dst[i*4+3] = (v & 0x8000u) ? 255 : 0;
            }
            return true;

        case SceneIPC::kTexBGRA4444:
            for (uint32_t i = 0; i < w * h; ++i)
            {
                const uint16_t v = (uint16_t)(src[i*2] | (src[i*2+1] << 8));
                dst[i*4+0] = Expand4(v & 0x0Fu);
                dst[i*4+1] = Expand4((v >> 4) & 0x0Fu);
                dst[i*4+2] = Expand4((v >> 8) & 0x0Fu);
                dst[i*4+3] = Expand4((v >> 12) & 0x0Fu);
            }
            return true;

        case SceneIPC::kTexDXT1:
        case SceneIPC::kTexDXT3:
        case SceneIPC::kTexDXT5:
        {
            const bool dxt1 = format == SceneIPC::kTexDXT1;
            const uint32_t blockBytes = dxt1 ? 8u : 16u;
            const uint32_t bw = (w + 3u) / 4u;
            const uint32_t bh = (h + 3u) / 4u;

            for (uint32_t by = 0; by < bh; ++by)
                for (uint32_t bx = 0; bx < bw; ++bx)
                {
                    const uint8_t* block = src +
                        ((size_t)by * bw + bx) * blockBytes;

                    Bgra texels[16];
                    uint8_t alpha[16];
                    if (dxt1)
                    {
                        DecodeBc1Colour(block, false, texels);
                        for (uint32_t i = 0; i < 16; ++i) alpha[i] = texels[i].a;
                    }
                    else if (format == SceneIPC::kTexDXT3)
                    {
                        // Sixteen four-bit alphas, then the colour block.
                        for (uint32_t i = 0; i < 16; ++i)
                        {
                            const uint8_t byteVal = block[i >> 1];
                            const uint32_t nibble = (i & 1) ? (byteVal >> 4)
                                                            : (byteVal & 0x0Fu);
                            alpha[i] = Expand4(nibble);
                        }
                        DecodeBc1Colour(block + 8, true, texels);
                    }
                    else
                    {
                        DecodeBc3Alpha(block, alpha);
                        DecodeBc1Colour(block + 8, true, texels);
                    }

                    // A block at the right or bottom edge of a texture whose
                    // size is not a multiple of four hangs over it; those
                    // texels are decoded and discarded.
                    for (uint32_t ty = 0; ty < 4; ++ty)
                    {
                        const uint32_t y = by * 4 + ty;
                        if (y >= h) break;
                        for (uint32_t tx = 0; tx < 4; ++tx)
                        {
                            const uint32_t x = bx * 4 + tx;
                            if (x >= w) break;
                            const uint32_t i = ty * 4 + tx;
                            uint8_t* out = dst + ((size_t)y * w + x) * 4;
                            out[0] = texels[i].b;
                            out[1] = texels[i].g;
                            out[2] = texels[i].r;
                            out[3] = alpha[i];
                        }
                    }
                }
            return true;
        }

        default:
            return false;
        }
    }
}

// Whether a blended draw's coverage includes the vertex colour's alpha.
//
// Direct3D 8 defaults D3DTSS_ALPHAOP to SELECTARG1 with ALPHAARG1 on the
// texture, so alpha comes from the texture alone unless a draw says
// otherwise. This game mostly leaves it there - which is why folding the
// vertex alpha into everything lightened the pitch away from what the game
// draws - and sets a modulate where it wants the two multiplied.
//
// An unreported stage state is all zeroes, which reads as the default and
// leaves every recording captured before this was sent behaving exactly as
// it did.
bool VertexAlphaContributes(uint32_t packedStage)
{
    const uint32_t op   = (packedStage >> 16) & 0xFFu;
    const uint32_t arg1 = (packedStage >> 24) & 0x0Fu;   // selector bits only

    switch (op)
    {
    case 0:                    // not reported
    case 1:                    // D3DTOP_DISABLE
        return false;

    // Selecting an argument: the vertex colour contributes only if that
    // argument is the diffuse colour. At stage 0, CURRENT is the diffuse.
    case 2:                    // D3DTOP_SELECTARG1
        return arg1 == 0 || arg1 == 1;      // D3DTA_DIFFUSE, D3DTA_CURRENT

    // Everything that combines two arguments takes both, and for stage 0
    // the second is the diffuse colour.
    case 3:                    // D3DTOP_MODULATE
    case 4:                    // D3DTOP_MODULATE2X
    case 5:                    // D3DTOP_MODULATE4X
    case 6:                    // D3DTOP_ADD
    case 7:                    // D3DTOP_ADDSIGNED
    case 8:                    // D3DTOP_ADDSIGNED2X
    case 9:                    // D3DTOP_SUBTRACT
    case 10:                   // D3DTOP_ADDSMOOTH
        return true;

    default:
        // The blend and bump ops. Treated as not contributing rather than
        // guessed at: this game does not use them for alpha, and a wrong
        // guess here is what the whole field exists to avoid.
        return false;
    }
}

// D3DTADDRESS_CLAMP is 3; BORDER and MIRRORONCE are treated as clamp, which
// is closer than wrapping and neither appears in this game.
uint32_t SamplerIndexForAddress(uint32_t packedAddress)
{
    const uint32_t u = packedAddress & 0xFFu;
    const uint32_t v = (packedAddress >> 8) & 0xFFu;
    const uint32_t uClamp = (u >= 3u) ? 1u : 0u;
    const uint32_t vClamp = (v >= 3u) ? 1u : 0u;
    return uClamp | (vClamp << 1);
}

TextureCache::TextureCache()
    : m_device(nullptr), m_alloc(nullptr), m_commandPool(VK_NULL_HANDLE)
    , m_lastUploadCarriesAlpha(false)
{
    for (uint32_t i = 0; i < kSamplerCount; ++i) m_samplers[i] = VK_NULL_HANDLE;
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

    // One sampler per addressing combination; the shader picks by index.
    m_samplerInfos.resize(kSamplerCount);
    for (uint32_t i = 0; i < kSamplerCount; ++i)
    {
        const VkSamplerAddressMode u = (i & 1u)
            ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        const VkSamplerAddressMode v = (i & 2u)
            ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;

        VkSamplerCreateInfo si{};
        si.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter        = VK_FILTER_LINEAR;
        si.minFilter        = VK_FILTER_LINEAR;
        si.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU     = u;
        si.addressModeV     = v;
        si.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy    = props.limits.maxSamplerAnisotropy;
        si.maxLod           = VK_LOD_CLAMP_NONE;
        si.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

        if (vkCreateSampler(m_device->Device(), &si, nullptr,
                            &m_samplers[i]) != VK_SUCCESS)
        {
            m_lastError = "sampler creation failed";
            return false;
        }
        m_samplerInfos[i] = VkDescriptorImageInfo{};
        m_samplerInfos[i].sampler = m_samplers[i];
    }

    m_images.resize(kCapacity);
    m_descriptors.resize(kCapacity);

    if (!CreateWhiteTexture()) return false;

    // Every slot starts pointing at white, so the array is valid before a
    // single game texture has arrived.
    // Images only: the sampler is a separate descriptor now, so the same
    // texture can be sampled wrapped or clamped without duplicating it.
    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        m_descriptors[i].sampler     = VK_NULL_HANDLE;
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
    if (!UploadImage(desc, white, sizeof(white), m_images[kWhiteTextureSlot]))
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

// Thin wrappers so the self-test can reach the decoders, which are
// otherwise private to this file.
bool DecodeTextureMip(uint32_t format, const uint8_t* src, size_t srcBytes,
                      uint32_t width, uint32_t height, uint8_t* dst)
{
    return DecodeMip(format, src, srcBytes, width, height, dst);
}

uint32_t TextureSourceMipBytes(uint32_t format, uint32_t width,
                               uint32_t height, uint32_t level)
{
    return SourceMipBytes(format, width, height, level);
}

bool TextureCache::UploadImage(const SceneIPC::TextureDesc& desc,
                               const uint8_t* pixels, size_t pixelBytes,
                               Image& out)
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

    // Decoded mip by mip, because the source layout and the destination
    // layout only agree for the two 32-bit formats. The producer packs its
    // levels tightly, largest first, in the source layout.
    {
        const uint8_t* src = pixels;
        size_t remaining = pixelBytes;
        uint8_t* dst = (uint8_t*)staging.mapped;

        for (uint32_t m = 0; m < mips; ++m)
        {
            const uint32_t w = desc.width  >> m ? desc.width  >> m : 1u;
            const uint32_t h = desc.height >> m ? desc.height >> m : 1u;
            const size_t srcBytes = SourceMipBytes(desc.format,
                                                   desc.width, desc.height, m);

            if (!DecodeMip(desc.format, src, remaining, w, h, dst))
            {
                // Either a format nothing here understands or a payload
                // shorter than its own description. Both are worth saying
                // out loud rather than rendering as white.
                m_lastError = "texture payload could not be decoded";
                m_alloc->DestroyBuffer(staging);
                DestroyImage(out);
                return false;
            }
            src += srcBytes;
            remaining -= srcBytes;
            dst += MipBytes(desc.width, desc.height, m);
        }
    }

    // X8R8G8B8 carries no alpha - the high byte is whatever the game happened
    // to leave there, usually zero. Uploaded as-is, every such surface fails
    // the any-hit alpha test and disappears; the pitch went first.
    //
    // The decoders above already write 255 for every format that genuinely
    // has no alpha channel, so this is only about the one format that has a
    // byte there and no meaning in it.
    if (desc.format == SceneIPC::kTexBGRX8)
    {
        uint8_t* p = (uint8_t*)staging.mapped;
        for (uint64_t b = 3; b < total; b += 4) p[b] = 255;
        ++m_stats.opaqueForced;
    }
    else
    {
        // An A8R8G8B8 texture whose alpha is zero everywhere is left alone:
        // it means what it says.
        //
        // This was briefly forced opaque, on the argument that the game
        // would not draw a surface that is invisible so an empty alpha
        // channel must mean alpha the texture does not have. That argument
        // was right about X8R8G8B8, which genuinely has no alpha and now has
        // its own wire format, and wrong here. The game does draw these:
        // they are unused decal slots - a chest number a player does not
        // have - blended in and contributing nothing. Forced opaque they
        // became solid black patches on players' chests.
        //
        // Counted, because a texture that can never be seen is still worth
        // knowing about.
        const uint8_t* p = (const uint8_t*)staging.mapped;
        bool anyAlpha = false;
        for (uint64_t b = 3; b < total && !anyAlpha; b += 4) anyAlpha = (p[b] != 0);
        if (!anyAlpha && total) ++m_stats.fullyTransparent;

        // And the other end of the same question: a texture that is opaque
        // everywhere carries no coverage, so a blended draw using it must be
        // getting its alpha from somewhere else.
        bool anyPartial = false;
        for (uint64_t b = 3; b < total && !anyPartial; b += 4)
            anyPartial = (p[b] != 255);
        m_lastUploadCarriesAlpha = anyPartial;
        if (!anyPartial && total) ++m_stats.opaqueAlpha;
    }

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

    const bool submitted = SubmitAndWait(m_device->GraphicsQueue(), cmd,
                                         "texture upload", m_lastError);

    vkFreeCommandBuffers(m_device->Device(), m_commandPool, 1, &cmd);
    m_alloc->DestroyBuffer(staging);
    if (!submitted) DestroyImage(out);
    return submitted;
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

        // Formats are decoded on the way in now, rather than only the two
        // that happen to match the image layout being accepted and the other
        // eight silently rendering white. kTexUnknown is the one the producer
        // itself could not read, and there is nothing here to decode.
        if (tex->desc.format == SceneIPC::kTexUnknown)
        {
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

        if (!UploadImage(tex->desc, tex->pixels.data(), tex->pixels.size(),
                         m_images[slot]))
        {
            ++m_stats.skippedFormat;
            // Left dirty deliberately, so a transient failure is retried.
            m_slotOf.erase(dirty[i]);
            if (slot + 1 == m_stats.resident) --m_stats.resident;
            continue;
        }

        m_descriptors[slot].sampler     = VK_NULL_HANDLE;
        m_descriptors[slot].imageView   = m_images[slot].view;
        m_descriptors[slot].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m_stats.bytesResident += m_images[slot].bytes;
        m_carriesAlpha[dirty[i]] = m_lastUploadCarriesAlpha;
        ++m_stats.uploadedThisFrame;
        scene.MarkTextureClean(dirty[i]);
    }
}

uint32_t TextureCache::Slot(uint64_t textureId) const
{
    auto it = m_slotOf.find(textureId);
    return (it == m_slotOf.end()) ? kWhiteTextureSlot : it->second;
}

bool TextureCache::TextureCarriesAlpha(uint64_t textureId) const
{
    auto it = m_carriesAlpha.find(textureId);

    // An untextured draw samples the white slot, which is opaque and says
    // nothing about coverage - the same case as an opaque texture.
    return (it == m_carriesAlpha.end()) ? false : it->second;
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

    for (uint32_t i = 0; i < kSamplerCount; ++i)
    {
        if (!m_samplers[i]) continue;
        vkDestroySampler(m_device->Device(), m_samplers[i], nullptr);
        m_samplers[i] = VK_NULL_HANDLE;
    }
    m_samplerInfos.clear();
    if (m_commandPool)
    {
        vkDestroyCommandPool(m_device->Device(), m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
    m_device = nullptr;
}
}
