/* Software VDP renderer. See vdp_render.h. */
#include <string.h>
#include "genesis.h"
#include "vdp_render.h"

/* Measured Genesis DAC output levels for the 3-bit colour channels, plus
 * the shadow and highlight levels used by shadow/highlight mode. */
static const uint8_t lv_norm[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };
static const uint8_t lv_shad[8] = { 0, 29, 52, 70, 87, 101, 116, 130 };
static const uint8_t lv_high[8] = { 130, 144, 158, 172, 187, 206, 228, 255 };

static uint32_t mkcol(uint16_t c, const uint8_t *lv)
{
    return 0xFF000000u | (uint32_t)lv[(c >> 1) & 7] << 16 | (uint32_t)lv[(c >> 5) & 7] << 8 | lv[(c >> 9) & 7];
}

uint32_t vdp_color(uint16_t c) { return mkcol(c, lv_norm); }

int vdp_display_width(void) { return (g_vreg[12] & 0x81) ? 320 : 256; }

static inline uint16_t vw(uint32_t a) { a &= 0xFFFE; return (uint16_t)(g_vram[a] << 8 | g_vram[a + 1]); }

/* One pixel of a pattern: 4-bit colour index, 0 = transparent. */
static inline int tile_px(uint32_t tile, int x, int y)
{
    uint8_t b = g_vram[(tile * 32 + y * 4 + (x >> 1)) & 0xFFFF];
    return (x & 1) ? (b & 15) : (b >> 4);
}

/*
 * Layer buffers hold one "ref" per pixel: which tile pixel each layer shows.
 * The 1x image uses only colour/palette/priority; the HD path also needs the
 * tile and position to sample replacement art. Index 0 is screen x = -ext_left.
 */
#define LR_PXS 11
#define LR_PYS 14
#define LR_HF  (1u << 17)
#define LR_VF  (1u << 18)
#define LR_IDXS 19
#define LR_PALS 23
#define LR_PRI (1u << 25)
#define LR_TILED (1u << 26)
#define LR_TILE(v) ((v) & 0x7FF)
#define LR_IDX(v)  (((v) >> LR_IDXS) & 15)
#define LR_PAL(v)  (((v) >> LR_PALS) & 3)

static uint32_t fa[VDP_H][VDP_MAX_W], fbuf_b[VDP_H][VDP_MAX_W], fs[VDP_H][VDP_MAX_W];
static int8_t flvl[VDP_H][VDP_MAX_W];      /* shadow/highlight level per pixel */
static const vdp_hd *cur_hd;

/* Ref for name-table entry `ent` at pattern pixel (px, py) (already flipped). */
static inline uint32_t mkref(uint16_t ent, int px, int py)
{
    uint32_t idx = (uint32_t)tile_px(ent & 0x7FF, px, py);
    return (uint32_t)(ent & 0x7FF) | (uint32_t)px << LR_PXS | (uint32_t)py << LR_PYS
         | (ent & 0x0800 ? LR_HF : 0) | (ent & 0x1000 ? LR_VF : 0) | idx << LR_IDXS
         | (uint32_t)((ent >> 13) & 3) << LR_PALS | (ent & 0x8000 ? LR_PRI : 0) | LR_TILED;
}

/* The 1x layer pixel: bit 7 priority, bits 4-5 palette, bits 0-3 colour. */
static inline uint8_t c8(uint32_t v)
{
    uint32_t idx = LR_IDX(v);
    return idx ? (uint8_t)((v & LR_PRI ? 0x80 : 0) | LR_PAL(v) << 4 | idx) : 0;
}

static const int plane_cells[4] = { 32, 64, 32, 128 };

static void draw_plane(uint32_t *out, int line, int plane, uint32_t base, int x0, int n, int W)
{
    int pw = plane_cells[g_vreg[16] & 3], ph = plane_cells[(g_vreg[16] >> 4) & 3];
    /* 128x128 is not a valid size; hardware limits total cells to 4096. */
    if (pw == 128 && ph > 32) ph = 32;
    if (ph == 128 && pw > 32) pw = 32;
    uint32_t hs = (g_vreg[13] & 0x3F) << 10;
    int hmode = g_vreg[11] & 3, vcol = (g_vreg[11] >> 2) & 1;
    uint32_t ha;
    switch (hmode) {
    case 0:  ha = hs; break;
    case 1:  ha = hs + (line & 7) * 4; break;
    case 2:  ha = hs + (line & ~7) * 4; break;
    default: ha = hs + line * 4; break;
    }
    int hsc = vw(ha + plane * 2) & 0x3FF;
    int wpx = pw * 8, hpx = ph * 8;
    for (int i = 0; i < n; i++) {
        int x = x0 + i;                        /* screen x, may be negative */
        int vs;
        if (vcol) {
            int col = x < 0 ? 0 : x >= W ? (W / 16) - 1 : x / 16;
            vs = g_vsram[(col * 2 + plane) % 40];
        } else {
            vs = g_vsram[plane];
        }
        int sy = (line + (vs & 0x3FF)) & (hpx - 1);
        int sx = (x - hsc) & (wpx - 1);
        uint16_t ent = vw(base + ((sy >> 3) * pw + (sx >> 3)) * 2);
        int px = sx & 7, py = sy & 7;
        if (ent & 0x0800) px = 7 - px;
        if (ent & 0x1000) py = 7 - py;
        out[i] = mkref(ent, px, py);
    }
}

static void draw_world(uint32_t *out, int line, const vdp_world_plane *w, int x0, int n)
{
    int wy = line + w->oy, j = (wy >> 3) - w->cy0, py0 = wy & 7;
    for (int i = 0; i < n; i++) {
        int wx = x0 + i + w->ox, k = (wx >> 3) - w->cx0;
        if (j < 0 || j >= w->h || k < 0 || k >= w->w) { out[i] = 0; continue; }
        uint16_t ent = w->cells[j * w->w + k];
        if (ent == 0xFFFF) { out[i] = 0; continue; }
        int px = wx & 7, py = py0;
        if (ent & 0x0800) px = 7 - px;
        if (ent & 0x1000) py = 7 - py;
        out[i] = mkref(ent, px, py);
    }
}

static void draw_window(uint32_t *out, int line, int x0, int n, int W, int h40)
{
    int hp = (g_vreg[17] & 0x1F) * 16, right = g_vreg[17] & 0x80;
    int vp = (g_vreg[18] & 0x1F) * 8, down = g_vreg[18] & 0x80;
    int full_line = down ? line >= vp : line < vp;
    uint32_t base = h40 ? (g_vreg[3] & 0x3C) << 10 : (g_vreg[3] & 0x3E) << 10;
    int wcells = h40 ? 64 : 32;
    for (int i = 0; i < n; i++) {
        int x = x0 + i;
        if (x < 0 || x >= W) continue;         /* window only covers the real display */
        int inwin = full_line || (right ? x >= hp : x < hp);
        if (!inwin) continue;
        uint16_t ent = vw(base + ((line >> 3) * wcells + (x >> 3)) * 2);
        int px = x & 7, py = line & 7;
        if (ent & 0x0800) px = 7 - px;
        if (ent & 0x1000) py = 7 - py;
        out[i] = mkref(ent, px, py);
    }
}

static int window_active(void)
{
    return (g_vreg[17] & 0x9F) != 0 || (g_vreg[18] & 0x9F) != 0;
}

static void draw_sprites(uint32_t *out, int line, int x0, int n, int h40, int limits, int wide)
{
    uint32_t sat = h40 ? (g_vreg[5] & 0x7E) << 9 : (g_vreg[5] & 0x7F) << 9;
    int max_spr = h40 ? 80 : 64;
    int max_line = h40 ? 20 : 16;
    int max_dots = h40 ? 320 : 256;
    int link = 0, count = 0, dots = 0, any_drawn = 0;
    int ly = line + 128;
    memset(out, 0, (size_t)n * sizeof *out);
    for (int k = 0; k < max_spr; k++) {
        uint32_t e = sat + link * 8;
        int y = vw(e) & 0x3FF;
        uint16_t s = vw(e + 2), t = vw(e + 4);
        int x = wide ? (int16_t)vw(e + 6) : (int)(vw(e + 6) & 0x1FF);
        int w = ((s >> 10) & 3) + 1, h = ((s >> 8) & 3) + 1;
        link = s & 0x7F;
        if (ly >= y && ly < y + h * 8) {
            if (limits && ++count > max_line) break;
            if (x == 0) {
                if (any_drawn) break;          /* sprite masking */
            } else {
                any_drawn = 1;
                int ty = ly - y;
                if (t & 0x1000) ty = h * 8 - 1 - ty;
                int sw = w * 8;
                if (limits && dots + sw > max_dots) sw = max_dots - dots;
                for (int tx = 0; tx < sw; tx++) {
                    int sx = x - 128 + tx;     /* screen x */
                    int i = sx - x0;
                    if (i < 0 || i >= n || LR_IDX(out[i])) continue;
                    int cx = (t & 0x0800) ? w * 8 - 1 - tx : tx;
                    uint16_t tile = (uint16_t)(((t & 0x7FF) + (cx >> 3) * h + (ty >> 3)) & 0x7FF);
                    uint32_t r = mkref((uint16_t)((t & 0xF800) | tile), cx & 7, ty & 7);
                    /* A pixel transparent in the original can still be
                     * opaque in replacement art: claim it if free. */
                    if (LR_IDX(r) || (!out[i] && cur_hd && cur_hd->tile[tile])) out[i] = r;
                }
            }
            if (limits) { dots += w * 8; if (dots >= max_dots) break; }
        }
        if (link == 0 || link >= max_spr) break;
    }
}

static inline uint32_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v; }

/* A layer at one low-res pixel, prepared for sampling its S x S block. */
typedef struct {
    const uint32_t *tex;      /* art texel (0,0) of the block; NULL = flat colour */
    int dx, dy;               /* texel steps for sub-pixel x / y (flips) */
    int opaque;               /* flat: 1 if the original pixel is opaque */
    uint32_t flat;            /* flat colour */
    int dr, dg, db, shift;    /* palette-change shift for art */
} hd_layer;

static int hd_prep(uint32_t v, int S, const uint32_t *pal, hd_layer *L)
{
    if (!(v & LR_TILED)) return 0;
    const vdp_hd_tile *t = cur_hd->tile[LR_TILE(v)];
    uint32_t idx = LR_IDX(v);
    if (!t) {
        if (!idx) return 0;
        L->tex = NULL; L->opaque = 1; L->flat = pal[LR_PAL(v) * 16 + idx];
        return 1;
    }
    int px = (int)((v >> LR_PXS) & 7), py = (int)((v >> LR_PYS) & 7), W = 8 * S;
    int x0 = px * S, y0 = py * S;
    L->dx = 1; L->dy = W;
    if (v & LR_HF) { x0 += S - 1; L->dx = -1; }
    if (v & LR_VF) { y0 += S - 1; L->dy = -W; }
    L->tex = t->rgba + y0 * W + x0;
    L->shift = 0;
    if (t->has_ref && idx) {
        /* follow palette changes (fades, flashes): shift by the difference
         * between the current colour and the one the art was made for */
        uint32_t cur = pal[LR_PAL(v) * 16 + idx], ref = t->ref[idx];
        if ((cur ^ ref) & 0xFFFFFF) {
            L->shift = 1;
            L->dr = (int)((cur >> 16) & 255) - (int)((ref >> 16) & 255);
            L->dg = (int)((cur >> 8) & 255) - (int)((ref >> 8) & 255);
            L->db = (int)(cur & 255) - (int)(ref & 255);
        }
    }
    return 1;
}

static inline int hd_get(const hd_layer *L, int x, int y, uint32_t *c)
{
    if (!L->tex) { *c = L->flat; return L->opaque; }
    uint32_t t = L->tex[y * L->dy + x * L->dx];
    if ((t >> 24) < 128) return 0;
    if (L->shift)
        t = clamp8((int)((t >> 16) & 255) + L->dr) << 16 | clamp8((int)((t >> 8) & 255) + L->dg) << 8
          | clamp8((int)(t & 255) + L->db);
    *c = t | 0xFF000000u;
    return 1;
}

typedef struct { const vdp_render_opts *o; int n; const uint32_t *pal; int bgi, sh; } hd_job;

static void compose_hd_lines(void *ctx, int l0, int l1)
{
    const hd_job *J = ctx;
    const vdp_hd *hd = J->o->hd;
    const uint32_t *pal = J->pal;
    int S = hd->scale, n = J->n, sh = J->sh;
    uint32_t back = pal[J->bgi];
    for (int line = l0; line < l1; line++) {
        const uint32_t *lo = J->o->fb_lo + (size_t)line * J->o->pitch_lo;
        for (int i = 0; i < n; i++) {
            uint32_t va = fa[line][i], vb = fbuf_b[line][i], vs = fs[line][i];
            uint32_t *dst = hd->fb + ((size_t)line * S) * hd->pitch + (size_t)i * S;
            int any = (va & LR_TILED && hd->tile[LR_TILE(va)]) || (vb & LR_TILED && hd->tile[LR_TILE(vb)])
                   || (vs & LR_TILED && hd->tile[LR_TILE(vs)]);
            if (!any || !(g_vreg[1] & 0x40)) {
                uint32_t c = lo[i];
                for (int y = 0; y < S; y++)
                    for (int x = 0; x < S; x++) dst[(size_t)y * hd->pitch + x] = c;
                continue;
            }
            int lvl = flvl[line][i];
            if (sh && LR_PAL(vs) == 3 && (LR_IDX(vs) == 14 || LR_IDX(vs) == 15)) vs = 0;   /* operator */
            /* candidates in priority order: S hi, A hi, B hi, S lo, A lo, B lo */
            hd_layer L[6];
            int nl = 0;
            uint32_t vv[3] = { vs, va, vb };
            for (int pass = 0; pass < 2; pass++)
                for (int k = 0; k < 3; k++) {
                    uint32_t v = vv[k];
                    if (!!(v & LR_PRI) != (pass == 0)) continue;
                    if (hd_prep(v, S, pal, &L[nl])) nl++;
                }
            for (int y = 0; y < S; y++) {
                uint32_t *d = dst + (size_t)y * hd->pitch;
                for (int x = 0; x < S; x++) {
                    uint32_t c = back;
                    for (int k = 0; k < nl; k++)
                        if (hd_get(&L[k], x, y, &c)) break;
                    if (sh && lvl) {
                        uint32_t r = (c >> 17) & 127, g = (c >> 9) & 127, b = (c >> 1) & 127;
                        if (lvl > 0) { r += 128; g += 128; b += 128; }
                        c = 0xFF000000u | r << 16 | g << 8 | b;
                    }
                    d[x] = c;
                }
            }
        }
    }
}

void vdp_render(uint32_t *fb, int pitch, const vdp_render_opts *o)
{
    int h40 = (g_vreg[12] & 0x81) != 0;
    int W = h40 ? 320 : 256;
    int extl = o ? o->ext_left : 0, extr = o ? o->ext_right : 0;
    int n = extl + W + extr;
    if (n > VDP_MAX_W) { n = VDP_MAX_W; }
    int x0 = -extl;
    int limits = o ? o->sprite_limits : 1;
    int sh = (g_vreg[12] & 0x08) != 0;
    cur_hd = o && o->hd && o->hd->fb ? o->hd : NULL;
    uint32_t pal[64], pal_s[64], pal_h[64];
    for (int i = 0; i < 64; i++) {
        pal[i] = mkcol(g_cram[i], lv_norm);
        pal_s[i] = mkcol(g_cram[i], lv_shad);
        pal_h[i] = mkcol(g_cram[i], lv_high);
    }
    int bgi = g_vreg[7] & 0x3F;
    uint32_t base_a = (g_vreg[2] & 0x38) << 10;
    uint32_t base_b = (g_vreg[4] & 0x07) << 13;
    int win = window_active();

    for (int line = 0; line < VDP_H; line++) {
        uint32_t *row = fb + (size_t)line * pitch;
        uint32_t *la = fa[line], *lb = fbuf_b[line], *ls = fs[line];
        if (!(g_vreg[1] & 0x40)) {             /* display disabled: backdrop */
            for (int i = 0; i < n; i++) { row[i] = pal[bgi]; la[i] = lb[i] = ls[i] = 0; flvl[line][i] = 0; }
            continue;
        }
        if (o && o->world[1]) draw_world(lb, line, o->world[1], x0, n);
        else draw_plane(lb, line, 1, base_b, x0, n, W);
        if (o && o->world[0]) draw_world(la, line, o->world[0], x0, n);
        else draw_plane(la, line, 0, base_a, x0, n, W);
        if (win) draw_window(la, line, x0, n, W, h40);
        draw_sprites(ls, line, x0, n, h40, limits, o ? o->wide_sprites : 0);

        for (int i = 0; i < n; i++) {
            uint8_t a = c8(la[i]), b = c8(lb[i]), s = c8(ls[i]);
            uint8_t hi = 0, lo = 0;            /* chosen pixel, high/low pass */
            /* Priority order: S hi, A hi, B hi, S lo, A lo, B lo, backdrop. */
            if (s & 0x80 && s & 15) hi = s;
            else if (a & 0x80 && a & 15) hi = a;
            else if (b & 0x80 && b & 15) hi = b;
            if (!hi) {
                if (s & 15 && !(s & 0x80)) lo = s;
                else if (a & 15 && !(a & 0x80)) lo = a;
                else if (b & 15 && !(b & 0x80)) lo = b;
            }
            uint8_t px = hi ? hi : lo;
            int idx = px ? ((px >> 4) & 3) * 16 + (px & 15) : bgi;
            flvl[line][i] = 0;
            if (!sh) { row[i] = pal[idx]; continue; }

            /* Shadow/highlight: a pixel is shadowed unless plane A or B is
             * high priority there. Sprite colours 3:14 and 3:15 act as
             * highlight and shadow operators instead of being drawn. */
            int shadowed = !((a & 0x80) || (b & 0x80));
            int op = 0;                         /* 1 highlight, 2 shadow */
            if ((s & 0x3F) == 0x3E || (s & 0x3F) == 0x3F) {
                op = (s & 0x3F) == 0x3E ? 1 : 2;
                /* the operator is invisible: pick what lies beneath it */
                uint8_t u = 0;
                if (a & 0x80 && a & 15) u = a;
                else if (b & 0x80 && b & 15) u = b;
                else if (a & 15) u = a;
                else if (b & 15) u = b;
                idx = u ? ((u >> 4) & 3) * 16 + (u & 15) : bgi;
            } else if (px == s && (s & 15) && (s & 0x80)) {
                shadowed = 0;                   /* high-priority sprites are never shadowed */
            } else if (px == s && (s & 15) && (s & 0x3F) == 0x0E) {
                shadowed = 0;                   /* colour 14 of any palette is drawn normally */
            }
            int level = shadowed ? -1 : 0;
            if (op == 1) level++;
            if (op == 2) level--;
            if (level < -1) level = -1;
            if (level > 1) level = 1;
            flvl[line][i] = (int8_t)level;
            row[i] = level < 0 ? pal_s[idx] : level > 0 ? pal_h[idx] : pal[idx];
        }
    }
    if (cur_hd) {
        vdp_render_opts lo = *o;
        lo.fb_lo = fb; lo.pitch_lo = pitch;
        hd_job job = { &lo, n, pal, bgi, sh };
        if (o->parallel) o->parallel(compose_hd_lines, &job, VDP_H);
        else compose_hd_lines(&job, 0, VDP_H);
    }
}

void vdp_render_used_tiles(uint8_t used[2048], int n)
{
    if (n > VDP_MAX_W) n = VDP_MAX_W;
    for (int y = 0; y < VDP_H; y++)
        for (int i = 0; i < n; i++) {
            uint32_t v[3] = { fa[y][i], fbuf_b[y][i], fs[y][i] };
            for (int k = 0; k < 3; k++)
                if (v[k] & LR_TILED) used[LR_TILE(v[k])] |= (uint8_t)(1u << LR_PAL(v[k]));
        }
}
