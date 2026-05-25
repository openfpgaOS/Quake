# Generic Parametric Span-List GPU Command

Full path:

`/home/alberto/Repos/Quake/docs/gpu_param_span_list_command.md`

## 1. Goal

Add a generic GPU command that draws a list of horizontal or vertical spans from
screen-space attribute planes. The command is intended to accelerate renderers
that already perform visibility and span generation on the CPU, such as Quake,
Doom, Duke-style column/row renderers, sprite rasterizers, and software polygon
pipelines.

This is not a Quake-only command. Quake is the first target because its current
fast path still asks the CPU to compute and submit per-span starting
`sdivz/tdivz/zi` values. A parametric span-list command lets the CPU submit one
surface header plus compact `{u, v, count}` span records, while the GPU computes
the per-span starts internally.

## 2. Proposed Command

Suggested name:

```c
#define GPU_CMD_DRAW_PARAM_SPAN_LIST 0x48
```

`0x48` is suggested because the current openfpgaOS span commands are:

```c
#define GPU_CMD_DRAW_PERSP_SPAN_GROUP  0x46
#define GPU_CMD_DRAW_AFFINE_SPAN_GROUP 0x47
```

If `0x48` is already used in the OS branch, choose the next free draw opcode and
keep the SDK, RTL decoder, and tests in lockstep.

## 3. Capability Bit

Add a runtime feature bit. Do not infer this from `OF_HW_GPU_SPAN`,
`OF_HW_GPU_PERSP`, or `OF_HW_GPU_FRAGPIPE`.

Suggested SDK name:

```c
#define OF_HW_GPU_PARAM_SPAN_LIST (1 << N)
```

The OS must advertise this bit only when the RTL command is implemented,
timing-clean, and covered by acceptance tests. Apps must fall back to existing
span-group commands when the bit is missing.

## 4. Command Packet Format

All GPU commands use the existing ring format:

```text
word 0: [31:24] opcode, [23:0] payload_words
word 1..N: command-specific payload
```

`CMD_DRAW_PARAM_SPAN_LIST` has a variable-size payload:

```text
payload_words = PARAM_SPAN_LIST_HEADER_WORDS
              + ceil(span_count / 2) * PARAM_SPAN_LIST_RECORD_PAIR_WORDS
```

The fixed header is 31 words. Span records are packed two records per
three-word record pair.

```c
#define OF_GPU_PARAM_SPAN_LIST_HEADER_WORDS 31u
#define OF_GPU_PARAM_SPAN_LIST_RECORDS_PER_PAIR 2u
#define OF_GPU_PARAM_SPAN_LIST_RECORD_PAIR_WORDS 3u
#define OF_GPU_PARAM_SPAN_MAX_RECORDS 512u
```

`OF_GPU_PARAM_SPAN_MAX_RECORDS` is an API limit. The SDK helper may split larger
submissions into multiple commands.

## 5. Header Layout

All fields are 32-bit words unless noted.

```text
word  0: fb_base
word  1: fb_major_step
word  2: fb_minor_step
word  3: tex_addr
word  4: tex_width
word  5: tex_w_mask
word  6: tex_h_mask
word  7: control

word  8: attr0_origin
word  9: attr0_du
word 10: attr0_dv
word 11: attr1_origin
word 12: attr1_du
word 13: attr1_dv
word 14: attr2_origin
word 15: attr2_du
word 16: attr2_dv

word 17: light_origin
word 18: light_du
word 19: light_dv

word 20: clamp0_min
word 21: clamp0_max
word 22: clamp1_min
word 23: clamp1_max
word 24: clamp2_min
word 25: clamp2_max

word 26: z_base
word 27: z_major_step
word 28: z_minor_step
word 29: span_count
word 30: reserved0

word 31..: packed span records
```

### 5.1. `control`

```text
bits  7:0   span flags, same public bits as existing span commands
            bit 0: COLORMAP
            bit 2: SKIP_ZERO
            bit 5: PERSP
            bit 6: TRANSLUC if implemented

bits 11:8   colormap_id
bits 15:12  attr_mode
bits 19:16  span_axis
bits 23:20  record_format
bits 27:24  z_mode
bits 31:28  reserved, must be zero
```

Suggested enums:

```c
enum {
    OF_GPU_PARAM_ATTR_AFFINE = 0,  /* attr0=s, attr1=t */
    OF_GPU_PARAM_ATTR_PERSP  = 1,  /* attr0=s/z, attr1=t/z, attr2=1/z */
    OF_GPU_PARAM_ATTR_SOLID  = 2,  /* no texture; optional future mode */
};

enum {
    OF_GPU_PARAM_AXIS_X = 0,  /* spans advance along +x: minor pixel step */
    OF_GPU_PARAM_AXIS_Y = 1,  /* spans advance along +y: minor row step */
};

enum {
    OF_GPU_PARAM_RECORD_U16V16_COUNT16 = 0,
};

enum {
    OF_GPU_PARAM_Z_NONE       = 0,
    OF_GPU_PARAM_Z_WRITE_ZI   = 1,  /* optional extension */
    OF_GPU_PARAM_Z_TEST_ZI    = 2,  /* optional extension */
    OF_GPU_PARAM_Z_TEST_WRITE = 3,  /* optional extension */
};
```

For the first implementation, `z_mode` may support only `NONE`. The command
should still reserve z fields so a later combined-z version does not need a new
wire format.

## 6. Span Record Format

Default record format packs two span records into three words:

```text
record A:
  u:     uint16
  v:     uint16
  count: uint16

record B:
  u:     uint16
  v:     uint16
  count: uint16
```

Packing:

```text
word K+0: [31:16] v0     [15:0] u0
word K+1: [31:16] u1     [15:0] count0
word K+2: [31:16] count1 [15:0] v1
```

That is 3 words for 2 records, or 48 bits per record. This keeps coordinates and
counts full-width while avoiding one 32-bit word per tiny span.

For odd `span_count`, the final unused record fields must be zero.

SDK helpers should hide this packing:

```c
typedef struct {
    uint16_t u;
    uint16_t v;
    uint16_t count;
} of_gpu_param_span_record_t;
```

## 7. Attribute Plane Semantics

The GPU computes the span start from screen coordinates:

```text
attrN_start = attrN_origin + u * attrN_du + v * attrN_dv
light_start = light_origin + u * light_du + v * light_dv
```

Then the span advances along the selected axis:

```text
if span_axis == X:
    fb_addr = fb_base + v * fb_major_step + u * fb_minor_step
    attrN_minor_step = attrN_du
    light_minor_step = light_du

if span_axis == Y:
    fb_addr = fb_base + u * fb_major_step + v * fb_minor_step
    attrN_minor_step = attrN_dv
    light_minor_step = light_dv
```

For normal linear framebuffers:

```text
horizontal rows:
    fb_major_step = framebuffer_stride
    fb_minor_step = 1
    span_axis = X

vertical columns:
    fb_major_step = 1
    fb_minor_step = framebuffer_stride
    span_axis = Y
```

The command should reject or no-op records with `count == 0`.

## 8. Attribute Modes

### 8.1. Affine Mode

```text
attr0 = s
attr1 = t
attr2 ignored
```

Per pixel:

```text
s = attr0_start + pixel_index * attr0_minor_step
t = attr1_start + pixel_index * attr1_minor_step
```

The fragment path can lower this to the existing affine span datapath.

### 8.2. Perspective Mode

```text
attr0 = sdivz
attr1 = tdivz
attr2 = zi
```

Per pixel:

```text
sdivz = attr0_start + pixel_index * attr0_minor_step
tdivz = attr1_start + pixel_index * attr1_minor_step
zi    = attr2_start + pixel_index * attr2_minor_step
s     = sdivz / zi
t     = tdivz / zi
```

The first implementation should lower generated spans into the existing
`SPAN_PERSP` fragment path rather than duplicating the reciprocal pipeline.

## 9. Fixed-Point Formats

Use the same formats as `CMD_DRAW_PERSP_SPAN_GROUP` unless the OS branch has
changed them:

```text
attr0/attr1 affine s,t:          signed 16.16
attr0/attr1 perspective sdivz,tdivz: signed 16.16 adjusted plane values
attr2 perspective zi:            signed 16.16, positive for valid pixels
attr*_du/dv:                     signed 16.16 per pixel
light_origin/light_du/light_dv:  signed Q6.16, low 24 bits used
clamp min/max:                   signed 16.16
```

Texture fetch uses the current I8 texture path:

```text
texel_addr = tex_addr + (t_int * tex_width) + s_int
```

If `tex_w_mask` or `tex_h_mask` is zero, preserve the existing openfpgaOS span
behavior: zero means no-wrap default, decoded internally as `0xFFFF` if that is
what the current RTL does for 0x46/0x47.

## 10. Clamping

The command includes per-attribute clamp fields so Quake can map `bbextents` and
`bbextentt` without CPU-side post-processing:

```text
clamp0_min/max apply to final s in affine or perspective mode
clamp1_min/max apply to final t in affine or perspective mode
clamp2_min/max reserved for zi/z extensions
```

If a clamp min/max pair is both zero, the implementation may treat it as
"disabled" for that attribute. SDK helpers should expose explicit flags if
ambiguous zero clamps become a problem.

For Quake:

```text
clamp0_min = 0
clamp0_max = bbextents
clamp1_min = 0
clamp1_max = bbextentt
```

## 11. Z Extension

The initial command can ship with `z_mode = NONE`. The header reserves:

```text
z_base
z_major_step
z_minor_step
z_mode
```

Future combined-z behavior:

```text
z_addr = z_base + v * z_major_step + u * z_minor_step
z_value derived from zi or attr2
```

For Quake, combined z-write is the next major win after parametric span-list
submission because it can replace the CPU `D_DrawZSpans` pass.

## 12. Quake Mapping

Quake world spans already have:

```text
d_sdivzorigin, d_tdivzorigin, d_ziorigin
d_sdivzstepu,  d_tdivzstepu,  d_zistepu
d_sdivzstepv,  d_tdivzstepv,  d_zistepv
sadjust, tadjust
bbextents, bbextentt
```

Quake's current `D_DrawSpans8` GPU perspective path bakes `sadjust/tadjust`
into the projection-space planes. The parametric command should use the same
math:

```text
attr0_origin = d_sdivzorigin * 65536 + sadjust * d_ziorigin
attr1_origin = d_tdivzorigin * 65536 + tadjust * d_ziorigin
attr2_origin = d_ziorigin    * 65536

attr0_du = d_sdivzstepu * 65536 + sadjust * d_zistepu
attr1_du = d_tdivzstepu * 65536 + tadjust * d_zistepu
attr2_du = d_zistepu    * 65536

attr0_dv = d_sdivzstepv * 65536 + sadjust * d_zistepv
attr1_dv = d_tdivzstepv * 65536 + tadjust * d_zistepv
attr2_dv = d_zistepv    * 65536
```

Framebuffer setup:

```text
fb_base = d_viewbuffer
fb_major_step = screenwidth
fb_minor_step = 1
span_axis = X
```

Texture setup:

```text
tex_addr = cacheblock or raw mip texture base
tex_width = cachewidth
tex_w_mask = 0 for current Quake no-wrap behavior
tex_h_mask = 0 for current Quake no-wrap behavior
```

Flags:

```text
flags = OF_GPU_SPAN_PERSP
if D_GPU_WORLD_LIGHT:
    flags |= OF_GPU_SPAN_COLORMAP
    light_origin = pq_world_light << 16
else:
    light_origin = 0
```

Span records come directly from `espan_t`:

```text
record.u = pspan->u
record.v = pspan->v
record.count = pspan->count
```

## 13. SDK API

Suggested public SDK structs:

```c
typedef struct {
    uint32_t fb_base;
    int32_t  fb_major_step;
    int32_t  fb_minor_step;

    uint32_t tex_addr;
    uint16_t tex_width;
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;

    uint8_t  flags;
    uint8_t  colormap_id;
    uint8_t  attr_mode;
    uint8_t  span_axis;
    uint8_t  z_mode;
    uint8_t  reserved[3];

    int32_t attr_origin[3];
    int32_t attr_du[3];
    int32_t attr_dv[3];

    int32_t light_origin;
    int32_t light_du;
    int32_t light_dv;

    int32_t clamp_min[3];
    int32_t clamp_max[3];

    uint32_t z_base;
    int32_t  z_major_step;
    int32_t  z_minor_step;
} of_gpu_param_span_list_t;

typedef struct {
    uint16_t u;
    uint16_t v;
    uint16_t count;
} of_gpu_param_span_record_t;

void of_gpu_draw_param_span_list(const of_gpu_param_span_list_t *params,
                                 const of_gpu_param_span_record_t *records,
                                 uint32_t record_count);
```

The helper should:

1. Clamp `record_count` to the API maximum per command.
2. Split larger lists into multiple commands.
3. Emit only complete commands into the DMA stream.
4. Skip zero-count records.
5. Preserve ring ordering with existing clear, texture, affine span, perspective
   span, triangle, kick, and fence commands.

## 14. RTL Implementation Plan

1. Add command decode for `GPU_CMD_DRAW_PARAM_SPAN_LIST`.
2. Load the 31-word header into state registers.
3. Stream packed span records from the command payload.
4. For each record, compute:

```text
fb_addr = fb_base + v * fb_major_step + u * fb_minor_step
attrN_start = attrN_origin + u * attrN_du + v * attrN_dv
light_start = light_origin + u * light_du + v * light_dv
```

5. Feed a generated scalar span into the existing affine or perspective span
   pipeline.
6. Reuse the existing perspective segment setup for `attr_mode = PERSP`.
7. After one record retires, advance to the next record.
8. After all records retire and framebuffer writes drain as required by the
   existing command model, return to command fetch.

The generated scalar span is equivalent to:

```text
fb_addr
tex_addr
count
flags
colormap_id
tex_width
tex_w_mask
tex_h_mask
minor step
attr starts
attr minor steps
light start
light minor step
```

## 15. Correctness Tests

Add Verilator tests in openfpgaOS:

```text
/home/alberto/Repos/openfpgaOS/src/fpga/test/tb_gpu_main.cpp
/home/alberto/Repos/openfpgaOS/src/fpga/test/tb_gpu_acceptance_main.cpp
```

Required tests:

1. `param_span_list_affine_rows`
   - Horizontal rows.
   - Identity texture.
   - Compare against CPU affine oracle.

2. `param_span_list_affine_columns`
   - Vertical columns.
   - Validates `span_axis`, `fb_major_step`, and `fb_minor_step`.

3. `param_span_list_persp_quake_rows`
   - Four adjacent Quake-like spans.
   - Non-zero `u`, variable `count`.
   - Non-zero major and minor perspective steps.
   - `COLORMAP | PERSP`.
   - Compare against CPU perspective oracle.

4. `param_span_list_persp_matches_046`
   - Encode the same spans as current `CMD_DRAW_PERSP_SPAN_GROUP`.
   - Encode them as `CMD_DRAW_PARAM_SPAN_LIST`.
   - Framebuffer output must match exactly or within the accepted rounding
     tolerance already used by the perspective tests.

5. `param_span_list_16px_boundaries`
   - Long spans crossing several internal perspective segment boundaries.
   - Includes starts not aligned to 16 pixels.

6. `param_span_list_negative_steps`
   - Negative `attr0_du` or `attr1_du`.
   - Ensures sign extension through the MAD path is correct.

7. `param_span_list_small_positive_zi`
   - Small positive `zi`.
   - Catches reciprocal normalization and clamp problems.

8. `param_span_list_mixed_dma_stream`
   - Command stream containing affine span group, param span list, clear rect,
     and fence.
   - Catches payload length and ring pointer bugs.

## 16. CPU Oracle

For each output pixel:

```c
int lane_u = record.u;
int lane_v = record.v;
int pixel = i;

int screen_u = lane_u + ((span_axis == X) ? pixel : 0);
int screen_v = lane_v + ((span_axis == Y) ? pixel : 0);

int64_t a0 = attr0_origin + screen_u * attr0_du + screen_v * attr0_dv;
int64_t a1 = attr1_origin + screen_u * attr1_du + screen_v * attr1_dv;
int64_t a2 = attr2_origin + screen_u * attr2_du + screen_v * attr2_dv;

if (attr_mode == PERSP) {
    s = div_fixed_16_16(a0, a2);
    t = div_fixed_16_16(a1, a2);
} else {
    s = a0;
    t = a1;
}

s = clamp(s, clamp0_min, clamp0_max);
t = clamp(t, clamp1_min, clamp1_max);
```

The oracle must match the hardware rounding policy. If the existing 0x46
perspective path intentionally approximates within 16-pixel segments, the test
should compare against that policy, not an ideal double-precision renderer,
unless the test is specifically measuring approximation error.

## 17. Diagnostics On Failure

Every test failure should print:

```text
test name
record index
u, v, count
pixel index
framebuffer address
expected texel/color
actual texel/color
attr0/attr1/attr2 at the pixel
light at the pixel
header words 0..30
packed record words around the failing record
```

For `param_span_list_persp_matches_046`, also print the equivalent 0x46 payload
words for the failing span group.

## 18. Performance Expectation

This command reduces CPU work by moving these operations from Quake to the GPU:

```text
fb_addr per span
sdivz/tdivz/zi integer MAD per span
per-span perspective descriptor packing
many command headers / group commands
```

Expected benefit for Quake:

1. Fewer command words per visible surface.
2. Less CPU descriptor work inside `D_DrawSpans8`.
3. Better command DMA efficiency.
4. Same fragment throughput as the existing perspective span path.

It does not remove CPU edge scanning or z-fill by itself. The follow-up
combined-z extension is needed to attack `D_DrawZSpans`.

## 19. Backward Compatibility

Existing apps remain unchanged:

- If `OF_HW_GPU_PARAM_SPAN_LIST` is missing, keep using `0x46` and `0x47`.
- Quake must continue to gate this path on runtime caps.
- The existing `OF_HW_GPU_PERSP` bit still means `0x46` is safe.
- `OF_HW_GPU_PARAM_SPAN_LIST` means the new parametric command is safe.

## 20. Acceptance Criteria

The OS agent should consider the command ready only when:

1. SDK constants, structs, and helper are committed.
2. RTL decoder rejects malformed payload sizes without wedging.
3. All new Verilator tests pass.
4. Existing GPU acceptance tests still pass.
5. The capability bit is advertised only in bitstreams that include the command.
6. Quake can enable the path through runtime caps and still fall back to 0x46 or
   affine spans on older bitstreams.
