/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// snd_mix.c -- portable sound mixing for PocketQuake

#include "quakedef.h"

#define PAINTBUFFER_SIZE 512

portable_samplepair_t paintbuffer[PAINTBUFFER_SIZE];

void SND_PaintChannelFrom16(channel_t *ch, sfxcache_t *sc, int count);

void SND_InitScaletable(void)
{
}

static void S_TransferPaintBuffer(int count)
{
    int out_idx;
    int out_mask;
    int *p;
    int step;
    int val;
    short *out;

    p = (int *)paintbuffer;
    count *= shm->channels;
    out_mask = shm->samples - 1;
    out_idx = paintedtime * shm->channels & out_mask;
    step = 3 - shm->channels;

    // 16-bit output
    out = (short *)shm->buffer;
    while (count--) {
        val = *p >> 8;
        p += step;
        if (val > 0x7fff)
            val = 0x7fff;
        else if (val < (short)0x8000)
            val = (short)0x8000;
        out[out_idx] = val;
        out_idx = (out_idx + 1) & out_mask;
    }
}

void S_PaintChannels(int endtime)
{
    int i;
    int end;
    channel_t *ch;
    sfxcache_t *sc;
    int ltime, count;

    while (paintedtime < endtime) {
        // If paintbuffer is smaller than DMA buffer
        end = endtime;
        if (endtime - paintedtime > PAINTBUFFER_SIZE)
            end = paintedtime + PAINTBUFFER_SIZE;

        // Clear the paint buffer
        memset(paintbuffer, 0, (end - paintedtime) * sizeof(portable_samplepair_t));

        // Paint in the channels
        ch = channels;
        for (i = 0; i < total_channels; i++, ch++) {
            if (!ch->sfx)
                continue;
            if (!ch->leftvol && !ch->rightvol)
                continue;

            sc = S_LoadSound(ch->sfx);
            if (!sc)
                continue;

            ltime = paintedtime;

            while (ltime < end) {
                // Paint up to end
                if (ch->end < end)
                    count = ch->end - ltime;
                else
                    count = end - ltime;

                if (count > 0) {
                    SND_PaintChannelFrom16(ch, sc, count);
                    ltime += count;
                }

                // If at end of loop, restart
                if (ltime >= ch->end) {
                    if (sc->loopstart >= 0) {
                        ch->pos = sc->loopstart;
                        ch->end = ltime + sc->length - sc->loopstart;
                    } else {
                        // Channel just stopped
                        ch->sfx = NULL;
                        break;
                    }
                }
            }
        }

        // Transfer to DMA buffer
        S_TransferPaintBuffer(end - paintedtime);

        paintedtime = end;
    }
}

// Short fade-out length (samples at 11025 Hz) to prevent clicks when SFX ends.
// 16 samples = ~1.5ms — fast enough to be inaudible as a fade, but removes the
// hard step to zero that causes a click in the Pocket's audio output.
#define SFX_FADEOUT 16

void SND_PaintChannelFrom16(channel_t *ch, sfxcache_t *sc, int count)
{
    short *sfx;
    int i;
    int leftvol = ch->leftvol;
    int rightvol = ch->rightvol;

    if (leftvol > 255) leftvol = 255;
    if (rightvol > 255) rightvol = 255;

    sfx = (short *)sc->data + ch->pos;

    // Check if this chunk reaches the end of a non-looping sound
    int samples_left = ch->end - (int)(paintedtime + ch->pos - ch->pos);
    int fade_start = -1;
    if (sc->loopstart < 0) {
        // Non-looping: apply fade on the last SFX_FADEOUT samples before ch->end
        int end_pos = sc->length;
        int remaining = end_pos - ch->pos;
        if (remaining <= count + SFX_FADEOUT) {
            fade_start = remaining - SFX_FADEOUT;
            if (fade_start < 0) fade_start = 0;
        }
    }

    for (i = 0; i < count; i++) {
        int s = sfx[i];
        // Apply fade-out envelope near end of non-looping sound
        if (fade_start >= 0 && (ch->pos + i) >= (sc->length - SFX_FADEOUT)) {
            int samples_to_end = sc->length - (ch->pos + i);
            if (samples_to_end <= 0)
                s = 0;
            else
                s = s * samples_to_end / SFX_FADEOUT;
        }
        paintbuffer[i].left += s * leftvol;
        paintbuffer[i].right += s * rightvol;
    }

    ch->pos += count;
}

void S_InitPaintChannels(void)
{
}
