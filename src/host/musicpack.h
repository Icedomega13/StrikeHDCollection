/*
 * Custom music (docs/15-custom-music.md): the player's own .ogg / .wav files
 * in <settings folder>/music/<game id>/ replace a game's songs. A file is
 * picked by the song's key ("117 title.ogg", "100117.wav") or by its name
 * ("title.ogg", "briefing.ogg"). Decoded whole when it starts, resampled to
 * the mixer's rate, looped unless the caller says otherwise.
 */
#ifndef DS_MUSICPACK_H
#define DS_MUSICPACK_H
#include <stdint.h>
#include <stddef.h>

/* The file for a song in dir (with a trailing separator): by key first,
 * then by name. Returns 1 and the path in out. */
int  musicpack_find(const char *dir, int key, const char *name, char *out, size_t cap);
int  musicpack_start(const char *path, int out_rate, int loop);
void musicpack_stop(void);
int  musicpack_playing(void);
void musicpack_set_volume(float v);
/* Add the music into interleaved stereo frames. */
void musicpack_mix(int16_t *stereo, int frames);

#endif
