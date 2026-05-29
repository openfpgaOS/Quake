# CR: GPU Lightmap Shading for Param-Spans

## Summary

Add a GPU command for Quake-style lightmapped world spans:

```text
GPU_CMD_DRAW_PARAM_SPAN_LIGHTMAP_LIST
OF_HW_GPU_PARAM_SPAN_LIGHTMAP
```

The command should reuse the existing param-span projection path, but shade raw
texture pixels with a small per-surface light grid before the colormap lookup.
Quake will still calculate lightstyles, dynamic lights, and the final
`blocklights[]` grid on the CPU. The GPU will consume that grid and perform the
per-pixel texture + light interpolation + colormap operation.

This replaces the expensive CPU `R_DrawSurfaceBlock8_mipN()` surface-cache bake
without falling back to the current single-light-per-surface approximation.

## Problem

Quake has two relevant world-surface paths today:

1. Correct CPU-lit cache path
   - CPU runs `R_BuildLightMap()`.
   - CPU runs `R_DrawSurfaceBlock8_mipN()`.
   - The GPU draws from the already-lit surface cache.
   - This is visually correct, but cache misses and dynamic lights cost CPU time
     and require D-cache clean/drain for rebuilt cache blocks.

2. Fast raw-texture GPU path
   - CPU runs `R_BuildLightMap()`.
   - GPU samples the raw texture and uses one colormap row for the whole
     surface.
   - This avoids the cache bake, but loses lightmap gradients and looks wrong
     on surfaces with shadows or light transitions.

The missing OS/GPU primitive is a span command that keeps Quake's CPU light
accumulation, but moves the repeated per-pixel block drawer work into hardware.

## Requirements

The new command must:

- Preserve Quake lightstyles and dynamic lights by consuming CPU-built
  `blocklights[]`.
- Preserve Quake texture wrapping from `texturemins[]`.
- Match the existing mip block lighting closely, preferably byte-for-byte
  against `R_DrawSurfaceBlock8_mip0/1/2/3()`.
- Work with `OF_GPU_PARAM_ATTR_PERSP_Q29`, including nonzero
  `q29_attr_shift`.
- Preserve the existing param-span z write/test behavior.
- Avoid a pointer to Quake's global `blocklights[]`; that buffer is reused for
  every surface while GPU commands are still queued.
- Avoid per-pixel random SDRAM reads for light values. That can be slower than
  the CPU cache bake.

## Proposed API

Add a new hardware capability bit:

```c
#define OF_HW_GPU_PARAM_SPAN_LIGHTMAP (1u << 19)
```

Add a new command ID:

```c
#define GPU_CMD_DRAW_PARAM_SPAN_LIGHTMAP_LIST 0x49
```

Add a lightmap span helper type in `of_gpu.h`:

```c
enum {
    OF_GPU_PARAM_LIGHTMAP_U16_8_8 = 0
};

typedef struct {
    of_gpu_param_span_list_t span;

    /* Signed 16.16 offsets applied after local surface clamp and before
     * raw texture wrap. These are Quake's texturemins[] adjusted for mip. */
    int32_t tex_s_offset;
    int32_t tex_t_offset;

    /* Quake blocklights grid. Width/height are normally <= 18. */
    uint8_t lightmap_width;
    uint8_t lightmap_height;

    /* block size in mip pixels: 1 << light_block_shift.
     * Quake uses 4,3,2,1 for mips 0,1,2,3. */
    uint8_t light_block_shift;
    uint8_t light_format;

    /* Row-major U16 copy of Quake blocklights[] after R_BuildLightMap().
     * The SDK emitter copies this into the command stream. The GPU must not
     * retain or dereference this pointer after emit. */
    const uint16_t *lightmap;
} of_gpu_param_span_lightmap_t;
```

Add:

```c
void of_gpu_draw_param_span_lightmap_list(
    const of_gpu_param_span_lightmap_t *params,
    const of_gpu_param_span_record_t *records,
    uint32_t record_count);
```

SDK validation:

- Require `OF_HW_GPU_PARAM_SPAN_LIGHTMAP`.
- Require `OF_HW_GPU_PARAM_SPAN_LIST`.
- If `span.attr_mode == OF_GPU_PARAM_ATTR_PERSP_Q29` and
  `span.q29_attr_shift != 0`, also require
  `OF_HW_GPU_PARAM_SPAN_Q29_SCALE`.
- Reject zero `record_count`, zero dimensions, or dimensions above the hardware
  maximum.
- Accept `light_block_shift` in the range `1..4`.
- Pack `lightmap_width * lightmap_height` U16 entries into the command stream.

## Wire Format

Use a new command rather than extending `GPU_CMD_DRAW_PARAM_SPAN_LIST`. The
existing command remains unchanged.

Command word:

```text
bits 31:24  GPU_CMD_DRAW_PARAM_SPAN_LIGHTMAP_LIST
bits 23:0   payload_words
```

Payload words:

```text
words  1..30  same fields as GPU_CMD_DRAW_PARAM_SPAN_LIST
              word 29: record_count
              word 30: q29_attr_shift

word      31  tex_s_offset, signed 16.16
word      32  tex_t_offset, signed 16.16
word      33  lightmap_desc
              bits  7:0   lightmap_width
              bits 15:8   lightmap_height
              bits 19:16  light_block_shift
              bits 23:20  light_format
              bits 31:24  reserved, must be zero
word      34  lightmap_word_count

words 35..N   packed lightmap data, two U16 entries per word, row major
remaining    span records, packed exactly like GPU_CMD_DRAW_PARAM_SPAN_LIST
```

`payload_words` must include the fixed payload, packed lightmap words, and
packed span record words.

The inline lightmap data is intentional. It avoids lifetime bugs with Quake's
global `blocklights[]`, avoids another app-visible cache-clean requirement, and
lets the GPU consume the light grid sequentially into local storage.

## Rendering Semantics

The existing param-span math produces local surface coordinates:

```text
s_local_16.16
t_local_16.16
```

These are in mip-surface pixels, local to the surface cache rectangle, before
raw texture wrapping.

For each pixel:

1. Compute perspective-correct `s_local` and `t_local` using the existing
   param-span/Q29 path.

2. Clamp local coordinates with the existing `clamp_min[]` and `clamp_max[]`.

3. Sample the raw mip texture:

```text
tex_s = (s_local + tex_s_offset) >> 16
tex_t = (t_local + tex_t_offset) >> 16
texel = texture[(tex_t & tex_h_mask) * tex_width + (tex_s & tex_w_mask)]
```

4. Sample the staged light grid using Quake's mip block size:

```text
B  = 1 << light_block_shift
sx = s_local >> 16
tx = t_local >> 16
lx = sx >> light_block_shift
ly = tx >> light_block_shift
fx = sx & (B - 1)
fy = tx & (B - 1)
```

Clamp `lx` and `ly` so the four grid samples are in range.

5. Interpolate light in the same direction and precision as Quake's block
   drawers:

```text
ll0 = L[ly + 0][lx + 0]
lr0 = L[ly + 0][lx + 1]
ll1 = L[ly + 1][lx + 0]
lr1 = L[ly + 1][lx + 1]

left_step  = signed_arith_shift(ll1 - ll0, light_block_shift)
right_step = signed_arith_shift(lr1 - lr0, light_block_shift)

light_left  = ll0 + fy * left_step
light_right = lr0 + fy * right_step

row_step = signed_arith_shift(light_left - light_right, light_block_shift)
light    = light_right + (B - 1 - fx) * row_step
```

This mirrors the current software loops, where the block drawer walks pixels
from right to left and indexes:

```text
colormap[(light & 0xff00) + texel]
```

6. Write the shaded pixel:

```text
dst = colormap[(light & 0xff00) + texel]
```

7. Apply z write/test exactly like the existing param-span path.

## RTL Notes

In `gpu_core.v` or the equivalent GPU core:

1. Add decode for `GPU_CMD_DRAW_PARAM_SPAN_LIGHTMAP_LIST`.

2. Reuse the existing param-span header decode for words `1..30`.

3. Add registers for:

```text
tex_s_offset
tex_t_offset
lightmap_width
lightmap_height
light_block_shift
light_format
lightmap_word_count
```

4. Load the inline lightmap into local scratch storage before consuming span
   records. Required storage for Quake is small:

```text
18 * 18 * 16 bits = 5184 bits
```

5. Use local scratch for light samples. Do not perform four SDRAM light reads
   per output pixel.

6. Keep the existing texture fetch, colormap, framebuffer write, and z paths.

7. Preserve legacy behavior for `GPU_CMD_DRAW_PARAM_SPAN_LIST`.

## Quake-Side Use

Quake should add a third world-light mode:

```text
pq_gpu_world_light 0  CPU-lit surface cache
pq_gpu_world_light 1  old single-light raw-texture approximation
pq_gpu_world_light 2  new GPU lightmap span path
```

For mode 2:

1. `D_GpuLightSurface()` or a new helper still calls `R_BuildLightMap()`.
2. Quake packs the relevant `blocklights[]` entries to U16.
3. The span path emits `GPU_CMD_DRAW_PARAM_SPAN_LIGHTMAP_LIST` with:
   - raw mip texture pointer
   - texture width and masks
   - signed texture offsets from `texturemins[]`
   - local surface clamp extents
   - Q29 perspective params
   - inline light grid
   - packed span records
4. If the capability is absent, Quake falls back to the CPU-lit cache path.

## Acceptance Tests

Add Verilator/SDK tests in the openfpgaOS GPU acceptance suite.

Required tests:

1. `test_param_span_lightmap_mip0_matches_quake_blockdrawer`
   - Build a raw texture, a 3x3 light grid, and spans covering multiple
     16x16 blocks.
   - Compare framebuffer output against a C oracle that implements
     `R_DrawSurfaceBlock8_mip0()` lighting.

2. `test_param_span_lightmap_all_mips_match_quake_blockdrawer`
   - Repeat for `light_block_shift` 4, 3, 2, and 1.

3. `test_param_span_lightmap_texture_offset_and_wrap`
   - Use negative and positive `tex_s_offset`/`tex_t_offset`.
   - Verify raw texture wrapping uses `tex_w_mask` and `tex_h_mask`.

4. `test_param_span_lightmap_q29_shifted_projection`
   - Use `OF_GPU_PARAM_ATTR_PERSP_Q29` with nonzero `q29_attr_shift`.
   - Verify shaded output matches a wide CPU oracle.

5. `test_param_span_lightmap_z_write_matches_param_span`
   - Enable z write/test and verify zbuffer output matches the existing
     param-span command.

6. `test_param_span_lightmap_inline_lifetime`
   - Emit a command, mutate the source lightmap buffer before GPU execution,
     and verify output uses the emitted inline values.

7. `test_param_span_lightmap_capability_gate`
   - SDK helper must not emit the command when
     `OF_HW_GPU_PARAM_SPAN_LIGHTMAP` is absent.

## Compatibility

Old bitstreams do not understand command `0x49`, so applications must gate this
path on `OF_HW_GPU_PARAM_SPAN_LIGHTMAP`.

The existing `GPU_CMD_DRAW_PARAM_SPAN_LIST` format and behavior must remain
unchanged. Quake can keep the CPU-lit cache path as the fallback for old OS/GPU
builds.

## Non-Goals

This CR does not move Quake's full lighting model into hardware. The GPU should
not calculate BSP lightmap accumulation, lightstyles, ambient light, or dynamic
lights. The CPU already does that correctly in `R_BuildLightMap()`.

This CR also does not replace the Q29 shifted-param-span correctness work.
The new command depends on the same projection and z behavior as the existing
param-span path.

## Performance Expectation

This should remove most of the CPU time currently charged to
`R_DrawSurfaceBlock8_mipN()` and the associated cache clean for rebuilt surface
cache blocks. The GPU will do extra light interpolation per pixel, but that work
is simple and local if the light grid is staged into scratch storage.

The implementation is likely to be slower than the current single-light
approximation, but should be much faster than rebuilding lit surface cache data
on CPU when surfaces miss cache or dynamic lights invalidate them.
