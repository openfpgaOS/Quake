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

/* The engine heap lives in SDRAM, sized from the caps descriptor.
 * We reserve 8 MB for the BSP/lightmap zone and hand the rest to Quake.
 * Mapping: 16 MB for host data / audio / GPU textures, rest (≈24 MB)
 * goes to quake zone so large maps fit comfortably. */
#define QUAKE_HEAP_SIZE   (24 * 1024 * 1024)

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

    parms.membase = malloc(QUAKE_HEAP_SIZE);
    if (!parms.membase) {
        printf("[quake] FATAL: out of heap (wanted %u bytes)\n",
               (unsigned)QUAKE_HEAP_SIZE);
        return 1;
    }
    parms.memsize = QUAKE_HEAP_SIZE;

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
