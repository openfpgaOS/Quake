# CR: Q29 Param-Span High-Angle Floor Texture Lines

## Summary

Quake now renders world surfaces through `CMD_DRAW_PARAM_SPAN_LIST` with
`OF_GPU_PARAM_ATTR_PERSP_Q29`. General perspective is correct, but high-angle
floor surfaces can show horizontal/vertical texture line patterns.

This should be fixed in openfpgaOS RTL rather than worked around in Quake. A
Quake workaround would split high-risk spans into shorter records, reducing the
benefit of the streaming param-span path and still leaving Q29 command semantics
weaker than advertised.

## Affected RTL Path

Full path:

```text
/home/alberto/Repos/openfpgaOS/src/fpga/common/gpu_core.v
```

Relevant locations in the current tree:

```text
PERSPECTIVE_SEG_SHIFT / PERSPECTIVE_SEG_LEN around gpu_core.v:2197
pss_zinv_clamp_r declaration/comment around gpu_core.v:2316
PSS_ADV_CLAMP clamp decision around gpu_core.v:4347
PSS_DIV_DONE / PSS_SLOPE around gpu_core.v:4432 and gpu_core.v:4534
```

## Likely Root Cause

`PARAM_ATTR_PERSP_Q29` uses the exact divider path, but it still inherits the
old perspective safety clamp:

```verilog
pss_zinv_clamp_r <=
    ((pss_zinv_adv_r[31] ^ pss_zinv_prev_r[31])
     && (pss_zinv_prev_r != 32'sd0)
     && (pss_zinv_adv_r != 32'sd0))
 || (pss_zinv_adv_abs_r < (pss_zinv_abs_na_r >> 2));
```

That `4x shrink` guard was meant to prevent reciprocal-LUT blowups near a
singularity. For valid oblique Quake floors, `1/z` can legitimately shrink by
more than 4x across a 16-pixel internal segment without crossing zero. In Q29
mode the exact divider can handle that case, but the guard replaces the computed
endpoint with the segment anchor:

```verilog
pss_s_end_r <= pss_zinv_clamp_r ? persp_anchor_s : s_div_result;
pss_t_end_r <= pss_zinv_clamp_r ? persp_anchor_t : t_div_result;
```

That makes the segment slope zero and can render a 16-pixel chunk with nearly
constant texture coordinates. On floors this appears as a regular line/grid
pattern, especially at high view angles.

## Required RTL Change

Disable the `4x shrink` clamp for `PARAM_ATTR_PERSP_Q29`. Keep only true
singularity protection for zero or sign-crossing `1/z`.

Suggested change in `PSS_ADV_CLAMP`:

```verilog
if (sp_persp_exact_div) begin
    pss_zinv_clamp_r <=
        (pss_zinv_adv_abs_r == 32'd0)
     || (((pss_zinv_adv_r[31] ^ pss_zinv_prev_r[31])
          && (pss_zinv_prev_r != 32'sd0)
          && (pss_zinv_adv_r != 32'sd0)));
end else begin
    pss_zinv_clamp_r <=
        ((pss_zinv_adv_r[31] ^ pss_zinv_prev_r[31])
         && (pss_zinv_prev_r != 32'sd0)
         && (pss_zinv_adv_r != 32'sd0))
     || (pss_zinv_adv_abs_r < (pss_zinv_abs_na_r >> 2));
end
```

Rationale:

- Q16.16/LUT perspective keeps the old protective clamp.
- Q29 exact-div perspective uses the mathematically correct endpoint when
  `1/z` remains positive.
- Zero/sign-cross still avoids invalid near-plane singularities.

## Required Acceptance Test

Add a Verilator test to:

```text
/home/alberto/Repos/openfpgaOS/src/fpga/test/tb_gpu_acceptance_main.cpp
```

Suggested test name:

```text
test_param_span_q29_high_angle_floor_no_flatten()
```

Test shape:

1. Emit one horizontal `CMD_DRAW_PARAM_SPAN_LIST` record with:
   - `attr_mode = OF_GPU_PARAM_ATTR_PERSP_Q29`
   - `span_axis = OF_GPU_PARAM_AXIS_X`
   - `count >= 16`
   - positive `attr2` at both x=0 and x=16
   - `attr2(x=16) < attr2(x=0) / 4`
2. Use a distinct non-flat texture pattern so repeated texcoords are visible:

```c
tex[t * tex_w + s] = (uint8_t)((s * 3 + t * 17 + 11) & 0xff);
```

3. Build the CPU oracle from the Q29 command semantics:

```c
num0 = attr0_origin + x * attr0_du;
num1 = attr1_origin + x * attr1_du;
zi   = attr2_origin + x * attr2_du;
s = (num0 << 16) / zi;
t = (num1 << 16) / zi;
s = clamp(s, clamp_min[0], clamp_max[0]);
t = clamp(t, clamp_min[1], clamp_max[1]);
```

4. Assert exact framebuffer equality for every pixel.
5. Add a targeted assertion that pixels inside the first 16-pixel segment are
   not flattened to the anchor texel when the CPU oracle changes texels.

Example high-angle setup:

```text
zi0  = 0.80
zi16 = 0.08
attr2_origin = zi0 * 2^29
attr2_du     = ((zi16 - zi0) / 16) * 2^29
```

Choose `attr0/attr1` so projected `s/t` visibly change across the segment while
remaining inside clamp bounds. The current RTL should fail this test because the
`4x shrink` guard flattens the segment. After the RTL change, it should pass.

## Quake-Side Confirmation

Temporary confirmation without changing RTL:

```text
pq_gpu_param 0
```

If the floor line pattern disappears on the slower fallback path, the issue is
in the Q29 param-span hardware path rather than surface-cache generation or
palette upload.

Quake should not permanently split spans unless the RTL fix is unavailable.
