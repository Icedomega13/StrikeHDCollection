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
};
