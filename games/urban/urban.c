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

/* Minimap: the pause map's list and icon builder ($0026BC, $00276C,
 * $002532), its world-to-map transform ($0024CE: world >> [$FF0616] +
 * (72, 32)); the scale per campaign comes from $0BBCA2 ($0037E0). */
static const strike_mm_cfg mcfg = {
    1, 0xFF104E, 0xFF1058, 0xFF1090, 0xFF10EE, 1,
    0xFF55DE, 0xFF130C, 0xFFFB56, 0xFF130E,
    0xFF49B4, 0x22, 0, 0, 0, 0,
    0xFF4542, 0xFF1382,
    0x0C360E, 0xFF13E4, 1, 0x0BBCA2,
};
static const game_mm_view mview = { 72, 32, 5, 0xFF049A, 8, 0xFF105A, 2 };
static int minimap(game_minimap *o) { return strike_minimap(&mcfg, o); }
static void minimap_select(int i) { strike_minimap_select(&mcfg, i); }
static int minimap_name(int i, char *b, int n) { return strike_minimap_name(&mcfg, i, b, n); }
static int minimap_units(int i, game_minimap *o) { return strike_minimap_units(&mcfg, i, o); }

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
    .minimap = minimap,
    .minimap_select = minimap_select,
    .minimap_name = minimap_name,
    .minimap_units = minimap_units,
    .mm_view = &mview,
};
