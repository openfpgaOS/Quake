# Quake for Analogue Pocket

A native port of id Software's **Quake** (1996) for the [Analogue Pocket](https://www.analogue.co/pocket), running as an openFPGA core. The engine is the original GPL Quake source, ported to the openfpgaOS runtime: game logic and the classic software renderer run on a VexiiRiscv RISC-V soft-CPU @ 100 MHz, with perspective-correct span rasterization, z-buffering and audio mixing offloaded to FPGA hardware.

- Full campaign, both official mission packs, and the X-Men: Ravages of Apocalypse total conversion
- Original CD soundtrack, streamed from disc rips and mixed in hardware
- 10 save slots and per-game settings, persisted to the SD card
- Twin-stick controls on analog controllers, classic d-pad scheme on the handheld
- Analogizer support: analog video out (RGBS/RGsB/YPbPr/Y·C) and SNAC controllers (NES, SNES, PC Engine, PSX, DB15)

Game data (`.pak` files) is **not** included. Use the data from your own copy of Quake — the shareware episode is freely distributable if you don't own the full game.

## Installing the core

1. Download `quake-vX.Y.Z.zip` from [releases](releases/).
2. Unzip it onto the root of your Pocket's SD card (it merges into `Assets/`, `Cores/`, and `Platforms/`).
3. Add game data as described below.

The core appears on the Pocket under **openFPGA → Quake**.

## Installing the PAKs

All game data goes into **`/Assets/quake/common/`** on the SD card. Filenames and subdirectories are case-sensitive — use exactly the names below.

| Game (selected on the Pocket) | Required files in `Assets/quake/common/` |
|---|---|
| **Quake** (registered) | `pak0.pak`, `PAK1.PAK` |
| **Shareware** (episode 1) | `pak0.pak` (shareware v1.06) |
| **Hipnotic** — Scourge of Armagon | `pak0.pak`, `PAK1.PAK`, `HIPNOTIC/PAK0.PAK` |
| **Rogue** — Dissolution of Eternity | `pak0.pak`, `PAK1.PAK`, `ROGUE/PAK0.PAK` |
| **X-Men** — Ravages of Apocalypse | `pak0.pak`, `PAK1.PAK`, `XMEN/PAK0.PAK` … `XMEN/PAK3.PAK` |

Each entry is a separate "game" in the Pocket's library list — pick the one you want to launch. The registered `pak0.pak`/`PAK1.PAK` come from any classic Quake release (CD, Steam, GOG). Mission pack PAKs go into their own uppercase subdirectory as shown.

## Adding the soundtrack

The Quake CD carries the game data on track 1 and Trent Reznor's soundtrack on audio tracks **2–11**. The core streams those audio tracks from raw CDDA files placed in **`/Assets/quake/common/cd/`**, named exactly:

```
Assets/quake/common/cd/Quake (USA) (Track 02).bin
Assets/quake/common/cd/Quake (USA) (Track 03).bin
   ...
Assets/quake/common/cd/Quake (USA) (Track 11).bin
```

Each file is headerless 16-bit / 44.1 kHz stereo PCM (standard CD audio). Depending on what you have, getting there looks like:

- **Redump-style rip (one `.bin` per track + `.cue`)** — you're done: copy the ten audio-track `.bin` files into `cd/`. The required names match the redump convention for *Quake (USA)*; if your rip is another region/edition, just rename the files to the names above (the names matter, the region doesn't).
- **Single-file image (`image.bin` + `image.cue`)** — split it into per-track files first, e.g. with [binmerge](https://github.com/putnam/binmerge) (`--split`) or any cue tool, then copy/rename the audio tracks as above.
- **Audio files (FLAC/WAV/MP3, e.g. your own CD rip or the OST)** — convert each track to raw PCM with ffmpeg:

  ```bash
  ffmpeg -i "02 - Quake Theme.flac" -f s16le -ar 44100 -ac 2 "Quake (USA) (Track 02).bin"
  ```

Sanity checks: the files must be raw — about 10.1 MB per minute of audio; if playback is loud static, the source wasn't decoded (e.g. a renamed `.flac`). Track numbering starts at **02** (track 1 is the data track, there is no music for it).

Music is optional — the game runs silently without the `cd/` directory — and is currently wired for the registered **Quake** game (the shareware and mission-pack entries don't bind music tracks). Music volume is in the in-game Options menu. Playback costs almost no CPU: tracks stream via DMA into a ring buffer and the FPGA mixer resamples them to 48 kHz in hardware.

## Saves and settings

- **10 save slots** per game, stored as `s0.sav` … `s9.sav` in `Assets/quake/common/` — saved through the normal Quake menu.
- Settings are persisted per game (`quake.cfg`, `shareware.cfg`, `hipnotic.cfg`, `rogue.cfg`, `xmen.cfg`).
- Advanced launch options live in the matching `.ini` (e.g. `quake.ini`) — command-line arguments, config name, and the UART render trace (`HOST_SPEEDS=1`) for debugging.

## Controls

### Handheld / digital pads

| Input | Action |
|---|---|
| D-pad ↑ / ↓ | move forward / back |
| D-pad ← / → | turn left / right |
| L1 + D-pad ← / → | strafe left / right |
| A | fire |
| B (tap) | open / use |
| B (hold) | run |
| X | jump |
| Y | crouch / swim down |
| R1 + A / B | look up / down |
| R1 + X / Y | next / previous weapon |
| L2 | run |
| R2 | fire |
| Start | menu |
| Select | scores / status |

### Analog controllers (dock, Bluetooth pads, PSX-Analog over SNAC)

| Input | Action |
|---|---|
| Left stick | move forward / back + strafe left / right |
| Right stick | look (yaw + pitch) |
| R2 | fire |

Buttons behave as in the digital table. In menus: d-pad to navigate, A/B to select, Start for back/escape.

Dock/Bluetooth pads use a reduced look sensitivity (`joy_docklook` console cvar, 0–1, default 0.8) and a softened movement-stick response near the centre (full deflection still gives full speed). A SNAC DualShock (PSX Analog) runs 1:1 on both sticks.

## Analogizer

The core supports the [Analogizer](https://github.com/RndMnkIII/Analogizer) adapter for analog video output and original controllers. Everything is configured from the Pocket menu under **Core Settings → Interact**:

- **Enable Analogizer** — Off / On
- **Analogizer Video Out** — RGBS, RGsB, YPbPr, Y/C NTSC, Y/C PAL, and RGBHV scanline modes (0 %, 50 %, HQ2x)
- **Video H / V Offset** — picture centering for your display
- **SNAC Adapter** — None, DB15 (+Fast), NES, SNES (+ A,B↔X,Y swap), PCE 2-button / 6-button / Multitap, PSX (+Fast / Analog / Analog Fast)
- **SNAC Controller Assignment** — SNAC→P1, SNAC→P2, or both

Notes:

- Digital SNAC pads (NES/SNES/PCE/DB15) use the d-pad control scheme; analog axes are automatically ignored for them, so there is no stick drift.
- **PSX Analog** (and Analog Fast) gives full twin-stick controls on a DualShock.
- Settings persist across sessions.

## Building from source

Requires a `riscv64-elf-gcc` toolchain (the build auto-detects `riscv64-unknown-elf-gcc` too).

```bash
cd src/quake
make            # build → build/quake/ (ELF + complete core layout)
make copy       # copy onto a mounted Pocket SD card
make package    # create releases/quake-vX.Y.Z.zip
```

The engine sources live in `src/quake/engine/` — stock id Software files plus the openfpgaOS drivers (`sys_of.c`, `vid_of.c`, `snd_of.c`, `cd_of.c`, `in_of.c`). The renderer keeps Quake's edge/span pipeline on the CPU and submits perspective-correct textured spans, z-writes, blits and fills to the FPGA GPU; alias models, sprites and particles draw CPU-side into an uncached framebuffer alias.

## Acknowledgements

- **id Software** — Quake, and for releasing the source under the GPL. Quake® is a registered trademark of id Software LLC.
- **Hipnotic Interactive** and **Rogue Entertainment** — the official mission packs.
- **Zero Gravity Entertainment** — X-Men: Ravages of Apocalypse.
- **Analogue** — the Pocket and the openFPGA framework.
- **RndMnkIII** — the Analogizer project that this core's analog video and SNAC support builds on.
- **ThinkElastic / openfpgaOS** — the RISC-V SoC, GPU gateware, kernel and SDK this port runs on.
- **musl libc** — the C library statically linked into the core.

## License

The Quake engine code (`src/quake/engine/`) is licensed under the **GNU GPL v2**, following id Software's source release. The openfpgaOS SDK components carry their own license (Apache-2.0, see SPDX headers). Game data is copyrighted by id Software and is not distributed with this core.
