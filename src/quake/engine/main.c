/*
 * main.c -- Quake app entry for openfpgaOS.
 *
 * Brings up the SDK, allocates the engine heap from the SDK-supplied
 * heap region, then hands control to Host_Init / Host_Frame.
 */

#include "of.h"
#include "of_caps.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "quakedef.h"

/* The engine heap lives in SDRAM.  Loading a map peaks at roughly the
 * .bsp file size PLUS its parsed size: COM_LoadStackFile parks the raw
 * file in the HIGH hunk (Hunk_TempAlloc) and it has to stay there for
 * the whole parse, while Mod_Load* builds the structures in the LOW
 * hunk.  id1 maps are 1-2 MB so a flat 24 MB was never tested near the
 * edge; the re-release BSP2 maps run 8-18 MB on disk, which puts the
 * peak past 24 MB (mg3's map1: 13.1 MB file + ~13 MB parsed = Hunk_Alloc
 * FULL).
 *
 * So size from the caps heap rather than hardcoding: subtract what the
 * surface cache will take (vid_of.c asks for heap/8, capped at 8 MB)
 * plus a reserve for GPU textures, audio buffers and misc allocations,
 * then step down a megabyte at a time until malloc actually succeeds.
 * Same step-down pattern the surface cache already uses, so a device
 * with less SDRAM degrades instead of failing to boot. */
#define QUAKE_HEAP_MIN      (16u * 1024u * 1024u)
#define QUAKE_HEAP_MAX      (40u * 1024u * 1024u)
#define QUAKE_HEAP_RESERVE  (6u * 1024u * 1024u)   /* textures/audio/misc */
#define QUAKE_SURFCACHE_CAP (8u * 1024u * 1024u)   /* mirrors SURFCACHE_MAX */

static void *Quake_AllocHeap(uint32_t *out_size)
{
    const struct of_capabilities *caps = of_get_caps();
    uint32_t heap = caps ? caps->heap_size : 0u;
    uint32_t want = QUAKE_HEAP_MAX;

    if (heap) {
        uint32_t surfcache = heap / 8u;
        if (surfcache > QUAKE_SURFCACHE_CAP)
            surfcache = QUAKE_SURFCACHE_CAP;
        uint32_t reserve = surfcache + QUAKE_HEAP_RESERVE;
        want = (heap > reserve) ? (heap - reserve) : QUAKE_HEAP_MIN;
    }

    if (want > QUAKE_HEAP_MAX) want = QUAKE_HEAP_MAX;
    if (want < QUAKE_HEAP_MIN) want = QUAKE_HEAP_MIN;
    want &= ~0xFFFFFu;                              /* whole MB */

    for (; want >= QUAKE_HEAP_MIN; want -= 1u * 1024u * 1024u) {
        void *p = malloc(want);
        if (p) {
            *out_size = want;
            return p;
        }
    }
    return NULL;
}

/* Engine symbols we call directly. */
void Host_Init(quakeparms_t *parms);
void Host_Frame(float time);
void Sys_Printf(char *fmt, ...);
void Sys_InitQuakeConfig(void);

static char *quake_argv_base[]     = { "quake", NULL };

static void Quake_SetArgs(quakeparms_t *parms, int argc, char **argv)
{
    if (argc > 0 && argv) {
        parms->argc = argc;
        parms->argv = argv;
        return;
    }

    parms->argc = 1;
    parms->argv = quake_argv_base;
}

int main(int argc, char **argv)
{
    of_video_init();
    of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
#ifndef OF_PC
    of_video_set_color_mode(OF_VIDEO_MODE_8BIT);

    /* Filename→slot bindings come from instance.json; the kernel
     * auto-discovers them from the APF bridge at boot (see slotdemo). */
#endif /* !OF_PC */

    quakeparms_t parms;
    memset(&parms, 0, sizeof(parms));
    parms.basedir = ".";
    parms.cachedir = NULL;
    Quake_SetArgs(&parms, argc, argv);
    Sys_InitQuakeConfig();

    {
        uint32_t heap_bytes = 0;
        parms.membase = Quake_AllocHeap(&heap_bytes);
        if (!parms.membase) {
            printf("[quake] FATAL: out of heap (wanted at least %u bytes)\n",
                   (unsigned)QUAKE_HEAP_MIN);
            return 1;
        }
        parms.memsize = (int)heap_bytes;
    }

    Sys_Printf("Quake heap: %p..%p (%d MB)\n",
               parms.membase,
               (char *)parms.membase + parms.memsize,
               parms.memsize / (1024 * 1024));

    Host_Init(&parms);

    /* Main loop — Quake's host runs one frame with a time delta. We
     * pull the delta from the SDK microsecond timer; vsync pacing is
     * handled inside of_video_flip() (double/triple-buffered).
     *
     * last_us is ONLY updated when we actually run a frame — otherwise
     * dt stops accumulating and the min-dt guard spins forever. */
    uint32_t last_us = of_time_us();
    for (;;) {
        uint32_t now_us = of_time_us();
        uint32_t dt_us  = now_us - last_us;
        if (dt_us < 1000) continue;
        last_us = now_us;
        float time = (float)dt_us * 1.0e-6f;
        if (time > 0.1f) time = 0.1f;
        Host_Frame(time);
    }
}
