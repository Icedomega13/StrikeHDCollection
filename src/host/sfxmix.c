/* Replacement sound-effect mixer. See sfxmix.h. */
#include "sfxmix.h"
#include <string.h>
#include <SDL3/SDL.h>

#define VOICES 8

typedef struct {
    int key;
    const int8_t *pcm;
    int len;
    uint32_t pos, step;        /* 16.16 fixed point */
    int vol;                   /* 0..256 */
    int age;
    int loop;                  /* restart at the end until stopped */
    int pan_l, pan_r;          /* 0..256 per side (256/256 = centre, full level) */
    int fade;                  /* > 0: fading out, frames left */
} voice;

static voice v[VOICES];
static int out_rate = 48000, clock_;

void sfx_mix_init(int rate) { out_rate = rate > 0 ? rate : 48000; sfx_stop_all(); }
void sfx_stop_all(void) { memset(v, 0, sizeof v); }

void sfx_play(int key, const int8_t *pcm, int len, int rate, float vol)
{
    if (!pcm || len <= 0 || rate <= 0) return;
    int slot = -1;
    for (int i = 0; i < VOICES; i++) if (v[i].pcm && v[i].key == key) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < VOICES; i++) if (!v[i].pcm) { slot = i; break; }
    if (slot < 0) {                                   /* steal the oldest */
        slot = 0;
        for (int i = 1; i < VOICES; i++) if (v[i].age < v[slot].age) slot = i;
    }
    voice *o = &v[slot];
    o->key = key; o->pcm = pcm; o->len = len; o->pos = 0;
    o->step = (uint32_t)(((uint64_t)rate << 16) / (uint64_t)out_rate);
    if (vol < 0) vol = 0;
    if (vol > 2) vol = 2;
    o->vol = (int)(vol * 256);
    o->age = ++clock_;
    o->loop = 0;
    o->fade = 0;
    o->pan_l = o->pan_r = 256;
}

static void set_pan(voice *o, float pan)
{
    pan = SDL_clamp(pan, -1.0f, 1.0f);
    o->pan_l = (int)(256 * (pan > 0 ? 1 - pan : 1));
    o->pan_r = (int)(256 * (pan < 0 ? 1 + pan : 1));
}

void sfx_loop(int key, const int8_t *pcm, int len, int rate, float vol)
{
    sfx_loop_pan(key, pcm, len, rate, vol, 0);
}

void sfx_loop_pan(int key, const int8_t *pcm, int len, int rate, float vol, float pan)
{
    for (int i = 0; i < VOICES; i++)
        if (v[i].pcm == pcm && v[i].key == key && v[i].loop) {   /* already going: just follow the level, pitch and pan */
            v[i].vol = (int)(SDL_clamp(vol, 0.0f, 2.0f) * 256);
            v[i].step = (uint32_t)(((uint64_t)rate << 16) / (uint64_t)out_rate);
            v[i].fade = 0;
            set_pan(&v[i], pan);
            return;
        }
    for (int i = 0; i < VOICES; i++) if (v[i].pcm && v[i].key == key && v[i].loop) v[i].pcm = NULL;   /* another sample now */
    sfx_play(key, pcm, len, rate, vol);
    for (int i = 0; i < VOICES; i++) if (v[i].pcm == pcm && v[i].key == key) { v[i].loop = 1; v[i].age = 1 << 30; set_pan(&v[i], pan); }
}

void sfx_stop(int key)
{
    for (int i = 0; i < VOICES; i++)
        if (v[i].pcm && v[i].key == key && !v[i].fade) v[i].fade = out_rate / 20;   /* 50 ms */
}

void sfx_mix(int16_t *s, int frames)
{
    for (int i = 0; i < VOICES; i++) {
        voice *o = &v[i];
        if (!o->pcm) continue;
        for (int f = 0; f < frames; f++) {
            uint32_t ip = o->pos >> 16;
            if ((int)ip >= o->len - 1) {
                if (!o->loop) { o->pcm = NULL; break; }
                o->pos -= (uint32_t)(o->len - 1) << 16;
                ip = o->pos >> 16;
            }
            int a = o->pcm[ip], b = o->pcm[ip + 1], fr = (int)(o->pos & 0xFFFF);
            int x = (a * 65536 + (b - a) * fr) >> 8;      /* 8-bit -> 16-bit, interpolated */
            x = x * o->vol >> 8;
            if (o->fade) {
                x = x * o->fade / (out_rate / 20);
                if (--o->fade <= 0) { o->pcm = NULL; break; }
            }
            for (int c = 0; c < 2; c++) {
                int y = s[f * 2 + c] + (x * (c ? o->pan_r : o->pan_l) >> 8);
                s[f * 2 + c] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
            }
            o->pos += o->step;
        }
    }
}
