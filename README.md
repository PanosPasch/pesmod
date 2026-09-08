# PESMod

PESMod is a modding plugin for **Pro Evolution Soccer 5 / 6** (Konami's
*Winning Eleven 9 / 10* family). It loads into the game as an `.asi`
plugin and uses runtime function hooks to extend the game without
touching the original executable on disk. It lets you:

- **Add custom leagues** and control which teams appear in each league
  slot of the selection screen — including extra league slots beyond the
  stock layout.
- **Override team kits** (shirt / shorts / socks / GK colors and kit
  models) from simple INI files, for any team.
- **Build custom squads and players** — assign players to squad slots and
  define brand-new players (name, shirt name, full attribute set).
- **Fix crashes** that the stock game hits when custom / out-of-range team
  IDs are introduced by the above.

Everything is driven by plain-text INI files dropped into the game
folder, so you can retune the mod without recompiling.

> **Disclaimer.** PESMod is an unofficial, fan-made modification. It is
> not affiliated with or endorsed by Konami. It ships **no** game assets —
> you supply your own legally-owned copy of the game. It works by patching
> game code in memory at runtime; use it at your own risk and keep backups
> of your save data.

---

## How it works

The game executable is a 32-bit Windows process. PESMod is built as a
32-bit DLL with an `.asi` extension. An **ASI loader** (a small proxy DLL
that the game loads at startup, such as
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader))
calls `LoadLibrary` on every `*.asi` file in the game folder, which runs
PESMod's `DllMain`.

From there PESMod:

1. Reads `PESMod.ini` for global settings.
2. Initialises [MinHook](https://github.com/TsudaKageyu/minhook) and
   installs hooks over a set of game functions (kit data, squad data,
   league/team-list loading, and the league-selection UI panel).
3. Each hook first looks for a matching INI override and otherwise falls
   through to the game's original behaviour, so anything you don't
   configure stays exactly as the stock game played it.

See [`docs/INTERNALS.md`](docs/INTERNALS.md) for the architecture and a
summary of the reverse-engineered game internals the hooks rely on.

> **Compatibility.** The hooks target a specific 32-bit retail build of the
> game (the executable the addresses were reverse-engineered from). A
> different game version or patch level will have different function
> addresses; those constants live in `src/hooks/*` and would need to be
> re-located for another build.

---

## Requirements

**To use a build:**

- A 32-bit retail PES5/PES6 installation (see the compatibility note above).
- An ASI loader installed in the game folder.

**To build from source:**

- Windows with **Visual Studio 2022** (or the Build Tools) including the
  **32-bit (x86) C++ toolset**.
- **CMake 3.20+**.

MinHook is vendored under [`include/MinHook/`](include/MinHook) as a
prebuilt 32-bit import library plus `MinHook.x86.dll`, so there is nothing
else to download.

---

## Building

PESMod **must** be built as 32-bit — the game is a 32-bit process and
Windows will silently refuse to load a 64-bit DLL into it.

```sh
# Configure (32-bit). Optionally point GAME_DIR at your install so the
# build auto-deploys; default is C:/Games/PES6.
cmake -B build -A Win32 -DGAME_DIR="C:/Path/To/Your/Game"

# Build
cmake --build build --config Release
```

The output is `build/Release/PESMod.asi`. If `GAME_DIR` points at a valid
game folder, the post-build step also copies `PESMod.asi` and the required
`MinHook.x86.dll` there automatically.

---

## Installation

Place the following files in your game folder (next to the game `.exe`):

| File              | Where it comes from                              |
| ----------------- | ------------------------------------------------ |
| `PESMod.asi`      | your build output (`build/Release/PESMod.asi`)   |
| `MinHook.x86.dll` | `include/MinHook/lib/MinHook.x86.dll`            |
| `PESMod.ini`      | the [`PESMod.ini`](PESMod.ini) in this repo      |
| an ASI loader     | e.g. Ultimate ASI Loader (provides the proxy DLL)|

Then create the configuration folders you need alongside them:

```
<game folder>/
├─ <game>.exe
├─ <asi loader proxy>.dll
├─ PESMod.asi
├─ MinHook.x86.dll
├─ PESMod.ini
├─ leagues/      ← custom league slots (optional)
├─ kits/         ← per-team kit overrides (optional)
├─ squad/        ← per-team squad assignments (optional)
└─ player/       ← custom player definitions (optional)
```

Launch the game. PESMod writes a `PESMod.log` next to the game executable
that you can use to confirm it loaded and to debug configuration; set
`verbose_logging = 1` in `PESMod.ini` for a live debug console as well.

---

## Configuration

Global options live in [`PESMod.ini`](PESMod.ini). Per-team and per-player
content lives in the `leagues/`, `kits/`, `squad/`, and `player/` folders.
Ready-to-copy examples are in [`samples/`](samples):

| Folder      | File pattern              | Purpose                                  | Sample                                            |
| ----------- | ------------------------- | ---------------------------------------- | ------------------------------------------------- |
| `leagues/`  | `setup.ini`, `<slot>.ini` | League slots and selection-panel layout  | [`samples/leagues/`](samples/leagues)             |
| `kits/`     | `<teamID>.ini`            | Kit colors and models for a team         | [`samples/kits/200.ini`](samples/kits/200.ini)    |
| `squad/`    | `<teamID>.ini`            | Player IDs assigned to a team's slots    | [`samples/squad/250.ini`](samples/squad/250.ini)  |
| `player/`   | `<playerID>.ini`          | A custom player's name and attributes    | [`samples/player/32768.ini`](samples/player/32768.ini) |

File names use **decimal** IDs (`kits/200.ini`, not `kits/0xC8.ini`).
Inside the files, numeric values accept decimal, hex (`0x...`), or octal.

See [`docs/CONFIG.md`](docs/CONFIG.md) for the complete key reference for
every file, and the comments inside each sample for inline guidance.

---

## Repository layout

```
PESMod/
├─ CMakeLists.txt        Build script (32-bit shared lib → PESMod.asi)
├─ PESMod.ini            Global runtime settings (ship next to the game)
├─ src/                  Mod source
│  ├─ dllmain.cpp        ASI entry point
│  ├─ mod_core.*         Startup / shutdown
│  ├─ hooks/             Game function hooks (kits, squads, leagues, UI)
│  ├─ render/            D3D8 interception + Vulkan ray-traced renderer
│  ├─ patching/          Memory patch + pattern-scan helpers
│  └─ utils/             Logger and INI config reader
├─ include/MinHook/      Vendored MinHook (BSD-2-Clause)
├─ samples/              Example INI configuration files
├─ docs/                 Internals, renderer, config reference, 010 template
└─ tools/ui-bin-viewer/  Web tool for inspecting the game's UI .bin layouts
```

---

## Renderer

`src/render/` holds a Direct3D 8 interception layer: PESMod wraps `IDirect3D8`
and `IDirect3DDevice8` so it can observe, record and ultimately replace the
game's rendering. It is the foundation for a Vulkan ray-traced renderer.

It is **off by default** — without `[render] enabled = 1` in `PESMod.ini` the
game's Direct3D path is left completely untouched. With it on, PESMod can
record per-frame statistics and dump a complete frame (geometry, textures, and
every draw call's pipeline state) for analysis.

Because the game is a 32-bit process and NVIDIA's 32-bit Vulkan ICD does not
expose the ray tracing extensions, hardware ray tracing cannot run in-process;
the renderer is therefore split between the in-game ASI and a separate 64-bit
render host. See [`docs/RENDERER.md`](docs/RENDERER.md) for the measurements
behind that, the reverse-engineered engine internals, and the architecture.

---

## UI BIN viewer

[`tools/ui-bin-viewer/`](tools/ui-bin-viewer) is a small Vite + React +
Three.js web app for inspecting the game's UI layout `.bin` files (texture
descriptors, sections, and draw records) in 3D. It is independent of the
mod build. See its [README](tools/ui-bin-viewer/README.md) to run it.

A companion [010 Editor](https://www.sweetscape.com/010editor/) binary
template for the same format is in
[`docs/templates/ui_bin_generalized.bt`](docs/templates/ui_bin_generalized.bt).

---

## Credits

- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu —
  vendored under `include/MinHook/`, licensed BSD-2-Clause (see the header
  in [`MinHook.h`](include/MinHook/include/MinHook.h)).
- The PES/WE modding community for documenting the game's data formats.

## License

PESMod's own code is released into the public domain under
[The Unlicense](LICENSE). Do whatever you like with it. The vendored
MinHook library keeps its own BSD-2-Clause license.
