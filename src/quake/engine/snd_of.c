/*
 * snd_of.c -- Quake sound driver over the openfpgaOS SDK.
 *
 * With SND_USE_HW_MIXER enabled, Quake SFX play as hardware mixer voices
 * and this file only provides the small dma_t compatibility buffer and
 * clear hook the old sound code expects.  With the switch disabled,
 * Quake's software mixer paints signed-16 interleaved stereo samples into
 * a 16 K-pair ring and SNDDMA_Submit drains that ring to of_audio_write().
 *
 * A 1 kHz timer ISR was tried for frame-rate-independent drainage but
 * calling OF_SVC->audio_write / audio_get_free from interrupt context
 * is not reentrant-safe with main-thread calls and manifested as a
 * progressive freeze after a few frames (mirrors of_midi_pump's
 * "no ECALL from ISR" warning in of_midi.c). Keeping this path
 * synchronous with the frame for now — at 15 fps the 1024-pair FIFO
 * (~21 ms) does underrun during long frames, but correctness beats
 * audio smoothness. Fix properly by either growing the HW FIFO or by
 * restructuring the kernel audio service to be reentrant.
 */

#include "quakedef.h"
#include "of.h"
#include "of_audio.h"

#include <string.h>

#ifndef SND_USE_HW_MIXER
#define SND_USE_HW_MIXER 1
#endif

#define SND_RATE            OF_AUDIO_RATE     /* 48000 */
#define SND_CHANNELS        2
#define SND_BITS            16
#if SND_USE_HW_MIXER
#define SND_BUFFER_SAMPLES  64
#else
/* 16 K stereo pairs ≈ 340 ms — engine has plenty of room to paint ahead
 * of whatever the FIFO can hold. Must be a power of two. */
#define SND_BUFFER_SAMPLES  16384
#endif

/* Interleaved L,R s16 — shm->buffer points here. */
static int16_t snd_buffer[SND_BUFFER_SAMPLES * SND_CHANNELS]
    __attribute__((aligned(4)));

/* Submission cursor (in stereo pairs), written + read only from the
 * main thread. */
static int submit_src_pos;

extern int paintedtime;                /* sample PAIRS, written by mixer */
extern volatile dma_t *shm;
extern volatile dma_t sn;

static volatile dma_t *const sn_state = &sn;

/* Drain as many painted pairs as will fit into the FIFO right now.
 * Called from SNDDMA_Submit (per-frame). */
static void audio_drain(void)
{
    const int mask = SND_BUFFER_SAMPLES - 1;
    int submit = submit_src_pos;
    int painted = paintedtime;
    while (submit < painted) {
        int before_wrap = SND_BUFFER_SAMPLES - (submit & mask);
        int chunk = painted - submit;
        if (chunk > before_wrap) chunk = before_wrap;

        int free_space = of_audio_free();
        if (free_space <= 0) break;
        if (chunk > free_space) chunk = free_space;

        int written = of_audio_write(
            &snd_buffer[(submit & mask) * SND_CHANNELS], chunk);
        if (written <= 0) break;
        submit += written;
    }
    submit_src_pos = submit;
}

qboolean SNDDMA_Init(void)
{
    of_audio_init();

    shm = sn_state;
    sn.channels = SND_CHANNELS;
    sn.samplebits = SND_BITS;
    sn.speed = SND_RATE;
    sn.samples = SND_BUFFER_SAMPLES * SND_CHANNELS; /* engine expects mono-sample count */
    sn.submission_chunk = 1;
    sn.samplepos = 0;
    sn.buffer = (unsigned char *)snd_buffer;
    sn.soundalive = true;
    sn.gamealive = true;
    sn.splitbuffer = false;

    paintedtime     = 0;
    submit_src_pos  = 0;
    memset(snd_buffer, 0, sizeof(snd_buffer));
    return true;
}

int SNDDMA_GetDMAPos(void)
{
    shm->samplepos = (submit_src_pos * SND_CHANNELS) & (shm->samples - 1);
    return shm->samplepos;
}

void SNDDMA_Submit(void)
{
    audio_drain();
}

void SNDDMA_Shutdown(void) { }

void SNDDMA_ClearBuffer(void)
{
    submit_src_pos = paintedtime;
    memset(snd_buffer, 0, sizeof(snd_buffer));
    of_audio_init();
}

/* The engine calls SNDDMA_FillRing from inside R_EdgeDrawing
 * (r_main.c:1512, :1540) — that's two extra drain points per frame
 * inside the render loop, which keeps the FIFO topped up when frame
 * times are long (15 fps frame ≈ 66 ms vs 21 ms FIFO). Main-thread
 * only, no ISR involvement, so audio_drain is safe here. */
int audio_timer_active = 0;
void Audio_TimerStart(void) { }
void SNDDMA_FillRing(void)  { audio_drain(); }
