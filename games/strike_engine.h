/*
 * Helpers shared by the three Strike games, which use the same map engine
 * (layer records with the camera at +$48/+$4A, see docs/07-widescreen.md).
 */
#ifndef STRIKE_ENGINE_H
#define STRIKE_ENGINE_H
#include "game.h"

/*
 * Widescreen playfield test: the scroll registers of `plane` (0 = A, 1 = B)
 * hold the camera of the layer record whose x/y words are at cam_x/cam_y
 * (RAM offsets), within a small tolerance for camera movement between the
 * scroll write and vblank.
 *
 * The match alone is not enough: a menu can show scroll 0 while a stale
 * camera is also 0. So the playfield is latched on when the match holds in a
 * frame where the game's scroll writer ran (g_ws_flight_frame, set by a
 * patch), and stays on while the match holds, even when the camera stops
 * and the writer stops being called. Any frame without the match (the
 * status/map screens reset scroll, menus, cutscenes) turns it off.
 */
static inline int strike_ws_active(uint16_t cam_x, uint16_t cam_y, int plane)
{
    static int latched;
    static uint64_t last_frame;
    if (g_frame < last_frame) latched = 0;          /* reset or state load */
    last_frame = g_frame;
    uint32_t hs = ((g_vreg[13] & 0x3F) << 10) + plane * 2;
    uint16_t hscroll = (uint16_t)(g_vram[hs & 0xFFFF] << 8 | g_vram[(hs + 1) & 0xFFFF]);
    uint16_t cx = (uint16_t)(g_ram[cam_x] << 8 | g_ram[cam_x + 1]);
    uint16_t cy = (uint16_t)(g_ram[cam_y] << 8 | g_ram[cam_y + 1]);
    int dx = (int)((hscroll + cx) & 0x3FF), dy = (int)((g_vsram[plane] - cy) & 0x3FF);
    if (dx > 512) dx = 1024 - dx;
    if (dy > 512) dy = 1024 - dy;
    if (dx > 24 || dy > 24 || !(g_vreg[1] & 0x40)) { latched = 0; return 0; }
    if (g_ws_flight_frame && g_ws_flight_frame + 2 >= g_frame) latched = 1;
    return latched;
}

/* Per-game addresses for the ultrawide painter (games/strike_world.c). */
typedef struct {
    uint32_t level_ptr;      /* RAM: pointer to the level descriptor */
    int ground_kind;         /* 0: Desert 64x64 sectors (+$62); 1: 32x16 blocks */
    uint32_t ground_tab;     /* kind 1: offset of the block table pointer */
    uint32_t tileset_ptr;    /* RAM: pointer to the tile-set table */
    uint32_t ground_rec, overlay_rec;   /* layer records */
    int ground_plane;        /* VDP plane of the ground (overlay is the other) */
    uint32_t obj_head;       /* RAM: head of the map-object list */
    uint8_t obj_mask;        /* flags at +$19 that mark a drawn object */
    int obj_add;             /* add the object's tile offset (Jungle/Urban) */
    uint16_t empty;          /* entry for an empty byte-map cell */
} strike_cfg;

/* RAM addresses of the player status (docs/10-status-hud.md). */
typedef struct {
    uint32_t player_ptr;     /* pointer to the player's vehicle object */
    uint16_t armor_off;      /* armour word in that object */
    uint32_t armor_max, fuel, fuel_max, load, lives;   /* words */
    uint32_t load_max;       /* byte holding the capacity; 0 = fixed */
    int load_fixed;
    uint16_t weapons_off;    /* player object: pointer to its 3 weapon records
                                ($40 apart, rounds left at +$12) */
    uint16_t pos_off;        /* player object: pointer to its position (+0 x, +4 y, +8 height) */
    uint32_t ground_rec;     /* ground layer record (camera clamp at +$30) */
} strike_hud_cfg;

static inline int strike_r16(uint32_t a) { a &= 0xFFFF; return g_ram[a] << 8 | g_ram[(a + 1) & 0xFFFF]; }

static inline int strike_hud(const strike_hud_cfg *c, game_hud *h)
{
    uint32_t p = (uint32_t)(strike_r16(c->player_ptr) << 16 | strike_r16(c->player_ptr + 2)) & 0xFFFFFF;
    if (p < 0xFF0000) return 0;
    h->armor = (int16_t)strike_r16(p + c->armor_off);
    h->armor_max = strike_r16(c->armor_max);
    h->fuel = (int16_t)strike_r16(c->fuel);
    h->fuel_max = strike_r16(c->fuel_max);
    h->load = (int16_t)strike_r16(c->load);
    h->load_max = c->load_max ? g_ram[c->load_max & 0xFFFF] : c->load_fixed;
    h->lives = (int16_t)strike_r16(c->lives);
    uint32_t w = (uint32_t)(strike_r16(p + c->weapons_off) << 16 | strike_r16(p + c->weapons_off + 2)) & 0xFFFFFF;
    for (int i = 0; i < 3; i++)
        h->ammo[i] = w >= 0xFF0000 ? (int16_t)strike_r16(w + i * 0x40 + 0x12) : -1;
    if (h->armor_max <= 0 || h->fuel_max <= 0) return 0;
    if (h->armor < 0) h->armor = 0;
    if (h->fuel < 0) h->fuel = 0;
    if (h->lives < 0) h->lives = 0;
    return 1;
}

static inline void strike_w16(uint32_t a, int v) { a &= 0xFFFF; g_ram[a] = (uint8_t)(v >> 8); g_ram[(a + 1) & 0xFFFF] = (uint8_t)v; }

/*
 * Graphics-extraction tour (host --extract): keeps the player alive and
 * fuelled, and moves it in rows across the whole map so every part of the
 * level, its buildings and its units get loaded and drawn. Call once per
 * frame at the frame boundary. Returns 0 while the playfield isn't up,
 * 1 while touring, 2 when the whole map has been covered.
 */
static inline int strike_tour(const strike_hud_cfg *c, int reset, int playfield)
{
    static int cx, cy, dir, started;
    if (reset) { started = 0; return 0; }
    if (!playfield) return started ? 1 : 0;
    uint32_t p = (uint32_t)(strike_r16(c->player_ptr) << 16 | strike_r16(c->player_ptr + 2)) & 0xFFFFFF;
    if (p < 0xFF0000) return 0;
    uint32_t pos = (uint32_t)(strike_r16(p + c->pos_off) << 16 | strike_r16(p + c->pos_off + 2)) & 0xFFFFFF;
    if (pos < 0xFF0000) return 0;
    /* stay alive */
    strike_w16(p + c->armor_off, strike_r16(c->armor_max));
    strike_w16(c->fuel, strike_r16(c->fuel_max));
    strike_w16(c->lives, 3);
    int miny = (int16_t)strike_r16(c->ground_rec + 0x30), minx = (int16_t)strike_r16(c->ground_rec + 0x32);
    int maxy = (int16_t)strike_r16(c->ground_rec + 0x34), maxx = (int16_t)strike_r16(c->ground_rec + 0x36);
    if (maxx <= minx || maxy <= miny) return 1;
    if (!started) { started = 1; cx = minx; cy = miny; dir = 1; }
    cx += dir * 6;
    if (cx > maxx || cx < minx) {
        cx = cx > maxx ? maxx : minx;
        dir = -dir;
        cy += 150;
        if (cy > maxy + 150) return 2;
    }
    int z = (int16_t)strike_r16(pos + 8);
    strike_w16(pos, cx + 0xA0);
    strike_w16(pos + 4, (cy > maxy ? maxy : cy) + 0x70 + z);
    return 1;
}

/*
 * Cheats, applied at the frame boundary while the playfield is up. Ammo
 * maxima are learnt continuously (each weapon's highest count since the
 * campaign began, i.e. its full load), so "unlimited ammo" refills to the
 * real capacity. Lives are held at their value when the cheat was enabled
 * (or any higher value gained since).
 */
static inline void strike_cheats(const strike_hud_cfg *c, int god, int fuel, int ammo, int lives)
{
    static int amax[3], lhold = -1;
    static uint32_t last_w;
    static uint64_t last_frame;
    if (g_frame < last_frame) { last_w = 0; lhold = -1; }
    last_frame = g_frame;
    uint32_t p = (uint32_t)(strike_r16(c->player_ptr) << 16 | strike_r16(c->player_ptr + 2)) & 0xFFFFFF;
    if (p < 0xFF0000) return;
    if (god) strike_w16(p + c->armor_off, strike_r16(c->armor_max));
    if (fuel) strike_w16(c->fuel, strike_r16(c->fuel_max));
    uint32_t w = (uint32_t)(strike_r16(p + c->weapons_off) << 16 | strike_r16(p + c->weapons_off + 2)) & 0xFFFFFF;
    if (w >= 0xFF0000) {
        if (w != last_w) { last_w = w; amax[0] = amax[1] = amax[2] = 0; }
        for (int i = 0; i < 3; i++) {
            uint32_t a = w + (uint32_t)i * 0x40 + 0x12;
            int v = (int16_t)strike_r16(a);
            if (v > amax[i] && v < 10000) amax[i] = v;
            if (ammo && v < amax[i]) strike_w16(a, amax[i]);
        }
    }
    if (lives) {
        int v = (int16_t)strike_r16(c->lives);
        if (lhold < 0 || v > lhold) lhold = v;
        if (v < lhold) strike_w16(c->lives, lhold);
    } else lhold = -1;
}

int strike_ws_world(const strike_cfg *c, vdp_world_plane out[2], int ext_left, int ext_right);

/*
 * Minimap (games/strike_minimap.c, docs/11-minimap.md): the pause map's list
 * of objectives and categories, and the units each one marks, found the way
 * the pause map's own icon builder finds them.
 */
typedef struct {
    int kind;                  /* unit-list layout: 0 Desert, 1 Jungle, 2 Urban */
    uint32_t obj_tab;          /* RAM: pointer to the list (word count, 12-byte entries) */
    uint32_t sel;              /* RAM word: selected entry */
    uint32_t unlocked;         /* RAM long: bit n-1 set = objective n available */
    uint32_t done_tab;         /* RAM words: negative = objective completed */
    int group_mul;             /* units of objective i carry group (i+1)*group_mul */
    uint32_t list1, list1_n;   /* static objects: first entry, count word */
    uint32_t list2, list2_n;   /* units: last entry (list grows down), count word */
    uint32_t player_ptr;       /* pointer to the player's vehicle */
    uint16_t pos_off;          /* vehicle: pointer to its position (+0 x, +4 y) */
    uint32_t home_flag1, home_flag2, home_ptr;   /* home-base icon (0 = none) */
    uint32_t carried;          /* Desert: list checked for entries with flag bit 1 */
    uint32_t level_ptr;        /* level descriptor pointer (cache key) */
    uint32_t ground_rec;       /* ground layer record (camera clamp at +$30) */
    /* Jungle/Urban set obj_tab and the unlocked mask only when the pause
     * screen opens; these compute them the same way while flying. */
    uint32_t tab_rom;          /* ROM: list pointer per campaign (0 = use obj_tab) */
    uint32_t campaign;         /* RAM word: campaign number */
    int mask_from_tab;         /* unlocked mask from done_tab: 1 sequential when level+$22 bit 1
                                  (Jungle $03A9C6, Urban $0037E0); 2 when campaign = 3 (Desert $005102) */
    uint32_t shift_rom;        /* ROM: map scale word per campaign (0 = 5) */
} strike_mm_cfg;

int  strike_minimap(const strike_mm_cfg *c, game_minimap *out);
void strike_minimap_select(const strike_mm_cfg *c, int index);
int  strike_minimap_name(const strike_mm_cfg *c, int index, char *buf, int n);
/* Every unit of list entry `index` (its type), each at its own position. */
int  strike_minimap_units(const strike_mm_cfg *c, int index, game_minimap *out);

#endif
