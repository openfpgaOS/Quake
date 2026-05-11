/*
 * vid_of.c -- Quake video backend + GPU owner for openfpgaOS.
 *
 * This is the single TU that includes the SDK's of_gpu.h — the header
 * instantiates file-scope static ring-buffer state (wrptr, fence,
 * MMIO base), so having two owners would silently desync them. Every
 * other engine file that needs to emit a span includes of_emit.h
 * instead, builds an of_emit_span_t locally, and calls of_emit_span().
 *
 * VID_Init brings up the GPU + palette + surface cache + z-buffer,
 * then binds the initial framebuffer. VID_Update drains pending GPU
 * spans, flips to the next back buffer, and re-binds for the next
 * frame.
 */

#include "quakedef.h"
#include "d_local.h"
#include "of_emit.h"

#include "of.h"
#include "of_video.h"
#include "of_cache.h"
#include "of_services.h"
#include "of_gpu.h"                 /* static ring state lives HERE only */
#include "sysreg_stub.h"            /* SYS_CYCLE_LO for GPU-wait profiling */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern viddef_t vid;

_Static_assert(sizeof(of_emit_span_t) == sizeof(of_gpu_span_t),
               "of_emit_span_t must mirror of_gpu_span_t");
_Static_assert(__builtin_offsetof(of_emit_span_t, colormap_id) ==
               __builtin_offsetof(of_gpu_span_t, colormap_id),
               "span colormap_id offset drifted");
_Static_assert(__builtin_offsetof(of_emit_span_t, fb_stride) ==
               __builtin_offsetof(of_gpu_span_t, fb_stride),
               "span fb_stride offset drifted");

#define BASEWIDTH     320
#define BASEHEIGHT    240
#define SURFCACHE_SIZE   (2 * 1024 * 1024)

unsigned short d_8to16table[256];
unsigned       d_8to24table[256];

/* GPU-utilisation probe: cumulative cycles the CPU spent blocked inside
 * of_emit_finish() — i.e. waiting for the GPU to drain submitted work.
 * Resets at frame start (R_RenderView_).  Synthesised cycles via
 * SYS_CYCLE_LO (= of_time_us()*105) so the units match other buckets.
 *
 * Interpretation:
 *   pq_prof_gpu_wait ≈ 0  → CPU is the bottleneck (GPU keeping up easily).
 *   pq_prof_gpu_wait > 0  → CPU was idle waiting for GPU for that span;
 *                            of_emit_finish() takes ~that long per call. */
unsigned int   pq_prof_gpu_wait_cycles;

/* Page-flip duration.  With OF_EMIT_CAP_FLIP this measures the
 * acquire_next syscall; otherwise it measures the conservative
 * kernel-driven of_video_flip() path. */
unsigned int   pq_prof_video_flip_cycles;

/* GPU command-path profiler.  These are architecture-native counters
 * for the openfpgaOS GPU path: span batches, command-DMA pressure,
 * ring pressure, texture-cache traffic, and compact GPU status. */
unsigned int   pq_gpu_batch_flushes_frame;
unsigned int   pq_gpu_batch_spans_frame;
unsigned int   pq_gpu_batch_words_frame;
unsigned int   pq_gpu_cmd_dma_flushes_frame;
unsigned int   pq_gpu_dma_waits_frame;
unsigned int   pq_gpu_dma_spin_iters_frame;
unsigned int   pq_gpu_ring_waits_frame;
unsigned int   pq_gpu_ring_spin_iters_frame;
unsigned int   pq_gpu_min_ring_free_frame;
unsigned int   pq_gpu_ring_free_frame;
unsigned int   pq_gpu_status_frame;
unsigned int   pq_gpu_rdptr_frame;
unsigned int   pq_gpu_wrptr_frame;
unsigned int   pq_gpu_fence_frame;
unsigned int   pq_gpu_tex_req_frame;
unsigned int   pq_gpu_tex_miss_frame;
unsigned int   pq_gpu_stall_total_frame;

/* Triple-buffer slot index used only when the runtime advertises the
 * GPU-triggered CMD_FLIP path.  Kernel-driven flips keep this at -1. */
static int     draw_idx = -1;
static int     pending_flip_idx = -1;
static uint32_t pending_flip_token;
static uint32_t of_emit_caps;
static int     of_emit_ready;
static of_gpu_debug_snapshot_t gpu_prof_start_snap;

/* Surface cache in BSS (cacheable SDRAM). 2 MB bank. */
static byte surfcache_storage[SURFCACHE_SIZE] __attribute__((aligned(64)));

/* Z-buffer: 320x240 × 2 bytes = 150 KB. Aligned so GPU DEPTH_WRITE
 * bursts land on cache-line boundaries. */
static short zbuffer_storage[BASEWIDTH * BASEHEIGHT]
    __attribute__((aligned(64)));

/* ---- of_emit_* API (the GPU owner's public surface) --------------- */

/* Span batch accumulator.
 *
 * Quake's world / sky / sprite / alias-poly inner loops submit one
 * of_gpu_span_t per scanline.  Routing each through of_gpu_draw_span
 * costs ~16 MMIO writes per span; at typical world frames (hundreds
 * to a few thousand spans) that's a real chunk of frame time.  Buffer
 * up to OF_GPU_BATCH_MAX_SPANS spans, then dispatch via the GPU's
 * DRAW_SPANS_BATCH command-DMA path when the runtime exposes SDRAM.
 *
 * fb_addr / tex_addr / light / flags / perspective fields are baked
 * per-span into the payload, so changes within a batch need no flush.
 * The accumulator MUST flush before any other GPU command the queued
 * spans should observe in ring order — bind_fb, bind_texture, clear,
 * triangles*, kick, finish.  of_emit_blit submits its per-row spans
 * via of_emit_span so it auto-batches without special handling. */
static of_gpu_span_t span_buf[OF_GPU_BATCH_MAX_SPANS];
static int           span_buf_count;

static inline int flush_span_batch(void)
{
    if (span_buf_count > 0) {
        int count = span_buf_count;
        int use_batch = of_emit_supports(OF_EMIT_CAP_SPAN_BATCH);
        int used_dma = use_batch && (_gpu_batch_buf != NULL);

        pq_gpu_batch_flushes_frame++;
        pq_gpu_batch_spans_frame += (unsigned int)count;

        if (use_batch) {
            pq_gpu_batch_words_frame +=
                1u + (unsigned int)count * OF_GPU_BATCH_WORDS_PER_SPAN;
            if (used_dma)
                pq_gpu_cmd_dma_flushes_frame++;
            of_gpu_draw_spans_batch(span_buf, count);
        } else {
            pq_gpu_batch_words_frame +=
                (unsigned int)count * (OF_GPU_BATCH_WORDS_PER_SPAN + 1u);
            for (int i = 0; i < count; i++)
                of_gpu_draw_span(&span_buf[i]);
        }

        span_buf_count = 0;
        return used_dma;
    }

    return 0;
}

int of_emit_supports(uint32_t cap)
{
    return (of_emit_caps & cap) == cap;
}

int of_emit_gpu_ready(void)
{
    return of_emit_ready;
}

void of_emit_prof_frame_start(void)
{
    if (!of_emit_ready)
        return;

    pq_gpu_batch_flushes_frame = 0;
    pq_gpu_batch_spans_frame = 0;
    pq_gpu_batch_words_frame = 0;
    pq_gpu_cmd_dma_flushes_frame = 0;
    pq_gpu_dma_waits_frame = 0;
    pq_gpu_dma_spin_iters_frame = 0;
    pq_gpu_ring_waits_frame = 0;
    pq_gpu_ring_spin_iters_frame = 0;
    pq_gpu_min_ring_free_frame = 0;
    pq_gpu_ring_free_frame = 0;
    pq_gpu_status_frame = 0;
    pq_gpu_rdptr_frame = 0;
    pq_gpu_wrptr_frame = 0;
    pq_gpu_fence_frame = 0;
    pq_gpu_tex_req_frame = 0;
    pq_gpu_tex_miss_frame = 0;
    pq_gpu_stall_total_frame = 0;

    of_gpu_debug_snapshot(&gpu_prof_start_snap, 1);
}

void of_emit_prof_frame_end(void)
{
    of_gpu_debug_snapshot_t snap;

    if (!of_emit_ready)
        return;

    of_gpu_debug_snapshot(&snap, 0);

    pq_gpu_status_frame = snap.status;
    pq_gpu_rdptr_frame = snap.rdptr;
    pq_gpu_wrptr_frame = snap.wrptr;
    pq_gpu_fence_frame = snap.fence_reached;
    pq_gpu_dma_waits_frame = snap.dma_waits;
    pq_gpu_dma_spin_iters_frame = snap.dma_spin_iters;
    pq_gpu_ring_waits_frame = snap.ring_waits;
    pq_gpu_ring_spin_iters_frame = snap.ring_spin_iters;
    pq_gpu_min_ring_free_frame = snap.min_ring_free;
    pq_gpu_ring_free_frame = snap.ring_free;
    pq_gpu_tex_req_frame =
        (snap.tex_req_count - gpu_prof_start_snap.tex_req_count) &
        OF_GPU_TEX_DBG_COUNTER_MASK;
    pq_gpu_tex_miss_frame =
        (snap.tex_miss_count - gpu_prof_start_snap.tex_miss_count) &
        OF_GPU_TEX_DBG_COUNTER_MASK;

    for (uint32_t i = 0; i < OF_GPU_STALL_COUNT; i++)
        pq_gpu_stall_total_frame +=
            snap.stall_count[i] - gpu_prof_start_snap.stall_count[i];
}

void of_emit_init(void)
{
    const struct of_capabilities *caps = of_get_caps();
    uint32_t hw = caps ? caps->hw_features : 0;
    int svc_ok = (OF_SVC &&
                  OF_SVC->magic == OF_SVC_MAGIC &&
                  OF_SVC->count >= 14);
    int flip_services = (svc_ok &&
                         OF_SVC->video_acquire_next &&
                         OF_SVC->video_buffer_addr);
    int flip_hw = flip_services;

#ifdef OF_HW_GPU_FLIP
    flip_hw = (hw & OF_HW_GPU_FLIP) != 0;
#endif

    if (!caps || caps->magic != OF_CAPS_MAGIC || caps->gpu_base == 0)
        Sys_Error("Quake requires openfpgaOS GPU caps\n");
    if ((hw & OF_HW_GPU_SPAN) == 0)
        Sys_Error("Quake requires OF_HW_GPU_SPAN\n");

    of_emit_caps = OF_EMIT_CAP_SPAN;
    if (hw & OF_HW_GPU_PERSP)
        of_emit_caps |= OF_EMIT_CAP_PERSP;
    if ((hw & (OF_HW_GPU_TRIANGLE | OF_HW_GPU_VCOLOR)) ==
        (OF_HW_GPU_TRIANGLE | OF_HW_GPU_VCOLOR))
        of_emit_caps |= OF_EMIT_CAP_TRIANGLES | OF_EMIT_CAP_VCOLOR;
    if (hw & OF_HW_GPU_FRAGPIPE)
        of_emit_caps |= OF_EMIT_CAP_FRAGPIPE;
    if (hw & OF_HW_GPU_ALPHA)
        of_emit_caps |= OF_EMIT_CAP_ALPHA;
    if (flip_hw && flip_services)
        of_emit_caps |= OF_EMIT_CAP_FLIP;
    if (caps->sdram_base != 0)
        of_emit_caps |= OF_EMIT_CAP_SPAN_BATCH;

    /* of_gpu_init resolves the MMIO base from caps before any GPU_CTRL
     * access.  It also ring-resets and enables the core. */
    of_gpu_init();
    of_emit_ready = 1;

    /* Z-buffer / depth-test removed from the GPU in lean Phase 2.3.
     * Quake's BSP visibility pass + paint order handle visibility on
     * the CPU — alias models are clipped against the BSP and rendered
     * back-to-front, no z-test needed. */
}

void of_emit_upload_colormap(const unsigned char *cm, uint32_t size)
{
    of_gpu_palookup_upload(0, cm, size);
}

void of_emit_bind_fb(uint32_t fb_addr, int fb_stride,
                     uint32_t zb_addr, int zb_stride_bytes)
{
    (void)zb_addr; (void)zb_stride_bytes;  /* Z buffer dropped */
    flush_span_batch();
    of_gpu_set_framebuffer(fb_addr, (uint16_t)fb_stride);
}

void of_emit_finish(void)
{
    flush_span_batch();
    /* Bracket the GPU drain itself.  flush_span_batch only submits any
     * queued command-DMA work; of_gpu_finish() is where the CPU waits
     * until every submitted raster command has retired. */
    extern unsigned int pq_prof_gpu_wait_cycles;
    extern cvar_t       pq_cycleprof;
    int profiling = (int)pq_cycleprof.value;
    unsigned int t0 = profiling ? SYS_CYCLE_LO : 0;
    of_gpu_finish();
    if (profiling) pq_prof_gpu_wait_cycles += SYS_CYCLE_LO - t0;
}

/* Publish the CPU-side write pointer to the GPU without waiting.
 *
 * In scalar mode, kicks flush queued spans immediately so the GPU can
 * overlap with the CPU.  In command-DMA batch mode, tiny per-surface
 * batches are counterproductive: each batch pays a cache flush + DMA
 * doorbell setup.  Defer those soft kicks until the span buffer fills
 * or an ordering boundary forces flush_span_batch() (bind/clear/tri/
 * finish/flip). */
void of_emit_kick(void)
{
    if (span_buf_count > 0 &&
        of_emit_supports(OF_EMIT_CAP_SPAN_BATCH) &&
        span_buf_count < OF_GPU_BATCH_MAX_SPANS)
        return;

    if (!flush_span_batch())
        of_gpu_kick();
}

void of_emit_cache_clean(const void *addr, uint32_t size)
{
    if (!addr || !size) return;
    OF_SVC->cache_clean_range((void *)(uintptr_t)addr, size);
}

void of_emit_clear(uint32_t flags, uint16_t color, uint16_t depth)
{
    (void)depth;  /* Z buffer dropped */
    flush_span_batch();
    of_gpu_clear(flags, color);
}

void of_emit_depth_test(of_emit_depth_func_t func)
{
    (void)func;  /* depth test dropped — no-op for ABI stability */
}

void of_emit_span(const of_emit_span_t *sp)
{
    if (!of_emit_supports(OF_EMIT_CAP_SPAN))
        return;
    span_buf[span_buf_count++] = *(const of_gpu_span_t *)sp;
    if (span_buf_count >= OF_GPU_BATCH_MAX_SPANS)
        flush_span_batch();
}

void of_emit_clear_rect(int dst_x, int dst_y, int w, int h, unsigned char color)
{
    if (w <= 0 || h <= 0) return;
    /* of_gpu_clear_rect uses the SET_FB global stride, which already
     * matches vid.rowbytes — no need to reissue SET_FB.  Span batch
     * must drain first so the queued spans land before this clear
     * (otherwise the clear would overwrite their pixels). */
    const uint32_t fb_addr = (uint32_t)(uintptr_t)
        (vid.buffer + (uint32_t)(dst_y * vid.rowbytes + dst_x));
    flush_span_batch();
    of_gpu_clear_rect(fb_addr, (uint16_t)w, (uint16_t)h, color);
}

void of_emit_blit(int dst_x, int dst_y,
                  int blit_w, int blit_h,
                  const unsigned char *src,
                  int src_pitch,
                  int src_x, int src_y,
                  int skip_key_ff)
{
    if (blit_w <= 0 || blit_h <= 0) return;

    extern viddef_t vid;
    const uint32_t fb_base = (uint32_t)(uintptr_t)vid.buffer;
    const int      fb_row  = vid.rowbytes;
    const uint8_t  flags   = skip_key_ff ? OF_EMIT_SKIP_ZERO : 0;

    /* Emit one affine span per row. The fragment processor on the SPAN
     * path writes raw I8 texel directly when COLORMAP isn't set, and
     * with the SKIP_ZERO flag (which actually discards 0xFF, matching
     * Quake's TRANSPARENT_COLOR) we get key-color transparency. */
    for (int row = 0; row < blit_h; row++) {
        of_emit_span_t sp = {
            .fb_addr   = fb_base + (uint32_t)((dst_y + row) * fb_row + dst_x),
            .tex_addr  = (uint32_t)(uintptr_t)(src
                          + (src_y + row) * src_pitch
                          + src_x),
            .s         = 0,
            .t         = 0,
            .sstep     = 0x10000,   /* 1 texel per pixel */
            .tstep     = 0,
            .count     = (uint16_t)blit_w,
            .light     = 0,
            .flags     = flags,
            .fb_stride = 1,
            .tex_width = (uint16_t)src_pitch,
        };
        of_emit_span(&sp);
    }
}

void of_emit_triangles(const of_emit_vertex_t *verts, uint32_t num_vertices)
{
    if (!of_emit_supports(OF_EMIT_CAP_TRIANGLES))
        return;
    flush_span_batch();
    of_gpu_draw_triangles((const of_gpu_vertex_t *)verts, num_vertices);
}

void of_emit_triangles_batch(const of_emit_vertex_t *verts,
                             uint32_t num_vertices)
{
    if (!of_emit_supports(OF_EMIT_CAP_TRIANGLES))
        return;
    flush_span_batch();
    of_gpu_draw_triangles_batch((const of_gpu_vertex_t *)verts, num_vertices);
}

void of_emit_bind_texture(const of_emit_texture_t *tex)
{
    flush_span_batch();
    of_gpu_texture_t gt = {
        .addr   = tex->addr,
        .width  = tex->width,
        .height = tex->height,
    };
    of_gpu_bind_texture(&gt);
}

/* ---- Palette + init + flip --------------------------------------- */

void VID_SetPalette(unsigned char *palette)
{
    /* Quake supplies 256 × RGB triplets, 0-255. Pack into 32-bit and
     * push through the bulk palette write for minimum MMIO traffic. */
    static uint32_t pal32[256];
    unsigned char *p = palette;
    for (int i = 0; i < 256; i++) {
        uint32_t r = *p++, g = *p++, b = *p++;
        pal32[i] = (r << 16) | (g << 8) | b;
        d_8to16table[i] = (unsigned short)(((r >> 3) << 11) |
                                           ((g >> 2) << 5) |
                                           (b >> 3));
        d_8to24table[i] = (unsigned)(r | (g << 8) | (b << 16));
    }
    of_video_palette_bulk(pal32, 256);
}

void VID_ShiftPalette(unsigned char *palette) { VID_SetPalette(palette); }

void VID_Init(unsigned char *palette)
{
    Sys_Printf("VID_Init: start\n");

    vid.maxwarpwidth = vid.width = vid.conwidth = BASEWIDTH;
    vid.maxwarpheight = vid.height = vid.conheight = BASEHEIGHT;
    vid.aspect = 1.0f;
    vid.numpages = 3;
    vid.colormap = host_colormap;
    /* Fullbright marker sits at byte (VID_GRADES*256), one past the
     * light table.  Two conventions in the wild:
     *   - id1 standard: marker = "starting palette index of the
     *     fullbright range" (typically 224 → fullbright_count=32).
     *   - some PAKs (incl. the one this build runs against): marker
     *     stores the count directly (32 → fullbright_count=32).
     * Distinguish heuristically: a count is small (≤127), a start
     * index is large (≥128). vid.fullbright is the COUNT throughout
     * the engine. */
    {
        int marker = vid.colormap[VID_GRADES * 256];
        vid.fullbright = (marker < 128) ? marker : (256 - marker);
    }

    vid.buffer    = vid.conbuffer   = (byte *)of_uncached(of_video_surface());
    vid.rowbytes  = vid.conrowbytes = BASEWIDTH;

    /* Z-buffer in SDRAM. The GPU reads/writes via AXI (cache-incoherent
     * with the CPU), so we route CPU-side access through the uncached
     * alias — the CPU fallback paths still work, just slower, but GPU
     * writes are always visible to subsequent CPU reads without a
     * cache flush. */
    d_pzbuffer = (short *)of_uncached(zbuffer_storage);
    D_InitCaches(surfcache_storage, SURFCACHE_SIZE);

    of_emit_init();
    Sys_Printf("GPU caps: hw=%08x span=%d persp=%d tri=%d vcolor=%d frag=%d alpha=%d flip=%d batch=%d\n",
               of_get_caps()->hw_features,
               of_emit_supports(OF_EMIT_CAP_SPAN),
               of_emit_supports(OF_EMIT_CAP_PERSP),
               of_emit_supports(OF_EMIT_CAP_TRIANGLES),
               of_emit_supports(OF_EMIT_CAP_VCOLOR),
               of_emit_supports(OF_EMIT_CAP_FRAGPIPE),
               of_emit_supports(OF_EMIT_CAP_ALPHA),
               of_emit_supports(OF_EMIT_CAP_FLIP),
               of_emit_supports(OF_EMIT_CAP_SPAN_BATCH));
    VID_SetPalette(palette);
    of_emit_upload_colormap(host_colormap, 64 * 256);

    /* HW clear all three back buffers. Hardware memset is faster than
     * CPU and avoids the cache flush the CPU memset would need. Use
     * kernel flips during startup so the kernel/display state is fully
     * settled before optional CMD_FLIP handoff. */
    for (int b = 0; b < 3; b++) {
        uint8_t *fb = of_video_surface();
        vid.buffer = vid.conbuffer = (byte *)of_uncached(fb);
        of_emit_bind_fb((uint32_t)(uintptr_t)fb, BASEWIDTH,
                        (uint32_t)(uintptr_t)zbuffer_storage,
                        BASEWIDTH * 2);
        of_emit_clear(OF_EMIT_CLEAR_COLOR, 0, 0);
        of_emit_finish();
        of_video_flip();
        of_video_wait_flip();
    }

    if (of_emit_supports(OF_EMIT_CAP_FLIP)) {
        /* Capture the kernel's current draw slot for CMD_FLIP. */
        draw_idx = of_video_acquire_next(-1, 0);
        uint8_t *fb = of_video_buffer_addr(draw_idx);
        if (!fb)
            Sys_Error("video_acquire_next returned invalid draw buffer\n");
        vid.buffer = vid.conbuffer = (byte *)of_uncached(fb);
        of_emit_bind_fb((uint32_t)(uintptr_t)fb, BASEWIDTH,
                        (uint32_t)(uintptr_t)zbuffer_storage,
                        BASEWIDTH * 2);
    } else {
        draw_idx = -1;
        uint8_t *fb = of_video_surface();
        vid.buffer = vid.conbuffer = (byte *)of_uncached(fb);
        of_emit_bind_fb((uint32_t)(uintptr_t)fb, BASEWIDTH,
                        (uint32_t)(uintptr_t)zbuffer_storage,
                        BASEWIDTH * 2);
    }

    Sys_Printf("VID_Init: fullbright=%d buffer=%p draw_idx=%d\n",
               vid.fullbright, vid.buffer, draw_idx);
}

void VID_Shutdown(void) { of_video_set_display_mode(OF_DISPLAY_TERMINAL); }

void VID_Update(vrect_t *rects)
{
    (void)rects;

    extern cvar_t pq_cycleprof;
    int profiling = (int)pq_cycleprof.value;
    unsigned int t0 = profiling ? SYS_CYCLE_LO : 0;

    if (of_emit_supports(OF_EMIT_CAP_FLIP)) {
        /* Runtime-advertised CMD_FLIP path: render commands and the
         * page swap stay ordered in the GPU ring.  Do not acquire the
         * next buffer here.  Deferring that wait to VID_WaitSync() lets
         * host/audio/input/server work overlap the GPU's remaining
         * raster work and the flip fence. */
        flush_span_batch();
        pending_flip_idx = draw_idx;
        pending_flip_token = of_gpu_flip_to(draw_idx);
        of_gpu_kick();

        if (profiling)
            pq_prof_video_flip_cycles = 0;
        draw_idx = -1;
        return;
    }

    /* Conservative 4031-compatible path: finish all GPU writes before
     * asking the kernel to flip, then render into the kernel's next
     * drawable surface. */
    of_emit_finish();

    t0 = profiling ? SYS_CYCLE_LO : 0;
    of_video_flip();
    if (profiling) pq_prof_video_flip_cycles = SYS_CYCLE_LO - t0;

    uint8_t *new_fb = of_video_surface();
    vid.buffer = vid.conbuffer = (byte *)of_uncached(new_fb);

    of_emit_bind_fb((uint32_t)(uintptr_t)new_fb, BASEWIDTH,
                    (uint32_t)(uintptr_t)zbuffer_storage, BASEWIDTH * 2);
    of_emit_kick();
}

void VID_WaitSync(void)
{
    if (!of_emit_supports(OF_EMIT_CAP_FLIP) || pending_flip_idx < 0)
        return;

    extern cvar_t pq_cycleprof;
    int profiling = (int)pq_cycleprof.value;
    unsigned int t0 = profiling ? SYS_CYCLE_LO : 0;

    /* Wait until CMD_FLIP reaches the display side-port, then wait for
     * that queued swap to present.  This happens at the next frame's
     * screen boundary, after host/audio/input/server work had a chance
     * to overlap the GPU. */
    draw_idx = of_video_acquire_next(pending_flip_idx, pending_flip_token);
    of_video_wait_flip();

    if (profiling)
        pq_prof_video_flip_cycles = SYS_CYCLE_LO - t0;

    pending_flip_idx = -1;
    pending_flip_token = 0;

    uint8_t *new_fb = of_video_buffer_addr(draw_idx);
    if (!new_fb)
        Sys_Error("video_acquire_next returned invalid draw buffer\n");
    vid.buffer = vid.conbuffer = (byte *)of_uncached(new_fb);

    of_emit_bind_fb((uint32_t)(uintptr_t)new_fb, BASEWIDTH,
                    (uint32_t)(uintptr_t)zbuffer_storage, BASEWIDTH * 2);
    of_emit_kick();
}

/* Loading disc overlays: Quake calls these during long PAK reads. We
 * ignore them on this backend because the triple-buffered flip would
 * make the icon flash, and PAK reads are short enough here. */
void D_BeginDirectRect(int x, int y, byte *pbitmap, int w, int h)
{ (void)x; (void)y; (void)pbitmap; (void)w; (void)h; }
void D_EndDirectRect(int x, int y, int w, int h)
{ (void)x; (void)y; (void)w; (void)h; }
