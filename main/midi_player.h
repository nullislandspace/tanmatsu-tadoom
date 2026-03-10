/* midi_player.h -- Minimal MIDI file sequencer for TinySoundFont */

#ifndef __MIDI_PLAYER_H__
#define __MIDI_PLAYER_H__

#include "tsf.h"

void midi_player_init(void);
void midi_player_load(const uint8_t *midi_data, int midi_len, int looping);
void midi_player_start(tsf *sf);
void midi_player_stop(void);
void midi_player_tick(tsf *sf, int sample_count);

#endif
