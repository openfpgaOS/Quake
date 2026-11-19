/*
 * vid_of.c -- Quake video backend + GPU owner for openfpgaOS.
 *
 * This is the single TU that includes the SDK's of_gpu.h — the header
 * instantiates file-scope static ring-buffer state (wrptr, fence,
 * MMIO base), so having two owners would silently desync them. Every
 * other engine file that needs to emit a span includes of_emit.h
 * instead, builds an of_emit_span_t locally, and calls of_emit_span().
 *
 * VID_Init brings up the GPU + palette + surface cache + z-buffer and
 * caches the fixed triple-buffer addresses. VID_Update drains pending GPU
 * spans and flips; draw commands carry absolute framebuffer addresses.
 */

#include "quakedef.h"
#include "d_local.h"
#include "of_emit.h"

#include "of.h"
#include "of_video.h"
#include "of_cache.h"
#include "of_services.h"
#include "of_gpu.h"                 /* static ring state lives HERE only */
#include "of_texture.h"             /* portable texture manager — driven ONLY here */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern viddef_t vid;
extern byte *r_warpbuffer;
extern cvar_t pq_gpu_safe_spans;

#define QUAKE_VIDEO_WIDTH  320
#define QUAKE_VIDEO_HEIGHT 200
#define ZBUFFER_MAX_WIDTH  320
#define ZBUFFER_MAX_HEIGHT 240
#define VIDEO_BUFFERS 3
/* Surface cache: lit/mipped world-texture blocks. Misses force a CPU
 * rebuild (the [surf] cache line in host_speeds — 26 ms / 40 misses per
 * frame in busy multiplayer scenes at the old fixed 2 MB). Size it from
 * available SDRAM at init instead: SURFCACHE_MIN is the guaranteed floor
 * (a static array, so it can never fail to allocate and never regresses
 * below the historical value); above that we malloc a fraction of the
 * app heap up to SURFCACHE_MAX, which is enough to hold a heavy scene's
 * texture working set so the miss rate falls toward zero. */
#define SURFCACHE_MIN    (2 * 1024 * 1024)
#define SURFCACHE_MAX    (8 * 1024 * 1024)
#define SURFCACHE_HEAP_FRACTION 8   /* take up to heap_size/8 */
#define PQ_GPU_SPAN_BATCH_MAX 256

#ifndef PQ_GPU_VALIDATE_DEST
#define PQ_GPU_VALIDATE_DEST 0
#endif

_Static_assert(OF_HW_GPU_PERSP == (1u << 13),
               "OF_HW_GPU_PERSP must remain runtime caps bit 13");
_Static_assert(OF_GPU_PERSP_SPAN_GROUP_MAX_LANES == 8u,
               "SDK perspective span API accepts eight lanes");
_Static_assert(OF_EMIT_COLORMAP == OF_GPU_SPAN_COLORMAP,
               "of_emit colormap flag must match GPU wire flag");
_Static_assert(OF_EMIT_SKIP_ZERO == OF_GPU_SPAN_SKIP_ZERO,
               "of_emit transparent-skip flag must match GPU wire flag");
_Static_assert(OF_EMIT_PERSP == OF_GPU_SPAN_PERSP,
               "of_emit perspective flag must match GPU wire flag");
_Static_assert(OF_HW_GPU_PARAM_SPAN_LIST == (1u << 15),
               "OF_HW_GPU_PARAM_SPAN_LIST must remain runtime caps bit 15");
_Static_assert(OF_HW_GPU_PARAM_SPAN_Z == (1u << 16),
               "OF_HW_GPU_PARAM_SPAN_Z must remain runtime caps bit 16");
_Static_assert(OF_HW_GPU_PARAM_SPAN_ZTEST == (1u << 17),
               "OF_HW_GPU_PARAM_SPAN_ZTEST must remain runtime caps bit 17");
_Static_assert(OF_HW_GPU_PARAM_SPAN_Q29_SCALE == (1u << 18),
               "OF_HW_GPU_PARAM_SPAN_Q29_SCALE must remain runtime caps bit 18");
_Static_assert(OF_HW_GPU_PARAM_TRI == (1u << 19),
               "OF_HW_GPU_PARAM_TRI must remain runtime caps bit 19");
_Static_assert(GPU_CMD_DRAW_PARAM_SPAN_LIST == 0x48,
               "param span-list opcode must match openfpgaOS");
_Static_assert(GPU_CMD_DRAW_PARAM_TRI == 0x49,
               "param-tri opcode must match openfpgaOS");
_Static_assert(OF_EMIT_PARAM_SPAN_MAX_RECORDS == OF_GPU_PARAM_SPAN_MAX_RECORDS,
               "param span-list max record count mismatch");
_Static_assert(OF_EMIT_PARAM_ATTR_AFFINE == OF_GPU_PARAM_ATTR_AFFINE &&
               OF_EMIT_PARAM_ATTR_PERSP == OF_GPU_PARAM_ATTR_PERSP &&
               OF_EMIT_PARAM_ATTR_SOLID == OF_GPU_PARAM_ATTR_SOLID &&
               OF_EMIT_PARAM_ATTR_PERSP_Q29 == OF_GPU_PARAM_ATTR_PERSP_Q29,
               "param span-list attr enum mismatch");
_Static_assert(OF_EMIT_PARAM_AXIS_X == OF_GPU_PARAM_AXIS_X &&
               OF_EMIT_PARAM_AXIS_Y == OF_GPU_PARAM_AXIS_Y,
               "param span-list axis enum mismatch");
_Static_assert(OF_EMIT_PARAM_Z_NONE == OF_GPU_PARAM_Z_NONE &&
               OF_EMIT_PARAM_Z_WRITE_ZI == OF_GPU_PARAM_Z_WRITE_ZI &&
               OF_EMIT_PARAM_Z_TEST_ZI == OF_GPU_PARAM_Z_TEST_ZI &&
               OF_EMIT_PARAM_Z_TEST_WRITE == OF_GPU_PARAM_Z_TEST_WRITE,
               "param span-list z enum mismatch");
_Static_assert(sizeof(of_emit_param_span_record_t) ==
               sizeof(of_gpu_param_span_record_t),
               "param span record layout mismatch");
_Static_assert(__builtin_offsetof(of_emit_param_span_record_t, count) ==
               __builtin_offsetof(of_gpu_param_span_record_t, count),
               "param span record count offset mismatch");
_Static_assert(sizeof(of_emit_param_span_list_t) ==
               sizeof(of_gpu_param_span_list_t),
               "param span list layout mismatch");
_Static_assert(__builtin_offsetof(of_emit_param_span_list_t, z_mode) ==
               __builtin_offsetof(of_gpu_param_span_list_t, z_mode),
               "param span list z_mode offset mismatch");
_Static_assert(__builtin_offsetof(of_emit_param_span_list_t, attr_origin) ==
               __builtin_offsetof(of_gpu_param_span_list_t, attr_origin),
               "param span list attr offset mismatch");

unsigned short d_8to16table[256];
unsigned       d_8to24table[256];

/* Triple-buffer slot index used only when the runtime advertises the
 * GPU-triggered CMD_FLIP path.  Kernel-driven flips keep this at -1. */
static int     draw_idx = -1;
static int     pending_flip_idx = -1;
static uint32_t pending_flip_token;
static int     flip_present_pending;
static int     zbuffer_cpu_cached;
static uint32_t of_emit_caps;
static int     of_emit_ready;
static int     of_emit_cpu_sync_needed;
static int     of_emit_span_cont;     /* OF_HW_GPU_SPAN_CONT advertised */
/* vid_of.c is the SINGLE TU that drives the portable texture manager
 * (of_texture.h) — it owns the placement/upload/fetch-domain decisions so the
 * rest of the engine deals only in opaque GPU addresses and a couple of
 * domain-bind calls.  of_texture_create() puts static textures in the best
 * memory (the dedicated fast tier where present, the other otherwise); dynamic
 * textures (sky/turbulent) stay out of the fast tier.  These handles are the
 * bind representatives: one static (the world's fetch domain) and one dynamic. */
static of_texture_t of_tex_world;     /* representative static texture (world)   */
static of_texture_t of_tex_dynamic;   /* representative dynamic texture (SDRAM)  */
static of_texture_t of_tex_sky;       /* the animated sky source (dynamic)       */
static int          of_tex_world_valid;
/* r_fasttex (read once per level): when 0, world textures go to main memory and
 * the domain binds no-op, isolating the triangle path from the fast-texture
 * path.  When 1 (and a fast tier exists), the normal fast path runs. */
extern cvar_t       r_fasttex;
static int          of_fasttex_active;
static int          of_fasttex_spilled;   /* world textures that didn't fit the fast tier */
/* OF_HW_GPU_VERT_TRI: GPU derives the attribute planes from raw vertices
 * (CMD_DRAW_VERT_TRI). Set in of_emit_init; selects the alias lowering. */
static int     g_vert_tri;
static uint8_t *vid_fb_raw[VIDEO_BUFFERS];
static byte    *vid_fb_uncached[VIDEO_BUFFERS];
static of_video_mode_t vid_mode;
static uint32_t vid_frame_bytes;

typedef struct of_emit_clean_once_s {
    uintptr_t addr;
    uint32_t size;
} of_emit_clean_once_t;

#define OF_EMIT_CLEAN_ONCE_SLOTS 512
#define OF_EMIT_CLEAN_ONCE_HASH_SIZE 1024
#define OF_EMIT_CLEAN_ONCE_HASH_MASK (OF_EMIT_CLEAN_ONCE_HASH_SIZE - 1u)
static of_emit_clean_once_t of_emit_clean_once_slots[OF_EMIT_CLEAN_ONCE_SLOTS];
static uint16_t of_emit_clean_once_hash[OF_EMIT_CLEAN_ONCE_HASH_SIZE];
static uint32_t of_emit_clean_once_next;

/* GPU sync counters for the host_speeds trace (of_emit_perf_take).
 * Microsecond phases are sampled only while host_speeds >= 2, latched once
 * per frame in VID_Update. */
extern cvar_t host_speeds;
static of_emit_perf_t vid_perf;
static uint32_t vid_perf_epoch;         /* tex flush epoch at the last take */
static int      vid_perf_timing;
uint32_t of_emit_tex_flush_epoch;

static inline uint32_t vid_perf_now(void)
{
    return vid_perf_timing ? (uint32_t)of_time_us() : 0u;
}

/* Surface cache in BSS (cacheable SDRAM). 2 MB bank. */
/* Floor allocation, always present. A larger cache (when SDRAM allows)
 * is malloc'd over this at init; this static block is the fallback. */
static byte surfcache_storage[SURFCACHE_MIN] __attribute__((aligned(64)));

/* Keep enough z storage for the old default 320x240 fallback as well as
 * Quake's preferred 320x200 source mode. */
static short zbuffer_storage[ZBUFFER_MAX_WIDTH * ZBUFFER_MAX_HEIGHT]
    __attribute__((aligned(64)));

/* ---- of_emit_* API (the GPU owner's public surface) --------------- */

/* Span stream.
 *
 * Quake's world / sky / sprite / alias-poly inner loops submit one
 * span per scanline.  of_emit_span packs each span straight into the open
 * affine or perspective lane group; a group closes on a key change, a lane
 * overlap, a full group, a span of the other kind, or every
 * PQ_GPU_SPAN_BATCH_MAX submitted spans (flush_span_batch, which also
 * kicks).
 *
 * The stream MUST flush before any other GPU command the queued
 * spans should observe in ring order: bind_fb, bind_texture, clear,
 * triangles*, kick, finish. */
static int span_pending;            /* spans submitted since the last flush */
/* pq_gpu_safe_spans sampled once per batch instead of once per span: the
 * cvar is a debug kill switch set from the console between frames, and the
 * float load plus fcvt sat on the dependency chain of every of_emit_span. */
static int span_safe_mode;

static inline int32_t gpu_i32_add_mul(int32_t a, int32_t step, int n)
{
    return (int32_t)((uint32_t)a + (uint32_t)step * (uint32_t)n);
}

static inline int32_t gpu_i32_sub_mul(int32_t a, int32_t step, int n)
{
    return (int32_t)((uint32_t)a - (uint32_t)step * (uint32_t)n);
}

static void gpu_range(uint32_t fb_addr, int32_t stride, uint32_t count,
                      uint64_t *lo, uint64_t *hi)
{
    int64_t first = (int64_t)(uint64_t)fb_addr;
    int64_t last = first;

    if (count > 1)
        last = first + (int64_t)stride * (int64_t)(count - 1u);

    if (first <= last) {
        *lo = (uint64_t)first;
        *hi = (uint64_t)last + 1u;
    } else {
        *lo = (uint64_t)last;
        *hi = (uint64_t)first + 1u;
    }
}

static int gpu_ranges_overlap(uint64_t a_lo, uint64_t a_hi,
                              uint64_t b_lo, uint64_t b_hi)
{
    return a_lo < b_hi && b_lo < a_hi;
}

static void vid_cache_framebuffers(void)
{
    for (int i = 0; i < VIDEO_BUFFERS; i++) {
        uint8_t *fb = of_video_buffer_addr(i);
        if (!fb)
            Sys_Error("video_buffer_addr(%d) returned NULL\n", i);
        vid_fb_raw[i] = fb;
        vid_fb_uncached[i] = (byte *)of_uncached(fb);
    }
}

static void vid_configure_mode(void)
{
    of_video_mode_t want = {
        .width = QUAKE_VIDEO_WIDTH,
        .height = QUAKE_VIDEO_HEIGHT,
        .stride = 0,
        .color_mode = OF_VIDEO_MODE_8BIT,
        .reserved = 0,
    };
    of_video_mode_t mode;

    if (of_video_check_mode(&want, &mode) == 0 &&
        of_video_set_mode(&mode) == 0) {
        of_video_get_mode(&mode);
    } else {
        of_video_get_mode(&mode);
    }

    if (mode.color_mode != OF_VIDEO_MODE_8BIT ||
        mode.width == 0 || mode.height == 0 || mode.stride == 0) {
        Sys_Error("Quake requires an 8-bit framebuffer mode\n");
    }
    if (mode.width > ZBUFFER_MAX_WIDTH || mode.height > ZBUFFER_MAX_HEIGHT) {
        Sys_Error("video mode %ux%u exceeds Quake zbuffer storage\n",
                  mode.width, mode.height);
    }

    vid_mode = mode;
    vid.maxwarpwidth = vid.width = vid.conwidth = mode.width;
    vid.maxwarpheight = vid.height = vid.conheight = mode.height;
    vid.rowbytes = vid.conrowbytes = mode.stride;
    vid_frame_bytes = (uint32_t)mode.stride * (uint32_t)mode.height;
}

static byte *vid_uncached_for_surface(uint8_t *fb)
{
    for (int i = 0; i < VIDEO_BUFFERS; i++) {
        if (vid_fb_raw[i] == fb)
            return vid_fb_uncached[i];
    }

    return (byte *)of_uncached(fb);
}

static void vid_set_buffer_index(int idx)
{
    if (idx < 0 || idx >= VIDEO_BUFFERS || !vid_fb_uncached[idx])
        Sys_Error("invalid video draw buffer index %d\n", idx);

    vid.buffer = vid.conbuffer = vid_fb_uncached[idx];
}

int VID_CurrentBufferIndex(void)
{
    if (draw_idx >= 0 && draw_idx < VIDEO_BUFFERS)
        return draw_idx;

    for (int i = 0; i < VIDEO_BUFFERS; i++) {
        if (vid.buffer == vid_fb_uncached[i])
            return i;
    }

    return -1;
}

#if PQ_GPU_VALIDATE_DEST
static void gpu_span_range(const of_emit_span_t *sp,
                           uint64_t *lo, uint64_t *hi)
{
    gpu_range(sp->fb_addr, sp->fb_stride, sp->count, lo, hi);
}

static int gpu_range_inside(uint64_t lo, uint64_t hi,
                            uintptr_t base, uint32_t size)
{
    uint64_t b = (uint64_t)base;
    return lo >= b && hi <= b + (uint64_t)size;
}

static int gpu_dest_range_known(uint64_t lo, uint64_t hi)
{
    for (int i = 0; i < VIDEO_BUFFERS; i++) {
        if (vid_fb_uncached[i] &&
            gpu_range_inside(lo, hi, (uintptr_t)vid_fb_uncached[i],
                             vid_frame_bytes))
            return 1;
    }

    if (r_warpbuffer &&
        gpu_range_inside(lo, hi, (uintptr_t)r_warpbuffer, 320u * 200u))
        return 1;

    return 0;
}

static void gpu_validate_dest_range(const char *what, uint64_t lo, uint64_t hi)
{
    static int reports;

    if (gpu_dest_range_known(lo, hi))
        return;

    if (reports < 16) {
        Sys_Printf("GPU %s destination out of range: %08x..%08x "
                   "vid=%p draw=%d\n",
                   what, (unsigned)lo, (unsigned)hi, vid.buffer, draw_idx);
        reports++;
    }
}

static void gpu_validate_span_dest(const of_emit_span_t *sp)
{
    uint64_t lo, hi;

    gpu_span_range(sp, &lo, &hi);
    gpu_validate_dest_range("span", lo, hi);
}

static void gpu_validate_rect_dest(uint32_t fb_addr, int fb_stride,
                                   int w, int h)
{
    uint64_t first = fb_addr;
    uint64_t last;

    if (w <= 0 || h <= 0)
        return;

    last = first + (uint64_t)(h - 1) * (uint32_t)fb_stride +
        (uint32_t)w;
    gpu_validate_dest_range("rect", first, last);
}

static void gpu_validate_param_dest(const of_emit_param_span_list_t *params,
                                    const of_emit_param_span_record_t *records,
                                    uint32_t record_count)
{
    int32_t span_step;

    if (!params || !records)
        return;

    span_step = (params->span_axis == OF_EMIT_PARAM_AXIS_Y) ?
        params->fb_major_step : params->fb_minor_step;

    for (uint32_t i = 0; i < record_count; i++) {
        if (records[i].count == 0)
            continue;

        uint64_t first = (uint64_t)params->fb_base +
            (uint64_t)records[i].v * (uint32_t)params->fb_major_step +
            (uint64_t)records[i].u * (uint32_t)params->fb_minor_step;
        int64_t last_addr = (int64_t)first +
            (int64_t)span_step * (int64_t)(records[i].count - 1u);
        uint64_t lo = first;
        uint64_t hi = first + 1u;

        if (last_addr < (int64_t)first) {
            lo = (uint64_t)last_addr;
            hi = first + 1u;
        } else {
            hi = (uint64_t)last_addr + 1u;
        }

        gpu_validate_dest_range("param", lo, hi);
    }
}

static void gpu_validate_known_counts(const of_emit_param_span_record_t *records,
                                      uint32_t record_count)
{
    static int reports;

    for (uint32_t i = 0; i < record_count; i++) {
        if (records[i].count != 0 && records[i].count <= 0xFFFu)
            continue;
        if (reports < 16) {
            Sys_Printf("GPU param record %u count %u outside 1..4095\n",
                       i, records[i].count);
            reports++;
        }
    }
}
#else
static inline void gpu_validate_span_dest(const of_emit_span_t *sp)
{
    (void)sp;
}

static inline void gpu_validate_rect_dest(uint32_t fb_addr, int fb_stride,
                                          int w, int h)
{
    (void)fb_addr; (void)fb_stride; (void)w; (void)h;
}

static inline void gpu_validate_param_dest(const of_emit_param_span_list_t *params,
                                           const of_emit_param_span_record_t *records,
                                           uint32_t record_count)
{
    (void)params; (void)records; (void)record_count;
}

static inline void gpu_validate_known_counts(const of_emit_param_span_record_t *records,
                                             uint32_t record_count)
{
    (void)records; (void)record_count;
}
#endif

typedef struct {
    of_gpu_persp_span_group_t group;
    int lanes;
    int base_y;
    int major_ready;
} gpu_persp_builder_t;

static int gpu_persp_decode_span(const of_emit_span_t *sp,
                                  int *row, int *start,
                                  uint32_t *row_fb,
                                  int32_t *row_sdivz,
                                  int32_t *row_tdivz,
                                  int32_t *row_zi)
{
    /* Normalize a horizontal framebuffer row span for the SDK perspective
     * group helper, which lowers to GPU_CMD_DRAW_PARAM_SPAN_LIST:
     *   row_fb = row base, start[] = original u, count[] = span length.
     * The group builder then uses major_fb_step = vid.rowbytes and
     * minor_fb_step = 1. Non-row spans fall back to a single-lane command. */
    if (sp->fb_stride != 1 || vid.rowbytes <= 0 || vid.height <= 0)
        return 0;

    uintptr_t fb = (uintptr_t)sp->fb_addr;
    uintptr_t base = (uintptr_t)vid.buffer;
    if (fb < base)
        return 0;

    uint32_t off = (uint32_t)(fb - base);
    uint32_t max_off = (uint32_t)vid.rowbytes * (uint32_t)vid.height;
    if (off >= max_off)
        return 0;

    int y = (int)(off / (uint32_t)vid.rowbytes);
    int u = (int)(off - (uint32_t)y * (uint32_t)vid.rowbytes);
    if (u < 0 || u > 0x7FFF)
        return 0;
    if ((uint32_t)u + (uint32_t)sp->count > (uint32_t)vid.rowbytes)
        return 0;

    *row = y;
    *start = u;
    *row_fb = sp->fb_addr - (uint32_t)u;
    *row_sdivz = gpu_i32_sub_mul(sp->sdivz, sp->sdivz_step, u);
    *row_tdivz = gpu_i32_sub_mul(sp->tdivz, sp->tdivz_step, u);
    *row_zi = gpu_i32_sub_mul(sp->zi_persp, sp->zi_step, u);
    return 1;
}

static int gpu_persp_try_append(gpu_persp_builder_t *builder,
                                const of_emit_span_t *sp)
{
    int row, start;
    uint32_t row_fb;
    int32_t row_sdivz, row_tdivz, row_zi;
    of_gpu_persp_span_group_t *group = &builder->group;

    if ((sp->flags & OF_EMIT_PERSP) == 0 ||
        sp->count == 0 ||
        !gpu_persp_decode_span(sp, &row, &start, &row_fb,
                               &row_sdivz, &row_tdivz, &row_zi))
        return 0;

    if (builder->lanes == 0) {
        memset(group, 0, sizeof(*group));
        group->fb_addr = row_fb;
        group->tex_addr = sp->tex_addr;
        group->flags = sp->flags;
        group->colormap_id = sp->colormap_id;
        group->minor_fb_step = sp->fb_stride;
        group->tex_width = sp->tex_width;
        group->tex_w_mask = sp->tex_w_mask;
        group->tex_h_mask = sp->tex_h_mask;
        group->sdivz = row_sdivz;
        group->tdivz = row_tdivz;
        group->zi_persp = row_zi;
        group->sdivz_minor_step = sp->sdivz_step;
        group->tdivz_minor_step = sp->tdivz_step;
        group->zi_minor_step = sp->zi_step;
        group->light = (int32_t)((uint32_t)(sp->light & 0x3Fu) << 16);
        builder->base_y = row;
        builder->major_ready = 0;
    } else {
        int lane = builder->lanes;

        if (lane >= (int)OF_GPU_PERSP_SPAN_GROUP_MAX_LANES ||
            row != builder->base_y + lane ||
            group->tex_addr != sp->tex_addr ||
            group->flags != sp->flags ||
            group->colormap_id != sp->colormap_id ||
            group->minor_fb_step != sp->fb_stride ||
            group->tex_width != sp->tex_width ||
            group->tex_w_mask != sp->tex_w_mask ||
            group->tex_h_mask != sp->tex_h_mask ||
            group->sdivz_minor_step != sp->sdivz_step ||
            group->tdivz_minor_step != sp->tdivz_step ||
            group->zi_minor_step != sp->zi_step ||
            group->light != (int32_t)((uint32_t)(sp->light & 0x3Fu) << 16))
            return 0;

        if (lane == 1 && !builder->major_ready) {
            group->major_fb_step = (int32_t)vid.rowbytes;
            if (row_fb != group->fb_addr + (uint32_t)group->major_fb_step)
                return 0;
            group->sdivz_major_step =
                (int32_t)((uint32_t)row_sdivz - (uint32_t)group->sdivz);
            group->tdivz_major_step =
                (int32_t)((uint32_t)row_tdivz - (uint32_t)group->tdivz);
            group->zi_major_step =
                (int32_t)((uint32_t)row_zi - (uint32_t)group->zi_persp);
            builder->major_ready = 1;
        } else if (builder->major_ready) {
            if (row_fb != group->fb_addr +
                    (uint32_t)group->major_fb_step * (uint32_t)lane ||
                row_sdivz != gpu_i32_add_mul(group->sdivz,
                                             group->sdivz_major_step, lane) ||
                row_tdivz != gpu_i32_add_mul(group->tdivz,
                                             group->tdivz_major_step, lane) ||
                row_zi != gpu_i32_add_mul(group->zi_persp,
                                          group->zi_major_step, lane))
                return 0;
        } else {
            return 0;
        }
    }

    group->start[builder->lanes] = (int16_t)start;
    group->count[builder->lanes] = sp->count;
    builder->lanes++;
    return 1;
}

static void gpu_flush_persp_group(gpu_persp_builder_t *builder)
{
    if (builder->lanes <= 0)
        return;

    builder->group.lane_count = (uint8_t)builder->lanes;
    of_gpu_draw_persp_span_group(&builder->group);
    builder->lanes = 0;
    builder->major_ready = 0;
}

static void gpu_draw_persp_span(const of_emit_span_t *sp)
    __attribute__((noinline, cold));
static void gpu_draw_persp_span(const of_emit_span_t *sp)
{
    of_gpu_persp_span_group_t group;

    memset(&group, 0, sizeof(group));
    group.fb_addr = sp->fb_addr;
    group.tex_addr = sp->tex_addr;
    group.lane_count = 1;
    group.flags = sp->flags;
    group.colormap_id = sp->colormap_id;
    group.minor_fb_step = sp->fb_stride;
    group.tex_width = sp->tex_width;
    group.tex_w_mask = sp->tex_w_mask;
    group.tex_h_mask = sp->tex_h_mask;
    group.count[0] = sp->count;
    group.sdivz = sp->sdivz;
    group.tdivz = sp->tdivz;
    group.zi_persp = sp->zi_persp;
    group.sdivz_minor_step = sp->sdivz_step;
    group.tdivz_minor_step = sp->tdivz_step;
    group.zi_minor_step = sp->zi_step;
    group.light = (int32_t)((uint32_t)(sp->light & 0x3Fu) << 16);

    of_gpu_draw_persp_span_group(&group);
}

static gpu_persp_builder_t span_persp;

/* Open affine lane group, staged in GPU wire order.
 *
 * Each lane is the 7-word 0x48 compact-direct record of
 * of_gpu_draw_affine_span_group(); the four shared header words are built
 * once when the group opens, so closing the group is four stores plus a
 * straight word copy into the ring instead of a per-lane gather out of a
 * struct of parallel arrays.
 *
 * Word 7 of each slot (past the wire record) is the lane's end address,
 * fb_addr + count: the exact lane range while every lane has unit stride
 * and does not wrap 32 bits.  Any other lane marks the group wide, which
 * uses the 64-bit range test.  span_affine_hi is the highest end of a
 * narrow group, so a span starting at or above it cannot overlap any lane
 * and skips the per-lane scan — the case every downward scanline walk
 * hits. */
#define SPAN_AFFINE_LANE_WORDS OF_GPU_AFFINE_SPAN_GROUP_LANE_WORDS
#define SPAN_AFFINE_SLOT_WORDS 8u    /* 7 wire words + end; power of two */
#define SPAN_AFFINE_SLOT_END   7u

static uint32_t span_affine_lane[OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES]
                               [SPAN_AFFINE_SLOT_WORDS];
static uint32_t span_affine_hdr[4];   /* lane count ORed into [0] at emit */
static uint32_t span_affine_hi;
static int32_t  span_affine_fb_step;
static uint16_t span_affine_tex_width;
static uint16_t span_affine_tex_w_mask;
static uint16_t span_affine_tex_h_mask;
static uint8_t  span_affine_flags;
static int      span_affine_lanes;
static int      span_affine_wide;
static int      span_group_ok;        /* OF_HW_GPU_SPAN_GROUP advertised */

/* Out of line so the SDK's publish path (cache flush, MMIO ring copy, DMA
 * doorbell — all cold) stays out of the span emitter's I-cache lines. */
static void gpu_span_reserve(uint32_t words) __attribute__((noinline));
static void gpu_span_reserve(uint32_t words)
{
    _gpu_stream_reserve_words(words);
}

/* Emit the open group as 0x48 commands of up to four native lanes: the
 * same words of_gpu_draw_affine_span_group() writes, with the command word
 * claimed alongside its payload instead of going through _gpu_ring_write.
 * The reservation stays per command — its size decides where the staged
 * stream is published, and moving a publish moves GPU execution against
 * the CPU's surface-cache reuse.  Every staged lane has a non-zero count
 * (of_emit_span drops empty spans), so the SDK's all-zero-chunk skip
 * cannot fire here. */
static void gpu_draw_affine_group(void) __attribute__((noinline));
static void gpu_draw_affine_group(void)
{
    uint32_t lanes = (uint32_t)span_affine_lanes;
    uint32_t left;
    const uint32_t *src;

    span_affine_lanes = 0;
    if (!span_group_ok)
        return;
    src = span_affine_lane[0];

    for (left = lanes; left != 0;) {
        uint32_t chunk = left;
        uint32_t words, *w;

        if (chunk > OF_GPU_AFFINE_SPAN_GROUP_MAX_NATIVE_LANES)
            chunk = OF_GPU_AFFINE_SPAN_GROUP_MAX_NATIVE_LANES;
        left -= chunk;
        words = 5u + SPAN_AFFINE_LANE_WORDS * chunk;

        _gpu_span_hdr_valid = 0;   /* overwrites the GPU's shared staging */
        gpu_span_reserve(words);
        w = _gpu_ring_claim();
        *w++ = ((uint32_t)GPU_CMD_DRAW_PARAM_SPAN_LIST << 24) | (words - 1u);
        *w++ = (chunk << 28) | span_affine_hdr[0];
        *w++ = span_affine_hdr[1];
        *w++ = span_affine_hdr[2];
        *w++ = span_affine_hdr[3];
        do {
            w[0] = src[0];
            w[1] = src[1];
            w[2] = src[2];
            w[3] = src[3];
            w[4] = src[4];
            w[5] = src[5];
            w[6] = src[6];
            w += SPAN_AFFINE_LANE_WORDS;
            src += SPAN_AFFINE_SLOT_WORDS;
        } while (--chunk);
        _gpu_ring_commit(words);
    }
}

static int gpu_affine_overlap_wide(const of_emit_span_t *sp)
    __attribute__((noinline));
static int gpu_affine_overlap_wide(const of_emit_span_t *sp)
{
    uint64_t lo, hi, lane_lo, lane_hi;

    gpu_range(sp->fb_addr, sp->fb_stride, sp->count, &lo, &hi);
    for (int i = 0; i < span_affine_lanes; i++) {
        /* end - fb_addr is the lane's count, stride or no stride. */
        gpu_range(span_affine_lane[i][0], span_affine_fb_step,
                  span_affine_lane[i][SPAN_AFFINE_SLOT_END] -
                      span_affine_lane[i][0],
                  &lane_lo, &lane_hi);
        if (gpu_ranges_overlap(lane_lo, lane_hi, lo, hi))
            return 1;
    }

    return 0;
}

/* The span stream is one surface's scanlines after another, so the group
 * key matches, the span lies past every open lane and the group has room:
 * hint that shape so the appending path is the fall-through. */
static inline void gpu_stream_affine_span(const of_emit_span_t *sp)
{
    const uint32_t fb = sp->fb_addr;
    const uint32_t end = fb + sp->count;
    const int narrow = sp->fb_stride == 1 && end > fb;
    int lane = span_affine_lanes;
    uint32_t *rec;

    if (__builtin_expect(span_persp.lanes > 0, 0))
        gpu_flush_persp_group(&span_persp);

    if (__builtin_expect(lane > 0, 1)) {
        int reject = span_affine_flags != sp->flags ||
                     span_affine_tex_width != sp->tex_width ||
                     span_affine_tex_w_mask != sp->tex_w_mask ||
                     span_affine_tex_h_mask != sp->tex_h_mask ||
                     span_affine_fb_step != sp->fb_stride;

        if (__builtin_expect(!reject, 1)) {
            if (__builtin_expect(narrow && !span_affine_wide, 1)) {
                if (__builtin_expect(fb < span_affine_hi, 0)) {
                    for (int i = 0; i < lane; i++) {
                        if (fb < span_affine_lane[i][SPAN_AFFINE_SLOT_END] &&
                            span_affine_lane[i][0] < end) {
                            reject = 1;
                            break;
                        }
                    }
                }
            } else {
                reject = gpu_affine_overlap_wide(sp);
            }
        }
        if (__builtin_expect(reject, 0)) {
            gpu_draw_affine_group();
            lane = 0;
        }
    }

    if (__builtin_expect(lane == 0, 0)) {
        span_affine_flags = sp->flags;
        span_affine_tex_width = sp->tex_width;
        span_affine_tex_w_mask = sp->tex_w_mask;
        span_affine_tex_h_mask = sp->tex_h_mask;
        span_affine_fb_step = sp->fb_stride;
        span_affine_hdr[0] = (((uint32_t)sp->flags & ~(uint32_t)OF_GPU_SPAN_PERSP)
                              & 0xFFu) << 20;
        span_affine_hdr[1] = (uint32_t)sp->tex_width;
        span_affine_hdr[2] = ((uint32_t)sp->tex_h_mask << 16) |
                             (uint32_t)sp->tex_w_mask;
        span_affine_hdr[3] = (uint32_t)(int32_t)sp->fb_stride;
        span_affine_wide = 0;
        span_affine_hi = 0;
    }

    rec = span_affine_lane[lane];
    rec[0] = fb;
    rec[1] = sp->tex_addr;
    rec[2] = (((uint32_t)sp->colormap_id & 0x0Fu) << 28) |
             (((uint32_t)sp->light & 0x3Fu) << 16) |
             _gpu_count12((uint32_t)sp->count);
    rec[3] = (uint32_t)sp->s;
    rec[4] = (uint32_t)sp->t;
    rec[5] = (uint32_t)sp->sstep;
    rec[6] = (uint32_t)sp->tstep;
    rec[SPAN_AFFINE_SLOT_END] = end;
    if (__builtin_expect(narrow, 1)) {
        if (end > span_affine_hi)
            span_affine_hi = end;
    } else {
        span_affine_wide = 1;
    }
    span_affine_lanes = ++lane;
    if (__builtin_expect(lane == (int)OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES, 0))
        gpu_draw_affine_group();
}

static inline void gpu_stream_persp_span(const of_emit_span_t *sp)
{
    if (span_affine_lanes > 0)
        gpu_draw_affine_group();

    if (!gpu_persp_try_append(&span_persp, sp)) {
        gpu_flush_persp_group(&span_persp);
        if (!gpu_persp_try_append(&span_persp, sp))
            gpu_draw_persp_span(sp);
    }
    if (span_persp.lanes == (int)OF_GPU_PERSP_SPAN_GROUP_MAX_LANES)
        gpu_flush_persp_group(&span_persp);
}

static inline int flush_span_batch(void)
{
    if (span_pending > 0) {
        int used_dma = (_gpu_batch_buf != NULL);

        gpu_flush_persp_group(&span_persp);
        if (span_affine_lanes > 0)
            gpu_draw_affine_group();
        of_gpu_kick();

        span_pending = 0;
        span_safe_mode = (int)pq_gpu_safe_spans.value;
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
    if (caps->sdram_base == 0)
        Sys_Error("Quake requires GPU command-DMA SDRAM\n");

    of_emit_caps = OF_EMIT_CAP_SPAN;
    /* Do not infer perspective support from FRAGPIPE. Only runtime caps select
     * perspective-capable submission paths. */
    if (hw & OF_HW_GPU_PERSP)
        of_emit_caps |= OF_EMIT_CAP_PERSP;
    if (hw & OF_HW_GPU_FRAGPIPE)
        of_emit_caps |= OF_EMIT_CAP_FRAGPIPE;
    if (hw & OF_HW_GPU_ALPHA)
        of_emit_caps |= OF_EMIT_CAP_ALPHA;
    if (flip_hw && flip_services)
        of_emit_caps |= OF_EMIT_CAP_FLIP;
    if (hw & OF_HW_GPU_PARAM_SPAN_LIST)
        of_emit_caps |= OF_EMIT_CAP_PARAM_SPAN_LIST;
    if ((hw & (OF_HW_GPU_PARAM_SPAN_LIST | OF_HW_GPU_PARAM_SPAN_Z)) ==
        (OF_HW_GPU_PARAM_SPAN_LIST | OF_HW_GPU_PARAM_SPAN_Z))
        of_emit_caps |= OF_EMIT_CAP_PARAM_SPAN_Z;
    if ((hw & (OF_HW_GPU_PARAM_SPAN_LIST | OF_HW_GPU_PARAM_SPAN_ZTEST)) ==
        (OF_HW_GPU_PARAM_SPAN_LIST | OF_HW_GPU_PARAM_SPAN_ZTEST))
        of_emit_caps |= OF_EMIT_CAP_PARAM_SPAN_ZTEST;
    if ((hw & (OF_HW_GPU_PARAM_SPAN_LIST | OF_HW_GPU_PARAM_SPAN_Q29_SCALE)) ==
        (OF_HW_GPU_PARAM_SPAN_LIST | OF_HW_GPU_PARAM_SPAN_Q29_SCALE))
        of_emit_caps |= OF_EMIT_CAP_Q29_SCALE;
    /* Hardware triangle edge walker (CMD_DRAW_PARAM_TRI). Shares the
     * param-span plane/z header, so it is only meaningful with the
     * span-list command present. The z-tested alias flavor additionally
     * requires OF_EMIT_CAP_PARAM_SPAN_ZTEST at the call site. */
    if ((hw & (OF_HW_GPU_PARAM_TRI | OF_HW_GPU_PARAM_SPAN_LIST)) ==
        (OF_HW_GPU_PARAM_TRI | OF_HW_GPU_PARAM_SPAN_LIST))
        of_emit_caps |= OF_EMIT_CAP_TRIANGLES;
    /* Hardware plane derivation (CMD_SET_TRI_STATE/DRAW_VERT_TRI): the
     * GPU forms the s*zi/t*zi/zi/light planes from raw per-vertex values,
     * so the alias lowering drops the per-triangle 2x2 solve + Q29
     * escalation entirely. Bitstreams that ship it also ship PARAM_TRI
     * (the 0x49 fallback), so gate the path on this flag and fall back
     * otherwise. */
    g_vert_tri = (hw & OF_HW_GPU_VERT_TRI) != 0;
    of_emit_span_cont = (hw & OF_HW_GPU_SPAN_CONT) != 0;
    /* 0x48 compact-direct lane form; cores without it drain the payload as
     * a no-op, so the affine group is dropped rather than emitted. */
    span_group_ok = (hw & OF_HW_GPU_SPAN_GROUP) != 0;
    span_safe_mode = (int)pq_gpu_safe_spans.value;
    of_emit_caps |= OF_EMIT_CAP_SPAN_BATCH;

    /* of_gpu_init resolves the MMIO base from caps before any GPU_CTRL
     * access.  It also ring-resets and enables the core. */
    of_gpu_init();
    of_emit_ready = 1;

    /* Bring up the portable texture manager.  It reads the fast-tier size from
     * caps itself; nothing here names a memory.  A tiny dynamic texture serves
     * as the dynamic-domain bind representative (its address is never sampled —
     * binding it just routes the fetch domain back for the raw-pointer draws:
     * sky, turbulent, alias, sprites, HUD). */
    of_texture_init();
    {
        static unsigned char of_tex_dynamic_dummy[16] __attribute__((aligned(16)));
        of_texture_create_dynamic(&of_tex_dynamic, of_tex_dynamic_dummy, 1, 1);
    }
    of_tex_world_valid = 0;

    /* Generic triangle depth is still not exposed here.  Param spans can
     * write and/or test d_pzbuffer when the runtime caps advertise it. */
}

void of_emit_upload_colormap(const unsigned char *cm, uint32_t size)
{
    /* Populate slot 0 of the GPU's 16KB-aligned SDRAM palookup storage
     * (_gpu_palookup_storage; GPU_PALOOKUP_BASE was set there by of_gpu_init).
     * of_texture_set_colormap (per level) then co-locates a copy into the fast
     * tier and lets of_texture_bind move GPU_PALOOKUP_BASE with the fetch domain. */
    of_gpu_palookup_upload(0, cm, size);
}

void of_emit_bind_fb(uint32_t fb_addr, int fb_stride,
                     uint32_t zb_addr, int zb_stride_bytes)
{
    (void)zb_addr; (void)zb_stride_bytes;  /* no generic z-buffer bind */
    flush_span_batch();
    of_gpu_set_framebuffer(fb_addr, (uint16_t)fb_stride);
}

/* of_emit_finish with the encode / publish / GPU-wait split timed. */
static void vid_timed_finish(void) __attribute__((noinline, cold));
static void vid_timed_finish(void)
{
    uint32_t t0, t1, t2, token;

    t0 = of_time_us();
    flush_span_batch();
    t1 = of_time_us();
    token = of_gpu_submit();
    t2 = of_time_us();
    of_gpu_wait(token);
    vid_perf.us_encode += t1 - t0;
    vid_perf.us_publish += t2 - t1;
    vid_perf.us_wait += of_time_us() - t2;
}

void of_emit_finish(void)
{
    vid_perf.finishes++;
    if (vid_perf_timing) {
        vid_timed_finish();
    } else {
        flush_span_batch();
        /* of_gpu_finish() is where the CPU waits until every submitted
         * raster command has retired. */
        of_gpu_finish();
    }
    of_emit_cpu_sync_needed = 0;
}

void of_emit_prepare_framebuffer_for_cpu(void)
{
    if (!of_emit_ready || !of_emit_cpu_sync_needed)
        return;
    vid_perf.finishes_prep++;
    of_emit_finish();
}

void of_emit_perf_take(of_emit_perf_t *out)
{
    vid_perf.tex_flushes = of_emit_tex_flush_epoch - vid_perf_epoch;
    vid_perf_epoch = of_emit_tex_flush_epoch;
    vid_perf.ring_waits = _gpu_dbg_ring_waits;
    vid_perf.ring_spins = _gpu_dbg_ring_spin_iters;
    vid_perf.ring_min_free = _gpu_dbg_min_ring_free;
    vid_perf.dma_waits = _gpu_dbg_dma_waits;
    vid_perf.dma_spins = _gpu_dbg_dma_spin_iters;
    *out = vid_perf;

    memset(&vid_perf, 0, sizeof(vid_perf));
    _gpu_dbg_ring_waits = 0;
    _gpu_dbg_ring_spin_iters = 0;
    _gpu_dbg_min_ring_free = OF_GPU_RING_SIZE;
    _gpu_dbg_dma_waits = 0;
    _gpu_dbg_dma_spin_iters = 0;
}

/* Publish the CPU-side write pointer to the GPU without waiting.
 *
 * In scalar mode, kicks flush queued spans immediately so the GPU can
 * overlap with the CPU.  In command-DMA batch mode, tiny per-surface
 * batches are counterproductive: each batch pays a cache flush + DMA
 * doorbell setup.  Defer those soft kicks until the span batch fills
 * or an ordering boundary forces flush_span_batch() (bind/clear/tri/
 * finish/flip). */
void of_emit_kick(void)
{
    if (span_pending > 0 &&
        of_emit_supports(OF_EMIT_CAP_SPAN_BATCH) &&
        span_pending < PQ_GPU_SPAN_BATCH_MAX)
        return;

    if (!flush_span_batch())
        of_gpu_kick();
}

void of_emit_publish(void)
{
    flush_span_batch();
    of_gpu_kick_now();
}

void of_emit_texture_cache_flush(void)
{
    if (of_emit_ready) {
        GPU_TEX_FLUSH = 1;
        of_emit_tex_flush_epoch++;
    }
}

void of_emit_cache_clean(const void *addr, uint32_t size)
{
    uintptr_t start, end, a;
    volatile const uint32_t *p;
    uint32_t sink = 0;

    if (!addr || !size) return;

    start = (uintptr_t)addr & ~(uintptr_t)(OF_GPU_CACHE_LINE_BYTES - 1u);
    end = (uintptr_t)addr + size;

    /* GPU texture/source buffers are consumed by an external AXI master.
     * Clean-only has been observed to leave stale L1 lines visible to
     * hardware readers; use cbo.flush and then issue cached reads on the
     * same d_axi stream so the writebacks retire before the GPU DMA wins
     * arbitration. */
    __asm__ volatile("fence" ::: "memory");
    for (a = start; a < end; a += OF_GPU_CACHE_LINE_BYTES)
        _gpu_cbo_flush_line((void *)a);
    __asm__ volatile("fence" ::: "memory");

    for (a = start; a < end; a += OF_GPU_CACHE_LINE_BYTES) {
        p = (volatile const uint32_t *)a;
        sink ^= *p;
    }
    (void)sink;

    /* This publishes CPU-written bytes to SDRAM.  GPU texture-cache
     * invalidation is intentionally explicit at ordered frame/scratch
     * reuse boundaries; writing GPU_TEX_FLUSH while texture fetches are
     * in flight can stall the renderer. */
}

static int of_emit_clean_ranges_overlap(uintptr_t a_addr, uint32_t a_size,
                                        uintptr_t b_addr, uint32_t b_size)
{
    uintptr_t a_end, b_end;

    if (!a_addr || !a_size || !b_addr || !b_size)
        return 0;

    a_end = a_addr + (uintptr_t)a_size;
    b_end = b_addr + (uintptr_t)b_size;
    return a_addr < b_end && b_addr < a_end;
}

static uint32_t of_emit_clean_once_hash_key(uintptr_t addr, uint32_t size)
{
    uint32_t x = (uint32_t)(addr >> 6) ^ (uint32_t)(addr >> 17) ^
                 (size * 2654435761u);

    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    return x & OF_EMIT_CLEAN_ONCE_HASH_MASK;
}

static void of_emit_clean_once_unindex(uint32_t slot)
{
    of_emit_clean_once_t *entry;
    uint32_t h;

    if (slot >= OF_EMIT_CLEAN_ONCE_SLOTS)
        return;

    entry = &of_emit_clean_once_slots[slot];
    if (!entry->addr || !entry->size)
        return;

    h = of_emit_clean_once_hash_key(entry->addr, entry->size);
    if (of_emit_clean_once_hash[h] == slot + 1u)
        of_emit_clean_once_hash[h] = 0;
}

void of_emit_cache_clean_forget(const void *addr, uint32_t size)
{
    uintptr_t a = (uintptr_t)addr;

    if (!addr || !size)
        return;

    for (uint32_t i = 0; i < OF_EMIT_CLEAN_ONCE_SLOTS; i++) {
        if (of_emit_clean_ranges_overlap(a, size,
                of_emit_clean_once_slots[i].addr,
                of_emit_clean_once_slots[i].size)) {
            of_emit_clean_once_unindex(i);
            of_emit_clean_once_slots[i].addr = 0;
            of_emit_clean_once_slots[i].size = 0;
        }
    }
}

void of_emit_cache_clean_once(const void *addr, uint32_t size)
{
    uintptr_t a = (uintptr_t)addr;
    uint32_t h, slot;

    if (!addr || !size)
        return;

    h = of_emit_clean_once_hash_key(a, size);
    slot = of_emit_clean_once_hash[h];
    if (slot != 0) {
        of_emit_clean_once_t *entry =
            &of_emit_clean_once_slots[slot - 1u];
        if (entry->addr == a && entry->size == size)
            return;
    }

    /* Preserve the original registry semantics on rare hash collisions. */
    for (uint32_t i = 0; i < OF_EMIT_CLEAN_ONCE_SLOTS; i++) {
        if (of_emit_clean_once_slots[i].addr == a &&
            of_emit_clean_once_slots[i].size == size) {
            of_emit_clean_once_hash[h] = (uint16_t)(i + 1u);
            return;
        }
    }

    of_emit_cache_clean(addr, size);

    of_emit_clean_once_unindex(of_emit_clean_once_next);
    of_emit_clean_once_slots[of_emit_clean_once_next].addr = a;
    of_emit_clean_once_slots[of_emit_clean_once_next].size = size;
    of_emit_clean_once_hash[h] = (uint16_t)(of_emit_clean_once_next + 1u);
    of_emit_clean_once_next =
        (of_emit_clean_once_next + 1u) % OF_EMIT_CLEAN_ONCE_SLOTS;
}

/* ====================================================================
 * Texture store — thin wrappers over the portable texture manager
 *
 * The engine never includes of_texture.h (it would duplicate the manager's and
 * of_gpu.h's static state); it calls these instead.  Placement, the fetch-
 * domain switch and the colormap move are all decided here.  Returned values
 * are opaque GPU addresses (what a span/tri record's tex_addr wants); for a
 * given mip, add the intra-texture byte offset.
 * ==================================================================== */

/* Reset the manager on level change (clears the static placement allocator).
 * The colormap lives in the GPU's SDRAM palookup storage, independent of the
 * manager, so it persists across the reset. */
void of_emit_tex_reset(void)
{
    of_texture_reset();
    of_tex_world_valid = 0;
    of_fasttex_spilled = 0;
    /* Fast textures ride the raw-miptex span path (not triangles), so they only
     * need r_fasttex + a fast tier. */
    of_fasttex_active = (r_fasttex.value != 0.0f) && of_texture_has_fast_mem();
    /* Co-locate the colormap exactly like the Doom port (r_gpu.c): the colormap
     * fetch follows the texture route, so with the world in the fast tier the
     * colormap must be there too.  of_texture_set_colormap, called as the FIRST
     * fast allocation (before any world texture, so its fast copy lands at 16KB-
     * aligned offset 0), uploads a copy of the SDRAM palookup (slot 0, already
     * filled with host_colormap) into the fast tier AND records both the fast and
     * SDRAM bases, so of_texture_bind moves GPU_PALOOKUP_BASE atomically with the
     * fetch domain.  Pass the 16KB-aligned _gpu_palookup_storage, never the
     * 16-byte-aligned host_colormap (the HW 16KB-aligns the slot). */
    if (of_fasttex_active)
        of_texture_set_colormap(_gpu_palookup_storage, 64 * 256);
}

/* 1 when world textures are being served from the fast tier this map (engine
 * gates the per-surface tex_addr and the fetch-domain binds on this). */
int of_emit_fasttex_active(void)
{
    return of_fasttex_active;
}

/* Create a STATIC texture (world miptex, alias skin, …).  Returns its GPU base
 * address; the first one created becomes the static-domain bind representative. */
/* 1 if the target advertises a dedicated fast texture tier (diagnostics). */
int of_emit_has_fast_mem(void)
{
    return of_emit_ready ? of_texture_has_fast_mem() : 0;
}

/* Fast-tier accounting for the spill diagnostic. */
uint32_t of_emit_fasttex_size(void)
{
    const struct of_capabilities *c = of_get_caps();
    return (c && c->version >= 3) ? c->tex_fast_size : 0u;
}
uint32_t of_emit_fasttex_budget_free(void)
{
    return of_emit_ready ? of_texture_budget_free() : 0u;
}
int of_emit_fasttex_spill_count(void)
{
    return of_fasttex_spilled;
}

uint32_t of_emit_tex_create(const void *pixels, uint16_t w, uint16_t h, uint32_t nbytes)
{
    of_texture_t t;
    if (of_fasttex_active)
        of_texture_create(&t, pixels, w, h, nbytes);          /* fast tier */
    else
        of_texture_create_dynamic(&t, (void *)(uintptr_t)pixels, w, h);  /* main memory in place */
    if (of_fasttex_active && !of_texture_in_fast_mem(&t))
        of_fasttex_spilled++;     /* didn't fit the tier → SDRAM addr, but route is CRAM1 */
    if (!of_tex_world_valid) {
        of_tex_world = t;
        of_tex_world_valid = 1;
    }
    return of_texture_gpu_addr(&t);
}

/* Register the animated sky as a DYNAMIC (in-place) texture; updated each
 * rebuild.  Returns its GPU base address (for the per-span sky tex_addr). */
static const void *of_sky_src;
uint32_t of_emit_sky_create(void *pixels, uint16_t w, uint16_t h)
{
    of_texture_create_dynamic(&of_tex_sky, pixels, w, h);
    of_sky_src = pixels;
    return of_texture_gpu_addr(&of_tex_sky);
}

/* of_emit_cache_clean over the cache lines holding bytes
 * [addr + r * row_stride, addr + r * row_stride + row_bytes) for r < rows:
 * every line flushed, then every line read back, each line once. */
static void of_emit_cache_clean_rows(const void *addr, uint32_t row_stride,
                                     uint32_t row_bytes, uint32_t rows)
    __attribute__((noinline, cold));
static void of_emit_cache_clean_rows(const void *addr, uint32_t row_stride,
                                     uint32_t row_bytes, uint32_t rows)
{
    const uintptr_t line_mask = OF_GPU_CACHE_LINE_BYTES - 1u;
    uintptr_t row, a, end, next;
    volatile const uint32_t *p;
    uint32_t sink = 0;

    __asm__ volatile("fence" ::: "memory");
    next = 0;
    for (row = (uintptr_t)addr; row < (uintptr_t)addr + rows * row_stride;
         row += row_stride) {
        end = row + row_bytes;
        for (a = row & ~line_mask; a < end; a += OF_GPU_CACHE_LINE_BYTES) {
            if (a >= next)
                _gpu_cbo_flush_line((void *)a);
        }
        next = a;
    }
    __asm__ volatile("fence" ::: "memory");

    next = 0;
    for (row = (uintptr_t)addr; row < (uintptr_t)addr + rows * row_stride;
         row += row_stride) {
        end = row + row_bytes;
        for (a = row & ~line_mask; a < end; a += OF_GPU_CACHE_LINE_BYTES) {
            if (a >= next) {
                p = (volatile const uint32_t *)a;
                sink ^= *p;
            }
        }
        next = a;
    }
    (void)sink;
}

/* Publish the freshly-rebuilt sky bytes to the GPU.  Use the robust clean
 * (cbo.flush + read-back), NOT of_texture_update()'s plain cache_clean_range —
 * the plain clean leaves stale L1 lines visible to the GPU's AXI reader, which
 * shows up as speckle in the sky each frame.  Only the first row_bytes of
 * each row_stride-byte row are cleaned: the span masks confine GPU sampling
 * to them. */
void of_emit_sky_update(uint32_t row_stride, uint32_t row_bytes, uint32_t rows)
{
    if (of_sky_src)
        of_emit_cache_clean_rows(of_sky_src, row_stride, row_bytes, rows);
}

/* GPU base address of an in-place source the GPU samples directly (turbulent
 * scratch rows, etc.) — memory-agnostic, matches of_texture's addressing. */
uint32_t of_emit_tex_source_addr(const void *src)
{
    of_texture_t t;
    of_texture_create_dynamic(&t, (void *)(uintptr_t)src, 1, 1);
    return of_texture_gpu_addr(&t);
}

/* Establish the GPU fetch domain for the next batch.  of_emit_bind_static()
 * routes to the static textures' tier (fast where present); of_emit_bind_dynamic()
 * routes to the dynamic tier for the dynamic + raw-pointer draws.  Each flips (draining
 * first) only when the domain actually changes; callers flush pending batched
 * geometry before switching. */
void of_emit_bind_static(void)
{
    /* No fast tier / r_fasttex 0 => one memory, no domain to flip. */
    if (!of_emit_ready || !of_fasttex_active || !of_tex_world_valid)
        return;
    vid_perf.finishes_bind++;
    of_emit_finish();                 /* push queued spans/tris to the ring + drain */
    /* of_texture_set_colormap recorded both colormap copies, so of_texture_bind
     * flips the texture fetch domain AND moves GPU_PALOOKUP_BASE to the matching
     * colormap (fast copy here) atomically — no manual poke (Doom r_gpu.c pattern). */
    of_texture_bind(&of_tex_world);
}
void of_emit_bind_dynamic(void)
{
    if (!of_emit_ready || !of_fasttex_active)
        return;
    vid_perf.finishes_bind++;
    of_emit_finish();
    of_texture_bind(&of_tex_dynamic);   /* restores the SDRAM colormap with the domain */
}

void of_emit_clear(uint32_t flags, uint16_t color, uint16_t depth)
{
    flush_span_batch();
    if (flags & OF_EMIT_CLEAR_COLOR) {
        of_emit_cpu_sync_needed = 1;
        of_gpu_clear_rect_strided((uint32_t)(uintptr_t)vid.buffer,
                                  (uint16_t)vid.width,
                                  (uint16_t)vid.height,
                                  (uint16_t)vid.rowbytes,
                                  (uint8_t)color);
    }
    if ((flags & OF_EMIT_CLEAR_DEPTH) && d_pzbuffer && d_zwidth > 0) {
        if (zbuffer_cpu_cached) {
            uint32_t count = d_zwidth * (uint32_t)vid.height;
            uint32_t pattern = (uint32_t)depth | ((uint32_t)depth << 16);
            uint32_t *dst = (uint32_t *)d_pzbuffer;
            uint32_t pairs = count >> 1;

            while (pairs--)
                *dst++ = pattern;
            if (count & 1)
                *(uint16_t *)dst = depth;
            return;
        }
        int z_stride = d_zwidth * (int)sizeof(short);
        of_gpu_clear_rect_strided((uint32_t)(uintptr_t)d_pzbuffer,
                                  (uint16_t)z_stride,
                                  (uint16_t)vid.height,
                                  (uint16_t)z_stride,
                                  (uint8_t)depth);
    }
}

void of_emit_depth_test(of_emit_depth_func_t func)
{
    (void)func;  /* no generic depth test command */
}

void of_emit_span(const of_emit_span_t *sp)
{
    int safe;

    if (!of_emit_supports(OF_EMIT_CAP_SPAN))
        return;
    gpu_validate_span_dest(sp);
    of_emit_cpu_sync_needed = 1;

    /* pq_gpu_safe_spans: every span in its own flushed batch. */
    safe = span_safe_mode;
    if (__builtin_expect(safe, 0))
        flush_span_batch();

    if (__builtin_expect(sp->count != 0, 1)) {
        if (__builtin_expect((sp->flags & OF_EMIT_PERSP) != 0, 0))
            gpu_stream_persp_span(sp);
        else
            gpu_stream_affine_span(sp);
    }

    if (__builtin_expect(++span_pending >= PQ_GPU_SPAN_BATCH_MAX, 0) || safe)
        flush_span_batch();
}

_Static_assert(sizeof(of_gpu_param_span_record_t) == 6u,
               "param span record must stay six wire bytes");
_Static_assert(OF_GPU_PARAM_SPAN_LIST_WORDS(1) == 34u &&
               OF_GPU_PARAM_SPAN_LIST_WORDS(2) == 34u,
               "long-form param span-list size mismatch");
_Static_assert(OF_GPU_PARAM_SPAN_MAX_RECORDS == 512u,
               "param span-list chunk size mismatch");
_Static_assert(MAXWIDTH <= 0xFFF,
               "span counts must fit the 12-bit record count");

/* Long-form GPU_CMD_DRAW_PARAM_SPAN_LIST, written straight into the ring.
 * Emits the same words as of_gpu.h _gpu_emit_param_span_list on cores
 * without OF_HW_GPU_SPAN_CONT, where its 0x58 header cache is never read.
 * The caller has validated z_mode and the Q29 shift capability; counts_or
 * is the OR of all record counts (any value with the same zero and 12-bit
 * overflow status gives the same words). */
static void gpu_emit_param_chunk(const of_gpu_param_span_list_t *p,
                                 const of_gpu_param_span_record_t *records,
                                 uint32_t n, uint32_t counts_or)
{
    uint32_t control, shift = 0;
    uint32_t *w;

    if (p->attr_mode == OF_GPU_PARAM_ATTR_PERSP_Q29)
        shift = (uint32_t)p->q29_attr_shift & 31u;
    control = ((uint32_t)p->flags & 0xFFu)
            | (((uint32_t)p->colormap_id & 0x0Fu) << 8)
            | (((uint32_t)p->attr_mode & 0x0Fu) << 12)
            | (((uint32_t)p->span_axis & 0x0Fu) << 16)
            | ((uint32_t)OF_GPU_PARAM_RECORD_U16V16_COUNT16 << 20)
            | (((uint32_t)p->z_mode & 0x0Fu) << 24);

    _gpu_span_hdr_valid = 0;
    _gpu_cmd_header(GPU_CMD_DRAW_PARAM_SPAN_LIST,
                    OF_GPU_PARAM_SPAN_LIST_WORDS(n));
    _gpu_emit_param_span_header_words(p, control, n, shift);

    w = _gpu_ring_claim();
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    if ((counts_or & ~0xFFFu) == 0u && n >= 16u &&
        ((uintptr_t)records & 3u) == 0u) {
        uint32_t bytes = n * 6u;
        __builtin_memcpy(w, records, bytes);
        if (n & 1u)
            __builtin_memset((uint8_t *)w + bytes, 0, 6u);
    } else
#endif
    {
        uint32_t pairs = n >> 1;
        for (uint32_t i = 0; i < pairs; i++) {
            const of_gpu_param_span_record_t *a = &records[2u * i];
            const of_gpu_param_span_record_t *b = a + 1;
            *w++ = ((uint32_t)a->v << 16) | (uint32_t)a->u;
            *w++ = ((uint32_t)b->u << 16) | _gpu_count12((uint32_t)a->count);
            *w++ = (_gpu_count12((uint32_t)b->count) << 16) | (uint32_t)b->v;
        }
        if (n & 1u) {
            const of_gpu_param_span_record_t *a = &records[n - 1u];
            *w++ = ((uint32_t)a->v << 16) | (uint32_t)a->u;
            *w++ = _gpu_count12((uint32_t)a->count);
            *w++ = 0u;
        }
    }
    _gpu_ring_commit(3u * ((n + 1u) >> 1));
}

/* SDK emitter for cores that advertise the 0x58 continuation. */
static void gpu_emit_param_chunk_sdk(const of_gpu_param_span_list_t *p,
                                     const of_gpu_param_span_record_t *records,
                                     uint32_t n)
    __attribute__((noinline, cold));
static void gpu_emit_param_chunk_sdk(const of_gpu_param_span_list_t *p,
                                     const of_gpu_param_span_record_t *records,
                                     uint32_t n)
{
    of_gpu_draw_param_span_list(p, records, n);
}

/* known_counts != 0: the caller guarantees every count is in 1..0xFFF. */
static int gpu_param_span_list(const of_emit_param_span_list_t *params,
                               const of_emit_param_span_record_t *records,
                               uint32_t record_count, uint32_t known_counts)
{
    const of_gpu_param_span_list_t *p;
    uint32_t submitted = 0, max_chunk;
    int attr_is_persp;

    if (!params || !records || record_count == 0)
        return 0;
    if (!of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_LIST))
        return 0;
    if (params->attr_mode == OF_EMIT_PARAM_ATTR_PERSP_Q29 &&
        params->q29_attr_shift != 0 &&
        !of_emit_supports(OF_EMIT_CAP_Q29_SCALE))
        return 0;
    attr_is_persp = params->attr_mode == OF_EMIT_PARAM_ATTR_PERSP ||
                    params->attr_mode == OF_EMIT_PARAM_ATTR_PERSP_Q29;
    switch (params->z_mode) {
    case OF_EMIT_PARAM_Z_NONE:
        break;
    case OF_EMIT_PARAM_Z_WRITE_ZI:
        if (!of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_Z))
            return 0;
        if (!attr_is_persp || (params->flags & OF_EMIT_SKIP_ZERO))
            return 0;
        break;
    case OF_EMIT_PARAM_Z_TEST_ZI:
    case OF_EMIT_PARAM_Z_TEST_WRITE:
        if (!of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_ZTEST))
            return 0;
        if (!attr_is_persp)
            return 0;
        break;
    default:
        return 0;
    }

    gpu_validate_param_dest(params, records, record_count);
    if (known_counts)
        gpu_validate_known_counts(records, record_count);
    if (span_pending > 0)
        flush_span_batch();

    /* of_emit_param_span_list_t mirrors of_gpu_param_span_list_t
     * field-for-field (the records cast below relies on the same
     * contract), so hand the caller's struct straight down instead
     * of staging a ~100-byte copy per surface. */
    p = (const of_gpu_param_span_list_t *)params;
    max_chunk = (int)pq_gpu_safe_spans.value ? 1u : OF_GPU_PARAM_SPAN_MAX_RECORDS;
    of_emit_cpu_sync_needed = 1;

    do {
        const of_gpu_param_span_record_t *r =
            (const of_gpu_param_span_record_t *)records;
        uint32_t chunk = record_count < max_chunk ? record_count : max_chunk;

        if (of_emit_span_cont) {
            gpu_emit_param_chunk_sdk(p, r, chunk);
        } else {
            uint32_t counts_or = known_counts;

            if (!counts_or) {
                for (uint32_t i = 0; i < chunk; i++)
                    counts_or |= r[i].count;
            }
            if (counts_or)
                gpu_emit_param_chunk(p, r, chunk, counts_or);
        }

        submitted += chunk;
        records += chunk;
        record_count -= chunk;
    } while (record_count != 0);

    return (int)submitted;
}

int of_emit_param_span_list(const of_emit_param_span_list_t *params,
                            const of_emit_param_span_record_t *records,
                            uint32_t record_count)
{
    return gpu_param_span_list(params, records, record_count, 0u);
}

int of_emit_param_span_list_known(const of_emit_param_span_list_t *params,
                                  const of_emit_param_span_record_t *records,
                                  uint32_t record_count)
{
    return gpu_param_span_list(params, records, record_count, 0xFFFu);
}

void of_emit_clear_rect(int dst_x, int dst_y, int w, int h, unsigned char color)
{
    if (w <= 0 || h <= 0) return;
    const uint32_t fb_addr = (uint32_t)(uintptr_t)
        (vid.buffer + (uint32_t)(dst_y * vid.rowbytes + dst_x));
    gpu_validate_rect_dest(fb_addr, vid.rowbytes, w, h);
    flush_span_batch();
    of_emit_cpu_sync_needed = 1;
    of_gpu_clear_rect_strided(fb_addr, (uint16_t)w, (uint16_t)h,
                              (uint16_t)vid.rowbytes, color);
}

void of_emit_clear_rect_addr(uint32_t fb_addr, int fb_stride,
                             int w, int h, unsigned char color)
{
    if (w <= 0 || h <= 0) return;
    gpu_validate_rect_dest(fb_addr, fb_stride, w, h);
    flush_span_batch();
    of_emit_cpu_sync_needed = 1;
    of_gpu_clear_rect_strided(fb_addr, (uint16_t)w, (uint16_t)h,
                              (uint16_t)fb_stride, color);
}

#define BLIT_LIST_MIN_ROWS 5
static of_emit_param_span_record_t blit_records[ZBUFFER_MAX_HEIGHT]
    __attribute__((aligned(4)));

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

    /* The fragment processor writes the raw I8 texel when COLORMAP isn't
     * set, and SKIP_ZERO (which actually discards 0xFF, matching Quake's
     * TRANSPARENT_COLOR) gives key-color transparency.
     *
     * From BLIT_LIST_MIN_ROWS rows up, one affine param-span list costs
     * fewer command words than a lane per row: record v = row, count =
     * blit_w, with s = u and t = v in 16.16 over the source rectangle, so
     * every pixel samples src + (src_y + v) * src_pitch + src_x + u, the
     * same texel as the per-row lane. */
    if (blit_h >= BLIT_LIST_MIN_ROWS && blit_h <= ZBUFFER_MAX_HEIGHT &&
        blit_w <= 0xFFF && src_pitch > 0 && src_pitch <= 0xFFFF &&
        !(int)pq_gpu_safe_spans.value &&
        of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_LIST)) {
        of_emit_param_span_list_t p;

        memset(&p, 0, sizeof(p));
        p.fb_base = fb_base + (uint32_t)(dst_y * fb_row + dst_x);
        p.fb_major_step = fb_row;
        p.fb_minor_step = 1;
        p.tex_addr = (uint32_t)(uintptr_t)(src + src_y * src_pitch + src_x);
        p.tex_width = (uint16_t)src_pitch;
        p.flags = flags;
        p.attr_mode = OF_EMIT_PARAM_ATTR_AFFINE;
        p.span_axis = OF_EMIT_PARAM_AXIS_X;
        p.z_mode = OF_EMIT_PARAM_Z_NONE;
        p.attr_du[0] = 0x10000;
        p.attr_dv[1] = 0x10000;
        for (int row = 0; row < blit_h; row++) {
            blit_records[row].u = 0;
            blit_records[row].v = (uint16_t)row;
            blit_records[row].count = (uint16_t)blit_w;
        }
        if (gpu_param_span_list(&p, blit_records, (uint32_t)blit_h, 0xFFFu))
            return;
    }

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

/* ---- Hardware triangles (CMD_DRAW_PARAM_TRI) ----------------------- */

/* Set around R_DrawViewModel (r_main.c): the viewmodel renders last and
 * neither reads nor writes depth. */
extern int pq_noz_mode;

/* Texture state captured for the triangle lowering: param headers carry
 * the skin address/size explicitly (the GPU's sticky texture bind only
 * serves the legacy scalar span path). */
static of_emit_texture_t of_emit_tri_tex;

/*
 * Lower one alias triangle to CMD_DRAW_PARAM_TRI.
 *
 * Attribute planes are solved in screen space through the three vertex
 * values (one reciprocal of the signed area, reused for every plane):
 *
 *   world aliases  — attr_mode PERSP, z_mode Z_TEST_WRITE:
 *     attr0 = s_tex * zi * 2^16, attr1 = t_tex * zi * 2^16, attr2 = zi
 *     in Q16.16 — the plain-PERSP RTL contract is "s/z, t/z, 1/z all in
 *     16.16" (only the Q29 mode uses the 2^13 numerator convention).
 *     The z window (attr2[16:1]) then stores zi*2^15 = v[5]>>16,
 *     byte-identical to the CPU writer in D_PolysetDrawFinalVerts,
 *     GEQUAL test.
 *   viewmodel      — same planes, z_mode NONE (pq_noz_mode).
 *
 * Everything is rebased to the triangle's bbox corner: fb/z bases are
 * offset, vertices and the clip rect shifted. This keeps the plane
 * origins evaluated inside the triangle — the light plane is decoded
 * from 24 bits (signed Q6.16) and would truncate if anchored at screen
 * (0,0) with a steep gradient.
 */
static void of_emit_tri_one(of_gpu_param_span_list_t *p,
                            const of_emit_vertex_t *v0,
                            const of_emit_vertex_t *v1,
                            const of_emit_vertex_t *v2,
                            int use_z,
                            int16_t cx0, int16_t cx1,
                            int16_t cy0, int16_t cy1)
{
    of_gpu_tri_vert_t tv[3];
    float x0, y0, x1f, y1f, x2f, y2f, det, inv;
    float a0[4], a1[4], a2[4];      /* s*zi, t*zi, zi, light */
    float org[4], du[4], dv[4];
    int bx, by, i;

    /* Q12.4 x, Q12.4 y (low bits zero — projected verts are integer). */
    x0  = (float)v0->x * (1.0f / 16.0f);
    y0  = (float)v0->y * (1.0f / 16.0f);
    x1f = (float)v1->x * (1.0f / 16.0f);
    y1f = (float)v1->y * (1.0f / 16.0f);
    x2f = (float)v2->x * (1.0f / 16.0f);
    y2f = (float)v2->y * (1.0f / 16.0f);

    det = (x1f - x0) * (y2f - y0) - (x2f - x0) * (y1f - y0);
    if (det == 0.0f)
        return;                     /* degenerate sliver */
    inv = 1.0f / det;

    /* bbox corner anchor (any in-triangle-adjacent point works) —
     * integer min on the raw Q12.4 coords, no float round trip */
    bx = v0->x;
    if (v1->x < bx) bx = v1->x;
    if (v2->x < bx) bx = v2->x;
    bx >>= 4;
    by = v0->y;
    if (v1->y < by) by = v1->y;
    if (v2->y < by) by = v2->y;
    by >>= 4;

    /* Vertex attribute values.  v->s/t are 16.16 texels, v->w is zi in
     * Q16.16 (v[5] >> 15, ziscale 2^31 source).  Plain ATTR_PERSP is
     * NOT scale-free: the RTL expects sZ/tZ as "s/z in 16.16" and zinv
     * as "1/z in 16.16" (gpu_core.v sp_sZ/sp_zinv; the sprite path
     * ships the same 2^16 on all three).  s*zi*2^16 =
     * s_16.16 * w_16.16 * 2^-16. */
    a0[0] = (float)v0->s * (float)v0->w * (1.0f / 65536.0f);
    a1[0] = (float)v1->s * (float)v1->w * (1.0f / 65536.0f);
    a2[0] = (float)v2->s * (float)v2->w * (1.0f / 65536.0f);
    a0[1] = (float)v0->t * (float)v0->w * (1.0f / 65536.0f);
    a1[1] = (float)v1->t * (float)v1->w * (1.0f / 65536.0f);
    a2[1] = (float)v2->t * (float)v2->w * (1.0f / 65536.0f);
    a0[2] = (float)v0->w;
    a1[2] = (float)v1->w;
    a2[2] = (float)v2->w;
    a0[3] = (float)(v0->r << 16);   /* colormap row, Q6.16 */
    a1[3] = (float)(v1->r << 16);
    a2[3] = (float)(v2->r << 16);

    {
        const float dx10 = x1f - x0, dy10 = y1f - y0;
        const float dx20 = x2f - x0, dy20 = y2f - y0;
        const float rx0 = x0 - (float)bx, ry0 = y0 - (float)by;

        for (i = 0; i < 4; i++) {
            float d10 = a1[i] - a0[i];
            float d20 = a2[i] - a0[i];

            du[i]  = (d10 * dy20 - d20 * dy10) * inv;
            dv[i]  = (d20 * dx10 - d10 * dx20) * inv;
            org[i] = a0[i] - rx0 * du[i] - ry0 * dv[i];
        }
    }

    /* int32 plane headroom. Sliver triangles (det of a few px² with a
     * large s·zi delta — the close, 3x-zi viewmodel is the worst case)
     * overflow the plain Q16.16-scale planes. NEVER drop them: a
     * dropped triangle reads as a crack between its neighbours.
     * Rescale into PERSP_Q29 with a per-triangle dynamic shift instead
     * (the world path's trick; z restores to the same zi*2^15 the
     * plain mode stores, so depth stays consistent). */
    {
        float fmax = 0.0f, c;

        for (i = 0; i < 3; i++) {
            c = fabsf(org[i]); if (c > fmax) fmax = c;
            c = fabsf(du[i]);  if (c > fmax) fmax = c;
            c = fabsf(dv[i]);  if (c > fmax) fmax = c;
        }

        if (fmax < 1073741824.0f) {
            p->attr_mode      = OF_GPU_PARAM_ATTR_PERSP;
            p->q29_attr_shift = 0;
        } else {
            /* value units = Q16.16-scale floats / 2^16; pick sh so the
             * scaled attrs stay < 2^30 (one headroom bit), exactly like
             * the world emission. */
            union { float f; uint32_t u; } mb, sb;
            float scale;
            int sh;

            if (!of_emit_supports(OF_EMIT_CAP_Q29_SCALE))
                return;             /* no dynamic scale: drop (unreachable
                                     * on bitstreams that ship PARAM_TRI) */

            mb.f = fmax * (1.0f / 65536.0f);
            sh = (int)((mb.u >> 23) & 0xFFu) - 126 - 1;
            if (sh < 0)
                sh = 0;
            else if (sh > 31)
                sh = 31;
            sb.u = (uint32_t)(127 + 29 - sh) << 23;     /* 2^(29-sh) */
            scale = sb.f * (1.0f / 65536.0f);

            for (i = 0; i < 3; i++) {
                org[i] *= scale;
                du[i]  *= scale;
                dv[i]  *= scale;
            }
            p->attr_mode      = OF_GPU_PARAM_ATTR_PERSP_Q29;
            p->q29_attr_shift = (uint8_t)sh;
        }
    }

    /* The light plane decodes from 24 bits (signed Q6.16) and has no
     * dynamic-scale mode — saturate sliver gradients rather than drop;
     * a wrong shade on a 1-2 px strip is invisible, a hole is not. */
    if (org[3] > 8388607.0f)  org[3] = 8388607.0f;
    if (org[3] < -8388608.0f) org[3] = -8388608.0f;
    if (du[3] > 8388607.0f)   du[3] = 8388607.0f;
    if (du[3] < -8388608.0f)  du[3] = -8388608.0f;
    if (dv[3] > 8388607.0f)   dv[3] = 8388607.0f;
    if (dv[3] < -8388608.0f)  dv[3] = -8388608.0f;

    /* Constant header fields (tex, flags, modes, clamps, z steps) are
     * prebuilt per batch by the caller; patch only what varies. */
    p->fb_base = (uint32_t)(uintptr_t)
        (d_viewbuffer + by * screenwidth + bx);
    for (i = 0; i < 3; i++) {
        p->attr_origin[i] = (int32_t)org[i];
        p->attr_du[i]     = (int32_t)du[i];
        p->attr_dv[i]     = (int32_t)dv[i];
    }
    p->light_origin = (int32_t)org[3];
    p->light_du     = (int32_t)du[3];
    p->light_dv     = (int32_t)dv[3];
    if (use_z) {
        p->z_base = (uint32_t)(uintptr_t)
            (d_pzbuffer + by * d_zwidth + bx);
    }

    tv[0].x = (int16_t)(v0->x - (bx << 4));
    tv[0].y = (int16_t)((v0->y >> 4) - by);
    tv[1].x = (int16_t)(v1->x - (bx << 4));
    tv[1].y = (int16_t)((v1->y >> 4) - by);
    tv[2].x = (int16_t)(v2->x - (bx << 4));
    tv[2].y = (int16_t)((v2->y >> 4) - by);

    of_gpu_draw_param_tri(p, tv,
                          (int16_t)(cx0 - bx), (int16_t)(cx1 - bx),
                          (int16_t)(cy0 - by), (int16_t)(cy1 - by));
}

void of_emit_triangles_batch(const of_emit_vertex_t *verts,
                             uint32_t num_vertices)
{
    of_gpu_param_span_list_t p;
    int16_t cx0, cx1, cy0, cy1;
    int use_z;
    uint32_t t;

    if (!verts || num_vertices < 3 ||
        !of_emit_supports(OF_EMIT_CAP_TRIANGLES))
        return;

    use_z = !pq_noz_mode;
    if (use_z && (!of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_ZTEST) ||
                  !d_pzbuffer || d_zwidth <= 0))
        return;                     /* caller gates on this too */

    /* Refresh window, right/bottom exclusive (the walker's fill rule
     * already excludes the bottom/right clip edges like the CPU). */
    cx0 = (int16_t)r_refdef.vrect.x;
    cy0 = (int16_t)r_refdef.vrect.y;
    cx1 = (int16_t)(r_refdef.vrect.x + r_refdef.vrect.width);
    cy1 = (int16_t)(r_refdef.vrect.y + r_refdef.vrect.height);

    flush_span_batch();
    of_emit_cpu_sync_needed = 1;

    if (g_vert_tri) {
        /* Hardware plane derivation. The GPU anchors each triangle
         * internally, so there is NO bbox rebase: fb/z bases are the
         * screen origin, the clip is the raw refresh window, and every
         * per-triangle field is batch-constant. One sticky state per
         * surface, then 14-word raw-vertex triangles — no CPU plane
         * solve, no Q29 escalation, no overflow handling.
         *
         * Sticky-state safety: nothing between this set_tri_state and
         * the draws below emits a 0x48/0x49 param command (the only
         * things that share — and would clobber — the staging the
         * sticky state lives in). flush_span_batch() ran above; the
         * per-batch re-arm also covers the of_emit_kick() d_polyse.c
         * issues between batches. */
        of_gpu_tri_state_t st;

        memset(&st, 0, sizeof(st));
        st.fb_base       = (uint32_t)(uintptr_t)d_viewbuffer;
        st.fb_major_step = screenwidth;
        st.fb_minor_step = 1;
        st.tex_addr      = of_emit_tri_tex.addr;
        st.tex_width     = of_emit_tri_tex.width;
        /* masks 0 = no wrap; the clamps below bound s,t to the skin */
        st.flags         = OF_EMIT_COLORMAP;
        st.colormap_id   = 0;
        st.z_mode        = use_z ? OF_GPU_PARAM_Z_TEST_WRITE
                                 : OF_GPU_PARAM_Z_NONE;
        st.clamp_min[0]  = 0;
        st.clamp_max[0]  = ((int32_t)of_emit_tri_tex.width  - 1) << 16;
        st.clamp_min[1]  = 0;
        st.clamp_max[1]  = ((int32_t)of_emit_tri_tex.height - 1) << 16;
        if (use_z) {
            st.z_base       = (uint32_t)(uintptr_t)d_pzbuffer;
            st.z_major_step = (int32_t)d_zwidth * (int32_t)sizeof(short);
            st.z_minor_step = (int32_t)sizeof(short);
        }
        st.clip_x0 = cx0; st.clip_x1 = cx1;
        st.clip_y0 = cy0; st.clip_y1 = cy1;
        of_gpu_set_tri_state(&st);

        for (t = 0; t + 3 <= num_vertices; t += 3) {
            const of_emit_vertex_t *a = &verts[t];
            const of_emit_vertex_t *b = &verts[t + 1];
            const of_emit_vertex_t *c = &verts[t + 2];
            /* x Q12.4 raw; y Q12.4 -> integer scanline (>>4, matching
             * the 0x49 lowering); s,t,zi raw Q16.16; light 6-bit row. */
            const int16_t vx[3]  = { a->x, b->x, c->x };
            const int16_t vy[3]  = { (int16_t)(a->y >> 4),
                                     (int16_t)(b->y >> 4),
                                     (int16_t)(c->y >> 4) };
            const int32_t vs[3]  = { a->s, b->s, c->s };
            const int32_t vt[3]  = { a->t, b->t, c->t };
            const int32_t vzi[3] = { a->w, b->w, c->w };
            const uint8_t vl[3]  = { a->r, b->r, c->r };

            of_gpu_draw_vert_tri(vx, vy, vs, vt, vzi, vl);
        }
        return;
    }

    /* Fallback (no OF_HW_GPU_VERT_TRI): CPU-solved planes via
     * CMD_DRAW_PARAM_TRI (0x49). Header template built once per batch;
     * of_emit_tri_one patches the per-triangle fields (fb/z base, attr +
     * light planes). */
    memset(&p, 0, sizeof(p));
    p.fb_major_step = screenwidth;
    p.fb_minor_step = 1;
    p.tex_addr  = of_emit_tri_tex.addr;
    p.tex_width = of_emit_tri_tex.width;
    p.flags     = OF_EMIT_COLORMAP;
    p.attr_mode = OF_GPU_PARAM_ATTR_PERSP;
    p.span_axis = OF_GPU_PARAM_AXIS_X;
    p.z_mode    = use_z ? OF_GPU_PARAM_Z_TEST_WRITE : OF_GPU_PARAM_Z_NONE;
    /* Perspective interpolation can overshoot the skin rect by a hair
     * at triangle edges; clamp s,t (post-divide 16.16 texels). */
    p.clamp_min[0] = 0;
    p.clamp_max[0] = ((int32_t)of_emit_tri_tex.width - 1) << 16;
    p.clamp_min[1] = 0;
    p.clamp_max[1] = ((int32_t)of_emit_tri_tex.height - 1) << 16;
    if (use_z) {
        p.z_major_step = (int32_t)d_zwidth * (int32_t)sizeof(short);
        p.z_minor_step = (int32_t)sizeof(short);
    }

    for (t = 0; t + 3 <= num_vertices; t += 3)
        of_emit_tri_one(&p, &verts[t], &verts[t + 1], &verts[t + 2],
                        use_z, cx0, cx1, cy0, cy1);
}

void of_emit_triangles(const of_emit_vertex_t *verts, uint32_t num_vertices)
{
    of_emit_triangles_batch(verts, num_vertices);
}

void of_emit_bind_texture(const of_emit_texture_t *tex)
{
    flush_span_batch();
    of_emit_tri_tex = *tex;
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

    vid_configure_mode();
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

    of_emit_init();
    vid_cache_framebuffers();
    vid.buffer = vid.conbuffer = vid_uncached_for_surface(of_video_surface());

    /* Use cached zbuffer only when no runtime GPU z path can read/write it.
     * If the GPU can touch d_pzbuffer, CPU access stays on the uncached alias
     * so alias/sprite/particle reads see GPU-produced world depth directly. */
    zbuffer_cpu_cached =
        !of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_Z) &&
        !of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_ZTEST);
    d_pzbuffer = zbuffer_cpu_cached ? zbuffer_storage
                                    : (short *)of_uncached(zbuffer_storage);

    /* Grow the surface cache with available SDRAM. Target a fraction of
     * the app heap, clamped to [MIN, MAX] and rounded to whole MB; malloc
     * from what's left after Quake's zone/hunk pool (already reserved in
     * main() before this runs). On any failure, fall back to the static
     * floor — no regression, no hard dependency on heap headroom. */
    {
        const struct of_capabilities *caps = of_get_caps();
        byte    *sc_ptr  = surfcache_storage;
        uint32_t sc_size = SURFCACHE_MIN;
        uint32_t target  = caps ? caps->heap_size / SURFCACHE_HEAP_FRACTION : 0;

        if (target > SURFCACHE_MAX) target = SURFCACHE_MAX;
        target &= ~((uint32_t)0xFFFFF);          /* whole MB */

        if (target > SURFCACHE_MIN) {
            byte *p = NULL;
            /* Step down by 1 MB until the heap can satisfy it. */
            for (uint32_t want = target; want > SURFCACHE_MIN;
                 want -= 1u * 1024u * 1024u) {
                p = (byte *)malloc(want);
                if (p) { sc_ptr = p; sc_size = want; break; }
            }
        }

        D_InitCaches(sc_ptr, (int)sc_size);
        Sys_Printf("Surface cache: %u KB (%s)\n", sc_size / 1024u,
                   sc_ptr == surfcache_storage ? "static floor" : "SDRAM");
    }

    Sys_Printf("Video mode: %ux%u stride=%u\n",
               vid_mode.width, vid_mode.height, vid_mode.stride);
    Sys_Printf("GPU caps: hw=%08x span=%d persp=%d param=%d pz=%d pzt=%d q29s=%d frag=%d alpha=%d flip=%d batch=%d zbuf=%s cpu_ring=%d uncached_publish=%d\n",
               of_get_caps()->hw_features,
               of_emit_supports(OF_EMIT_CAP_SPAN),
               of_emit_supports(OF_EMIT_CAP_PERSP),
               of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_LIST),
               of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_Z),
               of_emit_supports(OF_EMIT_CAP_PARAM_SPAN_ZTEST),
               of_emit_supports(OF_EMIT_CAP_Q29_SCALE),
               of_emit_supports(OF_EMIT_CAP_FRAGPIPE),
               of_emit_supports(OF_EMIT_CAP_ALPHA),
               of_emit_supports(OF_EMIT_CAP_FLIP),
               of_emit_supports(OF_EMIT_CAP_SPAN_BATCH),
               zbuffer_cpu_cached ? "cached" : "uncached",
               _gpu_cpu_ring, _gpu_publish_uncached);
    VID_SetPalette(palette);
    of_emit_upload_colormap(host_colormap, 64 * 256);

    /* HW clear all three back buffers. Hardware memset is faster than
     * CPU and avoids the cache flush the CPU memset would need. Use
     * kernel flips during startup so the kernel/display state is fully
     * settled before optional CMD_FLIP handoff. */
    for (int b = 0; b < 3; b++) {
        uint8_t *fb = of_video_surface();
        vid.buffer = vid.conbuffer = vid_uncached_for_surface(fb);
        of_emit_bind_fb((uint32_t)(uintptr_t)fb, vid.rowbytes,
                        (uint32_t)(uintptr_t)zbuffer_storage,
                        vid.width * 2);
        of_emit_clear(OF_EMIT_CLEAR_COLOR, 0, 0);
        of_emit_finish();
        of_video_flip();
        of_video_wait_flip();
    }

    if (of_emit_supports(OF_EMIT_CAP_FLIP)) {
        /* Capture the kernel's current draw slot for CMD_FLIP. */
        draw_idx = of_video_acquire_next(-1, 0);
        vid_set_buffer_index(draw_idx);
        of_emit_bind_fb((uint32_t)(uintptr_t)vid_fb_raw[draw_idx], vid.rowbytes,
                        (uint32_t)(uintptr_t)zbuffer_storage,
                        vid.width * 2);
    } else {
        draw_idx = -1;
        uint8_t *fb = of_video_surface();
        vid.buffer = vid.conbuffer = vid_uncached_for_surface(fb);
        of_emit_bind_fb((uint32_t)(uintptr_t)fb, vid.rowbytes,
                        (uint32_t)(uintptr_t)zbuffer_storage,
                        vid.width * 2);
    }

    Sys_Printf("VID_Init: fullbright=%d buffer=%p draw_idx=%d\n",
               vid.fullbright, vid.buffer, draw_idx);
}

void VID_Shutdown(void) { of_video_set_display_mode(OF_DISPLAY_TERMINAL); }

void VID_Update(vrect_t *rects)
{
    (void)rects;

    vid_perf_timing = host_speeds.value >= 2.0f;

    if (of_emit_supports(OF_EMIT_CAP_FLIP)) {
        uint32_t t0 = vid_perf_now(), vsync = 0;

        /* Runtime-advertised CMD_FLIP path: render commands and the page
         * swap stay ordered in the GPU ring.  Triple buffering lets the next
         * frame render while the previous flip is still waiting for scanout;
         * only block before queuing another flip. */
        flush_span_batch();
        if (flip_present_pending) {
            uint32_t t1;

            /* Let the GPU work on this frame during the present wait. */
            of_gpu_kick_now();
            t1 = vid_perf_now();
            of_video_wait_flip();
            vsync = vid_perf_now() - t1;
            flip_present_pending = 0;
        }

        pending_flip_idx = draw_idx;
        pending_flip_token = of_gpu_flip_to(draw_idx);
        of_gpu_kick();

        vid_perf.us_vsync += vsync;
        vid_perf.us_flip_submit += vid_perf_now() - t0 - vsync;
        draw_idx = -1;
        return;
    }

    /* Conservative 4031-compatible path: finish all GPU writes before
     * asking the kernel to flip, then render into the kernel's next
     * drawable surface. */
    of_emit_finish();
    of_emit_texture_cache_flush();
    vid_perf.frames++;

    of_video_flip();

    vid.buffer = vid.conbuffer = vid_uncached_for_surface(of_video_surface());
    of_emit_kick();
}

/* Wait for a CMD_FLIP fence.  A platform menu can freeze scanout and vblank
 * IRQs, and gpu_core holds CMD_FLIP while the previous swap is pending, so
 * the watchdog counts active vblanks, not spins or time: a paused display
 * waits indefinitely, a GPU that stops with vblanks running traps. */
static void vid_wait_flip_fence(uint32_t token) __attribute__((noinline, cold));
static void vid_wait_flip_fence(uint32_t token)
{
    of_video_timing_t timing;
    uint32_t last_vblank, active_vblanks = 0, spins = 0;

    of_video_get_timing(&timing);
    if (timing.vblank_count == 0 && timing.last_vblank_us == 0) {
        of_gpu_wait(token);             /* no vblank source: bounded spin */
        return;
    }

    last_vblank = timing.vblank_count;
    while (!of_gpu_fence_reached(token)) {
        if (++spins < 1024u)
            continue;
        spins = 0;
        of_video_get_timing(&timing);
        active_vblanks += timing.vblank_count - last_vblank;
        last_vblank = timing.vblank_count;
        if (active_vblanks >= 300u && !of_gpu_fence_reached(token))
            __builtin_trap();
    }
}

void VID_WaitSync(void)
{
    uint32_t t0;

    if (!of_emit_supports(OF_EMIT_CAP_FLIP) || pending_flip_idx < 0)
        return;

    /* Wait only until CMD_FLIP reaches the display side-port.  The returned
     * buffer is the third slot: not current scanout and not queued for the
     * next vsync.  The actual present wait is deferred to VID_Update(), just
     * before the next CMD_FLIP, so rendering can overlap scanout.  Waiting
     * for the fence here keeps the kernel's timeout fallback from swapping a
     * buffer the GPU is still drawing, and retires every queued command
     * before the texture-cache flush below. */
    t0 = vid_perf_now();
    if (!of_gpu_fence_reached(pending_flip_token))
        vid_wait_flip_fence(pending_flip_token);
    draw_idx = of_video_acquire_next(pending_flip_idx, pending_flip_token);
    vid_perf.us_fence += vid_perf_now() - t0;
    vid_perf.frames++;

    of_emit_cpu_sync_needed = 0;
    of_emit_texture_cache_flush();
    flip_present_pending = 1;

    pending_flip_idx = -1;
    pending_flip_token = 0;

    vid_set_buffer_index(draw_idx);
    of_emit_kick();
}

#define DIRECT_RECT_MAX_W 24
#define DIRECT_RECT_MAX_H 24
#define DIRECT_RECT_BUFFERS 3

static byte direct_rect_save[DIRECT_RECT_BUFFERS][DIRECT_RECT_MAX_W * DIRECT_RECT_MAX_H];
static int  direct_rect_active;
static int  direct_rect_x;
static int  direct_rect_y;
static int  direct_rect_w;
static int  direct_rect_h;

static void direct_rect_clamp(int *x, int *y, int *w, int *h)
{
    if (*w > DIRECT_RECT_MAX_W) *w = DIRECT_RECT_MAX_W;
    if (*h > DIRECT_RECT_MAX_H) *h = DIRECT_RECT_MAX_H;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x + *w > (int)vid.width) *x = (int)vid.width - *w;
    if (*y + *h > (int)vid.height) *y = (int)vid.height - *h;
}

void D_BeginDirectRect(int x, int y, byte *pbitmap, int w, int h)
{
    int src_w = w;

    if (!pbitmap || w <= 0 || h <= 0)
        return;

    direct_rect_clamp(&x, &y, &w, &h);
    if (w <= 0 || h <= 0)
        return;

    of_emit_prepare_framebuffer_for_cpu();

    for (int b = 0; b < DIRECT_RECT_BUFFERS; b++) {
        byte *fb = vid_fb_uncached[b];
        if (!fb)
            continue;
        for (int row = 0; row < h; row++) {
            byte *dst = fb + (y + row) * vid.rowbytes + x;
            byte *save = direct_rect_save[b] + row * DIRECT_RECT_MAX_W;
            memcpy(save, dst, (size_t)w);
            memcpy(dst, pbitmap + row * src_w, (size_t)w);
        }
    }

    direct_rect_active = 1;
    direct_rect_x = x;
    direct_rect_y = y;
    direct_rect_w = w;
    direct_rect_h = h;
}

void D_EndDirectRect(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;

    if (!direct_rect_active)
        return;

    for (int b = 0; b < DIRECT_RECT_BUFFERS; b++) {
        byte *fb = vid_fb_uncached[b];
        if (!fb)
            continue;
        for (int row = 0; row < direct_rect_h; row++) {
            byte *dst = fb + (direct_rect_y + row) * vid.rowbytes + direct_rect_x;
            byte *save = direct_rect_save[b] + row * DIRECT_RECT_MAX_W;
            memcpy(dst, save, (size_t)direct_rect_w);
        }
    }

    direct_rect_active = 0;
}
