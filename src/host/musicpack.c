/* Custom music: the player's own audio files in place of a game's songs.
 * See musicpack.h and docs/15-custom-music.md. */
#include "musicpack.h"
#include <SDL3/SDL.h>
#include <string.h>

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

static struct {
    int16_t *pcm;          /* stereo, at out_rate */
    int frames, pos, loop, rate;
    float vol;
    char path[1200];
    int playing;
} mp = { .vol = 1.0f };

/* "Game Over_3.ogg" and "game-over.ogg" both match "game over". */
static int same_words(const char *a, const char *b)
{
    for (;;) {
        while (*a == ' ' || *a == '_' || *a == '-') a++;
        while (*b == ' ' || *b == '_' || *b == '-') b++;
        if (!*a || !*b) return !*a && !*b;
        if (SDL_tolower((unsigned char)*a) != SDL_tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
}

typedef struct { int key; const char *name; char *found; size_t cap; int by_key; } search;

static SDL_EnumerationResult SDLCALL find_cb(void *ud, const char *dir, const char *fname)
{
    search *s = ud;
    const char *dot = SDL_strrchr(fname, '.');
    if (!dot || (SDL_strcasecmp(dot, ".ogg") && SDL_strcasecmp(dot, ".wav"))) return SDL_ENUM_CONTINUE;
    char stem[256];
    size_t n = (size_t)(dot - fname);
    if (n >= sizeof stem) return SDL_ENUM_CONTINUE;
    SDL_memcpy(stem, fname, n); stem[n] = 0;
    /* by number: the stem starts with the key and then a non-digit (or ends) */
    if (SDL_isdigit((unsigned char)stem[0])) {
        char *end = NULL;
        long k = SDL_strtol(stem, &end, 10);
        if (k == s->key && (!end || !SDL_isdigit((unsigned char)*end))) {
            SDL_snprintf(s->found, s->cap, "%s%s", dir, fname);
            s->by_key = 1;
            return SDL_ENUM_SUCCESS;
        }
    }
    /* by name: the whole stem names the song */
    if (!s->found[0] && s->name && s->name[0] && same_words(stem, s->name))
        SDL_snprintf(s->found, s->cap, "%s%s", dir, fname);
    return SDL_ENUM_CONTINUE;
}

int musicpack_find(const char *dir, int key, const char *name, char *out, size_t cap)
{
    if (!dir || !out || cap < 2) return 0;
    out[0] = 0;
    search s = { key, name, out, cap, 0 };
    SDL_EnumerateDirectory(dir, find_cb, &s);
    return out[0] != 0;
}

static int16_t *to_stereo_s16(const void *src, int bytes, const SDL_AudioSpec *spec, int out_rate, int *frames)
{
    SDL_AudioSpec dst = { SDL_AUDIO_S16, 2, out_rate };
    Uint8 *out = NULL;
    int out_len = 0;
    if (!SDL_ConvertAudioSamples(spec, (const Uint8 *)src, bytes, &dst, &out, &out_len)) return NULL;
    *frames = out_len / 4;
    return (int16_t *)out;
}

int musicpack_start(const char *path, int out_rate, int loop)
{
    if (mp.playing && !SDL_strcmp(mp.path, path)) { mp.loop = loop; return 1; }   /* already on: carry on */
    musicpack_stop();
    int frames = 0;
    int16_t *pcm = NULL;
    const char *dot = SDL_strrchr(path, '.');
    if (dot && !SDL_strcasecmp(dot, ".ogg")) {
        size_t n = 0;
        void *file = SDL_LoadFile(path, &n);
        if (!file) return 0;
        int ch = 0, rate = 0;
        short *dec = NULL;
        int samples = stb_vorbis_decode_memory((const unsigned char *)file, (int)n, &ch, &rate, &dec);
        SDL_free(file);
        if (samples <= 0 || !dec) return 0;
        SDL_AudioSpec spec = { SDL_AUDIO_S16, ch, rate };
        pcm = to_stereo_s16(dec, samples * ch * 2, &spec, out_rate, &frames);
        free(dec);
    } else {
        SDL_AudioSpec spec;
        Uint8 *buf = NULL;
        Uint32 len = 0;
        if (!SDL_LoadWAV(path, &spec, &buf, &len)) return 0;
        pcm = to_stereo_s16(buf, (int)len, &spec, out_rate, &frames);
        SDL_free(buf);
    }
    if (!pcm || frames <= 0) { SDL_free(pcm); return 0; }
    mp.pcm = pcm; mp.frames = frames; mp.pos = 0; mp.loop = loop; mp.rate = out_rate;
    SDL_strlcpy(mp.path, path, sizeof mp.path);
    mp.playing = 1;
    return 1;
}

void musicpack_stop(void)
{
    mp.playing = 0;
    SDL_free(mp.pcm);
    mp.pcm = NULL;
    mp.frames = mp.pos = 0;
    mp.path[0] = 0;
}

int musicpack_playing(void) { return mp.playing; }
void musicpack_set_volume(float v) { mp.vol = v; }

void musicpack_mix(int16_t *out, int frames)
{
    if (!mp.playing || !mp.pcm) return;
    for (int i = 0; i < frames; i++) {
        if (mp.pos >= mp.frames) {
            if (!mp.loop) { musicpack_stop(); return; }
            mp.pos = 0;
        }
        for (int c = 0; c < 2; c++) {
            int v = out[i * 2 + c] + (int)(mp.pcm[mp.pos * 2 + c] * mp.vol);
            out[i * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        mp.pos++;
    }
}
