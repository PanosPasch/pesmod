// =============================================================================
// custom_logo_loader.cpp — on-the-fly custom league logos from PNG files.
//
// Goal: a `leagues/<slot>.png` beside `leagues/<slot>.ini` is loaded as that
// league's logo, with no AFS/.txs editing and no dependence on the exe's
// prebuilt graphic descriptors.
//
// Why the stock path can't do this (see docs / the investigation):
//   LoadLeagueLogo -> FUN_0094e9e0 only ever requests ids that are in a
//   PREBUILT descriptor id-list baked into the exe (DAT_00bbd158[0x158]).
//   New ids are never asked for, so adding textures to unknow_00364.txs does
//   nothing. Instead we build the texture ourselves and register it straight
//   into the resolver's registry (DAT_03a67ee0) via FUN_00953660.
//
// The .txs texture blob format (reverse-engineered + byte-verified against
// unknow_00364.txs):
//
//   0x00  u32  magic (0x29857294 in stock data; not validated by loader)
//   0x04  u32  image count (1)
//   0x08  u32  total blob size
//   0x0C  u32  texture id      <- ResolveDisplayTexture matches this
//   0x18  u8   format (0x02)
//   0x19  u8   0x13 = 8bpp/256-colour, 0x14 = 4bpp/16-colour
//   0x1A  u8   width  as log2
//   0x1B  u8   height as log2
//   0x80  ..   palette: 256 * RGBA, alpha 0..128, PS2 CLUT-swizzled
//   0x480 ..   pixels: 8-bit indices, PS2 PSMT8-swizzled
//
// We emit 8bpp/256-colour. Pipeline: WIC decode PNG -> RGBA -> median-cut
// (RGBA, alpha-aware) -> CLUT-swizzled palette + PSMT8-swizzled indices.
// The game unswizzles + uploads to the GPU on register (in its own code), so
// we only produce the source blob — the encoder was proven byte-identical to
// stock texture data before this was written.
// =============================================================================

#include "club_hooks_common.h"
#include "../utils/logger.h"

#include <wincodec.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <unordered_map>

namespace {

// FUN_00953660 — register a texture blob into the resolver registry
// (DAT_03a67ee0) and upload it. Returns the registry node, or 0 if the id was
// already registered (then our blob is unused) or on failure.
using FN_RegisterTexture = uint32_t* (__cdecl*)(uint32_t blob);
const auto RegisterTexture = reinterpret_cast<FN_RegisterTexture>(0x00953660);

// FUN_00953630 — ResolveDisplayTexture: registry node for a display id, or 0.
using FN_ResolveTexture = uint32_t* (__cdecl*)(int displayId);
const auto ResolveTexture = reinterpret_cast<FN_ResolveTexture>(0x00953630);

// FUN_009536e0 — release a registry node (decref; at 0 it unlinks, releases the
// GPU texture, and frees the blob + node). Used to unload our customs so they
// match the stock load-on-entry / unload-on-exit lifecycle.
using FN_ReleaseTexture = uint32_t (__cdecl*)(uint32_t* node);
const auto ReleaseTexture = reinterpret_cast<FN_ReleaseTexture>(0x009536e0);

struct RGBA { uint8_t r, g, b, a; };

// PS2 PSMT8 swizzle: linear (x,y) -> byte offset in the swizzled index plane.
// Verified: re-swizzling an unswizzled stock texture reproduces it byte-exact.
inline int SwzIndex(int x, int y, int w)
{
    const int block = (y & ~0xf) * w + (x & ~0xf) * 2;
    const int swap  = (((y + 2) >> 2) & 1) * 4;
    const int ypos  = (((y & ~3) >> 1) + (y & 1)) & 7;
    const int col   = ypos * w * 2 + ((x + swap) & 7) * 4;
    return block + col + ((y >> 1) & 1) + ((x >> 2) & 2);
}

// PS2 256-entry CLUT swizzle (its own inverse): swap colours 8..15 with 16..23
// within each block of 32. `pal` is 256 * 4 bytes.
void ClutSwap(uint8_t* pal)
{
    for (int base = 0; base < 256; base += 32)
        for (int k = 0; k < 8; ++k)
            for (int c = 0; c < 4; ++c)
                std::swap(pal[(base + 8 + k) * 4 + c], pal[(base + 16 + k) * 4 + c]);
}

// Median-cut quantiser over RGBA (alpha is a 4th axis, so transparent regions
// cluster into their own entry). Fills a 256-entry palette + per-pixel index.
void MedianCut(const std::vector<RGBA>& px,
               std::vector<RGBA>& palette, std::vector<uint8_t>& index)
{
    std::vector<std::vector<int>> boxes(1);
    boxes[0].resize(px.size());
    for (size_t i = 0; i < px.size(); ++i) boxes[0][i] = static_cast<int>(i);

    auto axisRange = [&](const std::vector<int>& box, int& bestAxis) -> int {
        uint8_t mn[4] = {255,255,255,255}, mx[4] = {0,0,0,0};
        for (int i : box) {
            const uint8_t* v = &px[i].r;
            for (int c = 0; c < 4; ++c) { if (v[c] < mn[c]) mn[c] = v[c]; if (v[c] > mx[c]) mx[c] = v[c]; }
        }
        int best = 0; bestAxis = 0;
        for (int c = 0; c < 4; ++c) { int r = mx[c] - mn[c]; if (r > best) { best = r; bestAxis = c; } }
        return best;
    };

    while (boxes.size() < 256) {
        int bi = -1, bestRange = -1, bestAxis = 0;
        for (size_t k = 0; k < boxes.size(); ++k) {
            if (boxes[k].size() < 2) continue;
            int ax; int r = axisRange(boxes[k], ax);
            if (r > bestRange) { bestRange = r; bi = static_cast<int>(k); bestAxis = ax; }
        }
        if (bi < 0) break;
        std::vector<int> box = std::move(boxes[bi]);
        boxes.erase(boxes.begin() + bi);
        const int ax = bestAxis;
        std::sort(box.begin(), box.end(), [&](int a, int b) {
            return (&px[a].r)[ax] < (&px[b].r)[ax];
        });
        const size_t m = box.size() / 2;
        boxes.emplace_back(box.begin(), box.begin() + m);
        boxes.emplace_back(box.begin() + m, box.end());
    }

    palette.assign(256, RGBA{0,0,0,0});
    index.assign(px.size(), 0);
    for (size_t k = 0; k < boxes.size() && k < 256; ++k) {
        const auto& box = boxes[k];
        if (box.empty()) continue;
        uint32_t s[4] = {0,0,0,0};
        for (int i : box) { s[0] += px[i].r; s[1] += px[i].g; s[2] += px[i].b; s[3] += px[i].a; }
        const size_t n = box.size();
        palette[k] = RGBA{ uint8_t(s[0]/n), uint8_t(s[1]/n), uint8_t(s[2]/n), uint8_t(s[3]/n) };
        for (int i : box) index[i] = static_cast<uint8_t>(k);
    }
}

UINT NearestPo2(UINT v)
{
    UINT best = 16; int bd = 1 << 30;
    for (UINT p = 16; p <= 256; p <<= 1) {
        int dd = static_cast<int>(p) - static_cast<int>(v); if (dd < 0) dd = -dd;
        if (dd < bd) { bd = dd; best = p; }
    }
    return best;
}

// Decode a PNG to top-down 32bpp RGBA via WIC. With forceSize == 0 the image is
// scaled to the nearest square power-of-two (league logos); with forceSize > 0
// it is scaled to exactly forceSize x forceSize (team crests are a fixed 32x32).
bool DecodePngRGBA(const wchar_t* path, std::vector<RGBA>& out, int& outW, int& outH,
                   int forceSize = 0)
{
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didInit = SUCCEEDED(hrInit);

    IWICImagingFactory*    factory = nullptr;
    IWICBitmapDecoder*     decoder = nullptr;
    IWICBitmapFrameDecode* frame   = nullptr;
    IWICBitmapScaler*      scaler  = nullptr;
    IWICFormatConverter*   conv    = nullptr;
    bool ok = false;

    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) break;
        if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                    WICDecodeMetadataCacheOnDemand, &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;

        UINT fw = 0, fh = 0;
        if (FAILED(frame->GetSize(&fw, &fh)) || fw == 0 || fh == 0) break;
        // Force a SQUARE power-of-two: the blob's GS/size header fields are only
        // verified for square 8bpp textures, and logos map to square seats.
        // Crests pass an explicit size (the cell's fixed 32x32 8bpp footprint).
        const UINT n = forceSize > 0 ? static_cast<UINT>(forceSize)
                                     : NearestPo2(fw > fh ? fw : fh);
        const UINT tw = n, th = n;

        IWICBitmapSource* src = frame;
        if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant)))
            src = scaler;

        if (FAILED(factory->CreateFormatConverter(&conv))) break;
        if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppRGBA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom))) break;

        UINT w = 0, h = 0;
        if (FAILED(conv->GetSize(&w, &h)) || w == 0 || h == 0) break;
        out.resize(static_cast<size_t>(w) * h);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.size() * 4),
                                    reinterpret_cast<BYTE*>(out.data())))) break;
        outW = static_cast<int>(w); outH = static_cast<int>(h);
        ok = true;
    } while (false);

    if (conv)    conv->Release();
    if (scaler)  scaler->Release();
    if (frame)   frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (didInit) CoUninitialize();
    return ok;
}

// Build the 8bpp swizzled texture blob and register it (which uploads it).
// Returns the registry node, or 0 (blob freed) if the id was already taken.
uint32_t BuildAndRegister(int displayId, const std::vector<RGBA>& palette,
                          const std::vector<uint8_t>& index, int w, int h)
{
    if (!fn_GameAlloc || !RegisterTexture) return 0;

    int wl = 0, hl = 0;
    while ((1 << wl) < w) ++wl;
    while ((1 << hl) < h) ++hl;

    const int blobSize = 0x80 + 1024 + w * h;
    uint8_t* blob = reinterpret_cast<uint8_t*>(fn_GameAlloc(1, blobSize));
    if (!blob) return 0;
    std::memset(blob, 0, blobSize);

    // Header fields, all derived to match stock 8bpp textures exactly
    // (verified against every size in unknow_00364.txs). +0x14 (actual
    // width/height) is what the uploader reads — leaving it 0 renders white.
    const uint32_t lo2c = (w >> 7) ? static_cast<uint32_t>(w >> 7) : 1u;
    const uint32_t hi2c = ((w * h) >> 13) ? static_cast<uint32_t>((w * h) >> 13) : 1u;

    auto W32 = [&](int off, uint32_t v) { *reinterpret_cast<uint32_t*>(blob + off) = v; };
    W32(0x00, 0x29857294);
    W32(0x04, 1);
    W32(0x08, static_cast<uint32_t>(blobSize));
    W32(0x0C, static_cast<uint32_t>(displayId));
    W32(0x10, 0x00800480);                                   // palette@0x80 / pixels@0x480
    W32(0x14, static_cast<uint32_t>(w) | (static_cast<uint32_t>(h) << 16));   // width | height<<16
    blob[0x18] = 0x02; blob[0x19] = 0x13;                    // format / 8bpp-256
    blob[0x1A] = static_cast<uint8_t>(wl); blob[0x1B] = static_cast<uint8_t>(hl);  // log2 dims
    W32(0x1C, 0x10100001);
    W32(0x20, static_cast<uint32_t>((w * h) / 16));
    W32(0x24, 0x40);
    W32(0x28, static_cast<uint32_t>(w / 2) | (static_cast<uint32_t>(h / 2) << 16));
    W32(0x2C, lo2c | (hi2c << 16));

    // Palette: RGBA, alpha 0..255 -> 0..128, then CLUT-swizzle.
    uint8_t* pal = blob + 0x80;
    for (int c = 0; c < 256; ++c) {
        const RGBA p = (c < static_cast<int>(palette.size())) ? palette[c] : RGBA{0,0,0,0};
        int a = (p.a + 1) / 2; if (a > 128) a = 128;
        pal[c*4+0] = p.r; pal[c*4+1] = p.g; pal[c*4+2] = p.b; pal[c*4+3] = static_cast<uint8_t>(a);
    }
    ClutSwap(pal);

    // Pixels: PSMT8-swizzle the indices.
    uint8_t* pix = blob + 0x480;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            pix[SwzIndex(x, y, w)] = index[y * w + x];

    uint32_t* node = RegisterTexture(reinterpret_cast<uint32_t>(blob));
    if (!node && fn_GameFree)
        fn_GameFree(reinterpret_cast<uint32_t*>(blob));   // id already registered -> our blob unused
    return reinterpret_cast<uint32_t>(node);
}

// displayId -> attempted (so we build each custom logo exactly once per screen).
std::unordered_map<int, uint32_t> g_customLogo;

// Registry nodes we created this screen, to release on the next (re-)entry —
// giving our customs the same lifecycle as the stock preloaded textures.
std::vector<uint32_t*> g_customNodes;

// Synthetic display-id base for slots that have a PNG but no assigned logo id.
// Well above every stock texture id (which top out around 0x5000), so it never
// collides with a real graphic. One id per slot: base + slot.
constexpr int kSyntheticLogoBase = 0x00E00000;

} // namespace

// Register leagues/<slot>.png (if present) under the slot's display id, once.
// Called from UpsertLeagueVisualSlot just before ResolveDisplayTexture, where
// the (slot, displayId) pair is known and the id is about to be resolved.
// Register a PNG as a texture under `displayId` (once per screen), tracked for
// unload. Returns true if a texture is available under displayId afterwards
// (ours, or one already registered). Shared by league logos and team emblems.
bool RegisterCustomPng(int displayId, const wchar_t* pngPath)
{
    if (displayId == 0 || displayId == -1) return false;

    auto it = g_customLogo.find(displayId);
    if (it != g_customLogo.end())
        return it->second != 0 || ResolveTexture(displayId) != nullptr;

    // Already registered (stock, or shared)? Leave it — re-registering would
    // only bump the game's refcount and leak it.
    if (ResolveTexture(displayId)) { g_customLogo[displayId] = 0; return true; }

    if (GetFileAttributesW(pngPath) == INVALID_FILE_ATTRIBUTES) {
        g_customLogo[displayId] = 0;   // no PNG; don't look again this screen
        return false;
    }

    std::vector<RGBA> rgba; int w = 0, h = 0;
    if (!DecodePngRGBA(pngPath, rgba, w, h)) {
        g_customLogo[displayId] = 0;
        return false;
    }

    std::vector<RGBA> palette; std::vector<uint8_t> index;
    MedianCut(rgba, palette, index);
    const uint32_t node = BuildAndRegister(displayId, palette, index, w, h);
    if (node) g_customNodes.push_back(reinterpret_cast<uint32_t*>(node));
    g_customLogo[displayId] = node;
    Logger::Log("[CustomTex] id=0x%X %dx%d -> node=0x%08X%s",
                displayId, w, h, node, node ? "" : " (register failed)");
    return node != 0;
}

// Register leagues/<slot>.png under the slot's display id, once per screen.
void EnsureCustomLeagueLogo(int slot, int displayId)
{
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L".\\leagues\\%d.png", slot);
    RegisterCustomPng(displayId, path);
}

// Release every custom texture we registered and clear the caches, so the next
// screen (re-)entry rebuilds them fresh. Mirrors the stock preload/unload cycle
// (the stock unload FUN_009dab90 only frees its own tracked textures, never
// ours). Call at the start of InitClubSelectionScreen, before the preload.
void UnloadCustomLeagueLogos()
{
    if (!g_customNodes.empty())
        Logger::Log("[CustomLogo] unloading %d custom texture(s)",
                    static_cast<int>(g_customNodes.size()));
    for (uint32_t* node : g_customNodes)
        if (node) ReleaseTexture(node);
    g_customNodes.clear();
    g_customLogo.clear();
}

// -----------------------------------------------------------------------------
// Team crests — teams/<ID>.png
//
// The club selection copies each team's crest by loading it into a scratch
// texture id (0x7346..0x73ef) via FUN_00b3beb0, reading back the 32x32 8bpp
// palette+pixels, then releasing the scratch (FUN_00b3be00). A team with no
// crest data (e.g. an id outside the stock list) loads nothing, so the cell
// draws the white flag. We give such a team a crest by registering a fresh blob
// built from teams/<ID>.png under the same scratch id, on demand.
//
// The decode+quantise is cached per team id; only the (cheap) blob build +
// register runs each call. These nodes are NOT tracked in g_customNodes: the
// game releases the scratch itself after every use, so they must not be touched
// by UnloadCustomLeagueLogos.
// -----------------------------------------------------------------------------
namespace {

struct CrestData {
    bool                 tried = false;   // decode attempted (may have failed)
    bool                 ok    = false;   // usable 32x32 palette+index present
    std::vector<RGBA>    palette;
    std::vector<uint8_t> index;
};

std::unordered_map<int, CrestData> g_crest;   // teamId -> decoded 32x32 crest

constexpr int kCrestSize = 32;   // crest cell is a fixed 32x32 8bpp footprint

const CrestData* GetCrestData(int teamId)
{
    CrestData& c = g_crest[teamId];
    if (c.tried) return c.ok ? &c : nullptr;
    c.tried = true;

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L".\\teams\\%d.png", teamId);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return nullptr;   // no crest for this team

    std::vector<RGBA> rgba; int w = 0, h = 0;
    if (!DecodePngRGBA(path, rgba, w, h, kCrestSize))
        return nullptr;

    MedianCut(rgba, c.palette, c.index);
    c.ok = true;
    Logger::Log("[CustomCrest] teams/%d.png decoded %dx%d", teamId, w, h);
    return &c;
}

} // namespace

// See club_hooks_common.h. Returns the registry node (as int) for a fresh crest
// blob registered under `scratchId`, or 0 to fall back to the stock loader.
int RegisterCustomCrest(int scratchId, int teamId)
{
    const CrestData* c = GetCrestData(teamId);
    if (!c) return 0;

    // The caller clears the scratch before each use, so it should be free; if
    // something already holds it, defer to the stock path (returns that node).
    if (ResolveTexture(scratchId)) return 0;

    // Fresh game-allocated blob under the scratch id. The game reads it back and
    // releases (frees) it per use, exactly like a stock crest — so this is NOT
    // tracked in g_customNodes.
    const uint32_t node =
        BuildAndRegister(scratchId, c->palette, c->index, kCrestSize, kCrestSize);
    return static_cast<int>(node);
}

// Synthetic display-id base for team emblems (distinct from the league-logo
// synthetic range 0x00E0xxxx). One id per team: base + teamId.
namespace { constexpr int kEmblemIdBase = 0x00E10000; }

// See club_hooks_common.h. Register teams/<id>.png (once per screen, tracked for
// unload like a league logo) and return its registry node for the badge nodes,
// or 0 if the team has no custom emblem. The badge draws it with a full (0,0,1,1)
// UV, so the PNG's own square power-of-two decode is exactly what we want.
int GetCustomEmblemNode(int teamId)
{
    const int emblemId = kEmblemIdBase + teamId;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L".\\teams\\%d.png", teamId);
    if (!RegisterCustomPng(emblemId, path))
        return 0;
    return static_cast<int>(reinterpret_cast<uintptr_t>(ResolveTexture(emblemId)));
}

// For each slot that has NO logo id (-1) but does have a leagues/<slot>.png,
// stamp a synthetic display id into the panel slot (panel[+0x1DE4 + slot*STRIDE])
// so the slot counts as populated and its PNG can be registered + drawn.
// Must run before the visibility / selectable-count passes read the ids, i.e.
// at the very start of hook_BuildLeaguePanelVisualSlots_C.
void AssignSyntheticLogoIds(uint32_t* panel)
{
    if (!panel) return;
    for (int slot = 0; slot < MAX_PANEL_SLOTS; ++slot) {
        int* disp = reinterpret_cast<int*>(
            reinterpret_cast<uint8_t*>(panel) + 0x1DE4 + slot * PANEL_SLOT_STRIDE);
        if (*disp != -1) continue;                       // already has a logo id

        wchar_t path[MAX_PATH];
        _snwprintf_s(path, _TRUNCATE, L".\\leagues\\%d.png", slot);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;

        *disp = kSyntheticLogoBase + slot;
        Logger::Log("[CustomLogo] slot=%d had no logo id; assigned synthetic 0x%X",
                    slot, *disp);
    }
}
