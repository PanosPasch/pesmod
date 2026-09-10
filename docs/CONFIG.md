# Configuration reference

Every PESMod feature is driven by INI files placed in the game folder.
This document is the complete key reference; the files in
[`samples/`](../samples) are ready-to-copy starting points with the same
information inline.

**Conventions**

- File names use **decimal** IDs: `kits/200.ini`, `squad/250.ini`,
  `player/32768.ini`. (`kits/0xC8.ini` will *not* be found.)
- Inside files, numeric **values** accept decimal, hex (`0x...`), or octal.
- Colors are hex `#RRGGBB` or `0xRRGGBB`.
- All keys are optional unless stated otherwise; omitted keys keep the
  stock value.

---

## `PESMod.ini` — global settings

Lives next to the game executable.

| Section     | Key               | Default | Meaning                                                            |
| ----------- | ----------------- | ------- | ------------------------------------------------------------------ |
| `[general]` | `enabled`         | `1`     | `0` loads the mod but installs no hooks — the game runs untouched. |
| `[debug]`   | `verbose_logging` | `0`     | `1` opens a live console and echoes the log. `PESMod.log` is always written. |

---

## `leagues/` — league slots and selection-panel layout

### `leagues/setup.ini` — global league/panel options

Applied once at startup.

| Section    | Key                | Default | Meaning                                                                 |
| ---------- | ------------------ | ------- | ----------------------------------------------------------------------- |
| `[setup]`  | `team_slot_count`  | `20`    | How many `team_<n>` keys to read per `leagues/<id>.ini`. Clamped to 20. |
| `[panel]`  | `f0_side0`         | `5`     | Columns of the side-0 panel cell grid.                                  |
| `[panel]`  | `f4_side0`         | `4`     | Rows of the side-0 panel cell grid.                                     |
| `[panel]`  | `f0_side1`         | `4`     | Columns of the side-1 panel cell grid.                                  |
| `[panel]`  | `f4_side1`         | `5`     | Rows of the side-1 panel cell grid.                                     |
| `[panel]`  | `panel_count`      | auto    | Number of background container panels. Auto-grows to `max(3, ceil(slots/8))`; minimum 3. |

> The stock grids are 5×4 and 4×5 — both 20 cells, matching a league's
> 20-team maximum. Raising columns × rows above 20 produces empty/garbage
> cells in team selection, because each league only has 20 teams to fill
> them. Leave these alone unless your team data covers the larger grid.

### `leagues/<slot>.ini` — one league slot

The file's numeric stem is the panel slot index. Dropping in a file with
an index `>= 20` automatically grows the panel to fit (no code change).

| Section    | Key             | Meaning                                                                       |
| ---------- | --------------- | ----------------------------------------------------------------------------- |
| `[league]` | `name`          | League name (ASCII) shown on the slot.                                        |
| `[league]` | `display_id`    | Logo / display ID for the slot.                                               |
| `[league]` | `team_0`…`team_19` | Team IDs shown when this league is selected. Indices past `team_slot_count` are ignored; missing slots become the empty sentinel `0xFFFF`. |
| `[panel]`  | `pos_x` `pos_y` `pos_z` | Screen anchor for this container *page*. Only applies when the file is the first slot of a page (a multiple of 8: `0`, `8`, `16`, `24`, …). Use it to spread out extra pages (slots 24+), which otherwise stack at the stock anchor. |
| `[panel]`  | `scale` `scale_y` | Parsed but not yet applied.                                                 |

---

## `kits/<teamID>.ini` — team kit override

Overrides a team's kit colors and models. Works for stock teams too
(useful for retextures). Four variant sections, all optional:
`[variant0]` (home), `[variant1]` (away), `[variant2]` (third),
`[variant3]` (GK alt). A missing section falls back to safe defaults.

| Key             | Field                                  |
| --------------- | -------------------------------------- |
| `shirt_primary` | Primary shirt color (RGB hex)          |
| `color1`        | Secondary color slot (usually unused)  |
| `shirt`         | Shirt color                            |
| `shorts`        | Shorts color                           |
| `socks`         | Socks color                            |
| `gk`            | Goalkeeper color                       |
| `shirt_model`   | Shirt mesh id (default `0x20`)         |
| `shorts_model`  | Shorts mesh id (default `0x64`)        |
| `socks_model`   | Socks mesh id (default `0x64`)         |
| `edited`        | `1` = edited render path (recommended); `0` = stock path (requires recognised model bytes) |

Colors are stored as RGB555, so the low 3 bits per channel are truncated.

---

## `squad/<teamID>.ini` — team squad assignment

| Section   | Key                  | Meaning                                                                          |
| --------- | -------------------- | -------------------------------------------------------------------------------- |
| `[squad]` | `slot_0`…`slot_31`   | Player ID for each squad slot. Use `0xFFFF` to leave a slot at its stock value.  |
| `[squad]` | `attr_0`…`attr_31`   | Squad-number / position byte (u8) for each slot. Omitted slots use stock values. |

**Player ID ranges**

- `0`…`4999` — stock players.
- `0x7020`…`0x70A0` — alias-team reserved range (avoid for new players).
- `0x8000`…`0x80B7` — 184 IDs the stock game never uses; the home for new
  custom players. Pair each with a `player/<id>.ini`.

---

## `player/<playerID>.ini` — custom player

Synthesises a player record (`0x7C` bytes), cached for the process
lifetime. All keys live in `[player]`.

| Key            | Field                                                                            |
| -------------- | -------------------------------------------------------------------------------- |
| `template`     | Player ID to clone the base record from before overrides (default `1`). `0xFFFFFFFF`/`-1` skips cloning (starts all-zero — not recommended). |
| `name`         | Long name (UTF-16) at `+0x00`, max 15 wchars. Source is the system ANSI codepage. |
| `shirt_name`   | Shirt name (ASCII) at `+0x20`, max 15 chars.                                     |
| `callname_id`  | Call-name index (u16) at `+0x30`.                                                |
| `special_team` | Byte at `+0x6F`.                                                                 |
| *stat keys*    | One u8 each: `attack`, `defence`, `balance`, `stamina`, `speed`, `acceleration`, `response`, `agility`, `dribble_accuracy`, `dribble_speed`, `short_pass_accuracy`, `short_pass_speed`, `long_pass_accuracy`, `long_pass_speed`, `shot_accuracy`, `shot_power`, `shot_technique`, `free_kick_accuracy`, `swerve`, `heading`, `jump`, `teamwork`, `technique`, `aggression`, `mentality`, `gk_skills`, `height`, `weight`. |
| `byte_<off>`   | Raw byte handle at offset `<off>` (0…0x7B; any base). Value `= <number>` sets the whole byte; value `= true`/`false` sets/clears just bit 7 (`0x80`). Use for the still-unmapped fields and high-bit flags. |

Cloning from a template (the default) is recommended because several
record fields are not yet mapped; cloning gives them sensible values and
you override only the fields you care about. See
[INTERNALS.md](INTERNALS.md#player-record-layout) for the record layout.

---

## Notes on experimental hooks

The `MenuHooks` and `PlayerHooks` groups in `src/hooks/` are experimental
and **disabled by default** (commented out in `hooks_registry.cpp`). They
read some additional `PESMod.ini` sections (e.g. `[stamina]`) if enabled in
source, but those are not part of a normal build and are intentionally not
shipped in the default `PESMod.ini`.

---

Part of [PESMod](../README.md), licensed under the GNU General Public
License v3 or later. See [LICENSE](../LICENSE).
