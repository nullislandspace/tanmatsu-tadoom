/* midi_player.c -- Minimal MIDI file sequencer for OPL music */

#include <string.h>
#include <stdlib.h>
#include "midi_player.h"
#include "opl_player.h"
#include "esp_log.h"

static const char *TAG = "midi_player";

#define MAX_MIDI_TRACKS 32

typedef struct {
    const uint8_t *data;
    const uint8_t *end;
    const uint8_t *pos;
    unsigned long next_tick;
    int finished;
} midi_track_t;

static const uint8_t *midi_data_ptr = NULL;
static int midi_data_len = 0;
static int midi_looping = 0;
static int midi_playing = 0;

static midi_track_t tracks[MAX_MIDI_TRACKS];
static int num_tracks = 0;
static int ticks_per_quarter = 120;
static double us_per_tick = 500000.0 / 120.0; /* default 120 BPM */
static int midi_sample_rate = 44100;
static double samples_per_tick = 0;
static double sample_counter = 0;
static unsigned long current_tick = 0;

/* Read variable-length quantity from MIDI stream */
static unsigned long read_vlq(const uint8_t **p, const uint8_t *end)
{
    unsigned long val = 0;
    int i;
    for (i = 0; i < 4 && *p < end; i++) {
        uint8_t byte = *(*p)++;
        val = (val << 7) | (byte & 0x7F);
        if (!(byte & 0x80))
            break;
    }
    return val;
}

static uint16_t read_u16be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

void midi_player_set_sample_rate(int rate)
{
    midi_sample_rate = rate;
}

void midi_player_init(void)
{
    memset(tracks, 0, sizeof(tracks));
    num_tracks = 0;
    midi_playing = 0;
    current_tick = 0;
    sample_counter = 0;
}

void midi_player_load(const uint8_t *data, int len, int looping)
{
    midi_player_stop();

    if (!data || len < 14) return;

    midi_data_ptr = data;
    midi_data_len = len;
    midi_looping = looping;

    const uint8_t *p = data;
    const uint8_t *end = data + len;

    /* Parse MIDI header: "MThd" */
    if (memcmp(p, "MThd", 4) != 0) {
        ESP_LOGE(TAG, "Not a MIDI file");
        return;
    }
    p += 4;
    uint32_t header_len = read_u32be(p); p += 4;
    /* uint16_t format = read_u16be(p); */ p += 2;
    uint16_t ntrks = read_u16be(p); p += 2;
    uint16_t division = read_u16be(p); p += 2;

    /* Skip any remaining header bytes */
    p = data + 8 + header_len;

    ticks_per_quarter = division;
    us_per_tick = 500000.0 / (double)ticks_per_quarter; /* 120 BPM default */

    num_tracks = (ntrks > MAX_MIDI_TRACKS) ? MAX_MIDI_TRACKS : ntrks;

    /* Parse track chunks */
    for (int i = 0; i < num_tracks && p + 8 <= end; i++) {
        if (memcmp(p, "MTrk", 4) != 0) {
            ESP_LOGW(TAG, "Expected MTrk at offset %d", (int)(p - data));
            num_tracks = i;
            break;
        }
        p += 4;
        uint32_t track_len = read_u32be(p); p += 4;

        tracks[i].data = p;
        tracks[i].end = p + track_len;
        tracks[i].pos = p;
        tracks[i].next_tick = 0;
        tracks[i].finished = 0;

        /* Read initial delta time */
        const uint8_t *tp = tracks[i].pos;
        tracks[i].next_tick = read_vlq(&tp, tracks[i].end);
        tracks[i].pos = tp;

        p += track_len;
    }

    ESP_LOGI(TAG, "Loaded MIDI: %d tracks, %d ticks/quarter", num_tracks, ticks_per_quarter);
}

void midi_player_start(void)
{
    if (!midi_data_ptr || num_tracks == 0) return;

    /* Reset positions */
    const uint8_t *p = midi_data_ptr + 8;
    uint32_t header_len = read_u32be(midi_data_ptr + 4);
    p = midi_data_ptr + 8 + header_len;

    for (int i = 0; i < num_tracks; i++) {
        if (memcmp(p, "MTrk", 4) != 0) break;
        p += 4;
        uint32_t track_len = read_u32be(p); p += 4;

        tracks[i].data = p;
        tracks[i].end = p + track_len;
        tracks[i].pos = p;
        tracks[i].next_tick = 0;
        tracks[i].finished = 0;

        const uint8_t *tp = tracks[i].pos;
        tracks[i].next_tick = read_vlq(&tp, tracks[i].end);
        tracks[i].pos = tp;

        p += track_len;
    }

    current_tick = 0;
    sample_counter = 0;
    us_per_tick = 500000.0 / (double)ticks_per_quarter;
    samples_per_tick = (us_per_tick / 1000000.0) * (double)midi_sample_rate;

    midi_playing = 1;
}

void midi_player_stop(void)
{
    midi_playing = 0;
    current_tick = 0;
    sample_counter = 0;
}

/* Process MIDI events at the current tick for a single track */
static void process_track_events(midi_track_t *track)
{
    static uint8_t running_status[MAX_MIDI_TRACKS];
    int track_idx = (int)(track - tracks);

    while (!track->finished && track->next_tick <= current_tick) {
        if (track->pos >= track->end) {
            track->finished = 1;
            break;
        }

        uint8_t status = *track->pos;
        if (status & 0x80) {
            track->pos++;
            running_status[track_idx] = status;
        } else {
            status = running_status[track_idx];
        }

        uint8_t type = status & 0xF0;
        uint8_t channel = status & 0x0F;

        switch (type) {
        case 0x80: { /* Note Off */
            if (track->pos + 2 > track->end) { track->finished = 1; break; }
            uint8_t note = track->pos[0];
            track->pos += 2;
            opl_player_note_off(channel, note);
            break;
        }
        case 0x90: { /* Note On */
            if (track->pos + 2 > track->end) { track->finished = 1; break; }
            uint8_t note = track->pos[0];
            uint8_t vel = track->pos[1];
            track->pos += 2;
            if (vel == 0)
                opl_player_note_off(channel, note);
            else
                opl_player_note_on(channel, note, vel);
            break;
        }
        case 0xA0: /* Aftertouch */
            if (track->pos + 2 > track->end) { track->finished = 1; break; }
            track->pos += 2;
            break;
        case 0xB0: { /* Control Change */
            if (track->pos + 2 > track->end) { track->finished = 1; break; }
            uint8_t ctrl = track->pos[0];
            uint8_t val = track->pos[1];
            track->pos += 2;
            opl_player_control_change(channel, ctrl, val);
            break;
        }
        case 0xC0: { /* Program Change */
            if (track->pos + 1 > track->end) { track->finished = 1; break; }
            uint8_t prog = track->pos[0];
            track->pos += 1;
            opl_player_program_change(channel, prog);
            break;
        }
        case 0xD0: /* Channel Pressure */
            if (track->pos + 1 > track->end) { track->finished = 1; break; }
            track->pos += 1;
            break;
        case 0xE0: { /* Pitch Bend */
            if (track->pos + 2 > track->end) { track->finished = 1; break; }
            int bend = track->pos[0] | (track->pos[1] << 7);
            track->pos += 2;
            opl_player_pitch_bend(channel, bend);
            break;
        }
        case 0xF0: {
            if (status == 0xFF) { /* Meta event */
                if (track->pos + 1 > track->end) { track->finished = 1; break; }
                uint8_t meta_type = *track->pos++;
                unsigned long meta_len = read_vlq(&track->pos, track->end);

                if (meta_type == 0x51 && meta_len == 3) {
                    /* Set Tempo */
                    uint32_t tempo = ((uint32_t)track->pos[0] << 16) |
                                     ((uint32_t)track->pos[1] << 8) |
                                     track->pos[2];
                    us_per_tick = (double)tempo / (double)ticks_per_quarter;
                    samples_per_tick = (us_per_tick / 1000000.0) * (double)midi_sample_rate;
                } else if (meta_type == 0x2F) {
                    /* End of Track */
                    track->pos += meta_len;
                    track->finished = 1;
                    break;
                }
                track->pos += meta_len;
            } else if (status == 0xF0 || status == 0xF7) {
                /* SysEx */
                unsigned long sysex_len = read_vlq(&track->pos, track->end);
                track->pos += sysex_len;
            } else {
                track->finished = 1;
                break;
            }
            break;
        }
        default:
            track->finished = 1;
            break;
        }

        /* Read next delta time */
        if (!track->finished && track->pos < track->end) {
            const uint8_t *tp = track->pos;
            unsigned long delta = read_vlq(&tp, track->end);
            track->pos = tp;
            track->next_tick = current_tick + delta;
        }
    }
}

void midi_player_tick(int sample_count)
{
    if (!midi_playing) return;

    /* Convert samples to ticks and process events */
    sample_counter += sample_count;

    while (sample_counter >= samples_per_tick) {
        sample_counter -= samples_per_tick;
        current_tick++;

        int all_finished = 1;
        for (int i = 0; i < num_tracks; i++) {
            if (!tracks[i].finished) {
                process_track_events(&tracks[i]);
                if (!tracks[i].finished)
                    all_finished = 0;
            }
        }

        if (all_finished) {
            if (midi_looping && midi_data_ptr) {
                opl_player_all_notes_off();
                midi_player_start();
                return;
            } else {
                midi_playing = 0;
                return;
            }
        }
    }
}
