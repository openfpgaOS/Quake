# Change Request: Quake GPU math regressions for openfpgaOS

Full path:

`/home/alberto/Repos/Quake/docs/openfpgaos_quake_gpu_math_cr.md`

Target OS files:

- `/home/alberto/Repos/openfpgaOS/src/fpga/test/tb_gpu_main.cpp`
- `/home/alberto/Repos/openfpgaOS/src/fpga/common/gpu_core.v`
- `/home/alberto/Repos/openfpgaOS/src/sdk/include/of_gpu.h`
- `/home/alberto/Repos/openfpgaOS/src/sdk/include/of_caps.h`

## Summary

Quake now renders correctly only when the risky GPU math paths are disabled:

```text
pq_gpu_persp      0
pq_gpu_zwrite     0
pq_gpu_world_light 0
```

With those defaults, world surfaces use Quake's CPU perspective subdivision and
CPU z-fill again. This fixes walls, enemies, and most textures.

Two OS-side math problems still need deterministic tests:

1. `CMD_DRAW_PARAM_SPAN_LIST` / `0x48` perspective math does not match Quake
   world surface math at oblique angles.
2. The viewmodel still has one bad texture region on the gun's right side. That
   path uses Quake alias/no-z spans lowered to `CMD_DRAW_AFFINE_SPAN_GROUP`, so
   test affine texture coordinate carry, per-lane `tex_addr`, and colormap row
   selection.

Do not add or advertise a new cap as working until these tests pass on the RTL
and on hardware.

## Test 1: Quake param-span perspective plane math

Add this test to:

`/home/alberto/Repos/openfpgaOS/src/fpga/test/tb_gpu_main.cpp`

Suggested name:

```c
test_param_span_quake_projection_math()
```

### What it must prove

For a Quake world surface, these three renderings must produce identical pixels:

1. CPU reference using Quake's `D_DrawSpans8` 16-pixel subdivision math.
2. `CMD_DRAW_PARAM_SPAN_LIST` with absolute screen `u/v` records.
3. `CMD_DRAW_PARAM_SPAN_LIST` with rebased local `u/v` records.

Run the same test with:

```text
z_mode = OF_GPU_PARAM_Z_NONE
z_mode = OF_GPU_PARAM_Z_WRITE_ZI
```

For z-write mode, every written z value must match Quake's software z-buffer
format:

```c
z16 = (uint16_t)(((int)(zi * 0x8000 * 0x10000)) >> 16);
```

If the GPU uses signed 16.16 `zi`, the equivalent positive-zi check is:

```c
z16 = (uint16_t)(zi_q16 >> 1);
```

### CPU reference math

Use the same equations as Quake, including truncation toward zero:

```c
static int32_t q_i32(double x)
{
    return (int32_t)x;
}

static int clamp_i32(int x, int lo, int hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static void quake_ref_span(uint8_t *fb, uint16_t *zb,
                           const uint8_t *tex, int tex_w,
                           int x, int y, int count,
                           double d_sdivzorigin,
                           double d_tdivzorigin,
                           double d_ziorigin,
                           double d_sdivzstepu,
                           double d_tdivzstepu,
                           double d_zistepu,
                           double d_sdivzstepv,
                           double d_tdivzstepv,
                           double d_zistepv,
                           int32_t sadjust,
                           int32_t tadjust,
                           int32_t bbextents,
                           int32_t bbextentt,
                           int fb_stride,
                           int z_stride)
{
    double sdivz16stepu = d_sdivzstepu * 16.0;
    double tdivz16stepu = d_tdivzstepu * 16.0;
    double zi16stepu    = d_zistepu    * 16.0;

    double sdivz = d_sdivzorigin + y * d_sdivzstepv + x * d_sdivzstepu;
    double tdivz = d_tdivzorigin + y * d_tdivzstepv + x * d_tdivzstepu;
    double zi    = d_ziorigin    + y * d_zistepv    + x * d_zistepu;
    int izistep = q_i32(d_zistepu * 0x8000 * 0x10000);
    int izi = q_i32(zi * 0x8000 * 0x10000);

    int32_t s = q_i32(sdivz * (65536.0 / zi)) + sadjust;
    int32_t t = q_i32(tdivz * (65536.0 / zi)) + tadjust;
    s = clamp_i32(s, 0, bbextents);
    t = clamp_i32(t, 0, bbextentt);

    int px = x;
    while (count > 0) {
        int spancount = (count >= 16) ? 16 : count;
        count -= spancount;

        int32_t snext, tnext, sstep = 0, tstep = 0;
        if (count) {
            sdivz += sdivz16stepu;
            tdivz += tdivz16stepu;
            zi    += zi16stepu;
            snext = q_i32(sdivz * (65536.0 / zi)) + sadjust;
            tnext = q_i32(tdivz * (65536.0 / zi)) + tadjust;
            snext = clamp_i32(snext, 16, bbextents);
            tnext = clamp_i32(tnext, 16, bbextentt);
            sstep = (snext - s) >> 4;
            tstep = (tnext - t) >> 4;
        } else {
            double m = (double)(spancount - 1);
            sdivz += d_sdivzstepu * m;
            tdivz += d_tdivzstepu * m;
            zi    += d_zistepu    * m;
            snext = q_i32(sdivz * (65536.0 / zi)) + sadjust;
            tnext = q_i32(tdivz * (65536.0 / zi)) + tadjust;
            snext = clamp_i32(snext, 8, bbextents);
            tnext = clamp_i32(tnext, 8, bbextentt);
            if (spancount > 1) {
                sstep = q_i32((double)(snext - s) / (double)(spancount - 1));
                tstep = q_i32((double)(tnext - t) / (double)(spancount - 1));
            }
        }

        for (int i = 0; i < spancount; i++) {
            int si = s >> 16;
            int ti = t >> 16;
            fb[y * fb_stride + px] = tex[ti * tex_w + si];

            if (zb) {
                zb[y * z_stride + px] = (uint16_t)(izi >> 16);
                izi += izistep;
            }

            s += sstep;
            t += tstep;
            px++;
        }

        s = snext;
        t = tnext;
    }
}
```

### Deterministic input values

Use a texture where every sampled coordinate is distinguishable:

```c
tex_w = 96;
tex_h = 64;
tex[t * tex_w + s] = (uint8_t)((s * 3 + t * 17 + 11) & 0xff);
```

Use these Quake-like plane values:

```c
double d_sdivzorigin = -0.01975;
double d_tdivzorigin =  0.03450;
double d_ziorigin    =  0.00680;

double d_sdivzstepu =  0.000041;
double d_tdivzstepu = -0.000027;
double d_zistepu    =  0.0000032;

double d_sdivzstepv = -0.000083;
double d_tdivzstepv =  0.000117;
double d_zistepv    = -0.0000017;

int32_t sadjust = 21 << 16;
int32_t tadjust =  7 << 16;
int32_t bbextents = (95 << 16) - 1;
int32_t bbextentt = (63 << 16) - 1;
```

Draw at least these spans:

```c
{ .u = 13, .v = 37, .count = 73 },
{ .u =  9, .v = 41, .count = 81 },
{ .u = 31, .v = 46, .count = 64 },
{ .u =  5, .v = 56, .count = 92 },
```

These spans intentionally cross several 16-pixel perspective subdivision
boundaries and have non-zero `du`, `dv`, `zistepu`, and `zistepv`.

### Param-span packing under test

Build absolute-plane values exactly like Quake:

```c
int32_t attr0_origin = q_i32(d_sdivzorigin * 65536.0
                           + (double)sadjust * d_ziorigin);
int32_t attr1_origin = q_i32(d_tdivzorigin * 65536.0
                           + (double)tadjust * d_ziorigin);
int32_t attr2_origin = q_i32(d_ziorigin * 65536.0);

int32_t attr0_du = q_i32(d_sdivzstepu * 65536.0
                       + (double)sadjust * d_zistepu);
int32_t attr1_du = q_i32(d_tdivzstepu * 65536.0
                       + (double)tadjust * d_zistepu);
int32_t attr2_du = q_i32(d_zistepu * 65536.0);

int32_t attr0_dv = q_i32(d_sdivzstepv * 65536.0
                       + (double)sadjust * d_zistepv);
int32_t attr1_dv = q_i32(d_tdivzstepv * 65536.0
                       + (double)tadjust * d_zistepv);
int32_t attr2_dv = q_i32(d_zistepv * 65536.0);
```

Absolute submission:

```text
fb_base = FB_BASE
fb_major_step = screen_width
fb_minor_step = 1
record.u = screen x
record.v = screen y
attr_origin = attr*_origin
attr_du = attr*_du
attr_dv = attr*_dv
```

Rebased submission:

```c
int base_u = 5;
int base_v = 37;

fb_base = FB_BASE + base_v * screen_width + base_u;
record.u = screen_x - base_u;
record.v = screen_y - base_v;

attr0_origin_rebased = attr0_origin + base_u * attr0_du + base_v * attr0_dv;
attr1_origin_rebased = attr1_origin + base_u * attr1_du + base_v * attr1_dv;
attr2_origin_rebased = attr2_origin + base_u * attr2_du + base_v * attr2_dv;
```

Both submissions must match the same CPU reference framebuffer. If absolute and
rebased submissions differ, the bug is in record `u/v` multiply, sign handling,
origin rebasing, or width of the accumulated attribute math.

## Test 2: Quake alias/viewmodel affine texture carry math

Add this test to:

`/home/alberto/Repos/openfpgaOS/src/fpga/test/tb_gpu_main.cpp`

Suggested name:

```c
test_affine_span_group_quake_alias_carry_math()
```

### What it must prove

Quake viewmodel/no-z spans submit:

```c
tex_addr = pspanpackage->ptex;              // integer texel address
s        = pspanpackage->sfrac & 0xffff;    // fractional 16.16 only
t        = pspanpackage->tfrac & 0xffff;
sstep    = r_sstepx;                        // signed 16.16
tstep    = r_tstepx;                        // signed 16.16
flags    = OF_GPU_SPAN_COLORMAP;
```

The GPU affine span result must match this CPU address math:

```c
static void quake_alias_ref_span(uint8_t *fb,
                                 const uint8_t *skin,
                                 const uint8_t *cmap_row,
                                 int fb_x,
                                 int count,
                                 int skin_w,
                                 int base_s,
                                 int base_t,
                                 int32_t sfrac,
                                 int32_t tfrac,
                                 int32_t sstep,
                                 int32_t tstep)
{
    int base = base_t * skin_w + base_s;
    int32_t s = sfrac & 0xffff;
    int32_t t = tfrac & 0xffff;

    for (int i = 0; i < count; i++) {
        int tex_ofs = base + (s >> 16) + (t >> 16) * skin_w;
        fb[fb_x + i] = cmap_row[skin[tex_ofs]];
        s += sstep;
        t += tstep;
    }
}
```

Use a non-power-of-two skin and a non-identity colormap row:

```c
skin_w = 96;
skin_h = 64;
skin[t * skin_w + s] = (uint8_t)((s * 5 + t * 29 + 7) & 0xff);
cmap_row[i] = (uint8_t)((i + 0x31) & 0xff);
```

Submit one `CMD_DRAW_AFFINE_SPAN_GROUP` with four lanes:

```text
lane 0: base_s=48 base_t=18 sfrac=0xe000 tfrac=0x2000 sstep=-0x0000c000 tstep= 0x00003000 count=31
lane 1: base_s=22 base_t=31 sfrac=0x1000 tfrac=0xf000 sstep= 0x00016000 tstep= 0x00008000 count=29
lane 2: base_s=72 base_t=12 sfrac=0xf400 tfrac=0xf800 sstep=-0x00012000 tstep= 0x00011000 count=27
lane 3: base_s=11 base_t=44 sfrac=0x8000 tfrac=0x4000 sstep= 0x0000a000 tstep=-0x00007000 count=33
```

For each lane:

```text
tex_addr = SKIN_BASE + base_t * skin_w + base_s
tex_width = skin_w
tex_w_mask = 0
tex_h_mask = 0
flags = OF_GPU_SPAN_COLORMAP
light = selected non-identity colormap row
```

Before this group, submit any span/group with non-zero `tex_w_mask` and
`tex_h_mask`, then submit this alias group with both masks set to zero. The
alias result must not inherit stale wrap masks from the previous command.

### Acceptance criteria

- Every byte in the rendered framebuffer region matches `quake_alias_ref_span`.
- No texel read comes from outside the 96x64 skin allocation.
- Per-lane `tex_addr`, `s/t`, `s/t step`, `light`, and zero texture masks are
  honored independently.
- The test fails on any off-by-one fractional carry, stale mask, stale colormap,
  or per-lane texture address bug.

## Required result

After the OS fixes:

- Quake can set `pq_gpu_persp 1` without world texture stretching, flat-color
  holes, or angle-dependent corruption.
- Quake can keep the viewmodel/no-z GPU affine path enabled without the wrong
  gun texture on the right side.
- Quake should still select these paths only from runtime caps. Do not force
  caps in Quake.
