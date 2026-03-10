/* opl_player.c -- OPL2 FM synthesis music player using GENMIDI lump
 *
 * Uses Nuked-OPL3 emulator in OPL2 mode with instrument definitions
 * from the GENMIDI lump found in Doom WAD files. Replaces TinySoundFont
 * (sample-based synthesis) which saturated the PSRAM bus on ESP32-P4.
 *
 * OPL2 FM synthesis is pure computation with ~few KB state, causing
 * zero PSRAM bus contention with the game thread.
 */

#include <string.h>
#include <math.h>
#include "opl_player.h"
#include "opl2.h"
#include "esp_log.h"

static const char *TAG = "opl_player";

/* ------------------------------------------------------------------ */
/* GENMIDI lump structures (packed, matches WAD format exactly)        */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint8_t tremolo;    /* reg 0x20: AM/VIB/EGT/KSR/MULT */
    uint8_t attack;     /* reg 0x60: Attack Rate / Decay Rate */
    uint8_t sustain;    /* reg 0x80: Sustain Level / Release Rate */
    uint8_t waveform;   /* reg 0xE0: Waveform Select */
    uint8_t scale;      /* reg 0x40 bits 6-7: Key Scale Level */
    uint8_t level;      /* reg 0x40 bits 0-5: Total Level (volume) */
} genmidi_op_t;

typedef struct __attribute__((packed)) {
    genmidi_op_t modulator;
    uint8_t feedback;       /* reg 0xC0: Feedback/Connection */
    genmidi_op_t carrier;
    uint8_t unused;
    int16_t base_note_offset;
} genmidi_voice_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint8_t fine_tuning;
    uint8_t fixed_note;
    genmidi_voice_t voices[2];
} genmidi_instr_t;

#define GENMIDI_HEADER          "#OPL_II#"
#define GENMIDI_NUM_INSTRS      175
#define GENMIDI_NUM_MELODIC     128
#define GENMIDI_NUM_PERCUSSION  47
#define GENMIDI_PERCUSSION_START 35  /* MIDI note 35 = first percussion */

#define GENMIDI_FLAG_FIXED      0x0001  /* Use fixed_note for pitch */
#define GENMIDI_FLAG_2VOICE     0x0004  /* Double-voice instrument */

/* ------------------------------------------------------------------ */
/* OPL2 constants                                                     */
/* ------------------------------------------------------------------ */

#define NUM_OPL_VOICES    9
#define NUM_MIDI_CHANNELS 16
#define PERCUSSION_CHANNEL 9

/* OPL register base addresses */
#define OPL_REG_WSEN     0x01   /* Waveform Select Enable */
#define OPL_REG_TREMOLO  0x20   /* AM/VIB/EGT/KSR/MULT */
#define OPL_REG_LEVEL    0x40   /* KSL/TL */
#define OPL_REG_ATTACK   0x60   /* AR/DR */
#define OPL_REG_SUSTAIN  0x80   /* SL/RR */
#define OPL_REG_FNUM_LO  0xA0   /* F-number low 8 bits */
#define OPL_REG_KEYON    0xB0   /* Key-on / Block / F-num high */
#define OPL_REG_FEEDBACK 0xC0   /* Feedback / Connection */
#define OPL_REG_WAVEFORM 0xE0   /* Waveform Select */

/* Operator slot offsets for each OPL channel [channel][0=mod, 1=car] */
static const uint8_t slot_offsets[9][2] = {
    {0x00, 0x03}, {0x01, 0x04}, {0x02, 0x05},
    {0x08, 0x0B}, {0x09, 0x0C}, {0x0A, 0x0D},
    {0x10, 0x13}, {0x11, 0x14}, {0x12, 0x15}
};

/* ------------------------------------------------------------------ */
/* Voice and channel state                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    VOICE_FREE,
    VOICE_PLAYING,
    VOICE_RELEASING
} voice_state_t;

typedef struct {
    voice_state_t state;
    int midi_channel;
    int midi_note;
    int midi_velocity;
    int play_note;          /* Note actually used for pitch (after fixed_note) */
    uint32_t timestamp;
    const genmidi_voice_t *instrument;
} opl_voice_info_t;

typedef struct {
    int program;        /* Current patch (0-127) */
    int volume;         /* CC#7  (0-127, default 100) */
    int expression;     /* CC#11 (0-127, default 127) */
    int pitch_bend;     /* 0-16383, center 8192 */
} midi_channel_state_t;

/* ------------------------------------------------------------------ */
/* Module state                                                       */
/* ------------------------------------------------------------------ */

static opl2_chip_t opl_chip;
static opl_voice_info_t voices[NUM_OPL_VOICES];
static midi_channel_state_t midi_channels[NUM_MIDI_CHANNELS];
static const genmidi_instr_t *instruments = NULL;
static int genmidi_loaded = 0;
static uint32_t time_counter = 0;
static int initialized = 0;

/* Frequency table: fnum and block for each MIDI note 0-127 */
static uint16_t note_fnum[128];
static uint8_t  note_block[128];

/* Volume mapping table (attempt to match DMX volume curve) */
static const uint8_t volume_table[128] = {
      0,   1,   3,   5,   6,   8,  10,  11,
     13,  14,  16,  17,  19,  20,  22,  23,
     25,  26,  27,  29,  30,  32,  33,  34,
     36,  37,  39,  41,  43,  45,  47,  49,
     50,  52,  54,  55,  57,  59,  60,  61,
     63,  64,  66,  67,  68,  69,  71,  72,
     73,  74,  75,  76,  77,  79,  80,  81,
     82,  83,  84,  84,  85,  86,  87,  88,
     89,  90,  91,  92,  92,  93,  94,  95,
     96,  96,  97,  98,  99,  99, 100, 101,
    101, 102, 103, 103, 104, 105, 105, 106,
    107, 107, 108, 109, 109, 110, 110, 111,
    112, 112, 113, 113, 114, 114, 115, 115,
    116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 123,
    124, 124, 125, 125, 126, 126, 127, 127
};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static void init_freq_table(void)
{
    for (int n = 0; n < 128; n++) {
        double freq = 440.0 * pow(2.0, (n - 69) / 12.0);
        /* fnum = freq * 2^(20-block) / 49716.  Start at block 0 (largest
         * fnum) and increase block until fnum fits in 10 bits. */
        int fnum = (int)(freq * (1 << 20) / 49716.0 + 0.5);
        int block = 0;

        while (fnum > 1023 && block < 7) {
            fnum >>= 1;
            block++;
        }
        if (fnum > 1023) fnum = 1023;

        note_fnum[n] = (uint16_t)fnum;
        note_block[n] = (uint8_t)block;
    }
}

static void write_reg(uint16_t reg, uint8_t val)
{
    opl2_write(&opl_chip, reg, val);
}

/* Program one operator (modulator or carrier) of an OPL channel */
static void set_operator(int opl_ch, int op, const genmidi_op_t *data)
{
    int offset = slot_offsets[opl_ch][op];

    write_reg(OPL_REG_TREMOLO  + offset, data->tremolo);
    write_reg(OPL_REG_LEVEL    + offset, data->scale | data->level);
    write_reg(OPL_REG_ATTACK   + offset, data->attack);
    write_reg(OPL_REG_SUSTAIN  + offset, data->sustain);
    write_reg(OPL_REG_WAVEFORM + offset, data->waveform);
}

/* Update carrier (and possibly modulator) volume for a voice */
static void set_voice_volume(int vi)
{
    opl_voice_info_t *v = &voices[vi];
    if (!v->instrument) return;

    midi_channel_state_t *ch = &midi_channels[v->midi_channel];

    /* Effective MIDI volume (velocity × channel volume, curved) */
    int midi_vol = (volume_table[ch->volume] *
                    volume_table[v->midi_velocity]) / 127;

    /* Carrier: scale instrument's base volume by MIDI volume.
     * TL is attenuation (0=loud, 63=silent), so convert to linear
     * volume, scale, and convert back. */
    int car_level = v->instrument->carrier.level & 0x3F;
    int car_vol = 0x3F - car_level;            /* attenuation → volume */
    car_vol = (car_vol * midi_vol) / 128;
    int car_tl = (0x3F - car_vol) & 0x3F;     /* volume → attenuation */

    int car_offset = slot_offsets[vi][1];
    write_reg(OPL_REG_LEVEL + car_offset,
              v->instrument->carrier.scale | car_tl);

    /* For additive connection (bit 0 of feedback = 1), modulator
     * also contributes to output, so scale its volume too. */
    if (v->instrument->feedback & 0x01) {
        int mod_level = v->instrument->modulator.level & 0x3F;
        int mod_vol = 0x3F - mod_level;
        mod_vol = (mod_vol * midi_vol) / 128;
        int mod_tl = (0x3F - mod_vol) & 0x3F;

        int mod_offset = slot_offsets[vi][0];
        write_reg(OPL_REG_LEVEL + mod_offset,
                  v->instrument->modulator.scale | mod_tl);
    }
}

/* Set OPL frequency registers for a voice.  Writes key-on bit based
 * on voice state (PLAYING = key on, anything else = key off). */
static void set_voice_frequency(int vi)
{
    opl_voice_info_t *v = &voices[vi];
    int note = v->play_note;

    /* Apply instrument note offset (coarse, in 1/64 semitone units) */
    if (v->instrument) {
        note += v->instrument->base_note_offset / 64;
    }
    if (note < 0)   note = 0;
    if (note > 127) note = 127;

    uint16_t fnum = note_fnum[note];
    uint8_t  block = note_block[note];

    /* Apply pitch bend (±2 semitones range) */
    midi_channel_state_t *ch = &midi_channels[v->midi_channel];
    int bend = ch->pitch_bend - 8192;
    if (bend != 0) {
        double ratio = pow(2.0, (bend / 8192.0) * (2.0 / 12.0));
        int bent = (int)(fnum * ratio + 0.5);

        while (bent > 1023 && block < 7) {
            bent >>= 1;
            block++;
        }
        while (bent > 0 && bent < 256 && block > 0) {
            bent <<= 1;
            block--;
        }
        if (bent > 1023) bent = 1023;
        if (bent < 0) bent = 0;
        fnum = (uint16_t)bent;
    }

    write_reg(OPL_REG_FNUM_LO + vi, fnum & 0xFF);
    write_reg(OPL_REG_KEYON + vi,
              (v->state == VOICE_PLAYING ? 0x20 : 0x00) |
              ((block & 7) << 2) |
              ((fnum >> 8) & 3));
}

/* Find a voice to use for a new note */
static int alloc_voice(void)
{
    /* Prefer: free > oldest releasing > oldest playing */
    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        if (voices[i].state == VOICE_FREE)
            return i;
    }

    int best = -1;
    uint32_t oldest = UINT32_MAX;

    /* Oldest releasing voice */
    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        if (voices[i].state == VOICE_RELEASING && voices[i].timestamp < oldest) {
            oldest = voices[i].timestamp;
            best = i;
        }
    }
    if (best >= 0) return best;

    /* Oldest playing voice */
    oldest = UINT32_MAX;
    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        if (voices[i].timestamp < oldest) {
            oldest = voices[i].timestamp;
            best = i;
        }
    }
    return best >= 0 ? best : 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void opl_player_init(int sample_rate)
{
    opl2_init(&opl_chip, (uint32_t)sample_rate);

    /* Enable waveform selection (OPL2 register 0x01 bit 5) */
    write_reg(OPL_REG_WSEN, 0x20);

    memset(voices, 0, sizeof(voices));

    for (int i = 0; i < NUM_MIDI_CHANNELS; i++) {
        midi_channels[i].program = 0;
        midi_channels[i].volume = 100;
        midi_channels[i].expression = 127;
        midi_channels[i].pitch_bend = 8192;
    }

    init_freq_table();
    time_counter = 0;
    initialized = 1;

    ESP_LOGI(TAG, "OPL player initialized at %d Hz", sample_rate);
}

void opl_player_shutdown(void)
{
    if (initialized)
        opl_player_all_notes_off();
    initialized = 0;
    genmidi_loaded = 0;
    instruments = NULL;
}

int opl_player_load_genmidi(const void *data, int len)
{
    int expected = 8 + GENMIDI_NUM_INSTRS * (int)sizeof(genmidi_instr_t);
    if (!data || len < expected) {
        ESP_LOGE(TAG, "GENMIDI lump too small (%d bytes, need %d)", len, expected);
        return -1;
    }

    if (memcmp(data, GENMIDI_HEADER, 8) != 0) {
        ESP_LOGE(TAG, "Invalid GENMIDI header");
        return -1;
    }

    instruments = (const genmidi_instr_t *)((const uint8_t *)data + 8);
    genmidi_loaded = 1;

    ESP_LOGI(TAG, "GENMIDI loaded: %d instruments (%d melodic + %d percussion)",
             GENMIDI_NUM_INSTRS, GENMIDI_NUM_MELODIC, GENMIDI_NUM_PERCUSSION);
    return 0;
}

void opl_player_note_on(int channel, int note, int velocity)
{
    if (!initialized || !genmidi_loaded) return;
    if (channel < 0 || channel >= NUM_MIDI_CHANNELS) return;
    if (note < 0 || note > 127) return;

    if (velocity == 0) {
        opl_player_note_off(channel, note);
        return;
    }

    /* Look up instrument */
    const genmidi_instr_t *instr;
    int play_note = note;

    if (channel == PERCUSSION_CHANNEL) {
        int perc_idx = note - GENMIDI_PERCUSSION_START;
        if (perc_idx < 0 || perc_idx >= GENMIDI_NUM_PERCUSSION) return;
        instr = &instruments[GENMIDI_NUM_MELODIC + perc_idx];
        if (instr->flags & GENMIDI_FLAG_FIXED)
            play_note = instr->fixed_note;
    } else {
        int prog = midi_channels[channel].program;
        if (prog < 0 || prog >= GENMIDI_NUM_MELODIC) return;
        instr = &instruments[prog];
    }

    /* Allocate OPL voice */
    int vi = alloc_voice();
    if (vi < 0) return;

    /* Key-off old note if voice was active */
    if (voices[vi].state != VOICE_FREE) {
        write_reg(OPL_REG_KEYON + vi, 0);
    }

    /* Set up voice state */
    const genmidi_voice_t *vd = &instr->voices[0];
    voices[vi].state = VOICE_PLAYING;
    voices[vi].midi_channel = channel;
    voices[vi].midi_note = note;
    voices[vi].midi_velocity = velocity;
    voices[vi].play_note = play_note;
    voices[vi].timestamp = time_counter++;
    voices[vi].instrument = vd;

    /* Program instrument operators */
    set_operator(vi, 0, &vd->modulator);
    set_operator(vi, 1, &vd->carrier);

    /* Feedback + connection, output to both L+R (bits 4-5 = 0x30) */
    write_reg(OPL_REG_FEEDBACK + vi, vd->feedback | 0x30);

    /* Set volume (adjusts carrier TL for MIDI velocity/volume) */
    set_voice_volume(vi);

    /* Set frequency and key-on */
    set_voice_frequency(vi);
}

void opl_player_note_off(int channel, int note)
{
    if (!initialized) return;

    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        if (voices[i].state == VOICE_PLAYING &&
            voices[i].midi_channel == channel &&
            voices[i].midi_note == note) {

            voices[i].state = VOICE_RELEASING;
            voices[i].timestamp = time_counter++;

            /* Re-write frequency with key-on bit cleared.
             * set_voice_frequency checks state for the key-on bit. */
            set_voice_frequency(i);
            break;
        }
    }
}

void opl_player_program_change(int channel, int program)
{
    if (channel < 0 || channel >= NUM_MIDI_CHANNELS) return;
    midi_channels[channel].program = program;
}

void opl_player_control_change(int channel, int controller, int value)
{
    if (channel < 0 || channel >= NUM_MIDI_CHANNELS) return;

    switch (controller) {
    case 7:  /* Channel Volume */
        midi_channels[channel].volume = value;
        for (int i = 0; i < NUM_OPL_VOICES; i++) {
            if (voices[i].state == VOICE_PLAYING &&
                voices[i].midi_channel == channel)
                set_voice_volume(i);
        }
        break;

    case 11: /* Expression */
        midi_channels[channel].expression = value;
        break;

    case 120: /* All Sound Off */
    case 123: /* All Notes Off */
        for (int i = 0; i < NUM_OPL_VOICES; i++) {
            if (voices[i].midi_channel == channel &&
                voices[i].state != VOICE_FREE) {
                voices[i].state = VOICE_FREE;
                write_reg(OPL_REG_KEYON + i, 0);
            }
        }
        break;

    case 121: /* Reset All Controllers */
        midi_channels[channel].volume = 100;
        midi_channels[channel].expression = 127;
        midi_channels[channel].pitch_bend = 8192;
        break;
    }
}

void opl_player_pitch_bend(int channel, int bend)
{
    if (channel < 0 || channel >= NUM_MIDI_CHANNELS) return;
    midi_channels[channel].pitch_bend = bend;

    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        if (voices[i].state == VOICE_PLAYING &&
            voices[i].midi_channel == channel)
            set_voice_frequency(i);
    }
}

void opl_player_all_notes_off(void)
{
    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        voices[i].state = VOICE_FREE;
        write_reg(OPL_REG_KEYON + i, 0);
    }

    for (int i = 0; i < NUM_MIDI_CHANNELS; i++) {
        midi_channels[i].volume = 100;
        midi_channels[i].expression = 127;
        midi_channels[i].pitch_bend = 8192;
    }
}

void opl_player_render(int16_t *buffer, int num_samples)
{
    if (!initialized) {
        memset(buffer, 0, num_samples * 2 * sizeof(int16_t));
        return;
    }
    opl2_generate(&opl_chip, buffer, (uint32_t)num_samples);
}
