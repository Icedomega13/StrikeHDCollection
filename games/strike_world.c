/*
 * Ultrawide background painter for the Strike engine. See strike_engine.h
 * and docs/07-widescreen.md.
 *
 * The game keeps only a 512x512 px window of the map in each VDP plane.
 * For views wider than that, this rebuilds the plane contents in world
 * coordinates straight from the game's own map data, using the same
 * lookups as its strip-drawing routines. Inside the part of the window the
 * game has finished drawing, the real VRAM cells are used, so anything the
 * game changed there is kept exactly.
 */
#include "strike_engine.h"
#include "hw/genesis.h"

#define NONE 0xFFFFu

static inline uint32_t mr8(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a >= 0xFF0000) return g_ram[a & 0xFFFF];
    return a < g_rom_size ? g_rom[a] : 0;
}
static inline uint32_t mr16(uint32_t a) { return mr8(a) << 8 | mr8(a + 1); }
static inline uint32_t mr32(uint32_t a) { return mr16(a) << 16 | mr16(a + 2); }
static inline uint32_t mptr(uint32_t a) { return mr32(a) & 0xFFFFFF; }
static inline int ms16(uint32_t a) { return (int16_t)mr16(a); }

/* Tile base for a tile-map descriptor, as the strip drawers compute it.
 * Returns -1 when its tile set is not loaded in VRAM (the game draws
 * nothing then). */
static int tm_base(const strike_cfg *c, uint32_t tm)
{
    uint32_t ts = mptr(mptr(c->tileset_ptr) + mr16(tm + 4));
    int b;
    if (mr8(ts) & 2) {
        b = (int)mr16(mptr(ts + 6));
        if (b == 0) return -1;
    } else b = 0x34;
    return (b + (int)mr16(tm + 0x1E)) & 0xFFFF;
}

/* Entry (dx, dy) of a tile map with row stride `stride`. */
static uint16_t tm_cell(const strike_cfg *c, uint32_t tm, int base, int add, int stride, int dx, int dy)
{
    uint32_t mp = mptr(tm + 0x16);
    if (mr16(tm + 0x10) == 0x1A) {           /* byte map: tile + flip bits */
        uint32_t v = mr8(mp + 2 + (uint32_t)(dy * stride + dx));
        if (!v) return c->empty;
        uint32_t e = v & 0x3F;
        if (v & 0x80) e |= 0x1000;
        if (v & 0x40) e |= 0x0800;
        return (uint16_t)(e + base + mr16(mp) + add);
    }
    return (uint16_t)(mr16(mp + (uint32_t)(dy * stride + dx) * 2) + base + add);
}

static uint16_t ground_cell(const strike_cfg *c, uint32_t lvl, int tx, int ty)
{
    if (tx < 0 || ty < 0) return NONE;
    if (c->ground_kind == 0) {               /* Desert: 64x64-tile sectors */
        int w = (int)mr16(lvl + 2), n = (int)mr16(lvl);
        int sec = (ty >> 6) * w + (tx >> 6);
        if ((tx >> 6) >= w || sec >= n) return NONE;
        uint32_t blk = mptr(lvl + 0x62 + (uint32_t)sec * 4), tm = mptr(blk);
        int y0 = ms16(blk + 4), x0 = ms16(blk + 6), y1 = ms16(blk + 8), x1 = ms16(blk + 10);
        if (ty < y0 || ty >= y1 || tx < x0 || tx >= x1) return NONE;
        int base = tm_base(c, tm);
        if (base < 0) return NONE;
        return tm_cell(c, tm, base, 0, (int)mr16(tm), tx - x0, ty - y0);
    }
    /* Jungle / Urban: grid of 32x16-tile blocks */
    int w = (int)mr16(lvl + 6);
    if ((tx >> 5) >= w) return NONE;
    uint32_t tm = mptr(mptr(lvl + c->ground_tab) + (uint32_t)(((ty >> 4) * w + (tx >> 5)) * 4));
    if (!tm) return NONE;
    int base = tm_base(c, tm);
    if (base < 0) return c->empty;
    return tm_cell(c, tm, base, 0, 32, tx & 31, ty & 15);
}

/* World-pixel origin of the screen for a plane: the scroll registers,
 * unwrapped to the copy nearest the layer camera. */
static void plane_origin(const strike_cfg *c, uint32_t rec, int plane, int *ox, int *oy)
{
    uint32_t hs = ((g_vreg[13] & 0x3F) << 10) + plane * 2;
    int h = -(int16_t)(g_vram[hs & 0xFFFF] << 8 | g_vram[(hs + 1) & 0xFFFF]);
    int v = (int16_t)g_vsram[plane];
    int cx = ms16(rec + 0x4A), cy = ms16(rec + 0x48);
    (void)c;
    *ox = cx + ((((h - cx) + 256) & 511) - 256);
    *oy = cy + ((((v - cy) + 256) & 511) - 256);
}

/* Cells the game has completely drawn: the intersection of the window it
 * last finished (+0) and the one it is moving to (+$10). */
static void valid_rect(uint32_t rec, int r[4])
{
    r[0] = ms16(rec + 0x00); r[1] = ms16(rec + 0x02); r[2] = ms16(rec + 0x04); r[3] = ms16(rec + 0x06);
    int n0 = ms16(rec + 0x10), n1 = ms16(rec + 0x12), n2 = ms16(rec + 0x14), n3 = ms16(rec + 0x16);
    if (n0 > r[0]) r[0] = n0;
    if (n1 > r[1]) r[1] = n1;
    if (n2 < r[2]) r[2] = n2;
    if (n3 < r[3]) r[3] = n3;
}

#define WMAX_W (VDP_MAX_W / 8 + 2)
#define WMAX_H (VDP_H / 8 + 2)
static uint16_t cells[2][WMAX_W * WMAX_H];

static void setup(vdp_world_plane *o, uint16_t *buf, const strike_cfg *c, uint32_t rec, int plane, int extl, int extr, int W)
{
    plane_origin(c, rec, plane, &o->ox, &o->oy);
    o->cx0 = (o->ox - extl) >> 3;
    o->cy0 = o->oy >> 3;
    o->w = ((o->ox + W + extr - 1) >> 3) - o->cx0 + 1;
    o->h = ((o->oy + VDP_H - 1) >> 3) - o->cy0 + 1;
    if (o->w > WMAX_W) o->w = WMAX_W;
    if (o->h > WMAX_H) o->h = WMAX_H;
    o->cells = buf;
}

/* Copy the drawn part of the VRAM window over the painted cells. */
static void overlay_vram(vdp_world_plane *o, uint16_t *buf, uint32_t rec, int plane)
{
    int r[4];
    valid_rect(rec, r);
    uint32_t base = plane ? (g_vreg[4] & 0x07) << 13 : (g_vreg[2] & 0x38) << 10;
    for (int j = 0; j < o->h; j++) {
        int ty = o->cy0 + j;
        if (ty < r[0] || ty >= r[2]) continue;
        for (int i = 0; i < o->w; i++) {
            int tx = o->cx0 + i;
            if (tx < r[1] || tx >= r[3]) continue;
            uint32_t a = (base + (uint32_t)(((ty & 63) << 7) | ((tx & 63) << 1))) & 0xFFFF;
            buf[j * o->w + i] = (uint16_t)(g_vram[a] << 8 | g_vram[a + 1]);
        }
    }
}

/* 1 if many painted ground cells disagree with the drawn VRAM window. */
static int vram_mismatch(const vdp_world_plane *o, const uint16_t *buf, uint32_t rec, int plane)
{
    int r[4], n = 0, bad = 0;
    valid_rect(rec, r);
    uint32_t base = plane ? (g_vreg[4] & 0x07) << 13 : (g_vreg[2] & 0x38) << 10;
    for (int j = 0; j < o->h; j++) {
        int ty = o->cy0 + j;
        if (ty < r[0] || ty >= r[2]) continue;
        for (int i = 0; i < o->w; i++) {
            int tx = o->cx0 + i;
            uint16_t e = buf[j * o->w + i];
            if (tx < r[1] || tx >= r[3] || e == NONE) continue;
            uint32_t a = (base + (uint32_t)(((ty & 63) << 7) | ((tx & 63) << 1))) & 0xFFFF;
            n++;
            if ((uint16_t)(g_vram[a] << 8 | g_vram[a + 1]) != e) bad++;
        }
    }
    return n >= 64 && bad * 8 > n;
}

int strike_ws_world(const strike_cfg *c, vdp_world_plane out[2], int extl, int extr)
{
    int W = vdp_display_width();
    uint32_t lvl = mptr(c->level_ptr);
    if (!lvl) return 0;

    /* ground */
    {
        int p = c->ground_plane;
        vdp_world_plane *o = &out[p];
        uint16_t *buf = cells[p];
        setup(o, buf, c, c->ground_rec, p, extl, extr, W);
        for (int j = 0; j < o->h; j++)
            for (int i = 0; i < o->w; i++)
                buf[j * o->w + i] = ground_cell(c, lvl, o->cx0 + i, o->cy0 + j);
        /* Sanity check: in the playfield the ground the game drew matches the
         * map exactly. A cutscene or screen drawn into the plane while the
         * scroll still matches the camera does not, so this is not the
         * playfield: no widescreen. */
        if (vram_mismatch(o, buf, c->ground_rec, p)) return 0;
        overlay_vram(o, buf, c->ground_rec, p);
    }

    /* overlay: tile graphics of map objects (buildings etc.) */
    {
        int p = c->ground_plane ^ 1;
        vdp_world_plane *o = &out[p];
        uint16_t *buf = cells[p];
        setup(o, buf, c, c->overlay_rec, p, extl, extr, W);
        for (int k = 0; k < o->w * o->h; k++) buf[k] = NONE;
        uint32_t a = mptr(c->obj_head);
        for (int guard = 0; a && guard < 512; guard++, a = mptr(a)) {
            if ((mr8(a + 0x19) & c->obj_mask) != c->obj_mask) continue;
            uint32_t blk = mptr(a + 8), tm = mptr(blk);
            if (!tm) continue;
            int y0 = ms16(blk + 4), x0 = ms16(blk + 6), y1 = ms16(blk + 8), x1 = ms16(blk + 10);
            int bx0 = x0 > o->cx0 ? x0 : o->cx0, bx1 = x1 < o->cx0 + o->w ? x1 : o->cx0 + o->w;
            int by0 = y0 > o->cy0 ? y0 : o->cy0, by1 = y1 < o->cy0 + o->h ? y1 : o->cy0 + o->h;
            if (bx0 >= bx1 || by0 >= by1) continue;
            int base = tm_base(c, tm);
            if (base < 0) continue;
            int add = c->obj_add ? (int)mr16(mptr(mptr(a + 0x0C) + 0x1A) + 0x12) : 0;
            int stride = (int)mr16(tm);
            for (int ty = by0; ty < by1; ty++)
                for (int tx = bx0; tx < bx1; tx++)
                    buf[(ty - o->cy0) * o->w + (tx - o->cx0)] = tm_cell(c, tm, base, add, stride, tx - x0, ty - y0);
        }
        overlay_vram(o, buf, c->overlay_rec, p);
    }
    return 1;
}
