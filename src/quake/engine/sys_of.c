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
#include <dirent.h>

/* Quake globals */
qboolean isDedicated = false;

#define QUAKE_CONFIG_SECTION "quake"
#define QUAKE_DEFAULT_CONFIG "quake.cfg"
#define QUAKE_CONFIG_MARKER "// Quake 3.0 config"

static char sys_quake_config_name[MAX_OSPATH] = QUAKE_DEFAULT_CONFIG;
static int sys_quake_ini_present;

/* Resolve the "data file" part of a path for SDK fopen. Quake emits
 * paths like "./id1/pak0.pak" or "quake/id1/pak0.pak" based on
 * com_basedir; the SDK's filename mapping is by bare filename. Strip
 * everything before the last slash. */
static const char *basename_of(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static int ascii_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int ascii_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int d = ascii_tolower((unsigned char)*a) -
                ascii_tolower((unsigned char)*b);
        if (d)
            return d;
        a++;
        b++;
    }
    return ascii_tolower((unsigned char)*a) -
           ascii_tolower((unsigned char)*b);
}

static int Sys_ConfigSectionPresent(void)
{
    uint32_t cursor = 0;
    char key[64];
    char value[MAX_OSPATH];

    return of_config_next(QUAKE_CONFIG_SECTION, &cursor,
                          key, sizeof(key),
                          value, sizeof(value)) == 0;
}

void Sys_InitQuakeConfig(void)
{
    char value[MAX_OSPATH];

    snprintf(sys_quake_config_name, sizeof(sys_quake_config_name),
             "%s", QUAKE_DEFAULT_CONFIG);
    sys_quake_ini_present = 0;

    if (of_config_get(QUAKE_CONFIG_SECTION, "CONFIG",
                      value, sizeof(value)) == 0) {
        sys_quake_ini_present = 1;
        if (value[0]) {
            snprintf(sys_quake_config_name, sizeof(sys_quake_config_name),
                     "%s", value);
        }
        return;
    }

    sys_quake_ini_present = Sys_ConfigSectionPresent();
}

void Sys_QuakeConfigPath(char *out, int out_size)
{
    snprintf(out, (size_t)out_size, "%s", sys_quake_config_name);
}

static const char *Sys_MapConfigName(const char *path)
{
    const char *name = basename_of(path);
    if (!ascii_strcasecmp(name, "config.cfg"))
        return sys_quake_config_name;
    return name;
}

static int Sys_IsConfigPath(const char *path)
{
    return !ascii_strcasecmp(basename_of(path), "config.cfg");
}

static int Sys_TextContains(const char *text, const char *needle)
{
    return strstr(text, needle) != NULL;
}

static int Sys_ConfigBufferLooksQuake(const unsigned char *buf, size_t len)
{
    char text[1025];
    size_t n = len;
    static const char *bad_tokens[] = {
        "mouse_sensitivity",
        "sfx_volume",
        "music_volume",
        "show_messages",
        "screenblocks",
        "detaillevel",
        "frame_interpolation",
        "refresh_mode",
        "snd_samplerate",
        "snd_musicdevice",
        "video_driver",
        "window_position",
        "fullscreen",
        "aspect_ratio_correct",
        "smooth_pixel_scaling",
        "integer_scaling",
        "show_endoom",
        "png_screenshots",
        "autoload_path",
        "music_pack_path",
        "vanilla_savegame_limit",
        "vanilla_demo_limit",
        "player_name",
        "grabmouse",
        "joystick_",
        "mouseb_",
        "joyb_",
        "key_",
        "use_mouse",
        "use_joystick",
        "snd_",
        "key_menu_",
        "key_map_",
        "key_weapon",
        "key_multi_"
    };
    static const char *quake_tokens[] = {
        QUAKE_CONFIG_MARKER,
        "bind \"",
        "_cl_name",
        "_cl_color",
        "sensitivity",
        "m_pitch",
        "m_yaw",
        "m_forward",
        "m_side",
        "lookspring",
        "lookstrafe",
        "volume",
        "bgmvolume",
        "viewsize",
        "gamma",
        "crosshair",
        "sv_",
        "savedgamecfg",
        "saved1",
        "fraglimit",
        "timelimit",
        "teamplay",
        "noexit"
    };

    if (n > sizeof(text) - 1)
        n = sizeof(text) - 1;
    if (n == 0)
        return 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == 0 || c == 0xff) {
            n = i;
            break;
        }
        if (c < ' ' && c != '\n' && c != '\r' && c != '\t')
            return 0;
        if (c >= 0x80)
            return 0;
        text[i] = (char)c;
    }
    text[n] = '\0';

    if (n == 0)
        return 0;

    for (size_t i = 0; i < sizeof(bad_tokens) / sizeof(bad_tokens[0]); i++)
        if (Sys_TextContains(text, bad_tokens[i]))
            return 0;

    for (size_t i = 0; i < sizeof(quake_tokens) / sizeof(quake_tokens[0]); i++)
        if (Sys_TextContains(text, quake_tokens[i]))
            return 1;

    return 0;
}

static int Sys_ConfigFileLooksQuake(FILE *f)
{
    unsigned char buf[1024];
    long pos = ftell(f);
    size_t n;
    int ok;

    if (pos < 0)
        pos = 0;
    fseek(f, 0, SEEK_SET);
    n = fread(buf, 1, sizeof(buf), f);
    ok = Sys_ConfigBufferLooksQuake(buf, n);
    fseek(f, pos, SEEK_SET);

    return ok;
}

static int Sys_ResetConfigFile(const char *name)
{
    FILE *f = fopen(name, "wb");

    if (!f)
        return 0;

    fprintf(f, "%s\n", QUAKE_CONFIG_MARKER);
    fclose(f);
    return 1;
}

int Sys_QuakePackPath(char *dir, int pak_index, char *out, int out_size)
{
    const char *game = basename_of(dir);
    char key[64];
    int rc;

    if (!sys_quake_ini_present)
        return 0;
    if (!out || out_size <= 0)
        return -1;

    if (snprintf(key, sizeof(key), "PAK_%s_%d", game, pak_index) >=
        (int)sizeof(key))
        return -1;

    rc = of_config_get(QUAKE_CONFIG_SECTION, key, out, (uint32_t)out_size);
    if (rc == 0 && out[0])
        return 1;
    return -1;
}

/* Read an integer from the [quake] section of the .ini, or return def.
 * Lets debug knobs (e.g. host_speeds) be enabled without console input. */
int Sys_IniGetInt(const char *key, int def)
{
    char val[32];
    if (of_config_get(QUAKE_CONFIG_SECTION, key, val, sizeof(val)) == 0 && val[0])
        return atoi(val);
    return def;
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
    int i;
    CDAudio_DrainAsync();   /* free the slot bridge before a blocking read */
    i = findhandle();
    int mapped_config = Sys_IsConfigPath(path);
    const char *name = Sys_MapConfigName(path);
    FILE *f = fopen(name, "rb");
    if (!f && !mapped_config) f = fopen(path, "rb");
    if (f && mapped_config && !Sys_ConfigFileLooksQuake(f)) {
        fclose(f);
        f = NULL;
        if (Sys_ResetConfigFile(name))
            f = fopen(name, "rb");
    }
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
    CDAudio_DrainAsync();   /* free the slot bridge before a blocking read */
    return (int)fread(dest, 1, (size_t)count, sys_handles[handle]);
}

int Sys_FileWrite(int handle, void *data, int count)
{
    (void)handle; (void)data; (void)count;
    return 0;
}

int Sys_FileTime(char *path)
{
    int mapped_config;
    CDAudio_DrainAsync();   /* free the slot bridge before a blocking open */
    mapped_config = Sys_IsConfigPath(path);
    const char *name = Sys_MapConfigName(path);
    FILE *f = fopen(name, "rb");
    if (!f && !mapped_config) f = fopen(path, "rb");
    if (f) {
        int ok = !mapped_config || Sys_ConfigFileLooksQuake(f);
        fclose(f);
        if (!ok)
            ok = Sys_ResetConfigFile(name);
        return ok ? 1 : -1;
    }
    return -1;
}

void Sys_mkdir(char *path) { (void)path; }

/* ---------------- Save games ---------------------------------------- *
 * Save files are bound by the APF instance file: s0.sav..s9.sav map to the
 * nonvolatile save data slots 10..19 (the current instance's cfg file maps
 * to config slot 8).  Bound slots appear as flat names in the kernel's
 * virtual root — they must be resolved by slot id and opened by that name,
 * never by a guessed "<gamedir>/sN.sav" path (same rule as the CD track
 * resolution in cd_of.c: path-based opens of unbound names can wedge the
 * slot bridge). */

#define SYS_SAVE_SLOT_FIRST 10
#define SYS_SAVE_SLOT_COUNT 10

FILE *Sys_OpenSaveSlot(int idx, const char *mode)
{
    DIR *d;
    struct dirent *e;
    char name[64];
    int found = 0;

    if (idx < 0 || idx >= SYS_SAVE_SLOT_COUNT)
        return NULL;

    CDAudio_DrainAsync();   /* free the slot bridge before a blocking open */

    d = opendir("/");
    if (!d)
        return NULL;
    while ((e = readdir(d)) != NULL) {
        if ((int)e->d_ino - 1 == SYS_SAVE_SLOT_FIRST + idx) {
            snprintf(name, sizeof(name), "%s", e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);

    if (!found)
        return NULL;

    return fopen(name, mode);
}

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

/* Quake SRAM-fill coprocessor — not present on openfpgaOS.  Stubbed. */
void sram_fill_start(uint32_t dst, uint16_t value, uint32_t count)
{ (void)dst; (void)value; (void)count; }
void sram_fill_wait(void) { }

/* Backing store for the sysreg_stub.h perf-counter aliases. Every
 * read returns 0, every write goes here. Profiling shows zero cycles
 * while the rest of the codebase compiles unchanged. */
uint32_t _of_sysreg_ro_zero;
