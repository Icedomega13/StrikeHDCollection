/*
 * A small mixer for replacement sound effects (the Amiga samples), added
 * to the emulated sound output. Each voice belongs to a key (the game's
 * effect id): starting a key again restarts it, like a sound channel.
 */
#ifndef DS_SFXMIX_H
#define DS_SFXMIX_H
#include <stdint.h>

void sfx_mix_init(int out_rate);
void sfx_play(int key, const int8_t *pcm, int len, int rate, float vol);
void sfx_stop_all(void);
/* A looping voice (the rotor): starts it, or updates its level and pitch if
 * it is already running. sfx_stop fades a key's voice out. */
void sfx_loop(int key, const int8_t *pcm, int len, int rate, float vol);
void sfx_stop(int key);
/* The same, placed in the stereo field (-1 left .. 1 right). */
void sfx_loop_pan(int key, const int8_t *pcm, int len, int rate, float vol, float pan);
/* Add active voices into interleaved stereo frames. */
void sfx_mix(int16_t *stereo, int frames);

#endif
