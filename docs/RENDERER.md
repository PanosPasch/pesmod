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
### 4.3 Shading: what the shaders actually do

The captured bytecode settles this. `tools/sm1dis.py` disassembles the dumps.

**The transform.** Every vertex shader starts the same way:

```
m4x4   oPos, v0, c58        ; combined world-view-projection at c58..c61
dp4    oFog, v0, c67        ; fog plane
```

So `c58..c61` is a **single combined WVP matrix**, per draw — and it is the
number the scene builder needs, not `SetTransform`, which the shaders ignore.

**Those four registers are the matrix's columns, not its rows.** `m4x4`
expands to `dp4 oPos.x, v0, c58` / `.y, c59` / `.z, c60` / `.w, c61`, so
`clip.j = dot(v0, c(58+j))` — the usual Direct3D practice of transposing a
matrix before uploading it as constants. Reading them as rows costs a full
debugging cycle, because a transposed perspective matrix does not look
broken: it moves the projective terms out of the last column and into the
last row, so every vertex divides itself down toward the screen centre and
the result is a plausible image of nothing rather than obvious garbage.

Two independent measurements on match frame 5312 pin it:

| Reading | vertices in frustum | NDC of the world origin |
| ------- | ------------------- | ----------------------- |
| registers as rows | 0.0% (`\|ndc\|` under 0.02 everywhere) | (-0.0002, -0.0000) |
| registers as columns | 37.4% | (-0.041, -0.908) |

and the transposed reading reproduces `SetTransform`'s `view * proj` with rows
0 and 2 negated, to every printed decimal — an axis flip between the
fixed-function and shader paths, and a second confirmation from a source that
shares no code with the first.

The invariant worth remembering: for a perspective `VP` the last **column** is
the camera's forward axis, so its xyz length is 1. Transposed it is the
translation row instead, which on this frame measures 5185. One cheap check
separates them with three orders of magnitude to spare, and
`PESModIpcSelfTest convention` asserts it against these captured values.

**Consequence for reconstruction.** 403 of that frame's 405 world draws share
*one* `c58` block, so it is the shared view-projection and the vertices are
already in world space. Per-object placement is the exception, not the rule.

**At least five vertex layouts**, not two. An early capture showed only 24-
and 32-byte vertices and the renderer was built on that; counted across every
capture taken since, the picture is quite different:

| Stride | Declaration | Draws | Has normal |
| -----: | ----------- | ----: | ---------- |
| 40 B | `float3, d3dcolor, d3dcolor, float3, float2` | **5,944** | yes (offset 20) |
| 32 B | `float3, float3, float2` (± a stream-1 `float3`) | 3,012 | yes (offset 12) |
| 24 B | `float3, d3dcolor, float2` | 1,213 | no |
| 36 B | `float3, d3dcolor, float3, float2` | 616 | yes (offset 16) |
| 0 B | (no declaration bound) | 422 | — |

The 40-byte layout is the most common in the game, and rejecting anything
that was not 24 or 32 bytes dropped it entirely. The visible result was
players rendering as a floating head and a hand: heads and hands use other
layouts.

Position is at offset 0 in every one of them, which is what an acceleration
structure build requires. Nothing else is at a predictable offset, so
`GeometryDesc` now carries `uvOffset`, `normalOffset` and `colorOffset`
decoded from the game's own declaration rather than inferred from the stride.

A declaration feeding a real vertex shader binds plain input registers with
no semantics, so the attributes are identified structurally: the `float3` at
offset 0 is the position, a later `float3` is a normal, a `float2` is a
texture coordinate. Stream 0 only — the multi-stream variants put extra
`float3` data in stream 1, which is not a normal for stream 0's vertices and
would be read at the wrong stride if treated as one.

For the pre-lit class, shading really is just `mul oD0, v1, c72` — vertex
colour times a global tint. For the lit class, `vs_0007` computes real
lighting per vertex:

```
dp3    r11.x, v1, c95       ; N · L        (c95 = light direction)
max    r11.x, r11.x, c57.x  ; clamp at 0
mul    r10,   r11.x, c94    ; x light colour
dp3    r9.x,  v1, c93       ; N · up       (c93 = hemisphere axis)
mad    r9.x,  r9.x, c57.w, c57.w   ; remap -1..1 to 0..1
mad    r10,   c92, r9.x, r10       ; + sky colour x blend
mad    r10,   c91, v0.w, r10       ; + ground colour, unconditionally
mul    r8.xyz, r10, c69            ; scale the whole lit result
add    r8.xyz, r8, c68             ; + ambient
mul    oD0.xyz, r8, c72            ; x global tint
dp3    r6.xy, v1, -c63             ; specular half-vector
lit    r6, r6
mul    oD1, r6.z, c70              ; specular colour
```

**Three details worth transcribing rather than paraphrasing.** The
renderer approximated this rig for a long time and each shortcut showed:

- **`c95` is dotted directly, not negated.** Negating it leaves every
  upward-facing surface — the entire pitch — with `max(N·L, 0) == 0`, so no
  direct light and, because the shadow ray is only traced when `N·L > 0`, no
  shadows anywhere.
- **The hemisphere is not a `mix`.** The game *adds* ground unconditionally
  and adds sky scaled by the blend. A `mix` between them is both dimmer in
  the middle and flatter at the extremes.
- **`c69` scales the lit result before ambient is added.** Leaving it out
  makes everything uniformly too bright, which reads as washed out rather
  than as a missing multiply.

And `c95` and `c93` are **not unit vectors** — they arrive about 0.128 long
and the shader dots them raw. Normalising them, which looks like tidying,
multiplies the directional term by roughly eight.

**The lighting rig is readable, not lost.** Captured constant values:

| Register | Value | Role |
| -------- | ----- | ---- |
| `c95` | `(-0.4968, -0.7360, 0.4600)` | directional light vector — **unit length** |
| `c94` | `(0.97, 0.97, 0.97)` | light colour |
| `c93` | `(0, -1, 0)` | hemisphere axis |
| `c92` | `(0.53, 0.50, 0.42)` | sky colour (warm) |
| `c91` | `(0.22, 0.20, 0.18)` | ground colour |
| `c68` | `(0.1, 0.2, 0.2)` | ambient add |
| `c70` | `(0.2, 0.2, 0.2, 2.0)` | specular colour + exponent |
| `c63` | `(0.5949, 0.4195, 0.6856)` | specular half-vector — **unit length** |

That is a directional light plus a hemisphere ambient, with a normalised
direction and physically sensible colours. It can be lifted straight into the
ray tracer as the starting lighting rig.

**Per-pixel shading.** The pixel shaders do normal mapping:

```
ps_0003    tex t0                    ; base texture
           tex t1                    ; normal map
           texm3x2pad  t2, 1-t1      ; transform the normal
           texm3x2tex  t3, 1-t1      ; lighting lookup
           mad r1.xyz, t3, c1, v0    ; light x c1 + interpolated vertex colour
           mul r0.xyz, t0, r1        ; x base texture
           mov r0.w, v0.w
```

So the real model is `baseTexture x (lighting + vertexColour)`, with a
normal map feeding a lookup — not the `texture x vertexColour` that the fixed
function texture-stage states suggest. Those stage states describe only the
draws with no pixel shader bound; `"tssAuthoritative"` in each draw record
says which is which.

**Corrections this forced.** Two earlier conclusions in this document were
wrong and are retracted:

- *"There are no normals anywhere in the pipeline."* False. The 24-byte
  layout has none, but the 32-byte layout's `v1` is a normal and is consumed
  by `dp3 v1, c95`. The mistake was generalising from the one layout that had
  been decoded.
- *"No lights are ever set, so stadium lighting has to be authored."* False.
  `D3DRS_LIGHTING = 0` and the absence of `SetLight` calls are real, but they
  mean the *fixed-function* lighting pipeline is unused — because the shaders
  do the lighting from constants instead.

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
- The captured shader constants and lighting rig come from a menu frame; a
  match frame will have different per-object `c58` values, though the register
  *assignments* are fixed by the shader bytecode and will not move.
- `capture_on_first_3d` alone fires on the team-select and kit-preview menus,
  which draw a handful of 3D elements. `capture_min_3d_draws` (default 100)
  gates it so it lands on gameplay instead.
- `ApplyStateBlock` would desynchronise the shadow. The game does not use state
  blocks, and the proxy logs a one-time warning if that ever changes.

---
## 6. The render host

`src/render/host/` is the 64-bit half. It is a separate CMake project because
one CMake generate cannot emit both Win32 and x64 binaries:

```
cmake -S src/render/host -B build-host -A x64
cmake --build build-host --config Release
build-host/Release/PESModHost.exe --probe
```

### 6.1 Transport

`src/render/ipc/` is compiled into *both* the 32-bit ASI and the 64-bit host,
so its structures must have identical layout under both ABIs. A pointer,
`size_t`, bare `long`, or implicit padding would give the two sides different
readings of the same bytes — and the failure mode is not a crash but silently
wrong geometry. Every struct therefore uses fixed-width fields with explicit
padding, and every size, alignment and offset a bitness mistake would shift is
asserted at compile time, in both builds.

`ipc_selftest.cpp` checks this empirically rather than by inspection: built
both ways, the 32-bit producer writes a known stream and the 64-bit consumer
verifies every field, using ids with the high dword set (`0xDEADBEEF12345678`)
since truncation there is exactly what a mis-declared field causes.

One reliable byte ring carries two classes of traffic. Resource messages
(geometry, textures) are never dropped — the host cannot render an instance
whose vertices never arrived — while frame messages are dropped freely under
back-pressure, because the next frame supersedes them. Nothing blocks the
game's render thread.

### 6.2 What the producer sends

Geometry is keyed by a hash of the draw's *slice* of its buffers rather than
by the buffer: many draws share one vertex buffer and differ only by index
range, and each slice is its own mesh for BLAS purposes. Each slice's vertex
range is hashed every frame and re-uploaded when it changes, which is what
makes animated players work — the game skins on the CPU, so their vertices are
rewritten in place and buffer identity alone would look unchanged.

Instances carry the raw WVP from `c58..c61` as `clipTransform`. A separate
world matrix does not exist for shader draws, so rather than guess at a
factorisation on the game's render thread, the host divides out the shared
camera. The handful of genuinely fixed-function draws also carry a real
`worldTransform`, flagged `kInstanceWorldValid`, which gives the host a
known-good answer to check its factorisation against.

### 6.3 The Vulkan device

Ray tracing is required, not optional — an adapter without it has nothing to
offer a process that exists solely because RT is unreachable in the game — so
device selection rejects such adapters and says which extensions or features
were missing. Measured on this machine:

```
device            NVIDIA GeForce RTX 4090 Laptop GPU
api version       1.3.260          device-local mem  15.7 GB
shader group handle size / alignment   32 / 32
shader group base alignment            64
max ray recursion depth                31
max geometry / instance / primitive    16777215 / 16777215 / 536870911
```

The Intel integrated GPU is enumerated and correctly rejected. All ten
acceleration-structure and ray-pipeline entry points resolve through
`vkGetDeviceProcAddr`; none is exported by the loader library.

### 6.4 Acceleration structures

Measurement drove the shape here. The obvious mapping — one geometry, one
BLAS — does not survive this game: 82% of geometry ids are used in exactly
one draw ever, and the worst offenders are 4-vertex quads drawn hundreds of
times per frame from a shared index buffer (crowd, shadows, grass). One BLAS
per quad would mean tens of thousands of structures, nearly all built once
and discarded.

So geometry is split by size:

| Class | Threshold | Handling |
| ----- | --------- | -------- |
| Sprites | <= 2 triangles | transformed to world on the CPU, merged into **one** BLAS rebuilt per frame, one TLAS instance |
| Meshes | larger | persistent BLAS keyed by geometry id, rebuilt only when the content hash changes |

The persistent path is what makes CPU-skinned players affordable: their
topology never changes, so the buffers are reused and only the vertices are
re-uploaded.

**Transforms.** A TLAS instance needs an affine object-to-world matrix, but
shader draws only produce a combined WVP (c58..c61). The world matrix is
recovered as `WVP * inverse(VP)` using the view-projection shared by the
frame. That inversion is only as good as the VP it is given, so every
recovered matrix is checked for affinity and the failures are counted rather
than silently used — a wrong VP would otherwise scatter geometry across the
scene with no error anywhere.

**Cache coherence needs a channel, not an invariant.** The producer decides
whether to re-send a geometry from its own record of what it has already
sent; the host caches independently. The original arrangement was meant to
make disagreement impossible — the producer forgets an id after 60 frames of
not drawing it, the host keeps one for 900, so anything the producer believes
cached must still be there.

That reasoning is wrong, and measurably so: a live frame had **278 of 992
instances** referencing geometry the host had evicted and the producer would
never send again. The two windows count different clocks. The producer's
counts frames in which it *drew* the geometry; the host's counts frames it
actually *processed*. Whenever the host runs behind, the second advances more
slowly, and geometry drawn continuously can age out of the host's cache while
the producer still believes it is there. After that it is permanent, because
nothing ever tells the producer otherwise.

So the host asks instead of the producer guessing. The ring header carries a
256-slot table of geometry ids the host was told to draw and does not have;
the producer clears those from its sent-set at the start of each frame, and
the next draw re-sends them. One writer, one reader, and a lost request is
simply made again next frame, so a release store on the count is all the
ordering required. `PESModHost --resendtest` runs the whole loop in one
process with no GPU.

**Batching.** Builds are prepared first and recorded into a single command
buffer with sub-allocated scratch. Submitting and waiting per structure
measured ~7 ms of overhead each; the self-test dropped from 22.5 ms to
7.8 ms on that change alone, and the cost no longer scales with BLAS count.

`PESModHost --astest` pushes a synthetic scene through the real transport and
asserts the whole chain, so the GPU path is testable without a running game.

**Recovering the view-projection.** The first attempt took the VP from the
game's `SetTransform` calls and rejected 798 of 850 world transforms as
non-affine — the matrices the fixed-function pipeline is told about are not
the ones the shaders use. `ResolveViewProjection` instead treats the VP as
unknown and scores candidates against the data: the previous frame's answer,
the `SetTransform` product, then the distinct clip transforms the instances
themselves carry (an instance with an identity world has a clip transform
that *is* the VP). The winner must explain at least half the sampled
instances. Only the affinity check made the original failure visible at all,
which is the argument for keeping checks on things that "cannot" go wrong.

### 6.5 What is scene geometry, and what only looks like it

A draw list is not a scene. Two kinds of draw have to be removed before the
geometry means anything to a ray tracer.

**Topology.** An acceleration structure has one triangle topology: a list.
Direct3D 8 has three, and this game overwhelmingly uses the one that is not
a list — 403 of a match frame's 405 world draws are strips. Handing a
strip's indices to a consumer that groups them in threes builds triangles
from vertices that were never adjacent: a 513-triangle strip becomes 171
arbitrary ones spanning the whole mesh. That produced the long slivers that
fanned across the frame once the camera was right. The producer now expands
strips and fans itself, drops the degenerate triangles strips use to stitch
runs together (13,357 of 21,441 in that frame), and rebases indices onto the
vertex slice it actually sent — the two numbers only it knows.

**Geometry that must not occlude.** The sky is a dome about 75 units from
the camera; the stadium is thousands of units away. A rasteriser handles
that with draw order and `D3DRS_ZWRITEENABLE = FALSE`, so the sky never
writes depth and never occludes. A ray tracer has no equivalent — whatever
is nearest along the ray wins — so every primary ray hits the sky first and
the frame is a flat wall.

`ZWRITEENABLE` turns out to be exactly the right discriminator, because it
is the game stating the property we need. On a measured match frame it
selects 37 of 405 world draws and 242 of 21,441 triangles: the sky dome and
a set of two-triangle overlay sprites, and nothing else. Those instances
carry `kInstanceNoDepthWrite` and the builder leaves them out of the TLAS.
Before that rule the sky owned 98.5% of the frame; after it, the pitch,
stands, roof and floodlights are all visible.

---

### 6.6 Skinning

Accepting the 40-byte layout made players appear — scattered flat across the
pitch. Their vertex positions are not in object space at all.

The game skins on the GPU with a matrix palette. `vs_000B`, which draws
player bodies:

```
mul   r10, v2, c57.z        ; palette row = index colour * scale
mov   a0.x, r10.x
m4x3  r11, v0, c0           ; c[a0.x + 0..2] is one bone
mul   r11.xyz, r11.xyzz, v1.x
...                          ; repeated per influence
m4x4  oPos, r11, c58        ; only then the view-projection
```

So `v0` is bone-local, `v2` holds palette indices and `v1` the weights, both
as D3DCOLOR. Transformed by `c58` alone, every vertex lands wherever its
bone-local coordinates happen to fall.

**Detection is from the bytecode, not the declaration.** An `m4x3` against a
constant register is the signature, and the count of them is the number of
influences — one for `vs_0009`, two for `vs_0017`, three for `vs_000B`. The
unskinned shaders contain no `m4x3` at all. Matching on the declaration
instead would be wrong: a 24-byte pre-lit vertex also carries a D3DCOLOR,
and reading that diffuse colour as a bone index would wreck geometry that is
already correct.

**Skinning runs on the host, not the producer.** The bone-local vertices
never change, so they upload once and stay cached; only the pose crosses the
process boundary, at 57 float4 registers per instance. Skinning in the
producer would instead re-send 26,285 vertices every frame — about 48 MB/s,
on a path where geometry messages are deliberately undroppable. The pose is
applied in the same pass that copies vertices into the BLAS buffer, which
has to touch every vertex anyway.

D3DCOLOR expands to `(R,G,B,A)` as `xyzw` while the bytes are stored BGRA, so
influence *b* reads byte `2-b`. Getting that backwards swaps bone indices for
weights and produces geometry that is wrong without looking obviously wrong.

A skinned structure is rebuilt every frame by definition: its content hash
never changes because the vertices do not — the pose does. It is also built
with `PREFER_FAST_BUILD` rather than `PREFER_FAST_TRACE`, since a trace
quality optimised for is thrown away a frame later; static geometry keeps
fast-trace, being traced against for thousands of frames.

**One structure per instance, not per geometry.** Acceleration structures are
keyed by geometry id, which is right for a static mesh drawn twice and wrong
for a skinned one: each instance carries its own pose. Keyed by id alone,
two instances of one skinned mesh both queued a build into the same
`dstAccelerationStructure` in a single command buffer. That is explicitly
undefined, and it lost the device — on a frame with 868 instances over 154
geometries, so it happened immediately and often.

The key now mixes in which occurrence of that geometry the instance is, and
the batch additionally refuses any job whose destination another job already
claimed, counting it rather than trusting the keying. Structures unused for
120 frames are dropped, because a mesh drawn eight times one frame and three
the next leaves five behind that still reference live geometry.

This is also what the validation layer is for. Nothing about the symptom —
an unwritten output image — pointed at duplicate build destinations; the
VUID named it outright.

---

### 6.7 Coplanar layers, and why the pitch was noise

The game builds its pitch from **seven draws that all lie on the same
plane**: grass, then six blended overlays for markings, wear and shadow,
all covering the same 6,700 x 4,300 units. A rasteriser composites them in
draw order. A ray tracer sees seven surfaces at identical depth and takes
whichever traversal reaches first, which varies per ray — so the pitch came
out as black and white noise rather than grass.

The answer is in the textures. Alpha, measured across each layer:

| Layer | Texture | Max alpha | Below 0.5 |
| ----- | ------- | --------- | --------- |
| grass | 243 | 1.00 | 0% |
| shadow wash | 1297 | 0.14 | 100% |
| wear | 184 | 0.25 | 100% |
| markings | 185 | 0.85 | 97% |
| markings | 242 | 1.00 | 63% |
| surround | 9 | 1.00 | 5% |
| surround | 191 | 1.00 | 20% |

The two layers that contributed pure noise never reach alpha 0.5 anywhere,
while the grass beneath them is opaque everywhere. So `primary.rahit` tests
alpha and calls `ignoreIntersectionEXT` below a 0.5 cutoff: the noise layers
drop out of the tie entirely and the parts genuinely painted on the pitch
stay.

Three things have to line up for an any-hit shader to run at all, and each
silently disables it on its own:

- the geometry must not carry `VK_GEOMETRY_OPAQUE_BIT_KHR`;
- the instance must not carry `FORCE_OPAQUE` — the builder sets
  `FORCE_NO_OPAQUE` for blended or alpha-tested draws and `FORCE_OPAQUE`
  for the rest, so the fast path still covers most of the scene;
- **the ray must not pass `gl_RayFlagsOpaqueEXT`**, which overrides both.
  That last one is what kept the shader dormant after everything else was
  right, and it produces no error of any kind.

Shadow rays deliberately run the any-hit too, so the transparent overlays do
not shadow the grass they lie on. The self-test carries a fully transparent
bright-red instance on empty background: if the alpha test stops working it
paints red where nothing should be, and the check fails.

---

### 6.8 The ray tracing pipeline

Four shaders in three groups: raygen, two miss (sky and shadow), one
closest-hit. Rays are built by unprojecting NDC through the recovered inverse
view-projection at depth 0 and 1 — the camera and the geometry therefore come
from the same matrix, which is what makes a basis recovered only up to an
affine change of basis usable at all.

The SBT respects `shaderGroupHandleSize`, `shaderGroupHandleAlignment` and
`shaderGroupBaseAlignment` as three separate limits. On this GPU they are 32,
32 and 64; a table packed at handle size alone works for the raygen record
and then mis-addresses the miss records.

Hit shading uses `VK_KHR_ray_tracing_position_fetch` to read the triangle's
vertices in the hit shader, which avoids binding every mesh's vertex and index
buffers just to compute a geometric normal. Shading matches what `vs_0007`
does per vertex — clamped N·L against the directional light, plus the
hemisphere term, plus ambient — with a traced shadow ray replacing the game's
projected blob. Albedo is a placeholder: the game's baked vertex colour
already contains its own lighting, so using it would double-light the scene
(§4.3), and textures are not yet bound into the hit shader.

Two orientation hazards, both silent when wrong:

- **Matrices.** The game is row-vector row-major; GLSL is column-major and
  evaluates `M * v`. Those cancel, so the matrix is uploaded verbatim and no
  transpose happens anywhere. Adding one "for correctness" breaks it.
- **Y.** The launch index counts down from the top row, but D3D's NDC y is
  +1 at the top, so the raygen inverts y. Without it every frame is a perfect
  vertical mirror — which no coverage or pixel-count check notices. The
  self-test now asserts orientation directly: every vertex in the synthetic
  scene has y >= 0 and the test camera only scales, so all of the geometry
  must land above the midline.

**An empty alpha channel is not transparency.** 106 of 3,273 captured
textures are A8R8G8B8 with alpha zero in every texel: the game allocated an
alpha format and only ever wrote colour into it. Read as coverage, every
surface using one vanishes — the pitch went twice, once for X8R8G8B8 and
again for this. They are forced opaque on upload and counted, so the
per-frame report still says how many.

This is corrected rather than merely reported because the case has one
meaning left. While A8R8G8B8 and X8R8G8B8 shared a wire code, an empty alpha
channel was ambiguous between "no alpha" and "wrong format", and silently
forcing opacity would have hidden the second. X8 has its own code now.

The traced frame is written out by `image_write.cpp` — PNG through WIC by
default, PPM when the extension asks for it, since the self-test parses the
pixels back and Windows will not preview a PPM.

---

## 7. Roadmap

Done: the transport, the device, scene reconstruction, acceleration
structures, and a ray tracing pipeline that traces every live frame. What
remains is what turns a traced image into a renderer.

1. **Ordered transparency.** Alpha testing removes the coplanar tie but is
   binary: a surface is either there or absent. The game composites its
   overlays, so partially transparent markings are currently all-or-nothing.
   Proper blending needs ordered traversal, and is the next real step.
2. **Sprites carry no material.** The merged sprite batch bakes triangles
   from many draws into one structure, which loses their textures and UVs;
   it renders white. Giving each source draw its own geometry within that
   BLAS would restore per-sprite materials without going back to one
   structure per quad.
3. **Normals.** Currently geometric, so everything is faceted. The 32-byte
   layout carries real per-vertex normals; they have to be carried through
   the scene stream and interpolated in the hit shader.
4. **Presentation and input.** The host owns the visible window; the game
   window becomes the input sink. The HUD draws are composited on top
   unchanged, per the scope decision to leave the UI alone. Until this
   exists, `--save-every` writing PNGs is the only way to see output.
5. **Overlap.** Every submit is followed by `vkQueueWaitIdle`, so structure
   builds and traces are fully serialised. Fences would let them overlap.
6. **Lighting beyond the game's rig.** The captured constants are the seed —
   directional `c95`/`c94`, hemisphere `c93`/`c92`/`c91`, specular
   `c63`/`c70` — then physical stadium floodlights and a sky model.
7. **Path tracing + denoise.** DLSS Ray Reconstruction is already present in
   the game folder, making it the natural denoiser target.
