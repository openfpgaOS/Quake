/*
 * sys_of.c -- openfpgaOS system driver for Quake.
 *
 * Implements the Sys_* API Quake expects: file I/O (via SDK fopen),
 * time, printf, error, quit. File paths route through the SDK's
 * filename→slot registration done in main.c.
 */

#include "quakedef.h"
#include "of.h"
#include "of_services.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>

/* Quake globals */
qboolean isDedicated = false;

/* Hang-debug stage counters — Quake wrote these from various hot
 * paths so a post-mortem hex dump of SDRAM would show where execution
 * had last been. Not needed here, but still written by the engine. */
volatile unsigned int pq_dbg_stage;
volatile unsigned int pq_dbg_info;

/* Resolve the "data file" part of a path for SDK fopen. Quake emits
 * paths like "./id1/pak0.pak" or "quake/id1/pak0.pak" based on
 * com_basedir; the SDK's filename mapping is by bare filename. Strip
 * everything before the last slash. */
static const char *basename_of(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* ---------------- File I/O ------------------------------------------ */

#define MAX_HANDLES 16
static FILE *sys_handles[MAX_HANDLES];

static int findhandle(void)
{
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!sys_handles[i])
            return i;
    Sys_Error("out of handles");
    return -1;
}

int filelength(FILE *f)
{
    long pos = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return (int)end;
}

int Sys_FileOpenRead(char *path, int *hndl)
{
    int i = findhandle();
    const char *name = basename_of(path);
    FILE *f = fopen(name, "rb");
    if (!f) f = fopen(path, "rb");
    if (!f) {
        *hndl = -1;
        return -1;
    }
    sys_handles[i] = f;
    *hndl = i;
    return filelength(f);
}

int Sys_FileOpenWrite(char *path)
{
    (void)path;
    /* Config and save writes route through Sys_Save*; Quake's write
     * path for e.g. screenshots is rarely used and not persistable
     * here — return -1 so callers skip. */
    return -1;
}

void Sys_FileClose(int handle)
{
    if (handle > 0 && handle < MAX_HANDLES && sys_handles[handle]) {
        fclose(sys_handles[handle]);
        sys_handles[handle] = NULL;
    }
}

void Sys_FileSeek(int handle, int position)
{
    if (handle > 0 && handle < MAX_HANDLES && sys_handles[handle])
        fseek(sys_handles[handle], position, SEEK_SET);
}

int Sys_FileRead(int handle, void *dest, int count)
{
    if (handle <= 0 || handle >= MAX_HANDLES || !sys_handles[handle])
        return 0;
    return (int)fread(dest, 1, (size_t)count, sys_handles[handle]);
}

int Sys_FileWrite(int handle, void *data, int count)
{
    (void)handle; (void)data; (void)count;
    return 0;
}

int Sys_FileTime(char *path)
{
    const char *name = basename_of(path);
    FILE *f = fopen(name, "rb");
    if (!f) f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return -1;
}

void Sys_mkdir(char *path) { (void)path; }

/* ---------------- Save games ---------------------------------------- *
 * The SDK exposes up to 10 persistent save slots as `save:N`. We map
 * Quake's `save/s0.sav`..`save/s9.sav` strings onto them. Quake also
 * writes `config.cfg`; persist that into save:0 with a leading tag so
 * we don't collide with game saves — actually easier: use save:9 as a
 * config slot (Quake only uses 10 game slots by default). */

static int save_slot_from_path(const char *path)
{
    /* Accepted forms: ".../s0.sav" .. ".../s9.sav". */
    const char *p = strrchr(path, '/');
    p = p ? p + 1 : path;
    if (p[0] == 's' && p[1] >= '0' && p[1] <= '9' &&
        strcmp(p + 2, ".sav") == 0) {
        return p[1] - '0';
    }
    return -1;
}

/* Chocolate-doom-style save: open "save:N" via the SDK's fopen. These
 * override the Quake file handle path for save reads/writes so the
 * engine's host_cmd.c still calls Sys_FileOpenRead and friends. */

/* ---------------- Time ---------------------------------------------- */

float Sys_FloatTime(void)
{
    static int        initialized = 0;
    static uint32_t   t0_us = 0;
    uint32_t now = of_time_us();
    if (!initialized) { initialized = 1; t0_us = now; return 0.0f; }
    return (float)(now - t0_us) * 1.0e-6f;
}

/* ---------------- Printf / Error / Quit ----------------------------- */

void Sys_Error(char *error, ...)
{
    va_list argptr;
    va_start(argptr, error);
    printf("[quake] Sys_Error: ");
    vprintf(error, argptr);
    printf("\n");
    va_end(argptr);
    /* Bring terminal forward so diagnostics are visible. */
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    for (;;) { /* halt */ }
}

void Sys_Printf(char *fmt, ...)
{
    va_list argptr;
    va_start(argptr, fmt);
    vprintf(fmt, argptr);
    va_end(argptr);
    fflush(stdout);
}

void Sys_Quit(void) { of_exit(); }

char *Sys_ConsoleInput(void) { return NULL; }
void  Sys_Sleep(void)        { }
void  Sys_HighFPPrecision(void) { }
void  Sys_LowFPPrecision(void)  { }

/* Quake calls these inside the keyboard event loop. */
extern void IN_SendKeyEvents(void);
void Sys_SendKeyEvents(void) { IN_SendKeyEvents(); }

void Sys_MakeCodeWriteable(unsigned long a, unsigned long b)
{ (void)a; (void)b; }

/* DMA stats — stub for parity with Quake's terminal dump. */
void Sys_PrintDmaStats(void) { }

/* Quake's on-screen debug terminal.  The engine drives this as a
 * cursor-positioned grid (term_setpos + term_puts per row), but on
 * openfpgaOS there is no real TUI — output goes to UART.  Map each
 * row to one printed line: term_setpos becomes a no-op (row/col carry
 * no meaning over a serial stream), and term_puts emits the payload
 * followed by CRLF so each row lands on its own line in a UART log.
 * term_clear emits a leading blank line so consecutive dumps are
 * visually separated. */
void term_clear(void)                 { fputs("\r\n", stdout); }
void term_setpos(int row, int col)    { (void)row; (void)col; }
void term_puts(const char *s)         { if (s) { fputs(s, stdout); fputs("\r\n", stdout); } }

/* Quake SRAM-fill coprocessor — not present on openfpgaOS.  Stubbed. */
void sram_fill_start(uint32_t dst, uint16_t value, uint32_t count)
{ (void)dst; (void)value; (void)count; }
void sram_fill_wait(void) { }

/* Backing store for the sysreg_stub.h perf-counter aliases. Every
 * read returns 0, every write goes here. Profiling shows zero cycles
 * while the rest of the codebase compiles unchanged. */
uint32_t _of_sysreg_ro_zero;
