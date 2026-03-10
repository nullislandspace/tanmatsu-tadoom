/* i_sound_tanmatsu.c -- Sound system for Tanmatsu (replaces SDL/i_sound.c) */

#include <math.h>
#include <string.h>
#include <limits.h>

#include "config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "bsp/audio.h"

#include "z_zone.h"
#include "m_swap.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "lprintf.h"
#include "s_sound.h"
#include "doomdef.h"
#include "doomstat.h"
#include "doomtype.h"
#include "d_main.h"
#include "mmus2mid.h"

#include "midi_player.h"
#include "opl_player.h"

static const char *TAG = "i_sound";

/* Variables used by Boom from Allegro */
int snd_card = 1;
int mus_card = 1;
int detect_voices = 0;

static boolean sound_inited = false;

#define SAMPLECOUNT 256
#define MAX_CHANNELS 32
#define AUDIO_SAMPLE_RATE 22050

int snd_samplerate = AUDIO_SAMPLE_RATE;

int audio_fd;

typedef struct {
    int id;
    unsigned int step;
    unsigned int stepremainder;
    unsigned int samplerate;
    const unsigned char *data;
    const unsigned char *enddata;
    int starttime;
    int *leftvol_lookup;
    int *rightvol_lookup;
} channel_info_t;

static channel_info_t channelinfo[MAX_CHANNELS];
static int steptable[256];

/* Volume lookup in PSRAM (128*256 = 32768 entries) */
static int *vol_lookup = NULL;

/* Audio mutex for thread safety */
static SemaphoreHandle_t audio_mutex = NULL;

/* I2S handle */
static i2s_chan_handle_t i2s_handle = NULL;

/* Music state */
static boolean music_playing = false;
static boolean music_enabled = false;
static int music_volume = 15; /* 0-15 */

/* MIDI data for current song */
static uint8_t *current_midi_data = NULL;
static int current_midi_len = 0;

/* External BSP audio function */
extern void bsp_audio_initialize(uint32_t rate);

/* Stop a channel. If called from the mixer (audio task, Core 1), only
 * NULL the data pointer - do NOT call W_UnlockLumpNum because the zone
 * allocator has no thread safety. The lump unlock is deferred to
 * stopchan_and_unlock() which runs on the game thread. */
static void stopchan(int i)
{
    if (channelinfo[i].data) {
        channelinfo[i].data = NULL;
        /* W_UnlockLumpNum intentionally omitted here for thread safety.
         * The lump stays locked (PU_STATIC) until the channel is reused
         * by addsfx(), which calls stopchan_and_unlock() first. */
    }
}

/* Full stop + unlock, safe to call from game thread only. */
static void stopchan_and_unlock(int i)
{
    if (channelinfo[i].data) {
        channelinfo[i].data = NULL;
    }
    /* Unlock even if data was already NULL'd by the mixer */
    if (channelinfo[i].id) {
        W_UnlockLumpNum(S_sfx[channelinfo[i].id].lumpnum);
        channelinfo[i].id = 0;
    }
}

static int addsfx(int sfxid, int channel, const unsigned char *data, size_t len)
{
    stopchan_and_unlock(channel);

    channelinfo[channel].data = data;
    channelinfo[channel].enddata = channelinfo[channel].data + len - 1;
    channelinfo[channel].samplerate =
        (channelinfo[channel].data[3] << 8) + channelinfo[channel].data[2];
    channelinfo[channel].data += 8; /* Skip header */
    channelinfo[channel].stepremainder = 0;
    channelinfo[channel].starttime = gametic;
    channelinfo[channel].id = sfxid;

    return channel;
}

static void updateSoundParams(int handle, int volume, int seperation, int pitch)
{
    int slot = handle;
    int rightvol;
    int leftvol;
    int step = steptable[pitch];

    if (pitched_sounds)
        channelinfo[slot].step =
            step + (((channelinfo[slot].samplerate << 16) / snd_samplerate) - 65536);
    else
        channelinfo[slot].step =
            ((channelinfo[slot].samplerate << 16) / snd_samplerate);

    seperation += 1;

    leftvol = volume - ((volume * seperation * seperation) >> 16);
    seperation = seperation - 257;
    rightvol = volume - ((volume * seperation * seperation) >> 16);

    if (rightvol < 0) rightvol = 0;
    if (rightvol > 127) rightvol = 127;
    if (leftvol < 0) leftvol = 0;
    if (leftvol > 127) leftvol = 127;

    channelinfo[slot].leftvol_lookup = &vol_lookup[leftvol * 256];
    channelinfo[slot].rightvol_lookup = &vol_lookup[rightvol * 256];
}

void I_UpdateSoundParams(int handle, int volume, int seperation, int pitch)
{
    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    updateSoundParams(handle, volume, seperation, pitch);
    xSemaphoreGive(audio_mutex);
}

void I_SetChannels(void)
{
    int i, j;
    int *steptablemid = steptable + 128;

    for (i = 0; i < MAX_CHANNELS; i++)
        memset(&channelinfo[i], 0, sizeof(channel_info_t));

    for (i = -128; i < 128; i++)
        steptablemid[i] =
            (int)(pow(1.2, ((double)i / (64.0 * snd_samplerate / 11025))) * 65536.0);

    /* Allocate vol_lookup in PSRAM */
    if (!vol_lookup) {
        vol_lookup = heap_caps_malloc(128 * 256 * sizeof(int), MALLOC_CAP_SPIRAM);
        if (!vol_lookup) {
            lprintf(LO_ERROR, "I_SetChannels: Failed to allocate vol_lookup\n");
            return;
        }
    }

    for (i = 0; i < 128; i++)
        for (j = 0; j < 256; j++)
            vol_lookup[i * 256 + j] = (i * (j - 128) * 256) / 191;
}

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

int I_StartSound(int id, int channel, int vol, int sep, int pitch, int priority)
{
    const unsigned char *data;
    int lump;
    size_t len;

    if ((channel < 0) || (channel >= MAX_CHANNELS))
        return -1;

    lump = S_sfx[id].lumpnum;
    len = W_LumpLength(lump);

    if (len <= 8) return -1;

    len -= 8;
    data = W_LockLumpNum(lump);

    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    addsfx(id, channel, data, len);
    updateSoundParams(channel, vol, sep, pitch);
    xSemaphoreGive(audio_mutex);

    return channel;
}

void I_StopSound(int handle)
{
    if ((handle < 0) || (handle >= MAX_CHANNELS))
        return;
    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    stopchan_and_unlock(handle);
    xSemaphoreGive(audio_mutex);
}

boolean I_SoundIsPlaying(int handle)
{
    if ((handle < 0) || (handle >= MAX_CHANNELS))
        return false;
    return channelinfo[handle].data != NULL;
}

boolean I_AnySoundStillPlaying(void)
{
    boolean result = false;
    int i;

    for (i = 0; i < MAX_CHANNELS; i++)
        result |= channelinfo[i].data != NULL;

    return result;
}

/* Mix SFX into buffer (called with mutex held) */
static void I_MixSound(int16_t *stream, int len_samples)
{
    int dl, dr;
    unsigned char sample;

    for (int s = 0; s < len_samples; s++) {
        dl = 0;
        dr = 0;

        for (int chan = 0; chan < numChannels; chan++) {
            if (channelinfo[chan].data) {
                sample = (((unsigned int)channelinfo[chan].data[0] *
                           (0x10000 - channelinfo[chan].stepremainder)) +
                          ((unsigned int)channelinfo[chan].data[1] *
                           (channelinfo[chan].stepremainder))) >> 16;

                dl += channelinfo[chan].leftvol_lookup[sample];
                dr += channelinfo[chan].rightvol_lookup[sample];

                channelinfo[chan].stepremainder += channelinfo[chan].step;
                channelinfo[chan].data += channelinfo[chan].stepremainder >> 16;
                channelinfo[chan].stepremainder &= 0xffff;

                if (channelinfo[chan].data >= channelinfo[chan].enddata)
                    stopchan(chan);
            }
        }

        if (dl > SHRT_MAX) dl = SHRT_MAX;
        else if (dl < SHRT_MIN) dl = SHRT_MIN;
        if (dr > SHRT_MAX) dr = SHRT_MAX;
        else if (dr < SHRT_MIN) dr = SHRT_MIN;

        stream[s * 2] = (int16_t)dl;
        stream[s * 2 + 1] = (int16_t)dr;
    }
}

/* Audio task running on Core 1 */
static void audio_task(void *arg)
{
    int16_t mix_buffer[SAMPLECOUNT * 2]; /* stereo */
    int16_t music_buffer[SAMPLECOUNT * 2];
    size_t bytes_written;

    ESP_LOGI(TAG, "Audio task started on core %d", xPortGetCoreID());

    while (1) {
        /* Clear mix buffer */
        memset(mix_buffer, 0, sizeof(mix_buffer));

        /* Mix SFX */
        xSemaphoreTake(audio_mutex, portMAX_DELAY);
        I_MixSound(mix_buffer, SAMPLECOUNT);
        xSemaphoreGive(audio_mutex);

        /* Mix music from OPL synthesizer */
        if (music_playing && music_enabled) {
            int64_t t0 = esp_timer_get_time();
            midi_player_tick(SAMPLECOUNT);
            int64_t t1 = esp_timer_get_time();
            opl_player_render(music_buffer, SAMPLECOUNT);
            int64_t t2 = esp_timer_get_time();

            static int diag_cnt = 0;
            if (++diag_cnt >= 200) {
                ESP_LOGI(TAG, "midi_tick=%lld us, opl_render=%lld us",
                         (long long)(t1 - t0), (long long)(t2 - t1));
                diag_cnt = 0;
            }

            /* Mix music into SFX buffer with volume scaling */
            float vol_scale = (float)music_volume / 15.0f;
            for (int i = 0; i < SAMPLECOUNT * 2; i++) {
                int val = mix_buffer[i] + (int)(music_buffer[i] * vol_scale);
                if (val > SHRT_MAX) val = SHRT_MAX;
                else if (val < SHRT_MIN) val = SHRT_MIN;
                mix_buffer[i] = (int16_t)val;
            }
        }

        /* Write to I2S (blocks until DMA is ready) */
        if (i2s_handle) {
            i2s_channel_write(i2s_handle, mix_buffer, sizeof(mix_buffer),
                              &bytes_written, portMAX_DELAY);
        }
    }
}

void I_InitSound(void)
{
    if (sound_inited) return;

    lprintf(LO_INFO, "I_InitSound: Initializing Tanmatsu audio\n");

    /* Create audio mutex */
    audio_mutex = xSemaphoreCreateMutex();

    /* Initialize audio hardware via BSP.
     * bsp_audio_initialize() hardcodes I2S to 44100 Hz and enables the
     * channel.  To reconfigure the clock we must disable the channel
     * first, set the new rate, then re-enable. */
    bsp_audio_initialize(AUDIO_SAMPLE_RATE);
    bsp_audio_get_i2s_handle(&i2s_handle);
    i2s_channel_disable(i2s_handle);
    bsp_audio_set_rate(AUDIO_SAMPLE_RATE);
    i2s_channel_enable(i2s_handle);
    bsp_audio_set_amplifier(true);
    bsp_audio_set_volume(80);

    /* Initialize mixer */
    I_SetChannels();

    sound_inited = true;

    /* Initialize music */
    if (!nomusicparm)
        I_InitMusic();

    /* Create audio task on Core 1 */
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL,
                            configMAX_PRIORITIES - 2, NULL, 1);

    lprintf(LO_INFO, "I_InitSound: audio ready (%d Hz)\n", snd_samplerate);
}

void I_ShutdownSound(void)
{
    if (sound_inited) {
        sound_inited = false;
        lprintf(LO_INFO, "I_ShutdownSound\n");
    }
}

/*
 * MUSIC API -- OPL2 FM synthesis using GENMIDI lump from WAD
 */

void I_InitMusic(void)
{
    lprintf(LO_INFO, "I_InitMusic: Initializing OPL music\n");

    /* Initialize OPL synthesizer */
    opl_player_init(AUDIO_SAMPLE_RATE);

    /* Load GENMIDI lump from WAD */
    int lump = W_CheckNumForName("GENMIDI");
    if (lump < 0) {
        lprintf(LO_WARN, "I_InitMusic: GENMIDI lump not found, music disabled\n");
        return;
    }

    const void *data = W_LockLumpNum(lump);
    int len = W_LumpLength(lump);

    if (opl_player_load_genmidi(data, len) < 0) {
        lprintf(LO_WARN, "I_InitMusic: Failed to load GENMIDI\n");
        W_UnlockLumpNum(lump);
        return;
    }

    /* Keep GENMIDI lump locked -- opl_player holds a direct pointer */

    midi_player_set_sample_rate(AUDIO_SAMPLE_RATE);
    midi_player_init();

    music_enabled = true;
    lprintf(LO_INFO, "I_InitMusic: OPL music ready\n");
}

void I_ShutdownMusic(void)
{
    music_playing = false;
    music_enabled = false;
    opl_player_shutdown();
}

void I_PlaySong(int handle, int looping)
{
    if (!music_enabled || !current_midi_data) return;

    midi_player_load(current_midi_data, current_midi_len, looping);
    midi_player_start();
    music_playing = true;
}

void I_PauseSong(int handle)
{
    music_playing = false;
}

void I_ResumeSong(int handle)
{
    if (music_enabled && current_midi_data)
        music_playing = true;
}

void I_StopSong(int handle)
{
    music_playing = false;
    opl_player_all_notes_off();
    midi_player_stop();
}

void I_UnRegisterSong(int handle)
{
    if (current_midi_data) {
        free(current_midi_data);
        current_midi_data = NULL;
        current_midi_len = 0;
    }
}

int I_RegisterSong(const void *data, size_t len)
{
    if (len < 32)
        return 0;
    if (!music_enabled)
        return 0;

    /* Free previous MIDI data */
    if (current_midi_data) {
        free(current_midi_data);
        current_midi_data = NULL;
    }

    /* Convert MUS to MIDI if needed */
    if (memcmp(data, "MUS", 3) == 0) {
        MIDI *mididata = malloc(sizeof(MIDI));
        UBYTE *mid;
        int midlen;

        mmus2mid(data, mididata, 89, 0);
        MIDIToMidi(mididata, &mid, &midlen);

        current_midi_data = malloc(midlen);
        memcpy(current_midi_data, mid, midlen);
        current_midi_len = midlen;

        free(mid);
        free_mididata(mididata);
        free(mididata);
    } else {
        /* Already MIDI format */
        current_midi_data = malloc(len);
        memcpy(current_midi_data, data, len);
        current_midi_len = len;
    }

    return 0;
}

int I_RegisterMusic(const char *filename, musicinfo_t *song)
{
    return 1; /* Not supported */
}

void I_SetMusicVolume(int volume)
{
    music_volume = volume;
}

void I_UpdateMusic(void)
{
    /* Nothing to do - audio task handles rendering */
}
