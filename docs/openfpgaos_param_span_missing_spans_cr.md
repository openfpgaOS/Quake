# CR: Fix Missing Spans in GPU Perspective Span Rendering

## Summary

Quake still drops or flashes world-surface spans when any GPU perspective
world-span path is enabled:

```text
pq_gpu_persp=1
pq_gpu_param=1
pq_gpu_spanblit=0
```

The older helper-generated perspective path also fails:

```text
pq_gpu_persp=1
pq_gpu_param=0
pq_gpu_spanblit=0
```

That mode still shows missing black spans, runs slower, and produces visibly
wrong texture perspective. Disabling param-span z writes did not fix the black
spans either:

```text
pq_gpu_persp=1
pq_gpu_param=1
pq_gpu_zwrite=0
```

The same scene is stable when Quake uses CPU perspective stepping and CPU
framebuffer writes:

```text
pq_gpu_persp=0
pq_gpu_spanblit=0
```

This isolates the remaining visual corruption to the openfpgaOS GPU
perspective span execution path, not to page flipping, framebuffer address
ownership, palette programming, param-span z write, or Quake's basic CPU
renderer. `CMD_DRAW_PARAM_SPAN_LIST` is still involved because the SDK lowers
both the explicit param-span path and the older perspective helper path through
the unified param-span command, but the bug is no longer proven to be Q29-only.

This CR should also remove the experimental GPU lightmap capability from the
advertised OS/SDK surface. Quake testing shows that GPU lightmap shading does
not currently provide enough performance benefit to justify the extra command,
capability bit, SDK API, and RTL surface area.

## User-visible Problem

On Analogue Pocket hardware, Quake world surfaces intermittently show missing
horizontal spans. The symptom is most visible in bright, close-up, or high-FPS
scenes as black gaps or short missing strips in otherwise correctly rendered
walls/floors.

Important exclusions from the current diagnosis:

- The earlier blank-screen issue was caused by a Quake-side diagnostic cvar
  reading unregistered state during `VID_Init()`. That has been fixed and is
  not part of this CR.
- The basic CPU framebuffer path is correct.
- The simple affine span blit path is disabled in the failing test, so it is
  not required to reproduce the bug.
- `CMD_FLIP` ordering is not the trigger; the artifact follows GPU perspective
  rendering.
- Param-span z write is not the primary trigger; black spans remain with
  `pq_gpu_zwrite=0`.
- The older `pq_gpu_param=0` perspective helper path is also wrong, so the bug
  is not isolated to Quake's Q29 batch setup.

## Current Quake Test Matrix

Known-good but slower:

```text
pq_gpu_persp=0
pq_gpu_param=1
pq_gpu_spanblit=0
pq_gpu_world_light=0
```

Known-bad:

```text
pq_gpu_persp=1
pq_gpu_param=1
pq_gpu_spanblit=0
pq_gpu_world_light=0
```

Also known-bad:

```text
pq_gpu_persp=1
pq_gpu_param=0
pq_gpu_spanblit=0
pq_gpu_world_light=0
```

Useful isolation toggles:

```text
pq_gpu_persp=1, pq_gpu_param=1, pq_gpu_zwrite=0
pq_gpu_persp=1, pq_gpu_param=1, pq_gpu_safe_spans=1
pq_gpu_persp=1, pq_gpu_param=0
```

Observed interpretation:

- Disabling `pq_gpu_zwrite` does not fix it. Z-write may still have separate
  bugs, but it is not the cause of the blank spans.
- If `pq_gpu_safe_spans=1` fixes it, investigate multi-record batching,
  packed-record decoding, and record-chunk boundaries.
- `pq_gpu_param=0` does not fix it and also gives wrong perspective. Investigate
  the shared perspective segment FSM, helper lowering, and fragment issue/drain
  logic.

## Relevant OS Paths

Primary RTL:

```text
/home/alberto/Repos/openfpgaOS/src/fpga/common/gpu_core.v
```

Primary tests:

```text
/home/alberto/Repos/openfpgaOS/src/fpga/test/tb_gpu_acceptance_main.cpp
```

SDK/API command packing:

```text
/home/alberto/Repos/openfpgaOS/src/firmware/api/of_gpu.h
/home/alberto/Repos/openfpgaOS-SDK/src/sdk/include/of_gpu.h
```

Capability advertisement:

```text
/home/alberto/Repos/openfpgaOS/src/firmware/api/of_caps.h
/home/alberto/Repos/openfpgaOS/src/firmware/os/kernel/caps_table.c
/home/alberto/Repos/openfpgaOS-SDK/src/sdk/include/of_caps.h
```

## Suspected RTL Areas

The failure has two visible forms:

- missed or undrained generated spans, visible as black/untouched horizontal
  strips
- wrong perspective in the older helper path, visible as incorrect texture
  projection

Focus first on the shared perspective path and fragment drain/flush logic:

```text
of_gpu_draw_persp_span_group()
OF_GPU_PARAM_ATTR_PERSP
OF_GPU_PARAM_ATTR_PERSP_Q29
persp_active
persp_first_done
persp_seg_a_ready / persp_seg_b_ready
persp_swap_pending
sp_seg_left
sp_count
PSS_ADV / PSS_SLOPE / PSS_CONSTZ paths
src_done and fragment-pipe drain detection
S_FB_FLUSH
finish_fragment_stream_after_flush
fb_acc_valid / fb_acc_mask
```

Then review command/list control-flow around param-span batches:

```text
S_PAY_DATA
spanprod_prepare_next_record_chunk
spanprod_records_left / spanprod_record_count
S_SPANPROD_SELECT
S_SPANPROD_SETUP
S_SPANPROD_CAPTURE
z_acc_valid / z_acc_mask
```

The legacy helper path is especially important because it uses `attr_mode =
OF_GPU_PARAM_ATTR_PERSP` rather than `OF_GPU_PARAM_ATTR_PERSP_Q29`. A regression
that only exercises Q29 can pass while Quake still fails with `pq_gpu_param=0`.

Texture-coordinate math should be checked against the Quake CPU renderer for
both modes, not only against helper-vs-param internal equivalence.

The bug should not be worked around by adding frame-level fences or forcing
Quake to wait for the GPU after every surface. Those approaches hide the
symptom and lose the intended batching advantage.

## Proposed Fix

1. Add failing Verilator regressions that compare GPU perspective output
   against a Quake-style CPU oracle for both `OF_GPU_PARAM_ATTR_PERSP` and
   `OF_GPU_PARAM_ATTR_PERSP_Q29`.

2. Use sentinel-filled framebuffer memory so missed spans are detected as
   untouched bytes, not just pixel mismatches.

3. Exercise packed records across all boundary cases:

```text
record_count = 1, 2, 3, 4, 5, 7, 8, 9, 63, 64, 65, 511, 512
odd/even packed record pairs
zero-count records mixed with nonzero records
short tail spans with count < 16
long spans with count > 16
different framebuffer words per span to force fb_acc flushes
adjacent spans sharing destination words to exercise byte-lane merging
```

4. Run each case with:

```text
attr_mode = OF_GPU_PARAM_ATTR_PERSP
attr_mode = OF_GPU_PARAM_ATTR_PERSP_Q29
z_mode = NONE
z_mode = WRITE_ZI
q29_attr_shift = 0
q29_attr_shift > 0
```

5. Fix the RTL path that computes wrong perspective endpoints, drops generated
   spans, fails to flush the final accumulator, or advances chunk state before
   the previous generated span has fully drained.

6. Remove the experimental GPU lightmap capability from the advertised OS/SDK
   capability set. See "Remove GPU Lightmap Capability" below.

7. Keep the existing param-span command format. This is a correctness fix for
   already advertised features, not a new API.

## Required Acceptance Tests

Add tests to:

```text
/home/alberto/Repos/openfpgaOS/src/fpga/test/tb_gpu_acceptance_main.cpp
```

Suggested tests:

```text
test_persp_helper_matches_quake_cpu_oracle
test_persp_helper_all_spans_touch_sentinel_fb
test_persp_helper_tail_counts_no_drop
test_param_q29_matches_quake_cpu_oracle
test_param_q29_batch_matches_single_record_commands
test_param_q29_batch_all_spans_touch_sentinel_fb
test_param_q29_record_chunk_boundaries_no_drop
test_param_q29_tail_counts_no_drop
test_param_q29_zwrite_batch_no_drop
```

Each test should:

- Fill the framebuffer with a sentinel byte.
- Emit the same logical spans through both batched and single-record command
  forms.
- Compare every expected destination byte.
- Fail if any expected span byte remains sentinel.
- Include at least one case that forces framebuffer accumulator flushes between
  records.

## Quake-side Acceptance

After the RTL fix, Quake should be able to run with:

```text
pq_gpu_persp=1
pq_gpu_param=1
pq_gpu_spanblit=0
pq_gpu_world_light=0
```

Expected result:

- No missing black spans.
- Correct texture perspective in both `pq_gpu_param=1` and `pq_gpu_param=0`
  diagnostic modes.
- No stretched close-up wall/floor lines from Q29 overflow.
- No blank-screen regression.
- No new framebuffer wait inserted per surface.

Only after this is stable should Quake re-enable more aggressive defaults such
as GPU world z-write. GPU lightmap shading should remain removed unless a later
profiling pass shows a clear win.

## Remove GPU Lightmap Capability

Remove the experimental lightmap command/capability from the active advertised
OS/SDK surface:

```text
OF_HW_GPU_PARAM_SPAN_LIGHTMAP
OF_EMIT_CAP_PARAM_SPAN_LIGHTMAP
GPU_CMD_DRAW_PARAM_SPAN_LIGHTMAP_LIST
of_gpu_param_span_lightmap_t
of_gpu_draw_param_span_lightmap_list()
```

Implementation requirements:

- Stop advertising `OF_HW_GPU_PARAM_SPAN_LIGHTMAP` in firmware caps.
- Remove or compile out the SDK helper for
  `of_gpu_draw_param_span_lightmap_list()`.
- Remove Quake's dependency on `OF_EMIT_CAP_PARAM_SPAN_LIGHTMAP`; Quake should
  use CPU-lit surface cache data for world lighting.
- Remove or inert the RTL command decode for
  `GPU_CMD_DRAW_PARAM_SPAN_LIGHTMAP_LIST`.
- Keep command `0x49` reserved or rejected. Do not silently execute a partially
  supported lightmap path.
- Keep `CMD_DRAW_PARAM_SPAN_LIST` and `OF_HW_GPU_PARAM_SPAN_Q29_SCALE`; those
  are still required for the main Quake world-span path.

This intentionally supersedes the earlier GPU lightmap CR. The priority is now
to make ordinary Q29 param spans correct and fast with CPU-lit surface cache
input.
