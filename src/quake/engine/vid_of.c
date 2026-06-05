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
#define SURFCACHE_SIZE   (2 * 1024 * 1024)
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
_Static_assert(GPU_CMD_DRAW_PARAM_SPAN_LIST == 0x48,
               "param span-list opcode must match openfpgaOS");
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

#if defined(OF_GPU_STALL_COUNT) && defined(OF_GPU_TEX_DBG_COUNTER_MASK)
#define PQ_GPU_HAVE_HW_TEX_STALL_COUNTERS 1
#else
#define PQ_GPU_HAVE_HW_TEX_STALL_COUNTERS 0
#endif

/* Surface cache in BSS (cacheable SDRAM). 2 MB bank. */
static byte surfcache_storage[SURFCACHE_SIZE] __attribute__((aligned(64)));

/* Keep enough z storage for the old default 320x240 fallback as well as
 * Quake's preferred 320x200 source mode. */
static short zbuffer_storage[ZBUFFER_MAX_WIDTH * ZBUFFER_MAX_HEIGHT]
    __attribute__((aligned(64)));

/* ---- of_emit_* API (the GPU owner's public surface) --------------- */

/* Span batch accumulator.
 *
 * Quake's world / sky / sprite / alias-poly inner loops submit one
 * span per scanline.  The current SDK lowers compatibility affine and
 * perspective groups through GPU_CMD_DRAW_PARAM_SPAN_LIST, so the accumulator
 * keeps order boundaries while packing compatible spans into lanes.
 *
 * The accumulator MUST flush before any other GPU command the queued
 * spans should observe in ring order: bind_fb, bind_texture, clear,
 * triangles*, kick, finish.  of_emit_blit submits per-row spans through
 * of_emit_span so it gets the same grouping automatically. */
static of_emit_span_t span_buf[PQ_GPU_SPAN_BATCH_MAX];
static int           span_buf_count;

static inline int32_t gpu_i32_add_mul(int32_t a, int32_t step, int n)
{
    return (int32_t)((uint32_t)a + (uint32_t)step * (uint32_t)n);
}

static inline int32_t gpu_i32_sub_mul(int32_t a, int32_t step, int n)
{
    return (int32_t)((uint32_t)a - (uint32_t)step * (uint32_t)n);
}

static void gpu_span_range(const of_emit_span_t *sp,
                           uint64_t *lo, uint64_t *hi)
{
    int64_t first = (int64_t)(uint64_t)sp->fb_addr;
    int64_t last = first;
    uint32_t count = sp->count;

    if (count > 1)
        last = first + (int64_t)sp->fb_stride * (int64_t)(count - 1u);

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
#endif

static int gpu_affine_can_append(const of_gpu_affine_span_group_t *group,
                                 int lanes,
                                 const uint64_t *range_lo,
                                 const uint64_t *range_hi,
                                 const of_emit_span_t *sp,
                                 uint64_t sp_lo,
                                 uint64_t sp_hi)
{
    if (sp->flags & OF_EMIT_PERSP)
        return 0;
    if (lanes <= 0)
        return 1;
    if (lanes >= (int)OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES)
        return 0;
    if (group->flags != sp->flags ||
        group->tex_width != sp->tex_width ||
        group->tex_w_mask != sp->tex_w_mask ||
        group->tex_h_mask != sp->tex_h_mask ||
        group->fb_step != sp->fb_stride)
        return 0;

    for (int i = 0; i < lanes; i++) {
        if (gpu_ranges_overlap(range_lo[i], range_hi[i], sp_lo, sp_hi))
            return 0;
    }

    return 1;
}

static void gpu_affine_append(of_gpu_affine_span_group_t *group,
                              int *lanes,
                              uint64_t *range_lo,
                              uint64_t *range_hi,
                              const of_emit_span_t *sp,
                              uint64_t sp_lo,
                              uint64_t sp_hi)
{
    int lane = *lanes;

    if (lane == 0) {
        memset(group, 0, sizeof(*group));
        group->flags = sp->flags;
        group->tex_width = sp->tex_width;
        group->tex_w_mask = sp->tex_w_mask;
        group->tex_h_mask = sp->tex_h_mask;
        group->fb_step = sp->fb_stride;
    }

    group->fb_addr[lane] = sp->fb_addr;
    group->tex_addr[lane] = sp->tex_addr;
    group->count[lane] = sp->count;
    group->s[lane] = sp->s;
    group->t[lane] = sp->t;
    group->sstep[lane] = sp->sstep;
    group->tstep[lane] = sp->tstep;
    group->light[lane] = sp->light;
    group->colormap_id[lane] = sp->colormap_id;
    range_lo[lane] = sp_lo;
    range_hi[lane] = sp_hi;
    *lanes = lane + 1;
}

static void gpu_flush_affine_group(of_gpu_affine_span_group_t *group,
                                   int *lanes)
{
    if (*lanes <= 0)
        return;

    group->lane_count = (uint8_t)*lanes;
    of_gpu_draw_affine_span_group(group);
    *lanes = 0;
}

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

static inline int flush_span_batch(void)
{
    if (span_buf_count > 0) {
        int count = span_buf_count;
        int used_dma = (_gpu_batch_buf != NULL);
        of_gpu_affine_span_group_t affine_group;
        gpu_persp_builder_t persp_group;
        uint64_t range_lo[OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES];
        uint64_t range_hi[OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES];
        int affine_lanes = 0;

        memset(&persp_group, 0, sizeof(persp_group));

        for (int i = 0; i < count; i++) {
            of_emit_span_t *sp = &span_buf[i];
            uint64_t sp_lo, sp_hi;

            if (sp->count == 0)
                continue;

            if (sp->flags & OF_EMIT_PERSP) {
                gpu_flush_affine_group(&affine_group, &affine_lanes);
                if (!gpu_persp_try_append(&persp_group, sp)) {
                    gpu_flush_persp_group(&persp_group);
                    if (!gpu_persp_try_append(&persp_group, sp))
                        gpu_draw_persp_span(sp);
                }
                if (persp_group.lanes ==
                    (int)OF_GPU_PERSP_SPAN_GROUP_MAX_LANES)
                    gpu_flush_persp_group(&persp_group);
                continue;
            }

            gpu_flush_persp_group(&persp_group);
            gpu_span_range(sp, &sp_lo, &sp_hi);
            if (gpu_affine_can_append(&affine_group, affine_lanes,
                                      range_lo, range_hi,
                                      sp, sp_lo, sp_hi)) {
                gpu_affine_append(&affine_group, &affine_lanes,
                                  range_lo, range_hi,
                                  sp, sp_lo, sp_hi);
                if (affine_lanes ==
                    (int)OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES)
                    gpu_flush_affine_group(&affine_group, &affine_lanes);
                continue;
            }

            gpu_flush_affine_group(&affine_group, &affine_lanes);
            gpu_affine_append(&affine_group, &affine_lanes,
                              range_lo, range_hi,
                              sp, sp_lo, sp_hi);
        }

        gpu_flush_persp_group(&persp_group);
        gpu_flush_affine_group(&affine_group, &affine_lanes);
        of_gpu_kick();

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
    of_emit_caps |= OF_EMIT_CAP_SPAN_BATCH;

    /* of_gpu_init resolves the MMIO base from caps before any GPU_CTRL
     * access.  It also ring-resets and enables the core. */
    of_gpu_init();
    of_emit_ready = 1;

    /* Generic triangle depth is still not exposed here.  Param spans can
     * write and/or test d_pzbuffer when the runtime caps advertise it. */
}

void of_emit_upload_colormap(const unsigned char *cm, uint32_t size)
{
    of_gpu_palookup_upload(0, cm, size);
}

void of_emit_bind_fb(uint32_t fb_addr, int fb_stride,
                     uint32_t zb_addr, int zb_stride_bytes)
{
    (void)zb_addr; (void)zb_stride_bytes;  /* no generic z-buffer bind */
    flush_span_batch();
    of_gpu_set_framebuffer(fb_addr, (uint16_t)fb_stride);
}

void of_emit_finish(void)
{
    flush_span_batch();
    /* of_gpu_finish() is where the CPU waits until every submitted
     * raster command has retired. */
    of_gpu_finish();
    of_emit_cpu_sync_needed = 0;
}

void of_emit_prepare_framebuffer_for_cpu(void)
{
    if (!of_emit_ready || !of_emit_cpu_sync_needed)
        return;
    of_emit_finish();
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
        span_buf_count < PQ_GPU_SPAN_BATCH_MAX)
        return;

    if (!flush_span_batch())
        of_gpu_kick();
}

void of_emit_texture_cache_flush(void)
{
    if (of_emit_ready)
        GPU_TEX_FLUSH = 1;
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
    if (!of_emit_supports(OF_EMIT_CAP_SPAN))
        return;
    gpu_validate_span_dest(sp);
    of_emit_cpu_sync_needed = 1;

    if ((int)pq_gpu_safe_spans.value) {
        flush_span_batch();
        span_buf[span_buf_count++] = *sp;
        flush_span_batch();
        return;
    }

    span_buf[span_buf_count++] = *sp;
    if (span_buf_count >= PQ_GPU_SPAN_BATCH_MAX)
        flush_span_batch();
}

int of_emit_param_span_list(const of_emit_param_span_list_t *params,
                            const of_emit_param_span_record_t *records,
                            uint32_t record_count)
{
    uint32_t submitted = 0;
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
    flush_span_batch();

    while (record_count != 0) {
        uint32_t chunk = (int)pq_gpu_safe_spans.value ? 1u : record_count;
        if (chunk > OF_GPU_PARAM_SPAN_MAX_RECORDS)
            chunk = OF_GPU_PARAM_SPAN_MAX_RECORDS;

        /* of_emit_param_span_list_t mirrors of_gpu_param_span_list_t
         * field-for-field (the records cast below relies on the same
         * contract), so hand the caller's struct straight down instead
         * of staging a ~100-byte copy per surface. */
        of_emit_cpu_sync_needed = 1;
        of_gpu_draw_param_span_list(
            (const of_gpu_param_span_list_t *)params,
            (const of_gpu_param_span_record_t *)records, chunk);

        submitted += chunk;
        records += chunk;
        record_count -= chunk;
    }

    return (int)submitted;
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
    (void)verts;
    (void)num_vertices;
    /* Retired SDK triangle commands are intentionally not reached from the
     * current openfpgaOS API. World and alias GPU acceleration uses spans. */
}

void of_emit_triangles_batch(const of_emit_vertex_t *verts,
                             uint32_t num_vertices)
{
    (void)verts;
    (void)num_vertices;
    /* Compatibility no-op for disabled experimental triangle paths. */
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

    D_InitCaches(surfcache_storage, SURFCACHE_SIZE);

    Sys_Printf("Video mode: %ux%u stride=%u\n",
               vid_mode.width, vid_mode.height, vid_mode.stride);
    Sys_Printf("GPU caps: hw=%08x span=%d persp=%d param=%d pz=%d pzt=%d q29s=%d frag=%d alpha=%d flip=%d batch=%d zbuf=%s\n",
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
               zbuffer_cpu_cached ? "cached" : "uncached");
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

    if (of_emit_supports(OF_EMIT_CAP_FLIP)) {
        /* Runtime-advertised CMD_FLIP path: render commands and the page
         * swap stay ordered in the GPU ring.  Triple buffering lets the next
         * frame render while the previous flip is still waiting for scanout;
         * only block before queuing another flip. */
        flush_span_batch();
        if (flip_present_pending) {
            of_video_wait_flip();
            flip_present_pending = 0;
        }

        pending_flip_idx = draw_idx;
        pending_flip_token = of_gpu_flip_to(draw_idx);
        of_gpu_kick();

        draw_idx = -1;
        return;
    }

    /* Conservative 4031-compatible path: finish all GPU writes before
     * asking the kernel to flip, then render into the kernel's next
     * drawable surface. */
    of_emit_finish();
    of_emit_texture_cache_flush();

    of_video_flip();

    vid.buffer = vid.conbuffer = vid_uncached_for_surface(of_video_surface());
    of_emit_kick();
}

void VID_WaitSync(void)
{
    if (!of_emit_supports(OF_EMIT_CAP_FLIP) || pending_flip_idx < 0)
        return;

    /* Wait only until CMD_FLIP reaches the display side-port.  The returned
     * buffer is the third slot: not current scanout and not queued for the
     * next vsync.  The actual present wait is deferred to VID_Update(), just
     * before the next CMD_FLIP, so rendering can overlap scanout. */
    draw_idx = of_video_acquire_next(pending_flip_idx, pending_flip_token);

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
