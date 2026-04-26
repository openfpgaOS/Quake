/*
 * of_emit.h — public API for the openfpgaOS GPU owned by vid_of.c.
 *
 * Mirrors the SDK's of_gpu.h types (so engine files can build
 * descriptors directly without including of_gpu.h — which would
 * duplicate the ring-state statics).
 *
 * Call sites in the engine:
 *   d_scan.c  — world + water spans
 *   d_polyse.c— alias triangles (HW path)
 *   d_sprite.c— sprite quads
 *   vid_of.c  — init, clear, flip, z-buffer bind
 */

#ifndef OF_EMIT_H
#define OF_EMIT_H

#include <stdint.h>

/* ------- Span descriptor (bit-identical to of_gpu_span_t) ----------- */

#define OF_EMIT_COLORMAP    (1 << 0)
#define OF_EMIT_COLUMN      (1 << 1)
#define OF_EMIT_SKIP_ZERO   (1 << 2)
/* bits 3/4 (DEPTH_TEST/WRITE) reserved — Z buffer dropped in lean Phase 2.3 */
#define OF_EMIT_PERSP       (1 << 5)

typedef struct of_emit_span_s {
    uint32_t fb_addr;
    uint32_t tex_addr;
    int32_t  s, t;
    int32_t  sstep, tstep;
    uint16_t count;
    uint8_t  light;
    uint8_t  flags;
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

/* ------- Triangle vertex (bit-identical to of_gpu_vertex_t) --------- */

typedef struct of_emit_vertex_s {
    int16_t  x, y;   /* screen position, 12.4 fixed-point */
    uint16_t z;      /* 16-bit depth, 0=near 0xFFFF=far   */
    uint16_t pad;
    int32_t  s, t;   /* texture coords, 16.16 fixed-point */
    int32_t  w;      /* 1/W for perspective (0x10000 = affine) */
    uint8_t  r, g, b, a;  /* vertex colour / light / alpha */
} of_emit_vertex_t;

/* ------- Texture binding descriptor (mirror of of_gpu_texture_t) ---- */

typedef struct of_emit_texture_s {
    uint32_t addr;
    uint16_t width;
    uint16_t height;
} of_emit_texture_t;

void of_emit_bind_texture(const of_emit_texture_t *tex);

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
void of_emit_bind_fb(uint32_t fb_addr, int fb_stride,
                     uint32_t zb_addr, int zb_stride_bytes);
void of_emit_finish(void);
void of_emit_kick(void);    /* publish queued commands without waiting */
void of_emit_cache_clean(const void *addr, uint32_t size);
void of_emit_clear(uint32_t flags, uint16_t color, uint16_t depth);
void of_emit_depth_test(of_emit_depth_func_t func);

/* ------- Draw submission -------------------------------------------- */

void of_emit_span(const of_emit_span_t *sp);

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

/* Submit ONE triangle as its own DRAW_TRIANGLES command (3 vertices). */
void of_emit_triangles(const of_emit_vertex_t *verts, uint32_t num_vertices);

/* Submit N triangles in a single DRAW_TRIANGLES command. Saves one cmd
 * header + cmd_decode pass per triangle vs the per-call helper above.
 * num_vertices must be a positive multiple of 3, and the GPU state
 * (bound texture, depth func, etc.) must apply to every triangle in
 * the batch. */
void of_emit_triangles_batch(const of_emit_vertex_t *verts,
                             uint32_t num_vertices);

/* Clear flag bits mirrored from of_gpu.h (OF_GPU_CLEAR_*). */
#define OF_EMIT_CLEAR_COLOR (1 << 0)
#define OF_EMIT_CLEAR_DEPTH (1 << 1)

#endif /* OF_EMIT_H */
