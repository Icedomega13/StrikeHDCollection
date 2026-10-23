/*
 * A game in the collection. Each game's recompiled code is generated with
 * its own symbol prefix (ds_, js_, us_) and exposes one of these.
 */
#ifndef DS_GAME_H
#define DS_GAME_H
#include "runtime/cpu.h"
#include "hw/vdp_render.h"

/* Player status for the optional on-screen HUD. */
typedef struct {
    int armor, armor_max;
    int fuel, fuel_max;
    int load, load_max;
    int lives;
    int ammo[3];               /* guns, hydras, hellfires; -1 = unknown */
} game_hud;

typedef struct game_module {
    const char *id;            /* "desert" */
    const char *title;
    int year;
    const char *rom_sha1;      /* accepted dump */
    recomp_fn (*lookup)(uint32_t pc);
    /* Widescreen. ws_max_ext: largest supported extra width per side (px,
     * multiple of 8; 0 = no widescreen). ws_active(): 1 while the current
     * screen is the scrolling playfield that may be drawn wider; other
     * screens (menus, maps) are pillarboxed. */
    int ws_max_ext;
    int (*ws_active)(void);
    /* Ultrawide (views wider than the game's 512 px plane window): fill
     * world-space planes for the current frame; 0 if unavailable. Only
     * called while ws_active(). Extensions beyond ws_max_ext rely on it. */
    int (*ws_world)(vdp_world_plane out[2], int ext_left, int ext_right);
    /* Current player status for the HUD; 0 if not available. */
    int (*hud)(game_hud *out);
    /* Graphics extractor: one step of the map tour (see strike_tour). */
    int (*tour)(int reset, int playfield);
    /* Cheats, called once per frame on the playfield. */
    void (*cheats)(int god, int fuel, int ammo, int lives);
    int campaigns;             /* number of campaigns (campaign select) */
} game_module;

extern const game_module *const g_games[];
extern const int g_game_count;

const game_module *game_find_sha1(const char *sha1);
const game_module *game_find_id(const char *id);
/* Identify the loaded ROM (g_rom) by SHA-1; NULL if unsupported. */
const game_module *game_identify_rom(void);
/* Reset hardware + CPU and point dispatch at this game's code. */
void game_boot(const game_module *g);

#endif
