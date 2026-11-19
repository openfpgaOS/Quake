# Quake 1.0.8 for Analogue Pocket

2026-11-18. Pocket only; the MiSTer package stays at 1.0.7. The previous
Pocket release was 1.0.5.

## Changes

- Engine optimization pass across the renderer, sound, QuakeC interpreter,
  client and server code. In the port's CPU simulator the bit-exact part of
  this pass does about 30% less CPU work with byte-identical frames on three
  timedemos and a live single-player run.
- Single-precision trigonometry replaces the double-precision `sin`/`cos`
  path in view and math code. This one is not bit-exact: rendering can differ
  slightly from 1.0.5.
- CD music: a finished track chunk is folded into the playback ring over
  several frames instead of in one frame.
- Louder output. The bundled openfpgaOS 0.9.1 os25 core divides the final
  audio mix by 4 instead of 16, so effects and music are 12 dB louder.
  Several loud effects at once can now reach full scale and are limited
  there.
- GPU commands go directly into the GPU's command ring instead of being
  written to SDRAM and read back. The bundled os25 core no longer has the
  old readback path, so the executable and the core must be installed
  together.
- Faster CPU in the bundled os25 core: shift instructions complete in one
  pipeline stage instead of two, which saves 7.5% of cycles in the core's
  CPU test programs.

## Use the os25 variant

Only the os25 core was updated and tested. The bundled `os30.rbf_r` is
unchanged from the previous runtime: it keeps the quieter mixer and has not
been run with this executable.

## Verification

The packaged os25 core, kernel and executable were booted and played on a
Pocket. Frame rate was not measured on hardware, the new levels were not
measured for clipping, and no long-session soak was made. The desktop build
of this tree does not currently compile, so it was not used as a reference.

Three timing races between the CPU and GPU found during the optimization
pass are not fixed in this release. They affect render paths that are off by
default, such as `r_waterwarp 1`.

## Install

Extract the ZIP at the SD-card root and merge its folders. Put your `pak`
files in `Assets/quake/common/` as before. Install the whole ZIP:
`quake.elf`, `os25.rbf_r` and `os.bin` belong together and should not be
mixed with files from earlier releases.

The bundled os25 core stays at the requested 100 MHz and does not close
main-clock setup timing: seed 28 has setup slack -0.601 ns and worst hold
+0.092 ns. This remains an overclocked build.
