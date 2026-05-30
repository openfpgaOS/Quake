# Quake 3.0 for Analogue Pocket

Quake 3.0 is an openfpgaOS-based Analogue Pocket core for id Software's Quake engine. It runs the Quake executable from `quake.elf`, with each game or expansion instance selecting its command line, config file, and PAK slot map through an ini file.

The core is versioned as `1.0.0` in `dist/quake/Cores/ThinkElastic.quake/core.json`.

## Installation

Build the core:

```sh
make -C src/quake
```

The complete SD card layout is generated in:

```text
build/quake/
```

Copy `build/quake/Cores`, `build/quake/Assets`, and `build/quake/Platforms` to the SD card root. If replacing an older build, remove stale files from:

```text
Assets/quake/ThinkElastic.quake/
Assets/quake/common/
```

Old instance JSONs can keep obsolete slot mappings alive, so stale files should be deleted when changing instances.

## Required Data Files

Game data belongs in:

```text
Assets/quake/common/
```

Current supported layout:

| File | Used By |
| --- | --- |
| `pak0.pak` | Shareware, Quake, mission packs, X-Men |
| `PAK1.PAK` | Registered Quake, mission packs, X-Men |
| `HIPNOTIC/PAK0.PAK` | Scourge of Armagon |
| `ROGUE/PAK0.PAK` | Dissolution of Eternity |
| `XMEN/PAK0.PAK` | X-Men |
| `XMEN/PAK1.PAK` | X-Men |
| `XMEN/PAK2.PAK` | X-Men |
| `XMEN/PAK3.PAK` | X-Men |
| `XMEN/PROGS.DAT` | X-Men loose override |

The core does not ship commercial Quake data.

## Instances

Instances live in:

```text
Assets/quake/ThinkElastic.quake/
```

Supported instances:

| Instance | Ini | Command Line | Config Slot File |
| --- | --- | --- | --- |
| `Shareware.json` | `shareware.ini` | none | `shareware.cfg` |
| `Quake.json` | `quake.ini` | none | `quake.cfg` |
| `Hipnotic.json` | `hipnotic.ini` | `-hipnotic` | `hipnotic.cfg` |
| `Rogue.json` | `rogue.ini` | `-rogue` | `rogue.cfg` |
| `X-Men.json` | `x-men.ini` | `-game xmen` | `xmen.cfg` |

The APF instance JSON only names files and slot IDs. Runtime behavior is controlled by the ini in slot 2.

## Ini Format

Ini files live in:

```text
Assets/quake/common/
```

Example:

```ini
[os]
ELF=quake.elf
ARGS=-game xmen

[quake]
CONFIG=xmen.cfg
PAK_id1_0=slot:4
PAK_id1_1=slot:5
PAK_xmen_0=slot:6
PAK_xmen_1=slot:7
PAK_xmen_2=slot:20
PAK_xmen_3=slot:21
```

`[os]` is consumed by openfpgaOS before launching the app. `ELF` selects slot 3's executable and `ARGS` becomes the Quake command line.

`[quake]` is consumed by Quake. `CONFIG` names the per-instance config file in slot 8. `PAK_<game>_<index>` maps a Quake game directory and PAK index to an APF slot. Missing PAK keys are intentionally skipped; the core does not fall back to unrelated stale files when an ini is present.

## Slot Layout

| Slot | Purpose |
| ---: | --- |
| 0 | Instance JSON selector |
| 1 | `os.bin` |
| 2 | Per-instance `.ini` |
| 3 | `quake.elf` |
| 4-7 | Primary data slots |
| 8 | Per-instance nonvolatile config `.cfg` |
| 10-19 | Quake save slots `s0.sav` through `s9.sav` |
| 20-22 | Extra X-Men data slots |

Slot 8 is shared by the selected instance only through the filename in that instance JSON. If a wrong or stale config is present, Quake rejects obvious foreign config text and writes a clean Quake config marker.

## Controls

### Movement and View

| Input | Action |
| --- | --- |
| D-pad up/down | Move forward/backward |
| D-pad left/right | Turn left/right |
| L1 + D-pad left/right | Strafe left/right |
| L2 / R2 | Strafe left / right |
| Left stick | Move forward/back + turn |
| Right stick | Mouse look |

### Action Buttons

| Input | Action |
| --- | --- |
| A | Fire |
| R1 + A | Look up |
| B tap, less than 500 ms | Open/Use |
| B hold | Run |
| R1 + B | Look down |
| X | Jump |
| R1 + X | Next weapon |
| Y | Crouch / move down |
| R1 + Y | Previous weapon |

### System

| Input | Action |
| --- | --- |
| Start | Menu |
| Select | Automap/status overlay |

In menus and console-like navigation screens, D-pad navigates, A/B confirm, and Start acts as Escape/back.

## Saves and Configs

Save slots are named `s0.sav` through `s9.sav` and map to APF nonvolatile slots 10-19. Saves are namespaced by game/mod so unrelated Quake mods do not silently appear as valid saves. Foreign saves are hidden from load and marked before overwrite in the save menu.

Per-instance configs use slot 8:

| Instance | Config |
| --- | --- |
| Shareware | `shareware.cfg` |
| Quake | `quake.cfg` |
| Hipnotic | `hipnotic.cfg` |
| Rogue | `rogue.cfg` |
| X-Men | `xmen.cfg` |

Quake configs start with:

```text
// Quake 3.0 config
```

If Quake prints many `Unknown command` lines for Doom-style keys such as `mouse_sensitivity`, `sfx_volume`, `key_menu_*`, or `joyb_*`, the config slot contains stale data from another core. Use the current `quake.elf`; it rejects and resets those foreign configs.

## Renderer and Audio Notes

The renderer uses openfpgaOS GPU acceleration for world spans and related hot paths. Runtime capability detection is used; the app does not reserve hardcoded SDRAM GPU batch regions.

Audio uses the openfpgaOS mixer path for sound effects. Menus and level transitions stop active sounds to avoid stale playback while loading.

## Development

Common commands:

```sh
make -C src/quake          # build and assemble build/quake
make -C src/quake clean    # remove build output
make -C src/quake package  # create a release zip
```

The generated app ELF is:

```text
build/quake/Assets/quake/common/quake.elf
```

The source ELF before packaging is:

```text
.obj/quake/app.elf
```

## Repository Layout

| Path | Purpose |
| --- | --- |
| `src/quake/engine/` | Quake engine and openfpgaOS platform code |
| `src/sdk/` | Bundled openfpgaOS SDK headers/build support |
| `dist/quake/Cores/ThinkElastic.quake/` | Core APF metadata |
| `dist/quake/Assets/quake/ThinkElastic.quake/` | Instance JSON files |
| `dist/quake/Assets/quake/common/` | Ini files copied into the SD common folder |
| `dist/quake/Platforms/` | Platform metadata and image |
| `runtime/` | FPGA bitstream, loader, and OS binary |

## Troubleshooting

`Unknown command "mouse_sensitivity"` or many Doom-looking commands:

The per-instance config slot contains stale foreign text. Rebuild and deploy the current `quake.elf`; it validates and resets foreign configs.

Instance loads the wrong data:

Remove stale files from `Assets/quake/ThinkElastic.quake/` and redeploy the current instance JSONs. The core no longer uses APF `memory_writes` or app-id registers for instance selection.

Missing expansion assets:

Check the exact common layout above. The instance JSON must point at files that actually exist on the SD card, including case on filesystems that preserve it.

Save list shows strange or unrelated entries:

Those are stale or foreign save slots. Current saves include a namespace and the menu hides saves from other Quake games/mods.
