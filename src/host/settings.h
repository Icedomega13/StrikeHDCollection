/*
 * User settings (settings.ini in the settings folder). Everything here can
 * be changed from the pause menu and is applied immediately.
 */
#ifndef DS_SETTINGS_H
#define DS_SETTINGS_H

/* Genesis: everything as on the Mega Drive. Amiga: every Amiga asset that
 * is available. Remaster: what the community preferred from each version
 * (docs/13-amiga-ultimate.md). Custom: the options set one by one. */
enum { EDITION_GENESIS, EDITION_AMIGA, EDITION_REMASTER, EDITION_CUSTOM, EDITION_COUNT };
enum { WIN_WINDOWED, WIN_BORDERLESS, WIN_EXCLUSIVE, WIN_MODE_COUNT };
enum { RENDERER_AUTO, RENDERER_OPENGL, RENDERER_SOFTWARE, RENDERER_COUNT };
enum { ASPECT_4_3, ASPECT_SQUARE, ASPECT_STRETCH, ASPECT_COUNT };
enum { SCALE_SHARP, SCALE_INTEGER, SCALE_SMOOTH, SCALE_NEAREST, SCALE_COUNT };
enum { SCAN_OFF, SCAN_LIGHT, SCAN_MEDIUM, SCAN_STRONG, SCAN_COUNT };
enum { WS_OFF, WS_16_10, WS_16_9, WS_21_9, WS_32_9, WS_AUTO, WS_COUNT };
extern const int ws_ext_px[WS_COUNT];   /* extra pixels per side (WS_AUTO: computed from the window) */

typedef struct {
    /* display */
    int window_mode;          /* WIN_* */
    int fullscreen_mode;      /* index into the display's mode list, -1 = desktop */
    int window_w, window_h;   /* last windowed size */
    int vsync;
    int show_fps;
    int renderer;             /* RENDERER_*; selected at startup */
    /* picture */
    int aspect;               /* ASPECT_* */
    int scaling;              /* SCALE_* */
    int scanlines;            /* SCAN_* */
    int sprite_flicker;       /* 1 = original per-line sprite limits */
    int widescreen;           /* WS_* */
    int hd_graphics;          /* use HD packs when present */
    int status_hud;           /* armour/fuel/load/lives overlay */
    int minimap;              /* pause-map overlay in the lower left */
    int minimap_size;         /* 0 small, 1 medium, 2 large */
    /* audio */
    int volume;               /* 0-100 */
    int fm_level, psg_level;  /* 0-150 % */
    int mute_unfocused;
    int edition;              /* EDITION_*: presets for the per-feature Genesis/Amiga options */
    int amiga_sfx;            /* use mapped Amiga samples for sound effects */
    int amiga_vol;            /* Amiga effects level 0-150 % */
    int amiga_music;          /* play the Amiga music (modules) where one is assigned */
    int amiga_music_vol;      /* Amiga music level 0-150 % */
    int amiga_intro;          /* Desert Strike opens with the Amiga title photographs */
    int amiga_cutscenes;      /* cutscene pictures shown as their animated Amiga versions */
    int amiga_extras;         /* Amiga sounds for moments the Genesis is silent (objective jingle, warnings) */
    /* system */
    int ff_speed;             /* fast-forward multiplier 2-8 */
    int pause_unfocused;
    int authentic_speed;      /* 1 = original CPU speed (slowdown) */
    int launcher_intros;      /* live intros behind the game picker */
    int minimap_pos;          /* 0 lower left, 1 lower right, 2 upper right */
    int minimap_threats;      /* show enemy air defences near the helicopter */
    int continue_prompt;      /* Continue / Load Game / New Game when a game with saves is picked */
    int hd_dump;              /* write on-screen graphics for pack makers */
    int rewind;               /* keep a rewind history (left trigger / Backspace) */
    int autosave;             /* save on mission start and objective completion */
    int rumble;               /* gamepad rumble strength 0-100 (0 = off) */
    int hd_style;             /* Build HD Pack: 0 xBRZ, 1 AI (Real-ESRGAN, when installed) */
} settings_t;

extern settings_t g_set;

/* Cheats (pause menu > Cheats). Not saved: every session starts clean. */
typedef struct {
    int god, fuel, ammo, lives;
    int campaign;             /* 0-based campaign a new game starts at */
} cheats_t;
extern cheats_t g_cheat;

void settings_defaults(settings_t *s);
int  settings_load(const char *path);   /* 1 if the file existed */
int  settings_save(const char *path);

#endif
