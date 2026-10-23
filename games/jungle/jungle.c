#include "strike_engine.h"

recomp_fn js_recomp_lookup(uint32_t pc);   /* generated/jungle/recomp_table.c */

/* Playfield on screen: plane A scroll = layer camera at $1386/$1384
 * (written by the scroll writer, marked by a patch in hints.json). */
static int ws_active(void) { return strike_ws_active(0x1386, 0x1384, 0); }

/* Ultrawide painter addresses (see strike_engine.h, docs/07-widescreen.md) */
static const strike_cfg wcfg = { 0xFF283C, 1, 0x5C, 0xFF2982, 0xFF133C, 0xFF12E4, 1, 0xFF29CC, 0x48, 1, 0x0001 };
static int ws_world(vdp_world_plane out[2], int l, int r) { return strike_ws_world(&wcfg, out, l, r); }

/* Player status for the HUD (docs/10-status-hud.md) */
static const strike_hud_cfg hcfg = { 0xFF29E8, 0x2C, 0xFF1054, 0xFF10C6, 0xFF10D4, 0xFF10CA, 0xFF10CE, 0xFF122B, 0, 0x34, 0x22, 0xFF133C };
static int hud(game_hud *h) { return strike_hud(&hcfg, h); }
static int tour(int reset, int play) { return strike_tour(&hcfg, reset, play); }
static void cheats(int g, int f, int a, int l) { strike_cheats(&hcfg, g, f, a, l); }

const game_module game_jungle = {
    .id = "jungle",
    .title = "Jungle Strike",
    .year = 1993,
    .rom_sha1 = "7a7568e39341b1bb218280ee05c2b37c273317b5",
    .lookup = js_recomp_lookup,
    .ws_max_ext = 272,              /* > 56 px: planes painted from map data, 16-bit sprite X */
    .ws_active = ws_active,
    .ws_world = ws_world,
    .hud = hud,
    .tour = tour,
    .cheats = cheats,
    .campaigns = 9,
};
