# Quake for openfpgaOS — Setup

This folder ships the engine (`quake.elf`), the per-variant `.ini`
configs, and any CD audio tracks. You supply the game data — Quake's
PAK files are not redistributable, so drop them in yourself before
launching.

## 1. Add the game files

All PAK files go under this folder (`Assets/quake/common/`). Filenames
are matched case-insensitively, so the mixed casing below is just for
parity with the original CD-ROM layout.

### Quake — registered (full)
```
common/pak0.pak
common/PAK1.PAK
```

### Quake — shareware (Episode 1 only)
```
common/pak0.pak
```

### Mission Pack 1 — Scourge of Armagon (Hipnotic)
```
common/pak0.pak
common/PAK1.PAK
common/HIPNOTIC/PAK0.PAK
```

### Mission Pack 2 — Dissolution of Eternity (Rogue)
```
common/pak0.pak
common/PAK1.PAK
common/ROGUE/PAK0.PAK
```

### X-Men: Ravages of Apocalypse
```
common/pak0.pak
common/PAK1.PAK
common/XMEN/PAK0.PAK
common/XMEN/PAK1.PAK
common/XMEN/PAK2.PAK
```

### Optional — original CD soundtrack
Rip your Quake CD to 16-bit signed PCM `.bin` tracks and drop them in:
```
common/cd/Quake (USA) (Track 02).bin
common/cd/Quake (USA) (Track 03).bin
...
common/cd/Quake (USA) (Track 11).bin
```
Missing tracks are silently skipped — the game runs fine without them.

## 2. Pick a variant from the library

Each variant has its own entry in the Pocket library, backed by an
instance JSON in `Assets/quake/ThinkElastic.quake/`:

| Library entry  | Plays           | Requires                                                |
| -------------- | --------------- | ------------------------------------------------------- |
| `Quake`        | id1 (full)      | `pak0.pak`, `PAK1.PAK`                                  |
| `Shareware`    | E1 only         | `pak0.pak`                                              |
| `Hipnotic`     | Mission Pack 1  | id1 PAKs + `HIPNOTIC/PAK0.PAK`                          |
| `Rogue`        | Mission Pack 2  | id1 PAKs + `ROGUE/PAK0.PAK`                             |
| `X-Men`        | X-Men: ROA      | id1 PAKs + `XMEN/PAK0.PAK`, `PAK1.PAK`, `PAK2.PAK`      |

Saves (`s0.sav` … `s9.sav`) are created automatically in the Pocket's
non-volatile save area — no setup needed.

## 3. Controls

### Analogue Pocket (default mapping)

| Input                         | Action                          |
| ----------------------------- | ------------------------------- |
| D-pad ↑ / ↓                   | Move forward / back             |
| D-pad ← / →                   | Turn left / right               |
| L1 + D-pad ← / →              | Strafe left / right             |
| L2                            | Strafe left                     |
| R2                            | Fire                            |
| Left stick                    | Move + strafe                   |
| Right stick                   | Look (mouse-style aim)          |
| A                             | Fire                            |
| R1 + A                        | Look up                         |
| B (tap, < 500 ms)             | Use / open                      |
| B (hold)                      | Run                             |
| R1 + B                        | Look down                       |
| X                             | Jump                            |
| R1 + X                        | Next weapon                     |
| Y                             | Crouch / move down              |
| R1 + Y                        | Previous weapon                 |
| Start                         | Menu                            |
| Select                        | Show scores / status overlay    |

### Notes

- **Run is a *hold* of B**, not a toggle. Short taps (under half a
  second) trigger the use action — doors, buttons, lifts.
- **R1 is a chord modifier.** Holding R1 reroutes A / B / X / Y to
  look-up / look-down / next-weapon / previous-weapon.
- **Digital pads via the link port (SNAC).** Wired retro controllers
  are detected automatically; the right-stick mouse-look path stays
  disabled for them so the cursor doesn't drift on its own.
- **Rebinds.** Standard Quake `bind` commands work. Edit the per-variant
  `.cfg` next to its `.ini` (`quake.cfg`, `hipnotic.cfg`, `rogue.cfg`,
  `shareware.cfg`, `xmen.cfg`) and the engine will pick them up on
  next launch.
