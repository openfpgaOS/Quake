/*
 * of_emit.h — public API for the openfpgaOS GPU owned by vid_of.c.
 *
 * Engine files must not include of_gpu.h directly because that header owns
 * static ring state.  They build lightweight descriptors here; vid_of.c lowers
 * them to the SDK's unified parametric span command path.
 *
 * Call sites in the engine:
 *   d_scan.c  — world + water spans
 *   d_polyse.c— alias spans
 *   d_sprite.c— sprite quads
 *   vid_of.c  — init, clear, flip, z-buffer bind
 */

#ifndef OF_EMIT_H
#define OF_EMIT_H

#include <stdint.h>

/* ------- Span submit descriptor lowered by vid_of.c ------------------ */

#define OF_EMIT_COLORMAP    (1 << 0)
#define OF_EMIT_COLUMN      (1 << 1)
#define OF_EMIT_SKIP_ZERO   (1 << 2)
/* bits 3/4 reserved; z behavior is explicit in param-span z_mode. */
#define OF_EMIT_PERSP       (1 << 5)

typedef struct of_emit_span_s {
    uint32_t fb_addr;
    uint32_t tex_addr;
    int32_t  s, t;
    int32_t  sstep, tstep;
    uint16_t count;
    uint8_t  light;
    uint8_t  flags;
    uint8_t  colormap_id;
    int16_t  fb_stride;
    uint16_t tex_width;
    /* POT wrap masks (tex_w-1 / tex_h-1).  Both 0 = no wrap. */
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;
    int32_t  sdivz, tdivz;
    int32_t  zi_persp;
    int32_t  sdivz_step, tdivz_step;
    int32_t  zi_step;
} of_emit_span_t;

/* ------- Parametric span-list descriptors lowered by vid_of.c ------- */

#define OF_EMIT_PARAM_ATTR_AFFINE 0
#define OF_EMIT_PARAM_ATTR_PERSP  1
#define OF_EMIT_PARAM_ATTR_SOLID  2
#define OF_EMIT_PARAM_ATTR_PERSP_Q29 3

#define OF_EMIT_PARAM_AXIS_X 0
#define OF_EMIT_PARAM_AXIS_Y 1

#define OF_EMIT_PARAM_Z_NONE       0
#define OF_EMIT_PARAM_Z_WRITE_ZI   1
#define OF_EMIT_PARAM_Z_TEST_ZI    2
#define OF_EMIT_PARAM_Z_TEST_WRITE 3

#define OF_EMIT_PARAM_SPAN_MAX_RECORDS 512u

typedef struct of_emit_param_span_record_s {
    uint16_t u;
    uint16_t v;
    uint16_t count;
} of_emit_param_span_record_t;

typedef struct of_emit_param_span_list_s {
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
    uint8_t  q29_attr_shift;
    uint8_t  reserved[2];

    int32_t  attr_origin[3];
    int32_t  attr_du[3];
    int32_t  attr_dv[3];

    int32_t  light_origin;
    int32_t  light_du;
    int32_t  light_dv;

    int32_t  clamp_min[3];
    int32_t  clamp_max[3];

    uint32_t z_base;
    int32_t  z_major_step;
    int32_t  z_minor_step;
} of_emit_param_span_list_t;

/* ------- Hardware triangle descriptors (CMD_DRAW_PARAM_TRI) ---------
 *
 * vid_of.c lowers each vertex triple to one GPU param-tri command: the
 * same plane/control/z header as the param span list, with the GPU
 * walking the three vertices itself (ceil fill on both edges,
 * left-closed right-open — crack-free shared edges).  Gated on
 * OF_EMIT_CAP_TRIANGLES; alias models are the only client (d_polyse.c).
 */

typedef struct of_emit_vertex_s {
    int16_t  x, y;   /* screen position, 12.4 fixed-point */
    uint16_t z;      /* unused by the param-tri lowering (pack 0) — the
                        z plane is rebuilt from w */
    uint16_t pad;
    int32_t  s, t;   /* texture coords, 16.16 fixed-point */
    int32_t  w;      /* zi in Q16.16 (v[5]>>15); feeds the persp divide
                        and the z plane (z window stores w>>1) */
    uint8_t  r, g, b, a;  /* vertex colour / light / alpha */
} of_emit_vertex_t;

/* ------- Texture binding descriptor (mirror of of_gpu_texture_t) ---- */

typedef struct of_emit_texture_s {
    uint32_t addr;
    uint16_t width;
    uint16_t height;
} of_emit_texture_t;

void of_emit_bind_texture(const of_emit_texture_t *tex);

/* ------- Texture store (portable, target-agnostic) -----------------
 *
 * Thin wrappers over the openfpgaOS texture manager (driven entirely inside
 * vid_of.c).  The engine deals only in opaque GPU addresses + two domain binds;
 * which memory a texture lives in, the upload, the fetch-domain switch and the
 * colormap co-location are all hidden.  Same calls on Pocket and MiSTer. */

/* Reset the manager on level change (then re-create textures + colormap). */
void of_emit_tex_reset(void);

/* 1 if the target has a dedicated fast texture tier (for diagnostics). */
int of_emit_has_fast_mem(void);

/* 1 when world textures are resident in the fast tier this map: the world span
 * path must bind each surface's CRAM1 offset (texture_t.gpu_tex_addr) and wrap
 * the world pass in the fast fetch domain. */
int of_emit_fasttex_active(void);

/* Create a STATIC texture (world miptex, alias skin, …) from `pixels`; returns
 * its GPU base address.  For a given mip add the intra-texture byte offset. */
uint32_t of_emit_tex_create(const void *pixels, uint16_t w, uint16_t h, uint32_t nbytes);

/* Register the animated sky as a DYNAMIC (in-place) texture; returns its GPU
 * base address.  Call of_emit_sky_update() after each rebuild. */
uint32_t of_emit_sky_create(void *pixels, uint16_t w, uint16_t h);
void     of_emit_sky_update(uint32_t nbytes);

/* GPU base address of an SDRAM source the GPU samples in place (turbulent
 * scratch rows): memory-agnostic, matches the texture manager's addressing. */
uint32_t of_emit_tex_source_addr(const void *src);

/* Set the GPU fetch domain for the next batch (flips + drains only on change;
 * flush pending batched geometry first).  static = world textures' tier;
 * dynamic = SDRAM (sky, turbulent, alias, sprites, HUD). */
void of_emit_bind_static(void);
void of_emit_bind_dynamic(void);

/* ------- Enumerations matching of_gpu.h ----------------------------- */

typedef enum {
    OF_EMIT_DEPTH_FUNC_NONE    = 0,
    OF_EMIT_DEPTH_FUNC_ALWAYS  = 1,
    OF_EMIT_DEPTH_FUNC_LESS    = 2,
    OF_EMIT_DEPTH_FUNC_LEQUAL  = 3,
    OF_EMIT_DEPTH_FUNC_EQUAL   = 4,
    OF_EMIT_DEPTH_FUNC_GEQUAL  = 5,
    OF_EMIT_DEPTH_FUNC_GREATER = 6,
    OF_EMIT_DEPTH_FUNC_NOTEQUAL= 7,
} of_emit_depth_func_t;

/* ------- GPU lifecycle (implemented in vid_of.c) -------------------- */

void of_emit_init(void);
void of_emit_upload_colormap(const unsigned char *cm, uint32_t size);
/* Fast-tier accounting (spill diagnostic): total bytes, bytes still free, and the
 * count of world textures that overflowed the tier (placed in SDRAM instead). */
uint32_t of_emit_fasttex_size(void);
uint32_t of_emit_fasttex_budget_free(void);
int      of_emit_fasttex_spill_count(void);
void of_emit_bind_fb(uint32_t fb_addr, int fb_stride,
                     uint32_t zb_addr, int zb_stride_bytes);
void of_emit_finish(void);
void of_emit_kick(void);    /* publish queued commands without waiting */
void of_emit_prepare_framebuffer_for_cpu(void);
void of_emit_texture_cache_flush(void);
void of_emit_cache_clean(const void *addr, uint32_t size);
void of_emit_cache_clean_once(const void *addr, uint32_t size);
void of_emit_cache_clean_forget(const void *addr, uint32_t size);
void of_emit_clear(uint32_t flags, uint16_t color, uint16_t depth);
void of_emit_depth_test(of_emit_depth_func_t func);

/* Runtime GPU capability gates, derived from openfpgaOS caps at init.
 * SPAN is Quake's required floor; everything else is optional and must
 * be checked before using newer commands. */
#define OF_EMIT_CAP_SPAN       (1u << 0)
#define OF_EMIT_CAP_PERSP      (1u << 1)
#define OF_EMIT_CAP_TRIANGLES  (1u << 2) /* HW edge walker (PARAM_TRI);
                                            z-tested flavor also needs
                                            CAP_PARAM_SPAN_ZTEST */
#define OF_EMIT_CAP_VCOLOR     (1u << 3) /* vertex color cap without draw API */
#define OF_EMIT_CAP_FRAGPIPE   (1u << 4)
#define OF_EMIT_CAP_ALPHA      (1u << 5)
#define OF_EMIT_CAP_FLIP       (1u << 6)
#define OF_EMIT_CAP_SPAN_BATCH (1u << 7)
#define OF_EMIT_CAP_PARAM_SPAN_LIST (1u << 8)
#define OF_EMIT_CAP_PARAM_SPAN_Z    (1u << 9)
#define OF_EMIT_CAP_PARAM_SPAN_ZTEST (1u << 10)
#define OF_EMIT_CAP_Q29_SCALE       (1u << 11) /* local cap for OF_HW_GPU_PARAM_SPAN_Q29_SCALE */

int of_emit_supports(uint32_t cap);
int of_emit_gpu_ready(void);

/* ------- Draw submission -------------------------------------------- */

void of_emit_span(const of_emit_span_t *sp);
int of_emit_param_span_list(const of_emit_param_span_list_t *params,
                            const of_emit_param_span_record_t *records,
                            uint32_t record_count);

/* Solid-color rectangle fill via the GPU's CMD_CLEAR_RECT primitive.
 * (dst_x, dst_y) are in bound-FB pixel coords; (w, h) are pixels;
 * `color` is a paletted byte.  Single ring command, no per-pixel CPU
 * work — beats `Draw_Fill`'s tight uncached-SDRAM byte-write loop. */
void of_emit_clear_rect(int dst_x, int dst_y, int w, int h, unsigned char color);
void of_emit_clear_rect_addr(uint32_t fb_addr, int fb_stride,
                             int w, int h, unsigned char color);

/* 1-to-1 paletted blit of an `src_w × src_h` source rectangle (rooted
 * at (`src_x`, `src_y`) within `src_pitch`-wide image `src`) into the
 * currently-bound framebuffer at screen (`dst_x`, `dst_y`).  Affine,
 * no perspective, no colormap.  When `skip_key_ff` is non-zero,
 * source bytes equal to 0xFF are not written (Quake's TRANSPARENT_COLOR).
 * Caller is responsible for clipping; coords go straight to the GPU.
 * Internally emits one DRAW_SPAN per row. */
void of_emit_blit(int dst_x, int dst_y,
                  int blit_w, int blit_h,
                  const unsigned char *src,
                  int src_pitch,
                  int src_x, int src_y,
                  int skip_key_ff);

/* Hardware triangle submission (CMD_DRAW_PARAM_TRI).  Each vertex
 * triple becomes one GPU command; no-ops unless OF_EMIT_CAP_TRIANGLES.
 * World aliases z-test/write via the param z plane; the viewmodel
 * (pq_noz_mode) draws with z disabled. */
void of_emit_triangles(const of_emit_vertex_t *verts, uint32_t num_vertices);

void of_emit_triangles_batch(const of_emit_vertex_t *verts,
                             uint32_t num_vertices);

/* Clear flag bits mirrored from of_gpu.h (OF_GPU_CLEAR_*). */
#define OF_EMIT_CLEAR_COLOR (1 << 0)
#define OF_EMIT_CLEAR_DEPTH (1 << 1)

#endif /* OF_EMIT_H */
