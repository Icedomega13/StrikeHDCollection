/* settings.ini load/save. See settings.h. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "settings.h"
#include "crt.h"

settings_t g_set;
cheats_t g_cheat;
/* At TV pixel shape (320 px = 4:3): 384 px = 16:10, 432 = 16:9, 560 = 21:9, 864 = 32:9 */
const int ws_ext_px[WS_COUNT] = { 0, 32, 56, 120, 272, 0 };

void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->window_mode = WIN_WINDOWED;
    s->fullscreen_mode = -1;
    s->window_w = 896; s->window_h = 672;
    s->vsync = 1;
    s->aspect = ASPECT_4_3;
#if defined(__linux__)
    s->renderer = RENDERER_OPENGL;
#else
    s->renderer = RENDERER_AUTO;
#endif
    s->scaling = SCALE_SHARP;
    s->scanlines = SCAN_OFF;
    s->crt_glow = 1;
    s->crt_antimoire = 1;
    s->crt_dither = 1;
    s->sprite_flicker = 1;
    s->widescreen = WS_OFF;
    s->hd_graphics = 1;
    s->status_hud = 1;
    s->minimap = 1;
    s->minimap_size = 24;
    s->hud_scale = 100;
    s->minimap_pos = 1;            /* lower right: Desert writes its messages lower left */
    s->launcher_intros = 1;
    s->continue_prompt = 1;
    s->display_sync = 1;
    s->difficulty = 1;
    s->rstick_strafe = 1;
    s->hd_style = 1;
    s->rewind = 1;
    s->autosave = 1;
    s->rumble = 70;
    s->volume = 80;
    s->fm_level = 100; s->psg_level = 100;
    s->mute_unfocused = 0;
    s->edition = EDITION_REMASTER;
    s->amiga_sfx = 1;
    s->amiga_vol = 100;
    s->amiga_music = 1;
    s->amiga_music_vol = 100;
    s->amiga_intro = 1;
    s->amiga_extras = 1;
    s->amiga_cutscenes = 1;
    s->ff_speed = 4;
    s->pause_unfocused = 0;
}

/* name, field, min, max */
#define FIELDS(X) \
    X(window_mode, 0, WIN_MODE_COUNT - 1) X(fullscreen_mode, -1, 999) \
    X(window_w, 320, 16384) X(window_h, 224, 16384) X(vsync, 0, 1) X(show_fps, 0, 2) X(renderer, 0, RENDERER_COUNT - 1) \
    X(aspect, 0, ASPECT_COUNT - 1) X(scaling, 0, SCALE_COUNT - 1) X(scanlines, 0, SCAN_COUNT - 1) \
    X(crt, 0, CRT_PRESET_COUNT - 1) X(crt_flat, 0, 1) X(crt_glow, 0, 1) X(crt_wires, 0, 1) X(crt_antimoire, 0, 1) X(crt_dither, 0, 1) X(crt_mask, 0, 2) \
    X(sprite_flicker, 0, 1) X(widescreen, 0, WS_COUNT - 1) \
    X(volume, 0, 100) X(fm_level, 0, 150) X(psg_level, 0, 150) X(mute_unfocused, 0, 1) \
    X(ff_speed, 2, 8) X(pause_unfocused, 0, 1) X(hd_graphics, 0, 1) X(hd_dump, 0, 1) X(authentic_speed, 0, 1) X(display_sync, 0, 1) X(status_hud, 0, 1) X(launcher_intros, 0, 1) X(continue_prompt, 0, 1) X(difficulty, 0, 2) X(strafe_speed, 0, 2) X(rstick_strafe, 0, 1) X(minimap_pos, 0, 3) X(hud_pos, 0, 3) X(hud_scale, 50, 200) X(minimap_threats, 0, 1) \
    X(minimap, 0, 1) X(minimap_size, 0, 60) X(hd_style, 0, 1) \
    X(rewind, 0, 1) X(autosave, 0, 1) X(rumble, 0, 100) \
    X(edition, 0, EDITION_COUNT - 1) X(amiga_sfx, 0, 1) X(amiga_vol, 0, 150) X(amiga_music, 0, 1) X(amiga_music_vol, 0, 150) X(amiga_intro, 0, 1) X(amiga_extras, 0, 1) X(amiga_cutscenes, 0, 1)

int settings_load(const char *path)
{
    settings_defaults(&g_set);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64];
        int v;
        if (line[0] == '#' || line[0] == '[') continue;
        if (sscanf(line, " %63[a-z_] = %d", key, &v) != 2) continue;
#define X(n, lo, hi) if (!strcmp(key, #n)) g_set.n = v < lo ? lo : v > hi ? hi : v;
        FIELDS(X)
#undef X
    }
    fclose(f);
    return 1;
}

int settings_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# Desert Strike HD settings. Change these from the pause menu (Esc / Back).\n[settings]\n");
    settings_write(f, "");
    fclose(f);
    return 1;
}

void settings_write(FILE *f, const char *prefix)
{
#define X(n, lo, hi) fprintf(f, "%s%s = %d\n", prefix, #n, g_set.n);
    FIELDS(X)
#undef X
}
