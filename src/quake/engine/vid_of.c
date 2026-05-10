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

/* Page-flip duration: time inside of_video_acquire_next().  Now
 * captures only the rare 3-buffer-ceiling block (CPU outran the
 * display by more than one frame); steady-state value is ~10 µs of
 * book-keeping syscall.  Pre cr-gpu-triggered-flip this measured
 * the kernel-driven of_video_flip() which spent ~80 µs blocking on
 * framebuffer ownership; the GPU-triggered path moves the actual
 * vsync wait into the GPU's command processor, where it overlaps
 * with the next frame's CPU work. */
unsigned int   pq_prof_video_flip_cycles;

/* Triple-buffer slot index the GPU's CMD_FLIP will swap to this
 * frame.  Initialized by VID_Init via of_video_acquire_next(-1)
 * after the init-time kernel-driven flip cycle settles, then
 * advanced each VID_Update to the slot returned by
 * of_video_acquire_next(draw_idx).  See cr-gpu-triggered-flip.md. */
static int     draw_idx = -1;

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
 * up to OF_GPU_BATCH_MAX_SPANS spans, then dispatch via
 * of_gpu_draw_spans_batch — payload is built as cached scalar stores
 * into the SDRAM scratch buffer of_gpu_init() pinned, followed by one
 * cache_clean_range and a single CMD_DRAW_SPANS_BATCH header; the GPU
 * pulls the bytes over its own AXI master while the CPU returns to
 * game logic / next-frame setup.
 *
 * fb_addr / tex_addr / light / flags / perspective fields are baked
 * per-span into the payload, so changes within a batch need no flush.
 * The accumulator MUST flush before any other GPU command the queued
 * spans should observe in ring order — bind_fb, bind_texture, clear,
 * triangles*, kick, finish.  of_emit_blit submits its per-row spans
 * via of_emit_span so it auto-batches without special handling. */
static of_gpu_span_t span_buf[OF_GPU_BATCH_MAX_SPANS];
static int           span_buf_count;

static inline void flush_span_batch(void)
{
    if (span_buf_count > 0) {
        of_gpu_draw_spans_batch(span_buf, span_buf_count);
        span_buf_count = 0;
    }
}

void of_emit_init(void)
{
    /* Pulse both soft-reset and ring-reset so the FSM + ring BRAM are
     * in a known state, mirroring gpudemo's preamble. Without this,
     * stale bytes from boot-time can be consumed as commands. */
    GPU_CTRL = 6;
    { volatile int i; for (i = 0; i < 100; i++) ; }
    of_gpu_init();

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
    /* Bracket the GPU drain itself.  flush_span_batch is a CPU-side
     * cache flush + ring header — fast and predictable; the spin in
     * of_gpu_finish() is where CPU actually blocks on GPU. */
    extern unsigned int pq_prof_gpu_wait_cycles;
    extern cvar_t       pq_cycleprof;
    int profiling = (int)pq_cycleprof.value;
    unsigned int t0 = profiling ? SYS_CYCLE_LO : 0;
    of_gpu_finish();
    if (profiling) pq_prof_gpu_wait_cycles += SYS_CYCLE_LO - t0;
}

/* Publish the CPU-side write pointer to the GPU without waiting. Lets
 * the GPU start consuming a batch of queued commands while the CPU
 * moves on to the next batch's setup work. Without this, the GPU only
 * sees new work when the ring fills (slow-path publish in
 * _gpu_ring_ensure) or when of_emit_finish() is called. */
void of_emit_kick(void)
{
    flush_span_batch();
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
    flush_span_batch();
    of_gpu_draw_triangles((const of_gpu_vertex_t *)verts, num_vertices);
}

void of_emit_triangles_batch(const of_emit_vertex_t *verts,
                             uint32_t num_vertices)
{
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
    VID_SetPalette(palette);
    of_emit_upload_colormap(host_colormap, 64 * 256);

    /* Bind the first draw surface + z-buffer to the GPU before any
     * frame, so the initial clear has somewhere to write. GPU uses
     * the cached SDRAM address for its AXI master — same underlying
     * bytes as the CPU's uncached alias, no coherence concern. */
    for (int b = 0; b < 3; b++) of_video_flip();   /* cycle to buffer 0 */
    vid.buffer = vid.conbuffer = (byte *)of_uncached(of_video_surface());
    of_emit_bind_fb((uint32_t)(uintptr_t)of_video_surface(), BASEWIDTH,
                    (uint32_t)(uintptr_t)zbuffer_storage,
                    BASEWIDTH * 2);
    /* HW clear all three back buffers. Hardware memset is faster than
     * CPU and avoids the cache flush the CPU memset would need. */
    for (int b = 0; b < 3; b++) {
        of_emit_clear(OF_EMIT_CLEAR_COLOR | OF_EMIT_CLEAR_DEPTH,
                      0, 0 /* far */);
        of_emit_finish();
        of_video_flip();
        of_emit_bind_fb((uint32_t)(uintptr_t)of_video_surface(), BASEWIDTH,
                        (uint32_t)(uintptr_t)zbuffer_storage,
                        BASEWIDTH * 2);
    }
    /* Capture the kernel's current draw slot AFTER the init-time
     * kernel-driven flip cycle has cycled through all three buffers
     * and cleared them.  acquire_next(-1) is a pure read — sync to
     * hardware state, return current buf_draw without any handoff. */
    draw_idx = of_video_acquire_next(-1, 0);
    vid.buffer = vid.conbuffer = (byte *)of_uncached(of_video_buffer_addr(draw_idx));

    Sys_Printf("VID_Init: fullbright=%d buffer=%p draw_idx=%d\n",
               vid.fullbright, vid.buffer, draw_idx);
}

void VID_Shutdown(void) { of_video_set_display_mode(OF_DISPLAY_TERMINAL); }

void VID_Update(vrect_t *rects)
{
    (void)rects;

    /* GPU-triggered flip path (cr-gpu-triggered-flip.md).
     *
     * Old path (pre-CR): of_emit_finish() → of_video_flip().  The
     * finish spun on fence_reached + STATUS_BUSY waiting for
     * m_wr_inflight to drain (the workaround for the race the
     * fence-write-completion CR fixed); the flip syscall blocked on
     * buffer ownership in the kernel.  ~340 µs/frame combined.
     *
     * New path: emit CMD_FLIP for the slot we just rendered, kick
     * the ring, then ask the kernel for the next free draw slot.
     * CMD_FLIP's drain semantics (now in RTL) replace the CPU spin;
     * of_video_acquire_next blocks ONLY when the 3-buffer ceiling
     * is hit (CPU ran more than one frame ahead of the display) —
     * steady-state cost is ~10 µs of book-keeping. */

    /* Wait for previous frame's swap to complete BEFORE emitting the
     * next CMD_FLIP — the GPU's gpu_swap_req would otherwise race with
     * the slave's still-pending bit and overwrite the queued slot.
     * This wait sits AFTER render so it overlaps with rendering: when
     * render time exceeds vsync period the wait collapses to ~0 µs. */
    of_video_wait_flip();

    flush_span_batch();
    uint32_t flip_token = of_gpu_flip_to(draw_idx);
    of_gpu_kick();

    extern cvar_t pq_cycleprof;
    int profiling = (int)pq_cycleprof.value;
    unsigned int t0 = profiling ? SYS_CYCLE_LO : 0;
    draw_idx = of_video_acquire_next(draw_idx, flip_token);
    if (profiling) pq_prof_video_flip_cycles = SYS_CYCLE_LO - t0;

    /* Bind the new buffer + z-buffer to the GPU, then HW-clear the
     * z-buffer so alias/sprite depth tests start fresh.  FB clearing
     * isn't needed — world spans cover every pixel (or sky fills any
     * gaps on the CPU side).  Kick after queueing so the clear
     * overlaps the start of the next frame's CPU work (BSP traversal,
     * surface cache build, etc.) instead of stalling on the next
     * VID_Update's flush. */
    uint8_t *new_fb = of_video_buffer_addr(draw_idx);
    vid.buffer = vid.conbuffer = (byte *)of_uncached(new_fb);

    of_emit_bind_fb((uint32_t)(uintptr_t)new_fb, BASEWIDTH,
                    (uint32_t)(uintptr_t)zbuffer_storage, BASEWIDTH * 2);
    of_emit_clear(OF_EMIT_CLEAR_DEPTH, 0, 0);
    of_emit_kick();
}

void VID_WaitSync(void) { /* triple-buffered, non-blocking flip */ }

/* Loading disc overlays: Quake calls these during long PAK reads. We
 * ignore them on this backend because the triple-buffered flip would
 * make the icon flash, and PAK reads are short enough here. */
void D_BeginDirectRect(int x, int y, byte *pbitmap, int w, int h)
{ (void)x; (void)y; (void)pbitmap; (void)w; (void)h; }
void D_EndDirectRect(int x, int y, int w, int h)
{ (void)x; (void)y; (void)w; (void)h; }
