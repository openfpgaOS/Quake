/*
 * cd_of.c -- Quake CD-audio over the openfpgaOS hardware PCM mixer.
 *
 * The original Quake soundtrack ships as Red Book CDDA: tracks 2..N are
 * raw 44100 Hz / 16-bit / stereo / little-endian PCM (a ".bin" per track
 * in the cue/bin rip on the card, no header, no compression).  That maps
 * straight onto the OS hardware mixer, so we never decode or software-mix
 * a single music sample — the FPGA mixer DMAs the PCM out of SDRAM,
 * resamples 44100->48000, and sums it with the SFX voices in fabric.
 * The only CPU cost is a small, infrequent ring top-up in CDAudio_Update.
 *
 * Design (matches the SFX path in snd_dma.c, which also rides of_mixer):
 *
 *   - A per-channel SDRAM ring (CD_RING_SECONDS of audio).  Two mono
 *     mixer voices play it: voice L panned hard-left, voice R hard-right,
 *     both tagged OF_MIXER_GROUP_MUSIC and looped over the whole ring so
 *     they wrap forever.  The mixer is mono-per-voice, hence two voices
 *     for stereo (same trick as moddemo/pan_test.h).  MUSIC-group voices
 *     allocate at the opposite end of the slot range from SFX and prefer
 *     same-group steal victims, so music can never silence an in-flight
 *     SFX (see of_mixer_alloc_for_group).
 *
 *   - CDAudio_Update() (called once per frame from host.c, main thread,
 *     no ISR) reads the voice's play cursor and tops the ring back up
 *     *behind* the cursor.  The refill bytes come from the track file by
 *     async data-slot DMA (of_file_read_async): the CPU kicks off a read
 *     and returns, the bytes land in CRAM0 in the background, and a
 *     completion IRQ flags them; next frame the CPU only deinterleaves the
 *     finished chunk into the ring and flushes it.  The SD transfer itself
 *     costs the CPU nothing -- which matters, because a *blocking* fread
 *     of the 176 KB/s stream measured ~13% of total CPU.  (The synchronous
 *     fread path is kept as a fallback / [quake] CD_ASYNC=0.)  Because the
 *     hardware keeps playing the ring regardless of when we refill, a long
 *     frame just means more to top up next frame -- no audio deadline on
 *     the CPU and no underrun as long as we refill before the cursor laps
 *     the ring (CD_RING_SECONDS of slack).  Immune to the ISR-reentrancy
 *     limitation documented in snd_of.c.
 *
 *   - of_cache_flush_range() after every write: the mixer voice fetch is
 *     an external AXI master that reads DRAM directly and will see stale
 *     L1 lines otherwise (see of_cache.h).
 *
 * CDDA byte order is little-endian s16, matching the RISC-V core and the
 * mixer's expected sample format, so we read straight into int16 with no
 * byte swap.
 */

#include "quakedef.h"

#include "of.h"
#include "of_mixer.h"
#include "of_cache.h"
#include "of_config.h"
#include "of_file.h"
#include "of_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/* Same [section] sys_of.c reads PAK/CONFIG keys from in the .ini. */
#define QUAKE_CONFIG_SECTION "quake"

/* Quake.json binds CD audio tracks 2..11 to APF data slots 22..31, so
 * slot = track + 20.  The kernel exposes each bound slot in the virtual
 * root "/" with d_ino == slot + 1 (see apps/slotdemo). */
#define CD_SLOT_FOR_TRACK(t)  ((int)(t) + 20)

/* ---- CDDA source format (Red Book) -------------------------------- */
#define CD_SRC_RATE      44100
#define CD_SRC_CHANNELS  2
#define CD_BYTES_PER_FRAME (CD_SRC_CHANNELS * (int)sizeof(int16_t))  /* 4 */

/* Ring depth, in source frames per channel.  Two seconds gives an
 * enormous refill margin while costing only ~345 KB of SDRAM total. */
#define CD_RING_SECONDS  2
#define CD_RING_FRAMES   (CD_SRC_RATE * CD_RING_SECONDS)

/* Staging buffer for one file read + deinterleave pass (interleaved
 * L,R frames).  Small, static, main-thread only. */
#define CD_STAGING_FRAMES 4096

/* Async (DMA) refill chunk, in interleaved frames.  16384 frames = 64 KB
 * per data-slot DMA read; at the 44100 Hz consume rate that is ~0.37 s of
 * audio per read, i.e. only ~3 reads/sec — the CPU just issues the DMA and
 * deinterleaves on completion, it never waits on the SD transfer. Bounded
 * at runtime by of_file_async_max_read(). */
#define CD_CHUNK_FRAMES   16384

/* Music voices sit high enough that nothing reclaims them, but the
 * group machinery keeps them away from SFX regardless. */
#define CD_MIXER_PRIORITY 200

/* bgmvolume is registered in snd_dma.c (0..1). */
extern cvar_t bgmvolume;

static int   cd_initialized;
static int   cd_enabled;       /* mixer + ring available */

static int16_t *cd_ringL;
static int16_t *cd_ringR;
static int16_t  cd_staging[CD_STAGING_FRAMES * CD_SRC_CHANNELS];

static FILE  *cd_file;
static long   cd_track_start;  /* byte offset of audio start in the file */
static qboolean cd_playing;
static qboolean cd_looping;
static qboolean cd_paused;
static qboolean cd_menu_quiet;   /* music frozen while a menu is open */
static uint32_t cd_quiet_until;  /* ms deadline: hold the freeze after slot writes */

/* Track start requested while the bridge must stay quiet (level load,
 * menu open, post-save hold).  CDAudio_Update starts it once clear. */
static byte     cd_deferred_track;
static qboolean cd_deferred_looping;
static qboolean cd_deferred_valid;
static qboolean cd_ended;      /* non-looping track hit EOF */
static int    cd_silence_run;  /* frames of trailing silence emitted */
static byte   cd_track;

static of_mixer_handle_t cd_vL = OF_MIXER_HANDLE_INVALID;
static of_mixer_handle_t cd_vR = OF_MIXER_HANDLE_INVALID;

static int    cd_write_pos;    /* next ring frame to fill (producer) */
static int    cd_last_pos;     /* play cursor at previous refill */
static int    cd_last_vol;     /* last group volume pushed (0..255) */

/* ---- async (DMA) refill state ------------------------------------- */
static int       cd_async_enable = 1;  /* [quake] CD_ASYNC (default on) */
static int       cd_async_ok;          /* async usable for current track */
static int       cd_slot = -1;         /* data slot of current track */
static long      cd_file_size;         /* track length in bytes */
static long      cd_read_offset;       /* next byte offset to DMA from track */
static uint8_t  *cd_stage_dma;         /* CRAM0 staging for of_file_read_async */
static int       cd_chunk_frames;      /* frames per DMA read (<= CD_CHUNK_FRAMES) */
static int       cd_async_pending;     /* a DMA read is in flight */
static int       cd_async_frames;      /* frames the in-flight read delivers */
static int       cd_ring_valid;        /* produced frames ahead of play cursor */
static volatile int cd_async_done;     /* set by the completion IRQ callback */
static volatile int cd_async_result;   /* IRQ result: 0 ok, <0 error */

/* ---- track file resolution ---------------------------------------- *
 *
 * Files are not opened by guessed paths.  The launcher's Quake.json
 * binds each track to a data slot (track 2..11 -> slot 22..31) and the
 * kernel exposes every bound slot in the virtual root "/", where readdir
 * reports the file under d_name with d_ino == slot+1 (see apps/slotdemo,
 * apps/freadalign).  So the robust open is: enumerate "/", find the
 * entry whose slot id matches this track, and fopen() the exact d_name
 * the kernel hands back -- independent of how the cue/bin file is spelled
 * (spaces, parens, subfolder).
 *
 * Fallbacks, in case the slot mapping differs: fopen("slot:N") directly,
 * then a few literal name spellings / an optional [quake] "CDFMT".
 */
static const char *cd_path_formats[] = {
    "Quake (USA) (Track %02d).bin",
    "cd/Quake (USA) (Track %02d).bin",
    "track%02d.bin",
    "cd/track%02d.bin",
    "music/track%02d.bin",
};
static char cd_fmt_override[MAX_OSPATH];
static int  cd_fmt_override_have;
static int  cd_root_dumped;

/* One-shot dump of everything the launcher exposed, so a failed open is
 * diagnosable from the console: which slots exist and their real names. */
static void CDAudio_DumpRoot(void)
{
    DIR *d;
    struct dirent *e;

    if (cd_root_dumped)
        return;
    cd_root_dumped = 1;

    d = opendir("/");
    if (!d) {
        Con_Printf("CDAudio: opendir(\"/\") failed\n");
        return;
    }
    Con_Printf("CDAudio: files exposed to the core:\n");
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        long sz = (stat(e->d_name, &st) == 0) ? (long)st.st_size : -1;
        Con_Printf("  slot %2d  %-28s %9ld\n",
                   (int)e->d_ino - 1, e->d_name, sz);
    }
    closedir(d);
}

/* Enumerate "/" and open the file bound to `slot`, by its real d_name. */
static FILE *CDAudio_OpenSlotByDir(int slot)
{
    DIR *d;
    struct dirent *e;
    FILE *f = NULL;

    if (slot < 0)
        return NULL;
    d = opendir("/");
    if (!d)
        return NULL;
    while ((e = readdir(d)) != NULL) {
        if ((int)e->d_ino - 1 == slot) {
            f = fopen(e->d_name, "rb");
            if (f)
                Con_DPrintf("CDAudio: track via slot %d -> \"%s\"\n",
                            slot, e->d_name);
            break;
        }
    }
    closedir(d);
    return f;
}

static FILE *CDAudio_OpenTrack(byte track)
{
    char path[MAX_OSPATH];
    int  slot = CD_SLOT_FOR_TRACK(track);
    FILE *f;

    /* Primary: match the bound slot in the virtual root and open its
     * real name. */
    f = CDAudio_OpenSlotByDir(slot);
    if (f)
        return f;

    /* Fallback 1: open the slot directly (works the way PAK slots do). */
    if (slot >= 0) {
        snprintf(path, sizeof(path), "slot:%d", slot);
        f = fopen(path, "rb");
        if (f) {
            Con_DPrintf("CDAudio: track %d -> %s\n", (int)track, path);
            return f;
        }
    }

    /* Fallback 2: optional [quake] CDFMT override, then literal names. */
    if (!cd_fmt_override_have) {
        cd_fmt_override[0] = 0;
        if (of_config_get(QUAKE_CONFIG_SECTION, "CDFMT",
                          cd_fmt_override, sizeof(cd_fmt_override)) != 0)
            cd_fmt_override[0] = 0;
        cd_fmt_override_have = 1;
    }
    if (cd_fmt_override[0]) {
        snprintf(path, sizeof(path), cd_fmt_override, (int)track);
        if ((f = fopen(path, "rb")) != NULL)
            return f;
    }
    for (int i = 0; i < (int)(sizeof(cd_path_formats) / sizeof(cd_path_formats[0])); i++) {
        snprintf(path, sizeof(path), cd_path_formats[i], (int)track);
        if ((f = fopen(path, "rb")) != NULL) {
            Con_DPrintf("CDAudio: opened \"%s\"\n", path);
            return f;
        }
    }

    /* A game with no soundtrack installed asks for a new track on every
     * level change, so say this once and then keep quiet -- nothing about
     * the situation changes, and the root dump below is the useful part. */
    {
        static int warned;

        if (!warned) {
            warned = 1;
            Con_Printf("CDAudio: track %d (slot %d) not found on card "
                       "(no soundtrack bound for this game)\n",
                       (int)track, slot);
            CDAudio_DumpRoot();
        } else {
            Con_DPrintf("CDAudio: track %d (slot %d) not found\n",
                        (int)track, slot);
        }
    }
    return NULL;
}

/* Read `count` interleaved frames into cd_staging, honouring loop/EOF.
 * Always fills `count` frames (zero-padding past EOF on a non-looping
 * track) so the ring producer never stalls. */
static void CDAudio_ReadFrames(int count)
{
    int got = 0;

    while (got < count) {
        size_t want = (size_t)(count - got);
        size_t n = fread(&cd_staging[got * CD_SRC_CHANNELS], CD_BYTES_PER_FRAME,
                         want, cd_file);
        got += (int)n;

        if ((int)n < (int)want) {           /* short read => EOF */
            if (cd_looping) {
                fseek(cd_file, cd_track_start, SEEK_SET);
                if (n == 0 && got == 0) {
                    /* defensive: empty/short file, avoid spin */
                    memset(cd_staging, 0, (size_t)count * CD_BYTES_PER_FRAME);
                    return;
                }
            } else {
                cd_ended = true;
                memset(&cd_staging[got * CD_SRC_CHANNELS], 0,
                       (size_t)(count - got) * CD_BYTES_PER_FRAME);
                return;
            }
        }
    }
}

/* Produce `nframes` of audio into the ring at the write cursor. */
static void CDAudio_Produce(int nframes)
{
    while (nframes > 0) {
        int w = cd_write_pos;
        int contig = CD_RING_FRAMES - w;          /* up to ring end */
        int batch = nframes;
        if (batch > contig)            batch = contig;
        if (batch > CD_STAGING_FRAMES) batch = CD_STAGING_FRAMES;

        CDAudio_ReadFrames(batch);

        for (int i = 0; i < batch; i++) {
            cd_ringL[w + i] = cd_staging[i * 2 + 0];
            cd_ringR[w + i] = cd_staging[i * 2 + 1];
        }

        of_cache_flush_range(&cd_ringL[w], (uint32_t)batch * sizeof(int16_t));
        of_cache_flush_range(&cd_ringR[w], (uint32_t)batch * sizeof(int16_t));

        if (w == 0) {           /* keep the loop-seam guard in sync */
            cd_ringL[CD_RING_FRAMES] = cd_ringL[0];
            cd_ringR[CD_RING_FRAMES] = cd_ringR[0];
            of_cache_flush_range(&cd_ringL[CD_RING_FRAMES], sizeof(int16_t));
            of_cache_flush_range(&cd_ringR[CD_RING_FRAMES], sizeof(int16_t));
        }

        if (cd_ended)
            cd_silence_run += batch;
        else
            cd_silence_run = 0;

        cd_write_pos = (w + batch) % CD_RING_FRAMES;
        nframes -= batch;
    }
}

/* ---- async (DMA) refill ------------------------------------------- *
 *
 * The synchronous path above blocks the CPU on each SD read (~0.8 us per
 * byte of audio = ~13% of all CPU just to stream 176 KB/s).  The async
 * path hands the transfer to the data-slot DMA engine: we issue a read
 * and return, the bytes land in CRAM0 in the background, and a completion
 * IRQ sets cd_async_done.  Per frame the CPU only deinterleaves an already
 * finished chunk and issues the next one — the SD transfer costs it
 * nothing.  Only one read may be in flight at a time (bridge limit), so we
 * keep the ring deep (CD_RING_SECONDS) and refill a chunk whenever the
 * play cursor has freed up that much space. */

static void cd_async_cb(int token, int result)
{
    (void)token;
    cd_async_result = result;
    cd_async_done = 1;
}

/* Resolve the data slot id + byte size for a track via the virtual root. */
static int CDAudio_ResolveSlot(byte track, long *size_out)
{
    int want = CD_SLOT_FOR_TRACK(track);
    DIR *d;
    struct dirent *e;
    int found = -1;

    *size_out = -1;
    d = opendir("/");
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if ((int)e->d_ino - 1 == want) {
            struct stat st;
            if (stat(e->d_name, &st) == 0)
                *size_out = (long)st.st_size;
            found = want;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Deinterleave `frames` of finished CRAM0 staging into the ring at the
 * write cursor (wrapping), flushing each written span for the mixer DMA. */
static void cd_stage_to_ring(int frames)
{
    const int16_t *src = (const int16_t *)cd_stage_dma;
    int done = 0;

    while (done < frames) {
        int w = cd_write_pos;
        int contig = CD_RING_FRAMES - w;
        int batch = frames - done;
        if (batch > contig)
            batch = contig;

        for (int i = 0; i < batch; i++) {
            cd_ringL[w + i] = src[(done + i) * 2 + 0];
            cd_ringR[w + i] = src[(done + i) * 2 + 1];
        }
        of_cache_flush_range(&cd_ringL[w], (uint32_t)batch * sizeof(int16_t));
        of_cache_flush_range(&cd_ringR[w], (uint32_t)batch * sizeof(int16_t));

        if (w == 0) {           /* keep the loop-seam guard in sync */
            cd_ringL[CD_RING_FRAMES] = cd_ringL[0];
            cd_ringR[CD_RING_FRAMES] = cd_ringR[0];
            of_cache_flush_range(&cd_ringL[CD_RING_FRAMES], sizeof(int16_t));
            of_cache_flush_range(&cd_ringR[CD_RING_FRAMES], sizeof(int16_t));
        }

        cd_write_pos = (w + batch) % CD_RING_FRAMES;
        done += batch;
    }
}

#ifndef OF_PC
/* Kick off a DMA read of up to cd_chunk_frames at cd_read_offset, honouring
 * loop/EOF.  Non-blocking: sets cd_async_pending and pre-advances the read
 * offset.  Returns 0 if a read was issued, <0 on EOF (non-loop) or error. */
static int cd_async_issue(void)
{
    long remaining = cd_file_size - cd_read_offset;
    int  frames;
    int  tok;

    if (remaining <= 0) {
        if (!cd_looping) { cd_ended = true; return -1; }
        cd_read_offset = cd_track_start;
        remaining = cd_file_size - cd_read_offset;
        if (remaining <= 0) return -1;
    }

    frames = cd_chunk_frames;
    if ((long)frames * CD_BYTES_PER_FRAME > remaining)
        frames = (int)(remaining / CD_BYTES_PER_FRAME);
    if (frames <= 0)
        return -1;

    cd_async_done = 0;
    cd_async_result = -1;
    tok = of_file_read_async(cd_slot, (uint32_t)cd_read_offset, cd_stage_dma,
                             (uint32_t)(frames * CD_BYTES_PER_FRAME),
                             cd_async_cb);
    if (tok < 0) {
        cd_async_ok = 0;          /* drop to the sync path on the next frame */
        return tok;
    }
    cd_async_pending = 1;
    cd_async_frames  = frames;
    cd_read_offset  += (long)frames * CD_BYTES_PER_FRAME;
    return 0;
}

/* Block until the in-flight read finishes (used only at prefill / load,
 * where a stall is acceptable).  Returns frames delivered, 0 on failure. */
static int cd_async_wait(void)
{
    uint32_t start = of_time_ms();
    int frames = cd_async_frames;

    while (!cd_async_done) {
        of_file_async_poll();
        if (!cd_async_done && !of_file_async_busy())
            break;
        if ((uint32_t)(of_time_ms() - start) >= 2000u)
            break;
    }
    cd_async_pending = 0;
    return (cd_async_done && cd_async_result >= 0) ? frames : 0;
}
#else  /* OF_PC — no data-slot DMA; the sync fread path is always used */
static int cd_async_issue(void) { cd_async_ok = 0; return -1; }
static int cd_async_wait(void)  { cd_async_pending = 0; return 0; }
#endif /* OF_PC */

static int CDAudio_GroupVolume(void)
{
    float v = bgmvolume.value;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    return (int)(v * 255.0f + 0.5f);
}

static void CDAudio_StopVoices(void)
{
    if (cd_vL != OF_MIXER_HANDLE_INVALID) { of_mixer_stop_h(cd_vL); cd_vL = OF_MIXER_HANDLE_INVALID; }
    if (cd_vR != OF_MIXER_HANDLE_INVALID) { of_mixer_stop_h(cd_vR); cd_vR = OF_MIXER_HANDLE_INVALID; }
}

/* True while music must not touch the data-slot bridge: mid level-load
 * (svc_cdtrack arrives while the loader is still streaming pak data),
 * menu open, or within a post-save/load quiet hold. */
static int CDAudio_MustStayQuiet(void)
{
    extern qboolean scr_disabled_for_loading;

    if (scr_disabled_for_loading)
        return 1;
    if (key_dest == key_menu)
        return 1;
    return cd_quiet_until != 0 &&
           (int32_t)(of_time_ms() - cd_quiet_until) < 0;
}

void CDAudio_Play(byte track, qboolean looping)
{
    if (!cd_enabled)
        return;

    /* Starting a track is heavy: slot resolve, track open and a 345 KB
     * ring prefill through the data-slot bridge.  Mid-load / in-menu /
     * during a quiet hold that contends with the loader's own bridge
     * traffic (the intermittent load-game hang), so defer the start —
     * CDAudio_Update fires it once the world is up and the hold passed. */
    if (track >= 2 && CDAudio_MustStayQuiet()) {
        cd_deferred_track   = track;
        cd_deferred_looping = looping;
        cd_deferred_valid   = true;
        return;
    }
    cd_deferred_valid = false;   /* a direct play supersedes any deferral */

    cd_menu_quiet = false;

    /* Track 1 is the data track; only 2..N carry audio. */
    if (track < 2) {
        CDAudio_Stop();
        return;
    }

    if (cd_playing && track == cd_track && looping == cd_looping)
        return;   /* already playing this one */

    CDAudio_Stop();

    cd_file = CDAudio_OpenTrack(track);
    if (!cd_file)
        return;

    cd_track       = track;
    cd_looping     = looping;
    cd_paused      = false;
    cd_ended       = false;
    cd_silence_run = 0;
    cd_track_start = 0;    /* NOTE: track 2 carries a 2 s pregap; see README */
    fseek(cd_file, cd_track_start, SEEK_SET);

    /* Decide the steady-state refill path.  Prefill stays synchronous
     * (one-time at load); only the per-frame top-up goes async DMA. */
    cd_async_ok = 0;
    cd_async_pending = 0;
#ifndef OF_PC
    if (cd_async_enable && cd_stage_dma) {
        long sz = -1;
        int slot = CDAudio_ResolveSlot(track, &sz);
        if (slot >= 0 && sz > 0) {
            cd_slot = slot;
            cd_file_size = sz;
            cd_async_ok = 1;
        }
    }
#endif

    /* Pre-fill the entire ring before the voices start. */
    cd_write_pos = 0;
    CDAudio_Produce(CD_RING_FRAMES);   /* wraps write_pos back to 0 */
    cd_write_pos = 0;
    cd_last_pos  = 0;
    cd_read_offset = ftell(cd_file);   /* async resumes here */
    cd_ring_valid  = CD_RING_FRAMES;   /* ring is full after prefill */

    cd_vL = of_mixer_alloc_for_group_h(OF_MIXER_GROUP_MUSIC,
                                       (const uint8_t *)cd_ringL,
                                       CD_RING_FRAMES, CD_SRC_RATE,
                                       CD_MIXER_PRIORITY, 0);
    cd_vR = of_mixer_alloc_for_group_h(OF_MIXER_GROUP_MUSIC,
                                       (const uint8_t *)cd_ringR,
                                       CD_RING_FRAMES, CD_SRC_RATE,
                                       CD_MIXER_PRIORITY, 0);
    if (cd_vL == OF_MIXER_HANDLE_INVALID || cd_vR == OF_MIXER_HANDLE_INVALID) {
        Con_Printf("CDAudio: no free music voices\n");
        CDAudio_StopVoices();
        fclose(cd_file);
        cd_file = NULL;
        return;
    }

    of_mixer_set_loop_h(cd_vL, 0, CD_RING_FRAMES);
    of_mixer_set_loop_h(cd_vR, 0, CD_RING_FRAMES);

    /* Hard-pan: left voice all-left, right voice all-right.  bgmvolume
     * is applied via the MUSIC group volume so it scales both at once. */
    of_mixer_set_vol_lr_h(cd_vL, 255, 0);
    of_mixer_set_vol_lr_h(cd_vR, 0, 255);

    cd_last_vol = CDAudio_GroupVolume();
    of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, cd_last_vol);

    Con_DPrintf("CDAudio: track %d (%s refill)\n",
                (int)track, cd_async_ok ? "async DMA" : "sync");

    cd_playing = true;
}

void CDAudio_Stop(void)
{
    if (!cd_enabled)
        return;

    cd_deferred_valid = false;   /* a stop cancels any deferred start */

    /* Let any in-flight DMA finish so the single-slot bridge is free for
     * the next track. */
    if (cd_async_pending)
        cd_async_wait();
    cd_async_ok = 0;

    CDAudio_StopVoices();
    if (cd_file) {
        fclose(cd_file);
        cd_file = NULL;
    }
    cd_playing = false;
    cd_paused  = false;
}

void CDAudio_Pause(void)
{
    if (!cd_enabled || !cd_playing || cd_paused)
        return;
    /* Keep streaming (so position tracking stays valid and resume is
     * seamless) but mute the group. */
    of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, 0);
    cd_paused = true;
}

void CDAudio_Resume(void)
{
    if (!cd_enabled || !cd_playing || !cd_paused)
        return;
    cd_last_vol = CDAudio_GroupVolume();
    of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, cd_last_vol);
    cd_paused = false;
}

/* Free the single data-slot DMA bridge for the engine's own (blocking)
 * file reads.  The bridge serves one slot transfer at a time, so a blocking
 * fread issued while a CD read is in flight would hang it (= game freeze).
 * The engine's file primitives call this first; we wait for the in-flight
 * CD read to finish (bounded), but leave it pending for CDAudio_Update to
 * deinterleave.  Cheap no-op when nothing is in flight. */
void CDAudio_DrainAsync(void)
{
#ifndef OF_PC
    uint32_t start;

    if (!cd_async_pending || cd_async_done)
        return;                 /* idle, or already finished and queued */

    start = of_time_ms();
    while (!cd_async_done) {
        of_file_async_poll();           /* drive completion (IRQ also sets it) */
        if (cd_async_done)
            break;
        if ((uint32_t)(of_time_ms() - start) >= 200u) {
            cd_async_ok = 0;    /* read wedged — fall back to sync refill */
            break;
        }
    }
#endif
}

/* Keep music frozen (no voice consumption, no refill DMA) for at least
 * `ms` more milliseconds.  Called around save-slot writes: the kernel /
 * SD card may still be flushing the written data after fclose returns,
 * and a music DMA read issued into that window can wedge the single-user
 * data-slot bridge.  Music resumes seamlessly when the hold expires. */
void CDAudio_PostponeResume(int ms)
{
    uint32_t until = of_time_ms() + (uint32_t)ms;

    if (cd_quiet_until == 0 || (int32_t)(until - cd_quiet_until) > 0)
        cd_quiet_until = until ? until : 1;
}

/* Called after writing a save/config slot.  The kernel's nonvolatile
 * slot writeback can keep the data-slot DMA engine busy for an
 * unbounded time afterwards, and an of_file_read_async issued into
 * that state blocks inside the ecall — a hard freeze a few seconds
 * after saving, when the ring wants its first post-save refill.
 * Switch the refill to the synchronous fread path (plain file
 * syscalls, no DMA engine) for the remainder of this track; the next
 * track open re-evaluates async support from scratch. */
void CDAudio_NotifySlotWrite(void)
{
    if (!cd_enabled)
        return;

    CDAudio_DrainAsync();           /* retire anything already in flight */
    cd_async_pending = 0;

    if (cd_async_ok && cd_file)
        fseek(cd_file, cd_read_offset, SEEK_SET);  /* sync resumes here */
    cd_async_ok = 0;
}

void CDAudio_Update(void)
{
    int pos, consumed, vol;

    if (!cd_enabled)
        return;

    if (CDAudio_MustStayQuiet()) {
        /* Silence music while a menu is up, a level loads, or a quiet
         * hold is pending.  A mute is not enough: the streamer would
         * keep issuing data-slot DMA reads, which contend with save /
         * loader file I/O on the single-user bridge.  Freezing the
         * voice rate stops consumption, which stops refills — the
         * bridge stays idle and the stream resumes where it left off. */
        if (cd_playing && !cd_menu_quiet) {
            CDAudio_DrainAsync();           /* retire any in-flight read */
            of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, 0);
            of_mixer_set_rate_h(cd_vL, 0);  /* freeze play cursors */
            of_mixer_set_rate_h(cd_vR, 0);
            cd_menu_quiet = true;
        }
        return;                             /* no refills while quiet */
    }
    cd_quiet_until = 0;

    /* All clear — start a track that was requested while quiet. */
    if (cd_deferred_valid) {
        byte     t = cd_deferred_track;
        qboolean l = cd_deferred_looping;

        cd_deferred_valid = false;
        CDAudio_Play(t, l);
    }

    if (!cd_playing)
        return;
    if (cd_menu_quiet) {
        of_mixer_set_rate_h(cd_vL, CD_SRC_RATE);
        of_mixer_set_rate_h(cd_vR, CD_SRC_RATE);
        if (!cd_paused) {
            cd_last_vol = CDAudio_GroupVolume();
            of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, cd_last_vol);
        }
        cd_menu_quiet = false;
    }

    /* Track bgmvolume changes (menu slider) without a per-sample cost. */
    if (!cd_paused) {
        vol = CDAudio_GroupVolume();
        if (vol != cd_last_vol) {
            of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, vol);
            cd_last_vol = vol;
        }
    }

    /* Async: fold in a finished DMA read before measuring the cursor. */
    if (cd_async_ok && cd_async_pending && cd_async_done) {
        if (cd_async_result >= 0) {
            cd_stage_to_ring(cd_async_frames);
            cd_ring_valid += cd_async_frames;
        }
        cd_async_pending = 0;
    }

    pos = of_mixer_get_position_h(cd_vL);
    if (pos < 0)
        return;

    consumed = (pos - cd_last_pos + CD_RING_FRAMES) % CD_RING_FRAMES;
    if (consumed >= CD_RING_FRAMES)     /* clamp pathological jumps */
        consumed = CD_RING_FRAMES - 1;
    cd_last_pos = pos;

    if (cd_async_ok) {
        cd_ring_valid -= consumed;
        if (cd_ring_valid < 0)
            cd_ring_valid = 0;          /* underrun guard */

        /* Issue the next DMA read once the cursor has freed a whole chunk
         * and nothing is in flight (the bridge allows only one). */
        if (!cd_async_pending && !cd_ended &&
            CD_RING_FRAMES - cd_ring_valid >= cd_chunk_frames)
            cd_async_issue();

        /* If async dropped out mid-stream, resync the FILE* and continue
         * on the sync path next frame. */
        if (!cd_async_ok && cd_file)
            fseek(cd_file, cd_read_offset, SEEK_SET);

        /* Non-looping track: real audio exhausted and ring drained. */
        if (cd_ended && cd_ring_valid <= 0)
            CDAudio_Stop();
    } else {
        if (consumed > 0)
            CDAudio_Produce(consumed);

        /* Non-looping track fully drained (audio + a ring of silence). */
        if (cd_ended && cd_silence_run >= CD_RING_FRAMES)
            CDAudio_Stop();
    }
}

int CDAudio_Init(void)
{
    if (cd_initialized)
        return 0;
    cd_initialized = 1;

    /* +1 guard frame: the mixer's linear interpolator can fetch one
     * sample past loop_end at the ring seam (44100 source vs 48000
     * output -> fractional positions).  Mirror ring[0] there so the
     * interpolated seam sample is always continuous music instead of
     * stray heap bytes — that stray read is audible as an occasional
     * tiny click once per ring lap. */
    cd_ringL = (int16_t *)malloc((CD_RING_FRAMES + 1) * sizeof(int16_t));
    cd_ringR = (int16_t *)malloc((CD_RING_FRAMES + 1) * sizeof(int16_t));
    if (!cd_ringL || !cd_ringR) {
        free(cd_ringL); cd_ringL = NULL;
        free(cd_ringR); cd_ringR = NULL;
        Con_Printf("CDAudio: ring alloc failed, music disabled\n");
        return -1;
    }

    /* Optional async-DMA refill staging (CRAM0).  If unavailable, the
     * synchronous fread path is used (set [quake] CD_ASYNC=0 to force it). */
    cd_chunk_frames = CD_CHUNK_FRAMES;
#ifndef OF_PC
    {
        extern int Sys_IniGetInt(const char *key, int def);
        uint32_t maxr;
        cd_async_enable = Sys_IniGetInt("CD_ASYNC", 1);
        maxr = of_file_async_max_read();
        if (maxr > 0 && (uint32_t)(cd_chunk_frames * CD_BYTES_PER_FRAME) > maxr)
            cd_chunk_frames = (int)(maxr / CD_BYTES_PER_FRAME);
        if (cd_async_enable && cd_chunk_frames > 0)
            cd_stage_dma = (uint8_t *)of_file_dma_stage_alloc(
                (uint32_t)cd_chunk_frames * CD_BYTES_PER_FRAME, 2048);
    }
#endif

    cd_enabled = 1;
    Con_Printf("CD Audio Initialized (%s refill)\n",
               cd_stage_dma ? "async DMA" : "sync");
    return 0;
}

void CDAudio_Shutdown(void)
{
    CDAudio_Stop();
    cd_enabled = 0;
    /* Rings are kept allocated for the process lifetime (single app). */
}
