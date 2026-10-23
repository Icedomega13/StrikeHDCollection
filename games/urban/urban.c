#include "strike_engine.h"

recomp_fn us_recomp_lookup(uint32_t pc);   /* generated/urban/recomp_table.c */

/* Playfield on screen: plane A scroll = layer camera at $13CC/$13CA
 * (written by the scroll writer, marked by a patch in hints.json). */
static int ws_active(void) { return strike_ws_active(0x13CC, 0x13CA, 0); }

/* Ultrawide painter addresses (see strike_engine.h, docs/07-widescreen.md) */
static const strike_cfg wcfg = { 0xFF4542, 1, 0x68, 0xFF4690, 0xFF1382, 0xFF132A, 1, 0xFF4998, 0x48, 1, 0x0001 };
static int ws_world(vdp_world_plane out[2], int l, int r) { return strike_ws_world(&wcfg, out, l, r); }

/* Player status for the HUD (docs/10-status-hud.md) */
static const strike_hud_cfg hcfg = { 0xFF49B4, 0x2C, 0xFF1060, 0xFF10D6, 0xFF10E8, 0xFF10DA, 0xFF10DE, 0xFF1245, 0, 0x34, 0x22, 0xFF1382 };
static int hud(game_hud *h) { return strike_hud(&hcfg, h); }
static int tour(int reset, int play) { return strike_tour(&hcfg, reset, play); }
static void cheats(int g, int f, int a, int l) { strike_cheats(&hcfg, g, f, a, l); }

const game_module game_urban = {
    .id = "urban",
    .title = "Urban Strike",
    .year = 1994,
    .rom_sha1 = "897a8f6a08d6d7d6e5316f8047532b3e4603e705",
    .lookup = us_recomp_lookup,
    .ws_max_ext = 272,              /* > 56 px: planes painted from map data, 16-bit sprite X */
    .ws_active = ws_active,
    .ws_world = ws_world,
    .hud = hud,
    .tour = tour,
    .cheats = cheats,
    .campaigns = 10,
};
