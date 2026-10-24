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

/* Minimap (docs/11-minimap.md): what the pause map shows, live. Positions
 * are world-plane pixels (the same space as the scroll planes). */
#define GAME_MM_MAX 256
typedef struct {
    int16_t x, y;
    uint8_t obj;               /* index in the pause map's list */
    uint8_t main;              /* 1: a mission objective (shown at all times) */
} game_mm_mark;
typedef struct {
    uint32_t level;            /* identifies the level (terrain cache key) */
    int count, sel;            /* entries in the pause map's list; selected one */
    int n;
    game_mm_mark m[GAME_MM_MAX];
    int have_player, px, py;
    int have_home, hx, hy;     /* home base (Desert's frigate), if shown */
    int x0, y0, x1, y1;        /* visible world: camera range plus one screen */
    int shift;                 /* pause map scale: map pixel = world >> shift */
    int campaign;              /* 1-based campaign number (0 = unknown) */
} game_minimap;
/* Where the pause map draws the world, for capturing its terrain picture. */
typedef struct {
    int origin_x, origin_y;    /* screen position of world (0,0), 1x pixels */
    int shift;                 /* default scale (game_minimap.shift overrides) */
    uint32_t sentinel;         /* RAM word the pause map sets when it opens ... */
    uint16_t sentinel_val;     /* ... to this value */
    uint32_t page;             /* RAM word: pause screen page ... */
    uint16_t page_map;         /* ... value for the map page */
} game_mm_view;

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
    /* Minimap: fill out (0 if not on a level); select an entry of the
     * pause map's list (also changes the pause map's selection); name of an
     * entry (returns 0 normal, 1 locked, 2 completed; -1 none). */
    int (*minimap)(game_minimap *out);
    void (*minimap_select)(int index);
    int (*minimap_name)(int index, char *buf, int n);
    /* All units of a list entry's type (out->m, out->n), whether or not it is selected. */
    int (*minimap_units)(int index, game_minimap *out);
    const game_mm_view *mm_view;
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
