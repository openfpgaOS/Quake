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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern viddef_t vid;

#define BASEWIDTH     320
#define BASEHEIGHT    240
#define SURFCACHE_SIZE   (2 * 1024 * 1024)

unsigned short d_8to16table[256];
unsigned       d_8to24table[256];

/* Surface cache in BSS (cacheable SDRAM). 2 MB bank. */
static byte surfcache_storage[SURFCACHE_SIZE] __attribute__((aligned(64)));

/* Z-buffer: 320x240 × 2 bytes = 150 KB. Aligned so GPU DEPTH_WRITE
 * bursts land on cache-line boundaries. */
static short zbuffer_storage[BASEWIDTH * BASEHEIGHT]
    __attribute__((aligned(64)));

/* ---- of_emit_* API (the GPU owner's public surface) --------------- */

void of_emit_init(void)
{
    /* Pulse both soft-reset and ring-reset so the FSM + ring BRAM are
     * in a known state, mirroring gpudemo's preamble. Without this,
     * stale bytes from boot-time can be consumed as commands. */
    GPU_CTRL = 6;
    { volatile int i; for (i = 0; i < 100; i++) ; }
    of_gpu_init();

    /* Quake stores 1/z in the z-buffer (bigger = nearer). GEQUAL means
     * "write the pixel if the incoming fragment is at least as near as
     * what's already on the screen." This is the one depth-func
     * Quake's renderer was designed around; without it, alias models
     * either draw on top of walls they should hide behind or disappear
     * entirely. */
    of_gpu_depth_test(OF_GPU_DEPTH_GEQUAL);

    /* of_gpu_shade_mode / of_gpu_blend were removed from the SDK along
     * with their underlying SET_SHADE / SET_BLEND commands. Gouraud is
     * always-on after Phase 4d, and blend was never implemented in
     * the datapath — nothing to enable here. */
}

void of_emit_upload_colormap(const unsigned char *cm, uint32_t size)
{
    of_gpu_colormap_upload(cm, size);
}

/* Debug helper for the D_ALIAS_GOURAUD=0 "wrong textures" symptom in
 * d_polyse.c.  Two checks:
 *   1. CPU-side: is host_colormap[0..255] actually identity-passthrough
 *      as the bisection comment assumes?  If not, the engine's belief
 *      is wrong and the bug is in WAD/colormap.lmp data, not the GPU.
 *   2. GPU-side: render a 16x16 quad with a per-texel-distinct test
 *      texture and light=0 + COLORMAP into a scratch FB.  Read back
 *      and compare against host_colormap[0..255].  Mismatches mean
 *      the cmap upload landed in the wrong cells, OR the GPU samples
 *      cmap differently than expected, OR the cmap_bram contents
 *      aren't what was uploaded.
 *
 * Call once after of_emit_upload_colormap + of_emit_bind_fb.  Output
 * goes to UART via Sys_Printf — read with `tools/capture_ocr.sh` or
 * a serial terminal. */
static byte _dbg_test_tex[256];     /* 16x16 distinct-texel pattern */
static byte _dbg_scratch_fb[16*16]; /* tiny FB for the readback */

void of_dbg_verify_cmap_row0(const unsigned char *host_cm)
{
    /* ---- (1) CPU check: is the source data identity? ---- */
    int identity = 1;
    int first_mismatch = -1;
    for (int i = 0; i < 256; i++) {
        if (host_cm[i] != (unsigned char)i) {
            if (identity) { first_mismatch = i; identity = 0; }
        }
    }
    if (!identity) {
        Sys_Printf("DBG cmap row0: NOT IDENTITY (first mismatch i=%d → 0x%02x; expected 0x%02x).\n",
                   first_mismatch, (unsigned)host_cm[first_mismatch],
                   first_mismatch & 0xFF);
        Sys_Printf("DBG cmap row0[0..31]:");
        for (int i = 0; i < 32; i++) Sys_Printf(" %02x", (unsigned)host_cm[i]);
        Sys_Printf("\n");

        /* Re-read colormap.lmp directly from disk into a 512-byte-aligned
         * buffer.  This bypasses Quake's COM_LoadHunkFile to isolate
         * "is the file content wrong" from "is the load broken".
         *
         *   raw[0..7] = 00 01 02 03 04 05 06 07          → file fine,
         *                                                  COM_LoadHunkFile
         *                                                  is the broken
         *                                                  path.  Likely
         *                                                  the same fread-
         *                                                  alignment bug
         *                                                  fixed in
         *                                                  openfpgaOS
         *                                                  64b35bc, but
         *                                                  via a different
         *                                                  call site.
         *
         *   raw matches host_colormap                    → file content
         *                                                  itself is non-
         *                                                  standard for
         *                                                  this build (mod
         *                                                  WAD, custom
         *                                                  colormap.lmp).
         *
         * 16388 = 64*256 light table + 4-byte fullbright marker.  Use
         * 512-byte alignment per the prior fread-alignment incident; the
         * SDK fread is supposed to be fixed but worth being defensive
         * here. */
        static byte raw[16388] __attribute__((aligned(512)));
        FILE *f = fopen("colormap.lmp", "rb");
        if (!f) f = fopen("gfx/colormap.lmp", "rb");
        if (!f) f = fopen("id1/gfx/colormap.lmp", "rb");
        if (!f) {
            Sys_Printf("DBG: file-vs-hunk: fopen(colormap.lmp) failed; "
                       "can't isolate file vs load.\n");
            return;
        }
        memset(raw, 0xAA, sizeof raw);  /* sentinel so partial reads visible */
        int got = fread(raw, 1, sizeof raw, f);
        fclose(f);

        Sys_Printf("DBG file: read %d bytes (expected 16388).  raw[0..31]:", got);
        for (int i = 0; i < 32; i++) Sys_Printf(" %02x", (unsigned)raw[i]);
        Sys_Printf("\n");

        if (got <= 0) {
            Sys_Printf("DBG: fread returned %d — load path is broken.  "
                       "Same shape as the openfpgaOS fread-alignment bug "
                       "(64b35bc).  Check the SDK fopen/fread path Quake "
                       "uses (Sys_FileOpenRead → fopen).\n", got);
            return;
        }

        /* Compare raw vs host_colormap row 0 explicitly. */
        int matches = 0;
        for (int i = 0; i < 256; i++)
            if (raw[i] == host_cm[i]) matches++;
        Sys_Printf("DBG file vs hunk row 0: %d/256 bytes match.\n", matches);

        /* Is the file itself identity? */
        int file_identity = 1;
        for (int i = 0; i < 256; i++) {
            if (raw[i] != (byte)i) { file_identity = 0; break; }
        }
        if (file_identity) {
            Sys_Printf("DBG: file row 0 IS identity but host_colormap is "
                       "NOT — the load path corrupted/truncated it.  "
                       "Inspect COM_LoadHunkFile (host.c:988) and the "
                       "underlying Sys_FileOpenRead → fread chain.\n");
        } else if (matches == 256) {
            Sys_Printf("DBG: file content matches host_colormap.  Either "
                       "this WAD ships a non-identity colormap.lmp, or "
                       "vid.fullbright's offset (%d ints = %d bytes) is "
                       "wrong for this file format.  Standard id1 "
                       "colormap.lmp is 16388 bytes (16384 light table + "
                       "4-byte fullbright marker at the very end, offset "
                       "16384/4 = 4096 ints, NOT 2048).\n",
                       2048, 2048 * 4);
        } else {
            Sys_Printf("DBG: partial match (%d/256).  File content is "
                       "non-standard AND the load path possibly modified "
                       "it.  Mixed bug — investigate both.\n", matches);
        }
        return;
    }
    Sys_Printf("DBG cmap row0: host_colormap[0..255] is identity ✓\n");

    /* ---- (2) GPU readback: render through cmap row 0 ---- */
    /* Distinct-texel test texture: tex[t*16+s] = (t<<4)|s.  Identical
     * to tb_gpu's test_triangle_light_zero_identity_cmap pattern. */
    for (int t = 0; t < 16; t++)
        for (int s = 0; s < 16; s++)
            _dbg_test_tex[t * 16 + s] = (byte)((t << 4) | s);
    of_emit_cache_clean(_dbg_test_tex, sizeof(_dbg_test_tex));

    /* Clear the scratch FB and bind it. */
    memset(_dbg_scratch_fb, 0xCC, sizeof(_dbg_scratch_fb));
    of_emit_cache_clean(_dbg_scratch_fb, sizeof(_dbg_scratch_fb));
    of_emit_bind_fb((uint32_t)(uintptr_t)_dbg_scratch_fb, 16,
                    (uint32_t)(uintptr_t)d_pzbuffer, BASEWIDTH * 2);

    /* Bind test tex.  Then render two triangles forming a 16x16 quad
     * with affine UVs spanning the full texture.  All vertex.r=0
     * forces sp_light_q=0 → cmap row 0. */
    of_gpu_texture_t test_tex = {
        .addr = (uint32_t)(uintptr_t)_dbg_test_tex,
        .width = 16, .height = 16,
        .format = OF_GPU_TEXFMT_I8,
        .wrap_s = OF_GPU_WRAP_REPEAT, .wrap_t = OF_GPU_WRAP_REPEAT,
    };
    of_gpu_bind_texture(&test_tex);

    of_gpu_vertex_t v[6] = {
        /* Tri 0: (0,0)-(16,0)-(0,16) */
        { 0,    0,    0, 0, 0,         0,         0x10000, 0,0,0,0xFF },
        { 16*16,0,    0, 0, 16<<16,    0,         0x10000, 0,0,0,0xFF },
        { 0,    16*16,0, 0, 0,         16<<16,    0x10000, 0,0,0,0xFF },
        /* Tri 1: (16,0)-(16,16)-(0,16) */
        { 16*16,0,    0, 0, 16<<16,    0,         0x10000, 0,0,0,0xFF },
        { 16*16,16*16,0, 0, 16<<16,    16<<16,    0x10000, 0,0,0,0xFF },
        { 0,    16*16,0, 0, 0,         16<<16,    0x10000, 0,0,0,0xFF },
    };
    of_gpu_draw_triangles(&v[0], 3);
    of_gpu_draw_triangles(&v[3], 3);
    of_emit_finish();

    /* Read back via uncached alias so we don't hit a stale CPU line. */
    const byte *fb = (const byte *)of_uncached(_dbg_scratch_fb);
    int mismatches = 0;
    int first_bad_i = -1;
    byte first_bad_got = 0, first_bad_exp = 0;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            byte texel = (byte)((y << 4) | x);
            byte expected = host_cm[texel];   /* = texel for identity */
            byte got = fb[y * 16 + x];
            if (got != expected) {
                if (first_bad_i < 0) {
                    first_bad_i = y * 16 + x;
                    first_bad_got = got;
                    first_bad_exp = expected;
                }
                mismatches++;
            }
        }
    }
    if (mismatches == 0) {
        Sys_Printf("DBG cmap row0: GPU readback matches host_colormap (256/256) ✓\n");
        Sys_Printf("DBG: GPU light=0 + cmap row 0 path is correct.  D_ALIAS_GOURAUD=0\n");
        Sys_Printf("DBG  symptom must come from elsewhere (not the cmap path).\n");
    } else {
        Sys_Printf("DBG cmap row0: GPU readback mismatches=%d/256.\n", mismatches);
        Sys_Printf("DBG first @ FB[%d,%d] (texel 0x%02x): got 0x%02x, expected 0x%02x.\n",
                   first_bad_i & 15, first_bad_i >> 4,
                   first_bad_i & 0xFF, first_bad_got, first_bad_exp);
        Sys_Printf("DBG FB row 0:");
        for (int x = 0; x < 16; x++) Sys_Printf(" %02x", (unsigned)fb[x]);
        Sys_Printf("\n");
    }

    /* Restore the real FB binding so the rest of VID_Init isn't broken. */
    of_emit_bind_fb((uint32_t)(uintptr_t)of_video_surface(), BASEWIDTH,
                    (uint32_t)(uintptr_t)d_pzbuffer, BASEWIDTH * 2);
}

void of_emit_bind_fb(uint32_t fb_addr, int fb_stride,
                     uint32_t zb_addr, int zb_stride_bytes)
{
    of_gpu_set_framebuffer(fb_addr, (uint16_t)fb_stride);
    /* The z-buffer stride is in 16-bit words on the GPU side but
     * bytes on ours — divide. */
    of_gpu_set_zbuffer(zb_addr, (uint16_t)zb_stride_bytes);
}

void of_emit_finish(void) { of_gpu_finish(); }

void of_emit_cache_clean(const void *addr, uint32_t size)
{
    if (!addr || !size) return;
    OF_SVC->cache_clean_range((void *)(uintptr_t)addr, size);
}

void of_emit_clear(uint32_t flags, uint16_t color, uint16_t depth)
{
    of_gpu_clear(flags, color, depth);
}

void of_emit_depth_test(of_emit_depth_func_t func)
{
    of_gpu_depth_test((of_gpu_depth_func_t)func);
}

void of_emit_blend(of_emit_blend_t mode)
{
    of_gpu_blend((of_gpu_blend_t)mode);
}

void of_emit_shade_gouraud(int enable)
{
    of_gpu_shade_mode(enable);
}

uint32_t of_emit_stat_pixels(void) { return of_gpu_stat_pixels(); }
uint32_t of_emit_stat_spans(void)  { return of_gpu_stat_spans();  }

void of_emit_span(const of_emit_span_t *sp)
{
    of_gpu_draw_span((const of_gpu_span_t *)sp);
}

void of_emit_triangles(const of_emit_vertex_t *verts, uint32_t num_vertices)
{
    of_gpu_draw_triangles((const of_gpu_vertex_t *)verts, num_vertices);
}

void of_emit_bind_texture(const of_emit_texture_t *tex)
{
    of_gpu_texture_t gt = {
        .addr   = tex->addr,
        .width  = tex->width,
        .height = tex->height,
        .format = (of_gpu_texfmt_t)tex->format,
        .wrap_s = (of_gpu_wrap_t)tex->wrap_s,
        .wrap_t = (of_gpu_wrap_t)tex->wrap_t,
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
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));

    vid.buffer    = vid.conbuffer   = (byte *)of_uncached(of_video_surface());
    vid.rowbytes  = vid.conrowbytes = BASEWIDTH;

    /* Z-buffer in SDRAM. The GPU reads/writes via AXI (cache-incoherent
     * with the CPU), so we route CPU-side access through the uncached
     * alias — the CPU fallback paths still work, just slower, but GPU
     * writes are always visible to subsequent CPU reads without a
     * cache flush. */
    d_pzbuffer = (short *)of_uncached(zbuffer_storage);
    D_InitCaches(surfcache_storage, SURFCACHE_SIZE);

    of_emit_init();                  /* GPU up, GEQUAL, Gouraud on */
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
    vid.buffer = vid.conbuffer = (byte *)of_uncached(of_video_surface());

    Sys_Printf("VID_Init: fullbright=%d buffer=%p\n",
               vid.fullbright, vid.buffer);

    /* One-shot diagnostic for the D_ALIAS_GOURAUD=0 cmap-row-0 mystery
     * in d_polyse.c.  Verifies (1) host_colormap[0..255] really is
     * identity, and (2) the GPU agrees.  Output goes to UART.  Remove
     * the call once the symptom is understood. */
    of_dbg_verify_cmap_row0(host_colormap);
}

void VID_Shutdown(void) { of_video_set_display_mode(OF_DISPLAY_TERMINAL); }

void VID_Update(vrect_t *rects)
{
    (void)rects;

    of_emit_finish();
    of_video_flip();
    vid.buffer = vid.conbuffer = (byte *)of_uncached(of_video_surface());

    /* Bind the new buffer + z-buffer to the GPU, then HW-clear the
     * z-buffer so alias/sprite depth tests start fresh. FB clearing
     * isn't needed — world spans cover every pixel (or sky fills any
     * gaps on the CPU side). */
    of_emit_bind_fb((uint32_t)(uintptr_t)of_video_surface(), BASEWIDTH,
                    (uint32_t)(uintptr_t)zbuffer_storage, BASEWIDTH * 2);
    of_emit_clear(OF_EMIT_CLEAR_DEPTH, 0, 0);
}

void VID_WaitSync(void) { /* triple-buffered, non-blocking flip */ }

/* Loading disc overlays: Quake calls these during long PAK reads. We
 * ignore them on this backend because the triple-buffered flip would
 * make the icon flash, and PAK reads are short enough here. */
void D_BeginDirectRect(int x, int y, byte *pbitmap, int w, int h)
{ (void)x; (void)y; (void)pbitmap; (void)w; (void)h; }
void D_EndDirectRect(int x, int y, int w, int h)
{ (void)x; (void)y; (void)w; (void)h; }
