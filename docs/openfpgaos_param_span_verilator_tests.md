# openfpgaOS Param-Span Verilator Test Plan

## Context

Quake world rendering now uses `CMD_DRAW_PARAM_SPAN_LIST` with
`OF_GPU_PARAM_ATTR_PERSP_Q29` for perspective-correct world surfaces.
The remaining artifact is a deterministic floor texture line/grid pattern
that is visible when the player is stationary and mostly hidden by
forward/backward movement.

Quake-side diagnostics did not identify a CPU cache or flip-ordering issue:

- Draining before surface-cache reuse did not help.
- Flushing the GPU texture cache after surface-cache upload did not help.
- Draining before `CMD_FLIP` did not help.
- Cleaning surface-cache bytes on every draw did not help.
- Forcing surface-cache rebuilds every frame did not help.
- Splitting param-span commands to 4 records did not help.
- Disabling `pq_gpu_param` removes the floor artifact, but falls back to an
  older path with wall perspective errors and lower performance.
- Q16 param perspective produces a different stable line/square precision
  pattern, so it is not a usable replacement.

These tests should focus on the RTL/software contract for
`CMD_DRAW_PARAM_SPAN_LIST`, especially Q29 perspective span generation,
texture addressing, segment/tail advance, and framebuffer byte-lane writes.

## Test Cases

### 1. `param_q29_single_span_floor_oracle`

Drive one `CMD_DRAW_PARAM_SPAN_LIST` command with:

- `attr_mode = OF_GPU_PARAM_ATTR_PERSP_Q29`
- `span_axis = OF_GPU_PARAM_AXIS_X`
- one horizontal floor-like span
- `count >= 160`
- Quake-style clamp bounds

Compare every framebuffer byte against a C++ software oracle that computes
the expected Q29 projected texture coordinates and texel color.

### 2. `param_q29_segment_boundaries`

Render long Q29 spans that cross internal segment boundaries.

Use counts and inspection points around:

- `15/16`
- `31/32`
- `47/48`
- `63/64`

Assert there is no discontinuity in projected `s/t`, texel address, color, or
z at segment boundaries.

### 3. `param_q29_tail_advance_counts`

Render Q29 spans with counts:

- `1`
- `2`
- `3`
- `4`
- `7`
- `8`
- `15`
- `16`
- `17`
- `31`
- `32`
- `33`
- `127`
- `255`

Verify the final pixels and internal tail advance match the oracle.

### 4. `param_q29_grazing_floor_stress`

Use floor-like projection values:

- large `attr_dv`
- small/moderate `attr_du`
- small `zi` gradient
- long horizontal spans

This should mimic the floor surfaces where Quake shows the line/grid pattern.
Compare framebuffer output byte-for-byte against the oracle.

### 5. `param_q29_texture_address_bitwalk`

Use a diagnostic texture where texel values encode texture address bits.

Example:

```c
texel = (s ^ (t << 4) ^ (addr >> n)) & 0xff;
```

Run Q29 spans across this texture and verify exact output. This should expose
wrong address bits, carry errors, low-bit loss, or incorrect wrap behavior as
line/square artifacts.

### 6. `param_q29_wrap_16_32_64_128`

Run the same Q29 command shape with texture sizes:

- `16x16`
- `32x32`
- `64x64`
- `128x128`

Set `tex_w_mask` and `tex_h_mask` accordingly. Verify wrapping at both axes,
especially where `s/t` cross texture boundaries many times in one span.

### 7. `param_q29_clamp_edges`

Test projected coordinates below, at, and above:

- `clamp_min[0]`
- `clamp_max[0]`
- `clamp_min[1]`
- `clamp_max[1]`

Verify clamping matches Quake semantics exactly and does not create repeated
edge lines.

### 8. `param_q29_rebase_equivalence`

Render the same spans two ways:

1. Absolute record coordinates:
   - `fb_base = framebuffer`
   - records use absolute `u/v`

2. Rebased records:
   - `fb_base += base_v * stride + base_u`
   - `z_base += base_v * z_stride + base_u * 2`
   - `attr_origin[i] += base_u * attr_du[i] + base_v * attr_dv[i]`
   - records use local `u/v`

The framebuffer output must match exactly.

### 9. `param_q29_multirecord_1_to_512`

Test record counts:

- `1`
- `2`
- `3`
- `4`
- `5`
- `8`
- `16`
- `64`
- `511`
- `512`

Compare one multi-record command against separate single-record commands with
equivalent parameters.

### 10. `param_record_pair_odd_counts`

Test odd record counts:

- `1`
- `3`
- `5`
- `7`
- `9`
- `511`

Verify packed record-pair decoding and padded final record handling.

### 11. `param_q29_z_none_vs_z_write_color`

Render the same Q29 color span with:

- `z_mode = OF_GPU_PARAM_Z_NONE`
- `z_mode = OF_GPU_PARAM_Z_WRITE_ZI`

Color output must match exactly. The only expected difference is zbuffer
contents.

### 12. `param_q29_ztest_write_masked`

Use `z_mode = OF_GPU_PARAM_Z_TEST_WRITE` with a preloaded Quake-compatible
16-bit zbuffer.

Include passing and failing pixels. Verify:

- passing pixels write color and z
- failing pixels write neither color nor z
- z comparison matches Quake-compatible 16-bit depth semantics

### 13. `param_q29_skip_zero_ztest`

Use a texture containing transparent zero texels with `OF_GPU_SPAN_SKIP_ZERO`.

Verify skipped texels write neither color nor z for:

- `OF_GPU_PARAM_Z_TEST_ZI`
- `OF_GPU_PARAM_Z_TEST_WRITE`

### 14. `param_q29_static_repeat_300`

Replay the same Q29 command stream for 300 frames.

For each frame:

- clear framebuffer
- submit identical commands
- wait for fence
- hash framebuffer

The hash must be identical every frame and match the oracle.

### 15. `param_q29_origin_phase_sweep`

Sweep small increments of `attr_origin[0]`, `attr_origin[1]`, and
`attr_origin[2]`.

This mimics forward/backward player movement changing texture phase. Verify
every phase against the oracle and look for phase-specific line/grid failures.

### 16. `param_q29_rotation_gradient_sweep`

Sweep `attr_du` and `attr_dv` values approximating yaw rotation while keeping
origin phase fixed.

Verify all outputs against the oracle. This separates rotation-only changes
from forward/backward distance changes.

### 17. `param_q29_large_signed_range`

Stress signed numerator ranges:

- large positive `attr0/attr1`
- large negative `attr0/attr1`
- mixed-sign `attr_du/dv`
- values close to 32-bit boundaries

Catch signed shift, truncation, and overflow bugs.

### 18. `param_q29_near_zero_zi`

Use very small `zi` and tiny `zi` gradients.

Verify singularity handling only triggers where the oracle expects. This is
important for grazing floors where depth changes slowly across long spans.

### 19. `param_fb_byte_lane_strobes`

Render spans starting at framebuffer x positions:

- `0`
- `1`
- `2`
- `3`

Use counts that end on every byte lane. Verify byte strobes and write packing
do not create vertical line artifacts or corrupt neighboring pixels.

### 20. `param_interleaved_state_reset`

Interleave commands:

- clear rect
- direct affine span helper
- Q29 param span
- Q29 param z-write span
- fence
- another Q29 param span

Verify no stale state carries between command types or z modes.

### 21. `quake_bad_floor_capture_replay`

Add a Quake capture for one bad floor surface and replay it in Verilator.

Capture:

- full `of_gpu_param_span_list_t`
- all span records
- texture dimensions and masks
- texture bytes or texture hash plus embedded fixture
- expected framebuffer rectangle from CPU oracle
- zbuffer state when z mode is enabled

This should become the primary regression once a reproducible bad floor case
is captured.

## Oracle Requirements

The Verilator harness should include a C++ software oracle for the public
command semantics, not a copy of the RTL pipeline. The oracle should compute:

- framebuffer address from base, major/minor steps, axis, and record `u/v`
- Q29 projected `s/t`
- clamp behavior
- texture wrap/mask behavior
- transparent skip behavior
- Quake-compatible 16-bit z write/test behavior
- expected byte-lane framebuffer writes

The tests should report the first mismatching pixel with:

- command name
- record index
- pixel index
- framebuffer address
- expected and actual color
- expected and actual `s/t` texel address when available
- expected and actual z when z mode is enabled
