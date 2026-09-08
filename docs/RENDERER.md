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
### 2.1 Two pipelines, not one

`pes6.exe` imports exactly one graphics entry point, `Direct3DCreate8`, and
scanning the binary for compiled shader version tokens (`0xFFFE0101` and
friends) finds **none**. It is tempting to conclude the game is pure fixed
function. It is not, and the capture proved it.

The binary statically links the **D3DX8 shader assembler** — its opcode table
(`rsq add dp4 m4x4 sub sge dp3 m3x3 m4x3 mul mad mov vs.1.1`, `oPos`, `oFog`,
`c%d`) and its error strings ("D3DX8 Shader Assembler Version 0.91",
"coissue not supported in vertex shaders") are both present. The game
**assembles vs.1.1 shaders at runtime**, which is exactly why no compiled
bytecode appears on disk.

Measured from a real match frame, the game runs two pipelines side by side:

| Path | Vertex format | Transform source | Draws |
| ---- | ------------- | ---------------- | ----: |
| 2D menus / HUD | FVF `0x104`, `0x144`, `0x44` | pre-transformed (`XYZRHW`) | 22 |
| 3D scene, shader | declaration + **vs.1.1 shader** | **shader constants** | 422 |
| 3D scene, fixed function | FVF `0x142` | `SetTransform` | 16 |

**This changes where the transforms live.** A vertex shader ignores
`SetTransform` entirely and reads its matrices from constant registers, so for
the 422 shader-driven draws the `world`/`view`/`projection` matrices are not
authoritative — `SetVertexShaderConstant` is. The capture therefore shadows
c0..c95 and snapshots the whole register file per draw.

Two capture bugs came out of this, both now fixed:

- Recording only the decoded FVF made every shader draw report `fvf 0x0`,
  indistinguishable from "no vertex format bound". The raw `SetVertexShader`
  argument and its kind (`fvf` / `declaration` / `none`) are now both recorded.
- Declaration registers were labelled with fixed-function semantics
  (`POSITION`, `BLENDWEIGHT`, `NORMAL`…). Those names only mean something for a
  declaration used *without* a shader. With a shader bound they are plain input
  registers `v0..v15`, so labelling `v1` "BLENDWEIGHT" invented structure that
  was not there. Shader-bound declarations now print as `v0..vN`.

The scene is still reconstructible — the geometry, transforms and materials are
all observable — but through shader constants rather than the fixed-function
transform stack.

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

Every draw is classified **screen / world / unknown** — three ways, not two:

| Bound vertex format | Classification |
| ------------------- | -------------- |
| FVF with `D3DFVF_XYZRHW` | **screen** — pre-transformed HUD, menus, fade quads |
| FVF with `D3DFVF_XYZ` etc. | **world** |
| Declaration handle, declaration known | **world** |
| `0` (nothing bound), or a handle created before the hook | **unknown** |

The third bucket matters. An earlier two-way split treated "anything that is
not screen space" as world geometry, which meant a draw whose vertex format
could not be resolved was silently handed to the scene as if it were real
geometry — and it was that bug which first fired the `capture_on_first_3d`
trigger on a menu frame. Geometry the ray tracer cannot interpret must be
reported as unknown, not guessed at.

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

### 4.1 A match frame

Captured in-game via `capture_on_first_3d`. This is the workload the ray
tracer has to replace:

| Metric | Value |
| ------ | ----: |
| Draw calls | 460 |
| — world space | **438** |
| — screen space (HUD) | 22 |
| Triangles | 22,801 |
| — world space | **22,757** |
| Distinct WORLD matrices | **17** |
| Distinct VIEW / PROJECTION matrices | **1 / 1** |
| Vertex buffers referenced | 17 (all read back) |
| Index buffers referenced | 16 (all read back) |
| Textures referenced | 44 |
| `SetRenderState` | 519 (369 redundant, 71%) |

A single camera and 17 object transforms per frame means the TLAS is small and
cheap: 17 instances over ~23k triangles. That is a trivial scene by modern
standards — the entire per-frame geometry budget is smaller than one character
in a contemporary game.

### 4.2 The 3D vertex layout

The declaration-bound geometry is **24 bytes per vertex**, confirmed by
decoding the raw buffer bytes rather than trusting the declaration alone —
which was fortunate, since the declaration's register *names* turned out to be
mislabelled while the byte layout derived here was correct:

| Offset | Field | Evidence |
| -----: | ----- | -------- |
| 0 | `float3` position | ranges ±5542, ±2457, ±6400 — stadium scale, consistent with the view translation (66, 730, 3003) |
| 12 | `D3DCOLOR` diffuse | `FFFFFFFF`, `FFF2EBBC` — plausible ARGB, alpha always `FF` |
| 16 | `float2` texcoord | 0.0–1.0, exceeding 1.0 only where textures tile |

Reading offset 12 as the start of a `float3` normal instead yields `NaN`
(`0xFFFFFFFF` is not a finite float), which rules that layout out decisively.
The same 24-byte layout also appears as FVF `0x142` (`XYZ|DIFFUSE|TEX1`) on 16
`DrawPrimitive` calls, independently confirming it.

Index buffers are 16-bit (`D3DFMT_INDEX16`).

### 4.3 The material model

Uniform across all 438 world-space draws:

| State | Value | Consequence |
| ----- | ----- | ----------- |
| `D3DRS_LIGHTING` | **0** | fixed-function lighting is *off*; nothing is lit at draw time |
| `D3DRS_SPECULARENABLE` | 0 | no specular term |
| `D3DRS_COLORVERTEX` | 1 | vertex colour is the colour source |
| Stage 0 `COLOROP` | `MODULATE` | |
| Stage 1 `COLOROP` | `DISABLE` | a single texture stage |

So for any draw with **no pixel shader bound**, the shading model is:

```
pixel = texture(uv) x vertexColour
```

**Caveat, and why the next capture matters.** A bound pixel shader overrides
the texture stage states completely, so `D3DTSS_COLOROP` only describes the
shading when `SetPixelShader(0)` is in effect. The frame these numbers come
from was captured before pixel-shader binding was recorded, and the binary
does contain `ps.1.x` assembler support — so this model is confirmed only for
draws that turn out to have no pixel shader. Each draw record now carries
`"ps"` and a `"tssAuthoritative"` flag so the distinction is explicit.

**There are no normals anywhere in the pipeline** — not in the vertex format,
and no lights are ever set. All illumination is pre-baked into the vertex
colours (the warm `FFF2EBBC` tint is baked stadium lighting).

This is the central problem for the ray tracer, and it is exactly what RTX
Remix has to solve too:

- **Normals must be reconstructed** from triangle winding, then smoothed
  across shared vertices to avoid a faceted look.
- **Albedo must be recovered** from `texture x bakedLight`. Using the vertex
  colour as-is would double-light the scene once real lighting is added.

Other per-draw variation: `CULLMODE` is CW on 327 draws and NONE on 111, alpha
blending is on for 388, alpha test for 420, and fog for 436 of 438.

### 4.4 Menu rendering, for contrast

From a title/menu session (121 frames): 3.3 draws per frame, 65
`SetRenderState` calls of which 50 redundant, and two screen-space FVFs —
`0x104` (`XYZRHW|TEX0`, 24 B) and `0x144` (`XYZRHW|DIFFUSE|TEX0`, 28 B).

The redundant-state figure holds across both workloads at roughly 71–77%: most
`SetRenderState` calls set a value the device already had. A modern backend
that dedupes state gets that back for free.

---

## 5. Status

**Validated end to end, in-game:** proxy attach and `CreateDevice`
substitution; device `Reset` handling; state shadowing; per-frame statistics;
all three capture triggers (hotkey, frame index, first-3D); vertex, index and
user-pointer buffer dumping; texture dumping to DDS; session reporting; and
honest reporting of resources that could not be read back.

**Known limitations:**

- `D3DPOOL_DEFAULT` textures cannot be locked, and are reported as
  `"dumped": false` with a reason rather than dumped. In the match frame this
  affected one 3840x2160 surface.
- Vertex declarations created before the hook is installed cannot be resolved;
  draws using them classify as `unknown`.
- The material model in 4.3 is confirmed only for draws with no pixel shader
  bound. Pixel-shader capture exists but has not yet been exercised in-game.
- `ApplyStateBlock` would desynchronise the shadow. The game does not use state
  blocks, and the proxy logs a one-time warning if that ever changes.

---

## 6. Roadmap

1. **Scene reconstruction.** Group the 17 per-frame transforms into stable
   objects across frames, reconstruct normals, and de-light the vertex colours
   into albedo. Establish the world-unit scale (positions suggest roughly
   1 unit ~ 1 cm, given a pitch on the order of 10,500 x 6,800 units) so that
   lighting can be physical.
2. **The 64-bit render host.** Vulkan 1.3 with `VK_KHR_ray_tracing_pipeline`,
   BLAS per mesh, a 17-instance TLAS per frame, shared-memory scene transport
   from the ASI.
3. **Lighting.** Nothing can be inherited from the game — there are no lights
   in the pipeline at all — so stadium lighting has to be authored: floodlight
   rigs plus a sky/environment term.
4. **Path tracing + denoise.** DLSS Ray Reconstruction is already present in
   the game folder, making it the natural denoiser target.
5. **Presentation and input.** The host owns the visible window; the game
   window becomes the input sink. The 22 HUD draws are composited on top
   unchanged, per the scope decision to leave the UI alone.
