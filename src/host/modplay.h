/*
 * ProTracker (M.K.) module player for the Amiga music
 * (docs/13-amiga-ultimate.md). 4 channels, Amiga hard stereo softened,
 * PAL timing, the standard effects.
 */
#ifndef DS_MODPLAY_H
#define DS_MODPLAY_H
#include <stdint.h>

/* Start playing a module (the data must stay valid while it plays).
 * loop: restart at the end; otherwise stop. Returns 0 if not a module. */
int  mod_start(const uint8_t *data, int len, int out_rate, int loop);
void mod_stop(void);
int  mod_playing(void);
void mod_set_volume(float v);           /* 0..2 */
/* Add the music into interleaved stereo frames. */
void mod_mix(int16_t *stereo, int frames);

#endif
