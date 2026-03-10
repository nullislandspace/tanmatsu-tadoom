/* opl_player.h -- OPL2 FM synthesis music player using GENMIDI lump */

#ifndef __OPL_PLAYER_H__
#define __OPL_PLAYER_H__

#include <stdint.h>

void opl_player_init(int sample_rate);
void opl_player_shutdown(void);
int  opl_player_load_genmidi(const void *data, int len);
void opl_player_note_on(int channel, int note, int velocity);
void opl_player_note_off(int channel, int note);
void opl_player_program_change(int channel, int program);
void opl_player_control_change(int channel, int controller, int value);
void opl_player_pitch_bend(int channel, int bend);
void opl_player_all_notes_off(void);
void opl_player_render(int16_t *buffer, int num_samples);

#endif
