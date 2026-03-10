/* midi_player.h -- Minimal MIDI file sequencer for OPL music */

#ifndef __MIDI_PLAYER_H__
#define __MIDI_PLAYER_H__

#include <stdint.h>

void midi_player_set_sample_rate(int rate);
void midi_player_init(void);
void midi_player_load(const uint8_t *midi_data, int midi_len, int looping);
void midi_player_start(void);
void midi_player_stop(void);
void midi_player_tick(int sample_count);

#endif
