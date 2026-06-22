# PESMod internals

A summary of how PESMod is put together and of the game internals its
hooks rely on. This is an overview for contributors — it is not a complete
disassembly. Function addresses below refer to the specific retail
executable the mod was reverse-engineered against; treat them as labels,
not as portable constants.

## Architecture

```
ASI loader → DllMain (dllmain.cpp)
           → ModInitialise (mod_core.cpp)
               ├─ Logger::Init        (utils/logger.*)   PESMod.log
               ├─ Config::Load        (utils/config.*)   PESMod.ini
               ├─ MH_Initialize       (MinHook)
               └─ HooksRegistry::InstallAll (hooks/hooks_registry.cpp)
                     ├─ ClubHooks::Register        (the bulk of the mod)
                     └─ LeagueTeamsHook::Register
```

`ModShutdown` disables all hooks and uninitialises MinHook on a clean
unload. The `MenuHooks` and `PlayerHooks` groups are experimental and
disabled by default in `hooks_registry.cpp`.

## Hooking conventions

The hook source uses a consistent naming scheme:

- `fn_*` — a typed function pointer to a resolved game function.
- `orig_*` — a MinHook trampoline that calls the unmodified original.
- `hook_*` — our replacement. The standard shape is **INI override first,
  original behaviour as fallback**: look up a custom value for the current
  team / player / slot, and if none exists call `orig_*` so stock content
  is untouched.
- `RE_*` — a faithful re-implementation of a game function in C++, used
  when a hook needs to reproduce the original's logic exactly (for example
  to bound a table walk that the original left unbounded).

Function pointers are declared (`extern`) in `club_hooks_common.h`,
defined in `club_hooks_register.cpp`, and bound to addresses plus
installed via MinHook in `ClubHooks::Register()`.

## Kit-data pipeline

Files: `club_hooks_kit_data.cpp`. Configured by `kits/<teamID>.ini`.

Each team has a fixed kit-data record. Three accessors read different
sub-regions of it, one entry per kit variant (0–3):

| Region   | Accessor (game)        | Offset | Stride | Notes                      |
| -------- | ---------------------- | ------ | ------ | -------------------------- |
| Colors   | `GetTeamKitData`  (0x00865240) | `+0x000` | `0x3E` | shirt/shorts/socks/GK colors + model bytes |
| Extra B  | `GetTeamKitDataB` (0x00865380) | `+0x100` | `0x18` | club + national            |
| Extra C  | `GetTeamKitDataC` (0x00865430) | `+0x160` | `0x30` | club only; kit-graphic id at `+0x03` |

The club record is `0x220` bytes; national records are `0x160`.

**Why custom teams crash the stock game.** The accessors locate a team's
record by indexing a table (`GetClubTeamKitBase`: `base + (teamID-0x40) *
0x220`) with **no upper bound**. A custom or out-of-range team ID walks
past the table and returns a non-NULL but garbage pointer; the next byte
dereference faults. PESMod replaces the accessors: for a team with a
`kits/<id>.ini` it returns a synthesised, cached buffer; otherwise it
returns NULL safely, and the per-match preloader / descriptor builders are
hooked to treat NULL as "skip this team" instead of dereferencing it.

The synthesised buffer is built by `TryLoadCustomKitData`, cached for the
process lifetime (pointers handed to the kit pipeline must stay stable),
and never evicted.

## Squad-data pipeline

Files: `club_hooks_squad_data.cpp`. Configured by `squad/<teamID>.ini` and
`player/<playerID>.ini`.

Squad data flows through two chokepoints:

- **`GetTeamPlayerID(teamID, slot, mode)` (0x00862710)** — returns the
  player ID occupying a squad slot. Hooked to return `slot_N` from
  `squad/<teamID>.ini` when present, else the original.
- **`GetPlayerRecord` worker (0x00861b90)** — returns a pointer to a
  player's `0x7C`-byte record. Hooked to synthesise a record from
  `player/<playerID>.ini` (optionally cloned from a template player) when
  present, else the original. The mod hooks the internal *worker*, not the
  public wrapper at `0x00861d20`, because the match-time per-field
  accessors call the worker directly.

Player IDs are partitioned by range. The important ones for modding:

- `0..4999` — stock players (main table).
- `0x8000..0x80B7` — 184 reserved IDs the stock game never populates;
  the natural home for **new** custom players.

Both squad and player hooks use the same INI-override-then-trampoline
shape and a process-lifetime, pointer-stable cache. The worker accessor
uses a register-only calling convention, so it is hooked through a small
`__declspec(naked)` bridge that marshals the registers into a normal C
handler (and a matching tail-jump helper to call the original).

### Player record layout

The `0x7C`-byte player record, as used by the team-edit save path. Offsets
not listed are either strings or not yet mapped; cloning a template player
(the default in `player/<id>.ini`) fills them with sensible values.

| Offset      | Size | Field                                            |
| ----------- | ---- | ------------------------------------------------ |
| `+0x00`     | 0x20 | Long name (UTF-16, 16 wchars)                    |
| `+0x20`     | 0x10 | Shirt name (ASCII, NUL-terminated)               |
| `+0x30`     | 0x02 | Call-name id (u16)                               |
| `+0x32..+0x4B` | 1 each | Attributes: attack, defence, balance, stamina, speed, acceleration, response, agility, dribble accuracy/speed, short-pass accuracy/speed, long-pass accuracy/speed, shot accuracy/power/technique, free-kick accuracy, swerve, heading, jump, teamwork, technique, aggression, mentality, GK skills |
| `+0x58`     | 0x01 | Height                                           |
| `+0x59`     | 0x01 | Weight                                           |
| `+0x6F`     | 0x01 | "Special team" byte                              |

The high bit (`0x80`) of several stat bytes appears to carry a separate
still-unmapped flag (position / movement). The `byte_<off>` INI key lets
you set a whole byte or toggle just bit 7; see `samples/player/32768.ini`.

## Leagues and the selection panel

Files: `club_hooks_leagues.cpp`, `club_hooks_panel*.cpp`,
`league_teams_hook.cpp`. Configured by `leagues/setup.ini` and
`leagues/<slot>.ini`.

The league-selection screen is fed by `SetTeamList` (`FUN_00b07b00`) and a
stride-8 slot loader. PESMod hooks the loader so that, after the stock
slots are placed, `leagues/<n>.ini` overrides slot `n` — its league name,
display/logo id, and the `team_<k>` IDs shown when that league is picked.

At startup `LoadLeaguesSetup()` scans `leagues/*.ini` and raises
`MAX_PANEL_SLOTS` to `highest_id + 1`, so the panel allocation, scroll
controller, and teardown all grow to fit extra slots without a code
change. Each league holds up to `TEAM_SLOT_COUNT` (20) teams — a fixed
structural limit of the buffer the game reads.

### Panel "Layer A–D" labels

The panel source comments use informal labels for the stages of extending
the selection panel beyond its stock 20 seats. They are referenced here so
those comments make sense:

- **Layer A** — reachability: allocate and navigate to extra slots so they
  exist and can be selected.
- **Layer B** — placement: anchor the extra container "pages" on screen
  via the `[panel] pos_x/y/z` keys (`club_hooks_panel.cpp`,
  `club_hooks_leagues.cpp`). Per-slot positioning within a page is still
  baked into the layout art and is out of scope.
- **Layer C** — rendering: the node / memory layout that actually draws
  extra slots (`club_hooks_panel_v2.cpp`).
- **Layer D** — instrumentation: logging hooks over child-node and
  position-setter calls used to map the render path while solving Layer C
  (`club_hooks_screen.cpp`, `club_hooks_register.cpp`).

## UI BIN layout format

The selection-screen "chrome" is described by UI layout `.bin` blobs the
game relocates and registers at load time. Two families exist:

- a **simple** layout: a texture descriptor plus a section directory of
  draw / helper records (positions are `q4` fixed-point, UVs are `q12`);
- a **rich** layout: texture descriptor plus element / group / name banks.

The full structure is captured as an
[010 Editor](https://www.sweetscape.com/010editor/) template at
[`templates/ui_bin_generalized.bt`](templates/ui_bin_generalized.bt), and
the [`tools/ui-bin-viewer/`](../tools/ui-bin-viewer) web app parses and
renders the same format for visual inspection. The league-selection panel
binds one such rich layout (resource id `0x10385`) as its asset bank.
