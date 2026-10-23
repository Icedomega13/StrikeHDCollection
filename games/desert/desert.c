#include "strike_engine.h"

recomp_fn ds_recomp_lookup(uint32_t pc);   /* generated/desert/recomp_table.c */

/*
 * The scrolling playfield is on screen when plane B's scroll registers hold
 * the game's layer-B camera ($FF1AF2 x, $FF1AF0 y; written by $00BCBC) and
 * that routine has run recently. The status/map screen resets scroll to 0
 * while the camera keeps its flight value, so it fails the match.
 * (The 600-frame window just keeps menus with a stale (0,0) camera out.)
 */
static int ds_ws_active(void) { return strike_ws_active(0x1AF2, 0x1AF0, 1); }

/* Ultrawide painter addresses (see strike_engine.h, docs/07-widescreen.md) */
static const strike_cfg wcfg = { 0xFF24BA, 0, 0, 0xFF255C, 0xFF1AA8, 0xFF1A50, 1, 0xFF2508, 0x01, 0, 0x34 };
static int ws_world(vdp_world_plane out[2], int l, int r) { return strike_ws_world(&wcfg, out, l, r); }

/* Player status for the HUD (docs/10-status-hud.md) */
static const strike_hud_cfg hcfg = { 0xFF251C, 0x32, 0xFF1040, 0xFF10AA, 0xFF10BC, 0xFF10B2, 0xFF10B6, 0, 6, 0x3A, 0x28, 0xFF1AA8 };
static int hud(game_hud *h) { return strike_hud(&hcfg, h); }
static int tour(int reset, int play) { return strike_tour(&hcfg, reset, play); }
static void cheats(int g, int f, int a, int l) { strike_cheats(&hcfg, g, f, a, l); }

/* Minimap (docs/11-minimap.md): the pause map's list and icon builder ($003EBC, $003F6C,
 * $003D1A), its world-to-map transform ($003CCC: world/32 + (80, 40)). */
static const strike_mm_cfg mcfg = {
    0, 0xFF1030, 0xFF103A, 0xFF1070, 0xFF10C2, 2,
    0xFF2718, 0xFF1A34, 0xFFFBA8, 0xFF1A36,
    0xFF251C, 0x28, 0xFF268A, 0xFF1A26, 0xFF269C, 0xFF2522,
    0xFF24BA, 0xFF1AA8,
    0x03018C, 0xFF1B10, 2,     /* list and mask are set when the pause opens ($005094) */
};
static const game_mm_view mview = { 80, 40, 5, 0xFF0286, 8, 0xFF103C, 2 };
static int minimap(game_minimap *o) { return strike_minimap(&mcfg, o); }
static void minimap_select(int i) { strike_minimap_select(&mcfg, i); }
static int minimap_name(int i, char *b, int n) { return strike_minimap_name(&mcfg, i, b, n); }

const game_module game_desert = {
    .id = "desert",
    .title = "Desert Strike: Return to the Gulf",
    .year = 1992,
    .rom_sha1 = "d7e7d8c358eb845b84fb08f904cc0b95d0a4053d",
    .lookup = ds_recomp_lookup,
    .ws_max_ext = 272,              /* > 56 px: planes painted from map data, 16-bit sprite X */
    .ws_active = ds_ws_active,
    .ws_world = ws_world,
    .hud = hud,
    .tour = tour,
    .cheats = cheats,
    .campaigns = 4,
    .minimap = minimap,
    .minimap_select = minimap_select,
    .minimap_name = minimap_name,
    .mm_view = &mview,
};
