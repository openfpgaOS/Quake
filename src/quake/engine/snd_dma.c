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
// snd_dma.c -- Quake sound engine

#include "quakedef.h"
#include "of_caps.h"
#include "of_mixer.h"

#ifndef SND_USE_HW_MIXER
#define SND_USE_HW_MIXER 1
#endif

void S_PaintChannels(int endtime);
void SND_InitScaletable(void);
void SNDDMA_Submit(void);
void SNDDMA_FillRing(void);
void SNDDMA_ClearBuffer(void);

// ====================================================================
// Globals
// ====================================================================

channel_t   channels[MAX_CHANNELS];
int         total_channels;

#if SND_USE_HW_MIXER
static of_mixer_handle_t channel_mixer_handles[MAX_CHANNELS];
static int               channel_mixer_start_time[MAX_CHANNELS];
static int               channel_mixer_start_pos[MAX_CHANNELS];
static int               channel_mixer_length[MAX_CHANNELS];
static double            hw_sound_sample_accum;
#endif

int         snd_blocked = 0;
qboolean    snd_initialized = false;

volatile dma_t *shm = NULL;
volatile dma_t sn;

vec3_t      listener_origin;
vec3_t      listener_forward;
vec3_t      listener_right;
vec3_t      listener_up;

int         paintedtime __attribute__((section(".fastdata")));    // sample PAIRS — in BRAM for ISR access
static int  soundtime;      // position in mono samples that hardware has played
static int  buffers;
static int  oldsamplepos;

int         s_rawend;

static qboolean sound_started = false;

cvar_t bgmvolume = {"bgmvolume", "0.9", true};
cvar_t volume = {"volume", "0.75", true};
cvar_t nosound = {"nosound", "0"};
cvar_t precache = {"precache", "1"};
cvar_t ambient_level = {"ambient_level", "0"};
cvar_t ambient_fade = {"ambient_fade", "100"};
cvar_t _snd_mixahead = {"_snd_mixahead", "0.15"};

// ====================================================================
// Known SFX list
// ====================================================================

#define MAX_SFX 512
static sfx_t known_sfx[MAX_SFX];
static int num_sfx;

static sfx_t *ambient_sfx[NUM_AMBIENTS];

// ====================================================================
// Internal functions
// ====================================================================

#if SND_USE_HW_MIXER
static int S_MixerClampVolume(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static qboolean S_MixerChannelLoops(int idx)
{
    return idx < NUM_AMBIENTS ||
           idx >= NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS;
}

static int S_MixerChannelPriority(int idx)
{
    if (idx >= NUM_AMBIENTS && idx < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS)
        return 100;
    if (idx < NUM_AMBIENTS)
        return 20;
    return 10;
}

static int S_MixerChannelGroup(int idx)
{
    if (idx >= NUM_AMBIENTS && idx < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS)
        return OF_MIXER_GROUP_SFX;
    return OF_MIXER_GROUP_AUX;
}

static void S_MixerResetTime(void)
{
    hw_sound_sample_accum = 0.0;
    soundtime = 0;
    paintedtime = 0;
    buffers = 0;
    oldsamplepos = 0;
}

static void S_MixerAdvanceTime(void)
{
    if (!shm)
        return;
    hw_sound_sample_accum += (double)host_frametime * (double)shm->speed;
    soundtime = (int)hw_sound_sample_accum;
    paintedtime = soundtime;
}

static void S_MixerStopChannel(int idx)
{
    if (idx < 0 || idx >= MAX_CHANNELS)
        return;
    if (channel_mixer_handles[idx] != OF_MIXER_HANDLE_INVALID) {
        of_mixer_stop_h(channel_mixer_handles[idx]);
        channel_mixer_handles[idx] = OF_MIXER_HANDLE_INVALID;
    }
    channel_mixer_start_time[idx] = 0;
    channel_mixer_start_pos[idx] = 0;
    channel_mixer_length[idx] = 0;
}

static void S_MixerStopAllChannels(void)
{
    for (int i = 0; i < MAX_CHANNELS; i++)
        S_MixerStopChannel(i);
}

static const byte *S_MixerPCM(sfx_t *sfx, sfxcache_t *sc)
{
    if (sfx && sfx->mixer_data)
        return sfx->mixer_data;
    return sc ? sc->data : NULL;
}

static qboolean S_MixerStartChannel(int idx)
{
    channel_t *ch;
    sfxcache_t *sc;
    const byte *pcm;
    of_mixer_handle_t handle;
    uint32_t mixer_length;
    uint32_t mixer_rate;
    int mixer_loopstart;
    int mixer_pos;

    if (idx < 0 || idx >= MAX_CHANNELS)
        return false;

    ch = &channels[idx];
    if (!ch->sfx)
        return false;

    sc = S_LoadSound(ch->sfx);
    if (!sc)
        return false;

    pcm = S_MixerPCM(ch->sfx, sc);
    if (!pcm)
        return false;

    mixer_length = ch->sfx->mixer_data ?
        (uint32_t)ch->sfx->mixer_length : (uint32_t)sc->length;
    mixer_rate = ch->sfx->mixer_data ?
        (uint32_t)ch->sfx->mixer_speed : (uint32_t)sc->speed;
    mixer_loopstart = ch->sfx->mixer_data ?
        ch->sfx->mixer_loopstart : sc->loopstart;
    if (mixer_length == 0 || mixer_rate == 0)
        return false;

    handle = of_mixer_alloc_for_group_h(S_MixerChannelGroup(idx),
                                        pcm,
                                        mixer_length,
                                        mixer_rate,
                                        S_MixerChannelPriority(idx),
                                        0);
    if (handle == OF_MIXER_HANDLE_INVALID)
        return false;

    channel_mixer_handles[idx] = handle;
    channel_mixer_start_time[idx] = paintedtime;
    channel_mixer_start_pos[idx] = ch->pos;
    channel_mixer_length[idx] = sc->length;

    if (S_MixerChannelLoops(idx) && mixer_loopstart >= 0)
        of_mixer_set_loop_h(handle, mixer_loopstart, (int)mixer_length);

    if (ch->pos > 0 && ch->pos < sc->length) {
        mixer_pos = ch->sfx->mixer_data ?
            (int)((long long)ch->pos * mixer_rate / shm->speed) : ch->pos;
        if (mixer_pos < 0)
            mixer_pos = 0;
        if ((uint32_t)mixer_pos >= mixer_length)
            mixer_pos = (int)mixer_length - 1;
        of_mixer_set_position_h(handle, mixer_pos);
    }

    return true;
}

static void S_MixerUpdateChannelPos(int idx)
{
    channel_t *ch;
    int pos;

    if (idx < 0 || idx >= MAX_CHANNELS)
        return;
    if (S_MixerChannelLoops(idx) || channel_mixer_length[idx] <= 0)
        return;

    ch = &channels[idx];
    pos = channel_mixer_start_pos[idx] +
          (paintedtime - channel_mixer_start_time[idx]);
    if (pos < 0)
        pos = 0;
    if (pos > channel_mixer_length[idx])
        pos = channel_mixer_length[idx];
    ch->pos = pos;
}

static void S_MixerApplyChannelVolume(int idx)
{
    channel_t *ch;

    if (idx < 0 || idx >= MAX_CHANNELS)
        return;
    if (channel_mixer_handles[idx] == OF_MIXER_HANDLE_INVALID)
        return;

    ch = &channels[idx];
    of_mixer_set_vol_lr_h(channel_mixer_handles[idx],
                          S_MixerClampVolume(ch->leftvol),
                          S_MixerClampVolume(ch->rightvol));
}

static void S_MixerSyncChannel(int idx)
{
    channel_t *ch;
    int left, right;
    qboolean looping;

    if (idx < 0 || idx >= MAX_CHANNELS)
        return;

    ch = &channels[idx];
    if (!ch->sfx) {
        S_MixerStopChannel(idx);
        return;
    }

    looping = S_MixerChannelLoops(idx);
    S_MixerUpdateChannelPos(idx);
    if (!looping && ch->end <= paintedtime) {
        S_MixerStopChannel(idx);
        ch->sfx = NULL;
        return;
    }

    left = S_MixerClampVolume(ch->leftvol);
    right = S_MixerClampVolume(ch->rightvol);
    if (looping && left == 0 && right == 0) {
        S_MixerStopChannel(idx);
        return;
    }

    if (channel_mixer_handles[idx] == OF_MIXER_HANDLE_INVALID &&
        !S_MixerStartChannel(idx))
        return;

    S_MixerApplyChannelVolume(idx);
}
#endif

sfx_t *S_FindName(char *name)
{
    int i;
    sfx_t *sfx;

    if (!name)
        Sys_Error("S_FindName: NULL");

    if (Q_strlen(name) >= MAX_QPATH)
        Sys_Error("Sound name too long: %s", name);

    // See if already loaded
    for (i = 0; i < num_sfx; i++) {
        if (!Q_strcmp(known_sfx[i].name, name))
            return &known_sfx[i];
    }

    if (num_sfx == MAX_SFX)
        Sys_Error("S_FindName: out of sfx_t");

    sfx = &known_sfx[num_sfx];
    if (sfx->cache.data)
        Cache_Free(&sfx->cache);
    if (sfx->mixer_data)
        free(sfx->mixer_data);
    memset(sfx, 0, sizeof(*sfx));
    Q_strcpy(sfx->name, name);
    num_sfx++;

    return sfx;
}

// ====================================================================
// Spatialization
// ====================================================================

void SND_Spatialize(channel_t *ch)
{
    vec_t dist;
    vec_t scale;
    vec3_t source_vec;

    // Anything coming from the view entity will always be full volume
    if (ch->entnum == cl.viewentity) {
        ch->leftvol = ch->rightvol = (int)(ch->master_vol * volume.value);
        return;
    }

    // Distance attenuation + stereo panning
    VectorSubtract(ch->origin, listener_origin, source_vec);

    dist = VectorNormalize(source_vec) * ch->dist_mult;

    scale = (1.0 - dist) * volume.value;
    if (scale < 0) scale = 0;

    // Stereo panning: dot product with listener right vector
    vec_t dot = source_vec[0] * listener_right[0]
              + source_vec[1] * listener_right[1]
              + source_vec[2] * listener_right[2];

    // dot = -1..1: center sounds are full volume in both channels,
    // hard-panned sounds reach 2x in one channel (matches original Quake)
    ch->rightvol = (int)(ch->master_vol * scale * (1.0 + dot));
    ch->leftvol  = (int)(ch->master_vol * scale * (1.0 - dot));
}

// ====================================================================
// Channel management
// ====================================================================

channel_t *SND_PickChannel(int entnum, int entchannel)
{
    int ch_idx;
    int first_to_die;
    int life_left;

    // Check for replacement sound, or find the best one to replace
    first_to_die = -1;
    life_left = 0x7fffffff;

    for (ch_idx = NUM_AMBIENTS; ch_idx < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS; ch_idx++) {
        if (entchannel != 0 &&
            channels[ch_idx].entnum == entnum &&
            (channels[ch_idx].entchannel == entchannel || entchannel == -1)) {
            // Always override sound from same entity
            first_to_die = ch_idx;
            break;
        }

        // Don't let monster sounds override player sounds
        if (channels[ch_idx].entnum == cl.viewentity && entnum != cl.viewentity && channels[ch_idx].sfx)
            continue;

        if (channels[ch_idx].end - paintedtime < life_left) {
            life_left = channels[ch_idx].end - paintedtime;
            first_to_die = ch_idx;
        }
    }

    if (first_to_die == -1)
        return NULL;

    if (channels[first_to_die].sfx) {
#if SND_USE_HW_MIXER
        S_MixerStopChannel(first_to_die);
#endif
        channels[first_to_die].sfx = NULL;
    }

    return &channels[first_to_die];
}

// ====================================================================
// Public API
// ====================================================================

void S_Startup(void)
{
    if (!snd_initialized)
        return;

#if SND_USE_HW_MIXER
    if (!of_has_feature(OF_HW_MIXER)) {
        Con_Printf("S_Startup: OF_HW_MIXER unavailable.\n");
        sound_started = false;
        return;
    }
#endif

    if (!SNDDMA_Init()) {
        Con_Printf("S_Startup: SNDDMA_Init failed.\n");
        sound_started = false;
        return;
    }

#if SND_USE_HW_MIXER
    of_mixer_init(OF_MIXER_MAX_VOICES, shm ? shm->speed : OF_MIXER_OUTPUT_RATE);
    of_mixer_stop_all();
    of_mixer_set_master_volume(255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_SFX, 255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_AUX, 255);
    S_MixerStopAllChannels();
    S_MixerResetTime();
#endif

    sound_started = true;
}

void S_Init(void)
{
    Con_Printf("\nSound Initialization\n");

    Cvar_RegisterVariable(&nosound);
    Cvar_RegisterVariable(&volume);
    Cvar_RegisterVariable(&precache);
    Cvar_RegisterVariable(&bgmvolume);
    Cvar_RegisterVariable(&ambient_level);
    Cvar_RegisterVariable(&ambient_fade);
    Cvar_RegisterVariable(&_snd_mixahead);

    snd_initialized = true;

    S_Startup();

    if (!sound_started)
        return;

    SND_InitScaletable();

    num_sfx = 0;

    // Load ambient sounds
    ambient_sfx[AMBIENT_WATER] = S_PrecacheSound("ambience/water1.wav");
    ambient_sfx[AMBIENT_SKY] = S_PrecacheSound("ambience/wind2.wav");

    S_StopAllSounds(true);

    Con_Printf("Sound initialized: %d Hz, %d-bit\n", shm->speed, shm->samplebits);
}

void S_Shutdown(void)
{
    if (!sound_started)
        return;

#if SND_USE_HW_MIXER
    S_MixerStopAllChannels();
#endif

    sound_started = false;
    snd_initialized = false;
    SNDDMA_Shutdown();
    shm = NULL;
}

sfx_t *S_PrecacheSound(char *name)
{
    sfx_t *sfx;

    if (!snd_initialized || nosound.value)
        return NULL;

    sfx = S_FindName(name);

    // Cache it in
    if (precache.value)
        S_LoadSound(sfx);

    return sfx;
}

void S_TouchSound(char *name)
{
    sfx_t *sfx;

    if (!sound_started)
        return;

    sfx = S_FindName(name);
    Cache_Check(&sfx->cache);
}

void S_ClearPrecache(void)
{
}

void S_BeginPrecaching(void)
{
}

void S_EndPrecaching(void)
{
}

void S_StartSound(int entnum, int entchannel, sfx_t *sfx, vec3_t origin,
                  float fvol, float attenuation)
{
    channel_t *target_chan, *check;
    sfxcache_t *sc;
    int vol;
    int ch_idx;
    int skip;

    if (!sound_started)
        return;

    if (!sfx)
        return;

    if (nosound.value)
        return;

    vol = fvol * 255;

    // Pick a channel to play on
    target_chan = SND_PickChannel(entnum, entchannel);
    if (!target_chan)
        return;

    // Spatialize
    memset(target_chan, 0, sizeof(*target_chan));
    VectorCopy(origin, target_chan->origin);
    target_chan->dist_mult = attenuation / 1000.0f;
    target_chan->master_vol = vol;
    target_chan->entnum = entnum;
    target_chan->entchannel = entchannel;
    SND_Spatialize(target_chan);

    if (!target_chan->leftvol && !target_chan->rightvol)
        return; // Not audible at all

    // New channel
    sc = S_LoadSound(sfx);
    if (!sc) {
        target_chan->sfx = NULL;
        return;
    }

    target_chan->sfx = sfx;
    target_chan->pos = 0;
    target_chan->end = paintedtime + sc->length;

    // If an identical sound has also been started this frame, offset the pos
    // a bit to prevent the sounds from being on top of each other and clipping
    check = &channels[NUM_AMBIENTS];
    for (ch_idx = NUM_AMBIENTS; ch_idx < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS; ch_idx++, check++) {
        if (check == target_chan)
            continue;
        if (check->sfx == sfx && !check->pos) {
            skip = rand() % (int)(0.1 * shm->speed);
            if (skip >= target_chan->end - paintedtime)
                skip = target_chan->end - paintedtime - 1;
            target_chan->pos += skip;
            target_chan->end -= skip;
            break;
        }
    }

#if SND_USE_HW_MIXER
    ch_idx = (int)(target_chan - channels);
    if (S_MixerStartChannel(ch_idx))
        S_MixerApplyChannelVolume(ch_idx);
#endif
}

void S_StopSound(int entnum, int entchannel)
{
    int i;

    for (i = NUM_AMBIENTS; i < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS; i++) {
        if (channels[i].entnum == entnum &&
            channels[i].entchannel == entchannel) {
#if SND_USE_HW_MIXER
            S_MixerStopChannel(i);
#endif
            channels[i].end = 0;
            channels[i].sfx = NULL;
            return;
        }
    }
}

void S_StopAllSounds(qboolean clear)
{
    int i;

    if (!sound_started)
        return;

#if SND_USE_HW_MIXER
    S_MixerStopAllChannels();
#endif

    total_channels = MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS;

    for (i = 0; i < MAX_CHANNELS; i++) {
        if (channels[i].sfx)
            channels[i].sfx = NULL;
    }

    memset(channels, 0, MAX_CHANNELS * sizeof(channel_t));

    if (clear)
        S_ClearBuffer();
}

void S_ClearBuffer(void)
{
    int clear;

    if (!sound_started || !shm || !shm->buffer)
        return;

#if SND_USE_HW_MIXER
    S_MixerStopAllChannels();
    S_MixerResetTime();
#endif

    clear = 0; // 16-bit: silence is 0
    memset(shm->buffer, clear, shm->samples * shm->samplebits / 8);
    SNDDMA_ClearBuffer();
}

void S_StaticSound(sfx_t *sfx, vec3_t origin, float vol, float attenuation)
{
    channel_t *ss;
    sfxcache_t *sc;

    if (!sfx)
        return;

    if (total_channels == MAX_CHANNELS) {
        Con_Printf("total_channels == MAX_CHANNELS\n");
        return;
    }

    ss = &channels[total_channels];
    total_channels++;

    sc = S_LoadSound(sfx);
    if (!sc)
        return;

    if (sc->loopstart == -1) {
        Con_Printf("Sound %s not looped\n", sfx->name);
        return;
    }

    ss->sfx = sfx;
    VectorCopy(origin, ss->origin);
    ss->master_vol = vol;
    ss->dist_mult = (attenuation / 64) / 1000.0f;
    ss->end = paintedtime + sc->length;

    SND_Spatialize(ss);
}

// ====================================================================
// Ambient sound update
// ====================================================================

static void S_UpdateAmbientSounds(void)
{
    mleaf_t *l;
    float vol;
    int ambient_channel;
    channel_t *chan;

    if (!snd_initialized || !sound_started)
        return;

    // Calc ambient sound levels
    if (!cl.worldmodel)
        return;

    l = Mod_PointInLeaf(listener_origin, cl.worldmodel);
    if (!l || !ambient_level.value) {
        for (ambient_channel = 0; ambient_channel < NUM_AMBIENTS; ambient_channel++) {
#if SND_USE_HW_MIXER
            S_MixerStopChannel(ambient_channel);
#endif
            channels[ambient_channel].sfx = NULL;
        }
        return;
    }

    for (ambient_channel = 0; ambient_channel < NUM_AMBIENTS; ambient_channel++) {
        chan = &channels[ambient_channel];
        chan->sfx = ambient_sfx[ambient_channel];

        vol = ambient_level.value * l->ambient_sound_level[ambient_channel];
        if (vol < 8)
            vol = 0;

        // Don't adjust volume too fast
        if (chan->master_vol < vol) {
            chan->master_vol += host_frametime * ambient_fade.value;
            if (chan->master_vol > vol)
                chan->master_vol = vol;
        } else if (chan->master_vol > vol) {
            chan->master_vol -= host_frametime * ambient_fade.value;
            if (chan->master_vol < vol)
                chan->master_vol = vol;
        }

        chan->leftvol = chan->rightvol = (int)(chan->master_vol * volume.value);
    }
}

// ====================================================================
// GetSoundtime - derive playback position from hardware
// ====================================================================

static void GetSoundtime(void)
{
    int fullsamples;
    int samplepos;

    fullsamples = shm->samples / shm->channels;

    // Get current position from driver (submit_src_pos based)
    samplepos = SNDDMA_GetDMAPos();

    if (samplepos < oldsamplepos) {
        // Buffer wrapped
        buffers++;
    }
    oldsamplepos = samplepos;

    soundtime = buffers * fullsamples + samplepos / shm->channels;
}

// ====================================================================
// S_Update - called once per frame
// ====================================================================

void S_Update(vec3_t origin, vec3_t forward, vec3_t right, vec3_t up)
{
    int i, j;
    int total;
    channel_t *ch;
    channel_t *combine;
    int endtime;
    int samps;

    if (!sound_started || (snd_blocked > 0))
        return;

    VectorCopy(origin, listener_origin);
    VectorCopy(forward, listener_forward);
    VectorCopy(right, listener_right);
    VectorCopy(up, listener_up);

#if SND_USE_HW_MIXER
    S_MixerAdvanceTime();
#endif

    // Update ambient sounds
    S_UpdateAmbientSounds();

    // Update spatialization for all sounds
    combine = NULL;

    for (i = NUM_AMBIENTS; i < total_channels; i++) {
        ch = &channels[i];
        if (!ch->sfx)
            continue;
        SND_Spatialize(ch);
        if (!ch->leftvol && !ch->rightvol)
            continue;

        // Try to combine static sounds with a previous channel of the same
        // sound effect so we don't mix five torches every frame
        if (i >= MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS) {
            // See if it can just use the last one
            if (combine && combine->sfx == ch->sfx) {
                combine->leftvol += ch->leftvol;
                combine->rightvol += ch->rightvol;
                ch->leftvol = ch->rightvol = 0;
                continue;
            }
            // Search for one
            combine = channels + MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS;
            for (j = MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS; j < i; j++, combine++) {
                if (combine->sfx == ch->sfx)
                    break;
            }

            if (j == total_channels) {
                combine = NULL;
            } else {
                if (combine != ch) {
                    combine->leftvol += ch->leftvol;
                    combine->rightvol += ch->rightvol;
                    ch->leftvol = ch->rightvol = 0;
                }
                continue;
            }
        }
    }

#if SND_USE_HW_MIXER
    for (i = 0; i < total_channels; i++)
        S_MixerSyncChannel(i);
    return;
#endif

    // Anchor paintedtime to actual playback progress
    GetSoundtime();

    if (paintedtime < soundtime)
        paintedtime = soundtime;

    // Mix ahead by _snd_mixahead seconds, capped by ring buffer size
    samps = shm->samples / shm->channels;
    endtime = soundtime + (int)(_snd_mixahead.value * shm->speed);
    if (endtime - soundtime > samps)
        endtime = soundtime + samps;

    S_PaintChannels(endtime);

    if (audio_timer_active)
        SNDDMA_FillRing();
    else
        SNDDMA_Submit();
}

void S_ExtraUpdate(void)
{
    if (!sound_started)
        return;

#if SND_USE_HW_MIXER
    return;
#endif

    if (audio_timer_active)
        SNDDMA_FillRing();
    else
        SNDDMA_Submit();
}

void S_LocalSound(char *sound)
{
    sfx_t *sfx;

    if (nosound.value)
        return;
    if (!sound_started)
        return;

    sfx = S_PrecacheSound(sound);
    if (!sfx) {
        Con_Printf("S_LocalSound: can't cache %s\n", sound);
        return;
    }
    S_StartSound(cl.viewentity, -1, sfx, vec3_origin, 1, 1);
}

void S_AmbientOff(void)
{
}

void S_AmbientOn(void)
{
}
