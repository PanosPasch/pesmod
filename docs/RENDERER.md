# Renderer: understanding pes6.exe's engine, and replacing it

This document covers the renderer work: what the game's Direct3D 8 engine
actually does, the interception layer PESMod uses to find that out, and the
architecture of the Vulkan ray-traced renderer being built on top of it.

Addresses refer to `pes6.exe`, md5 `678e9ac0e741207dddb9aa33caed47a0`
(the *PES 6 WeHellas Greek Superleague 2006-07* build). Treat them as labels,
not portable constants — see the compatibility note in the main README.

---

## 1. The constraint that shapes everything

**Ray tracing cannot run inside the game's process.** The game is a 32-bit
executable, and NVIDIA's 32-bit Vulkan ICD does not expose the ray tracing
extensions at all. Measured on an RTX 4090 Laptop GPU, driver 31.0.15.4692,
with the same probe built both ways:

| Extension | 32-bit ICD | 64-bit ICD |
| --------- | :--------: | :--------: |
| `VK_KHR_acceleration_structure`   | ✗ | ✓ |
| `VK_KHR_ray_tracing_pipeline`     | ✗ | ✓ |
| `VK_KHR_ray_query`                | ✗ | ✓ |
| `VK_KHR_deferred_host_operations` | ✗ | ✓ |
| *(total device extensions)*       | 187 | 202 |

`VK_KHR_buffer_device_address`, `VK_KHR_spirv_1_4` and
`VK_EXT_descriptor_indexing` — the usual RT prerequisites — *are* present in
32-bit. Only the acceleration-structure and ray-pipeline families are absent.
This is a driver policy, not a configuration problem, so no amount of loader
or SDK work makes hardware ray tracing reachable from inside `pes6.exe`.

**Consequence — the architecture is forced into two processes:**

```
  pes6.exe (32-bit)                        render host (64-bit)
  ┌───────────────────────────┐            ┌──────────────────────────────┐
  │ game logic, animation, AI │            │ Vulkan 1.3 + KHR ray tracing │
  │                           │            │                              │
  │ D3D8 calls                │            │ BLAS / TLAS build            │
  │   ↓                       │  shared    │ path tracing                 │
  │ PESMod interception layer │──memory──▶ │ denoise + present            │
  │   • state shadow          │  scene     │                              │
  │   • geometry extraction   │  stream    │                              │
  │   • material/texture ids  │            │                              │
  └───────────────────────────┘            └──────────────────────────────┘
```

The 32-bit side never touches Vulkan. It observes, extracts and forwards.

---

## 2. The game's Direct3D 8 engine

### 2.1 It is pure fixed-function

`pes6.exe` imports exactly one graphics entry point: `Direct3DCreate8`. There
are **no vertex shaders in the binary at all** — `SetVertexShader` is only ever
called with FVF codes, never with handles from `CreateVertexShader`.

That is the single most important fact for this project. It means every draw's
vertex layout is fully described by its FVF, and the object and camera
transforms are plain `SetTransform` matrices rather than being buried inside
shader constants. The scene is therefore *reconstructible* — which is exactly
the property that makes fixed-function DX8/DX9 titles the class of game
RTX Remix targets.

### 2.2 The device layer

The engine's D3D wrapper is small and localised around `0x00403c00–0x00406000`:

| Address | Role |
| ------- | ---- |
| `FUN_00404c60` | D3D init: `Direct3DCreate8(0xdc)`, `GetDeviceCaps`, HAL→REF fallback |
| `FUN_004051d0` | Mode/format selection loop (`CheckDeviceType`) |
| `FUN_00405300` | `CreateDevice` + present parameters + first `Clear` |
| `FUN_00403e10` | Frame begin: `TestCooperativeLevel` → `BeginScene` → `Clear` |
| `FUN_00405850` | Frame end: `EndScene` → `Present(0,0,0,0)` + device-lost recovery |
| `FUN_004059a0` | `GetDevice()` — literally `mov eax,[0x00f83e68]; ret` |
| `DAT_00f83e68` | the `IDirect3DDevice8*` global (200+ call sites) |
| `DAT_00f83eb8` | the `IDirect3D8*` global |
| `DAT_00f83ef0` | the cached `D3DCAPS8` |

Every render call site in the binary dispatches through `DAT_00f83e68` by
hard-coded vtable offset. `FUN_00405300` writes that global with whatever
`CreateDevice` returns — which is why substituting a proxy object at creation
captures the entire engine without patching a single byte of game code.

Draw submission lives in a second cluster around `0x00873000–0x0088e000`.

### 2.3 Vtable offsets, confirmed against the binary

The vendored [`d3d8_min.h`](../src/render/d3d8/d3d8_min.h) must present a
byte-compatible vtable. Its layout was cross-checked against real call sites:

| Method | Index | Offset | Evidence in `pes6.exe` |
| ------ | ----: | -----: | ---------------------- |
| `TestCooperativeLevel` | 3 | `0x0C` | `0x00403e15`: `ff 51 0c`, then `cmp eax,0x88760868` (`D3DERR_DEVICELOST`) |
| `Present`      | 15 | `0x3C`  | `FUN_00405850`: `(this,0,0,0,0)` |
| `BeginScene`   | 34 | `0x88`  | `FUN_00405850` |
| `EndScene`     | 35 | `0x8C`  | `FUN_00405850` |
| `Clear`        | 36 | `0x90`  | `FUN_00405300`: `(this,0,0,3,0,1.0f,0)` = `TARGET\|ZBUFFER` |
| `SetRenderState` | 50 | `0xC8` | `FUN_0087be70`: `(this,0x1b,1)` = `ALPHABLENDENABLE` |
| `SetTexture`   | 61 | `0xF4`  | `FUN_0087be70` |
| `SetTextureStageState` | 63 | `0xFC` | `FUN_0087be70` |
| `DrawPrimitiveUP` | 72 | `0x120` | `FUN_0087be70`: `(this,5,2,ptr,0x14)` — tri-strip, stride 20 |
| `SetVertexShader` | 76 | `0x130` | `FUN_0087be70`: `(this,0x44)` = FVF `XYZRHW\|DIFFUSE` |

`FUN_0087be70` is the screen fade: it sets up alpha blending, disables Z and
lighting, and draws one full-screen `XYZRHW|DIFFUSE` quad. Decoding it end to
end is what pinned down the offsets.

### 2.4 Device creation parameters (observed at runtime)

```
Direct3DCreate8(220)                 // 0xdc, matches FUN_00404c60
device:  HAL, behaviorFlags 0x80     // D3DCREATE_MIXED_VERTEXPROCESSING
backbuffer: 640x480 A8R8G8B8, windowed, SwapEffect DISCARD
depth:   D24S8 (auto depth stencil)
```

`0x80` (mixed) versus `0x20` (software) is chosen by `FUN_00405300` from a
device-caps bit — the game never asks for pure hardware vertex processing.

---

## 3. The interception layer

Source lives under [`src/render/`](../src/render):

```
src/render/
├─ render_config.*        [render] section of PESMod.ini
├─ render_hooks.*         hooks Direct3DCreate8, installs the proxies
├─ d3d8/
│  ├─ d3d8_min.h          vendored minimal D3D8 interfaces (no DX8 SDK needed)
│  ├─ d3d8_guids.cpp      IID_IDirect3D8 / IID_IDirect3DDevice8
│  └─ d3d8_util.*         FVF decoding, surface sizing, enum names
└─ capture/
   ├─ proxy_d3d8.*        IDirect3D8 wrapper — intercepts CreateDevice
   ├─ proxy_device.*      IDirect3DDevice8 wrapper — all 97 methods
   ├─ state_tracker.*     shadow of the fixed-function pipeline state
   ├─ resource_registry.* every VB / IB / texture the game creates
   └─ frame_capture.*     per-frame stats + full single-frame dumps
```

### 3.1 Why hook `Direct3DCreate8` and not the game

Hooking the D3D8 export rather than a game address means the layer is
independent of the executable's addresses, and it sits correctly underneath
`d3d8to9` / ReShade if those are ever re-enabled in the game folder — whatever
`d3d8.dll` is actually loaded is the thing that gets hooked. The built `.asi`
does **not** statically import `d3d8.dll`; it resolves it at runtime, so it
cannot perturb the game's DLL load order.

### 3.2 Why there are no resource proxy objects

The obvious way to see buffer contents is to wrap every
`IDirect3DVertexBuffer8` and shadow it on `Unlock`. That means three more COM
wrapper classes plus unwrapping at every binding site — each one a chance for
an unwrapped pointer to leak through and crash the game.

Instead the registry records metadata at creation, and the capture path simply
`Lock`s a buffer read-only when it needs the bytes. The only thing that blocks
that is `D3DUSAGE_WRITEONLY`, so **while capture is enabled the proxy strips
that flag at creation**. That trades a little upload bandwidth — in capture
mode only, which is off by default — for a whole category of wrapper bugs that
never get written. The manifest records `writeOnlyStripped` per resource so the
report always says when the capture was intrusive.

Resources that still refuse to be read (typically `D3DPOOL_DEFAULT` textures)
are recorded as `"dumped": false` with a reason, never as a file that does not
exist.

### 3.3 The 2D / 3D split

The load-bearing classification is `D3DFVF_XYZRHW`. A draw using it is
pre-transformed screen space — HUD, menus, the fade quad — and stays on the
game's own path. Everything else is world-space geometry with real
world/view/projection matrices, and is what the ray tracer takes ownership of.

This is a single bit test on the FVF, and it is exact: there is no heuristic
involved, because a fixed-function pipeline cannot draw world geometry any
other way.

### 3.4 Output

Per capture, under `capture_dir`:

```
frame_000120/
├─ manifest.json     device params, frame stats, resource table w/ dump status
├─ draws.json        every draw: FVF, stride, bindings, W/V/P matrices,
│                    render states, texture-stage ops, material
├─ buffers/          vb_*.bin, ib_*.bin, up_*.bin (user-pointer draws)
└─ textures/         tex_*.dds (all mip levels)
```

Plus `session_report.md`, rewritten after every capture and every 600 frames
so a hard kill still leaves a usable report — the game's DRM wrapper ignores a
polite terminate, so a clean shutdown cannot be relied on.

### 3.5 Configuration

All under `[render]` in `PESMod.ini`, all defaulting to off:

| Key | Meaning |
| --- | ------- |
| `enabled` | install the interception layer at all |
| `capture` | record statistics and allow frame dumps |
| `capture_key` | virtual-key that dumps the next frame (`0x78` = F9) |
| `capture_on_first_3d` | auto-dump the frame after the first world-space draw |
| `capture_at_frame` | auto-dump a specific frame index (unattended runs) |
| `capture_dir` / `session_report` | output paths |

`capture_on_first_3d` exists because the match scene is several menus deep and
the game acquires DirectInput exclusively — neither a hotkey nor synthetic
input is reliable, but "capture the moment 3D geometry first appears" is.

---

## 4. Measured behaviour

From a menu/title-screen session (121 frames). **These numbers describe 2D
menu rendering only** — see the status note in §5.

| Metric | Per frame |
| ------ | --------: |
| Draw calls | 3.3 (range 2–22) |
| `SetRenderState` | 65.1 |
| — of which redundant | **50.1 (77%)** |
| `SetTextureStageState` | 98.5 |
| `SetTexture` | 8.2 |
| `SetTransform` | 3.1 |

Two vertex formats appear in menus, both screen-space:

| FVF | Layout | Stride |
| --- | ------ | -----: |
| `0x104` | `XYZRHW\|TEX0(2f)` | 24 B |
| `0x144` | `XYZRHW\|DIFFUSE\|TEX0(2f)` | 28 B |

Resource totals at that point: 6 vertex buffers (528 KB), 5 index buffers
(32 KB), 90 textures (128 MB at level 0, dominated by one 3840×2160
`D3DPOOL_DEFAULT` surface).

The redundant-state figure is the notable one: roughly three quarters of all
`SetRenderState` calls set a value the device already had. A modern backend
that dedupes state gets that for free.

---

## 5. Status

**Validated end to end:** proxy attach, `CreateDevice` substitution, device
`Reset` handling, state shadowing, per-frame statistics, the hotkey and
frame-index and first-3D triggers, texture dumping to DDS, user-pointer draw
capture, session reporting, and honest reporting of resources that could not
be read back.

**Implemented but not yet exercised:** the vertex- and index-buffer dump path,
and every world-space code path. The runs so far only reached the title and
menu screens, which draw exclusively `XYZRHW` geometry with `DrawPrimitiveUP`,
so no `DrawIndexedPrimitive` from a real vertex buffer has been captured yet.
Getting there needs a match to be started; `capture_on_first_3d = 1` will then
dump it automatically.

---

## 6. Roadmap

1. **Capture a match frame.** Everything below is written against real data
   from a real 3D frame rather than assumptions.
2. **Scene reconstruction.** Group draws into stable objects, derive world
   transforms, resolve textures to materials, and work out how the game's
   coordinate system maps to metres so lighting units mean something.
3. **The 64-bit render host.** Vulkan 1.3 with `VK_KHR_ray_tracing_pipeline`,
   BLAS per mesh, TLAS per frame, shared-memory scene transport from the ASI.
4. **Path tracing + denoise.** With DLSS Ray Reconstruction available in the
   game folder already, that is the natural denoiser target.
5. **Presentation and input.** The host owns the visible window; the game
   window becomes the input sink. 2D overlay draws are composited on top,
   unchanged, per the scope decision to leave the UI alone.
