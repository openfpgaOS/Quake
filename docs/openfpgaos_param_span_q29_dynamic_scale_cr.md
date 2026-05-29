# CR: Dynamic Scaling for Q29 Param-Spans

## Summary

`CMD_DRAW_PARAM_SPAN_LIST` with `OF_GPU_PARAM_ATTR_PERSP_Q29` can render Quake
world surfaces correctly and efficiently, but its signed 32-bit Q29 fields have
a fixed range of about `[-4, +4)`. Very close, angled walls can produce valid
Quake `s/z`, `t/z`, or `1/z` values outside that range. Today those values must
either wrap, causing stretched texture lines and flashing spans, or be rejected
by Quake and rendered through a slower fallback path.

Add a per-command dynamic scale for Q29 perspective attributes so Quake can keep
using batched param-spans even when the unscaled Q29 values exceed `int32_t`.

## Problem

Quake's software renderer defines each world surface as three linear planes:

```text
attr0 = s/z numerator
attr1 = t/z numerator
attr2 = 1/z denominator
```

The Q29 param-span command sends those planes as signed 32-bit values:

```text
attr_origin[3], attr_du[3], attr_dv[3]
```

That is high precision, but it is not high range. When the camera is extremely
close to a wall and the view angle is steep, `1/z` can exceed Q3.29. The value
is still valid mathematically, but not representable by the current wire
format. Casting the 64-bit Quake-side value to `int32_t` wraps. Falling back to
legacy spans avoids corruption but is too slow because it loses surface-level
batching and GPU world z-write for those surfaces.

## Proposed Semantics

For `OF_GPU_PARAM_ATTR_PERSP_Q29`, add a shared unsigned right shift:

```text
q29_attr_shift = 0..31
```

The CPU divides all three perspective planes by `2^q29_attr_shift` before
emitting the command:

```text
wire_attrN = trunc_toward_zero(real_attrN / 2^q29_attr_shift)
```

The projected texture coordinate is unchanged in principle because numerator
and denominator use the same scale:

```text
(attr0 / 2^k) / (attr2 / 2^k) == attr0 / attr2
(attr1 / 2^k) / (attr2 / 2^k) == attr1 / attr2
```

The GPU should use the scaled values directly for perspective projection. For
Quake z writes/tests, the GPU must restore the Q29 denominator scale before
deriving the 16-bit z value:

```text
unscaled_zinv = scaled_zinv << q29_attr_shift
source_z_half = unscaled_zinv[29:14]
```

If the restored value would overflow the internal z path, saturate to the
largest representable positive z value rather than wrapping.

## Wire Format

Keep `GPU_CMD_DRAW_PARAM_SPAN_LIST` and the current 31-word header size.

Use header word 30, currently emitted as zero and ignored by RTL, as a param
extension word:

```text
word 30 bits  4:0  q29_attr_shift
word 30 bits 31:5  reserved, must be zero
```

`q29_attr_shift == 0` preserves the current command semantics exactly.

Add an advertised capability bit so software only emits nonzero shifts when the
runtime bitstream supports them:

```text
OF_HW_GPU_PARAM_SPAN_Q29_SCALE
OF_EMIT_CAP_PARAM_SPAN_Q29_SCALE
```

## SDK/API Changes

Use one byte from the existing param-span reserved area:

```c
typedef struct {
    ...
    uint8_t  z_mode;
    uint8_t  q29_attr_shift;
    uint8_t  reserved[2];
    ...
} of_gpu_param_span_list_t;
```

SDK emit behavior:

```text
if attr_mode != OF_GPU_PARAM_ATTR_PERSP_Q29:
    q29_attr_shift must be emitted as 0
if q29_attr_shift != 0 and capability is absent:
    reject or clamp to legacy zero-shift behavior; do not emit scaled attrs
write q29_attr_shift into header word 30 bits 4:0
```

Quake-side behavior once the cap exists:

1. Compute the unscaled Q29 planes in 64-bit.
2. Evaluate the surface rectangle bounds.
3. Pick the smallest shift that makes every origin/du/dv and every bounded
   corner fit in signed 32-bit.
4. Emit the shifted Q29 param-span command.
5. Fall back to the legacy GPU perspective path only when no valid shift keeps
   `attr2` positive and nonzero.

## RTL Changes

In `gpu_core.v`:

1. Capture header word 30 into a new register, e.g.:

```verilog
reg [4:0] spanprod_q29_attr_shift;
```

2. Carry that shift into the active span state:

```verilog
reg [4:0] sp_q29_attr_shift;
```

3. Leave the perspective reciprocal/project path using the scaled values.

4. Apply the inverse shift only where the denominator is interpreted as Quake
   z, not where it is used as a projection denominator:

```verilog
if (sp_persp_q29_mode) begin
    restored_z = saturating_left_shift(sp_z_value, sp_q29_attr_shift);
    source_z_half = restored_z[29:14];
end
```

5. Preserve existing behavior when `sp_q29_attr_shift == 0`.

## Acceptance Tests

Add Verilator coverage in:

```text
/home/alberto/Repos/openfpgaOS/src/fpga/test/tb_gpu_acceptance_main.cpp
```

Required tests:

1. `test_param_span_q29_scaled_projection_matches_unscaled_oracle`
   - Build a surface whose unscaled `attr0/attr1/attr2` exceed signed 32-bit.
   - Emit the scaled command with `q29_attr_shift > 0`.
   - Compare framebuffer output against a CPU oracle using the original
     unscaled 64-bit planes.

2. `test_param_span_q29_scaled_z_write_matches_unscaled_zi`
   - Enable `OF_GPU_PARAM_Z_WRITE_ZI`.
   - Emit scaled Q29 attrs.
   - Verify zbuffer values match the unscaled `1/z` expectation.

3. `test_param_span_q29_shift_zero_is_legacy_exact`
   - Emit the same current Q29 command with `q29_attr_shift == 0`.
   - Verify byte-for-byte framebuffer and zbuffer results match existing tests.

4. `test_param_span_rejects_reserved_scale_bits`
   - Set word 30 reserved bits `31:5`.
   - Verify the command is rejected or ignored according to existing invalid
     header behavior.

## Compatibility

Old bitstreams ignore header word 30. Therefore software must gate nonzero
`q29_attr_shift` on the new capability bit. With no capability, Quake should
keep its temporary representability guard and fall back only for unsafe close
surfaces.

New bitstreams with `q29_attr_shift == 0` must remain compatible with all
existing Q29 tests and Quake command streams.
