/* HD graphics packs: replacement art for 8x8 patterns. See hdpack.h. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "hdpack.h"
#include "hw/genesis.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include "stb_image_write.h"

/* ---------------- hashing ---------------- */
static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
#define FNV0 0xCBF29CE484222325ull

/* ---------------- table: pattern hash -> art ---------------- */
typedef struct {
    uint64_t key;          /* 0 = empty */
    vdp_hd_tile t;
    int ref_pending;       /* waiting for a palettes.txt entry */
} entry;

static entry *tab;
static size_t tab_cap, tab_n;
static int pack_scale;
static uint8_t vram_copy[0x10000];
static uint64_t tile_hash[2048];
static int copy_valid;

static entry *find(uint64_t k)
{
    if (!tab_cap) return NULL;
    size_t i = (size_t)(k ^ (k >> 29)) & (tab_cap - 1);
    while (tab[i].key) {
        if (tab[i].key == k) return &tab[i];
        i = (i + 1) & (tab_cap - 1);
    }
    return NULL;
}

static entry *insert(uint64_t k)
{
    if ((tab_n + 1) * 2 > tab_cap) {
        size_t oc = tab_cap;
        entry *old = tab;
        tab_cap = oc ? oc * 2 : 4096;
        tab = calloc(tab_cap, sizeof *tab);
        tab_n = 0;
        for (size_t i = 0; i < oc; i++)
            if (old[i].key) *insert(old[i].key) = old[i];
        free(old);
    }
    size_t i = (size_t)(k ^ (k >> 29)) & (tab_cap - 1);
    while (tab[i].key && tab[i].key != k) i = (i + 1) & (tab_cap - 1);
    if (!tab[i].key) { tab[i].key = k; tab_n++; }
    return &tab[i];
}

void hdpack_unload(void)
{
    for (size_t i = 0; i < tab_cap; i++)
        if (tab[i].key) free((void *)tab[i].t.rgba);
    free(tab);
    tab = NULL; tab_cap = tab_n = 0;
    pack_scale = 0;
    copy_valid = 0;
}

int hdpack_scale(void) { return pack_scale; }
int hdpack_count(void) { return (int)tab_n; }

/* ---------------- loading ---------------- */
static int parse_hex64(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v << 4 | (uint64_t)d;
    }
    *out = v;
    return v != 0;
}

static uint32_t *load_png(const char *path, int *w, int *h)
{
    size_t len;
    void *data = SDL_LoadFile(path, &len);
    if (!data) return NULL;
    int n;
    uint8_t *px = stbi_load_from_memory(data, (int)len, w, h, &n, 4);
    SDL_free(data);
    if (!px) return NULL;
    /* RGBA bytes -> ARGB words */
    uint32_t *o = malloc((size_t)*w * *h * 4);
    for (int i = 0; i < *w * *h; i++)
        o[i] = (uint32_t)px[i * 4 + 3] << 24 | (uint32_t)px[i * 4] << 16 | (uint32_t)px[i * 4 + 1] << 8 | px[i * 4 + 2];
    stbi_image_free(px);
    return o;
}

/* Copy the 8x8-pattern cell (cx, cy) of an image whose cells are `cell`
 * pixels, resampled to the pack scale. */
static uint32_t *cut(const uint32_t *img, int iw, int cell, int cx, int cy)
{
    int S = pack_scale, n = 8 * S;
    uint32_t *o = malloc((size_t)n * n * 4);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int sx = cx * cell + x * cell / n, sy = cy * cell + y * cell / n;
            o[y * n + x] = img[(size_t)sy * iw + sx];
        }
    return o;
}

static void add_art(uint64_t k, uint32_t *rgba, const uint32_t *ref)
{
    entry *e = find(k);
    if (e) { free(rgba); return; }        /* first one wins */
    e = insert(k);
    e->t.rgba = rgba;
    if (ref) { e->t.has_ref = 1; memcpy(e->t.ref, ref, sizeof e->t.ref); }
    else e->ref_pending = 1;
}

static int parse_pal(const char *s, uint32_t pal[16])
{
    for (int i = 0; i < 16; i++) {
        unsigned v;
        int used;
        if (sscanf(s, " %6x%n", &v, &used) != 1) return 0;
        pal[i] = 0xFF000000u | v;
        s += used;
    }
    return 1;
}

/* A sheet: spr_x.png / ctx_x.png + .txt. The .txt holds "w h", then h rows
 * of w cell tokens, then optional palette lines. A token is a pattern hash,
 * or "-" for none, optionally followed by ":h" / ":v" (the cell is shown
 * mirrored, so its art is mirrored back) and ":pN" (its colours are line
 * "palN"). "pal" alone is the colours for every cell (sprite sheets). */
typedef struct { uint64_t key; uint8_t hf, vf, pal; } sheet_cell;

static void load_sheet(const char *png, const char *txt)
{
    FILE *f = fopen(txt, "r");
    if (!f) return;
    int w = 0, h = 0;
    if (fscanf(f, "%d %d", &w, &h) != 2 || w < 1 || h < 1 || w > 128 || h > 64) { fclose(f); return; }
    sheet_cell *cells = calloc((size_t)w * h, sizeof *cells);
    char tok[96];
    for (int i = 0; i < w * h && fscanf(f, "%95s", tok) == 1; i++) {
        sheet_cell *c = &cells[i];
        c->pal = 255;
        if (!parse_hex64(tok, &c->key)) { c->key = 0; continue; }
        for (const char *q = tok + 16; *q; q++) {
            if (q[0] != ':') continue;
            if (q[1] == 'h') c->hf = 1;
            else if (q[1] == 'v') c->vf = 1;
            else if (q[1] == 'p' && q[2] >= '0' && q[2] <= '3') c->pal = (uint8_t)(q[2] - '0');
        }
    }
    uint32_t pal[5][16];
    int has_pal[5] = { 0 };
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "pal", 3)) {
            int n = line[3] >= '0' && line[3] <= '3' ? line[3] - '0' : 4;
            has_pal[n] = parse_pal(line + (n < 4 ? 4 : 3), pal[n]);
        }
    }
    fclose(f);
    int iw, ih;
    uint32_t *img = load_png(png, &iw, &ih);
    if (img && iw % (w * 8) == 0 && ih * w == iw * h) {
        int cell = iw / w;
        if (!pack_scale) pack_scale = SDL_clamp(cell / 8, 1, 4);
        int S = pack_scale, n = 8 * S;
        for (int cy = 0; cy < h; cy++)
            for (int cx = 0; cx < w; cx++) {
                sheet_cell *c = &cells[cy * w + cx];
                if (!c->key || find(c->key)) continue;
                uint32_t *t = cut(img, iw, cell, cx, cy);
                if (c->hf || c->vf) {            /* back to the pattern's own orientation */
                    uint32_t *u = malloc((size_t)n * n * 4);
                    for (int y = 0; y < n; y++)
                        for (int x = 0; x < n; x++)
                            u[y * n + x] = t[(c->vf ? n - 1 - y : y) * n + (c->hf ? n - 1 - x : x)];
                    free(t);
                    t = u;
                }
                int pi = c->pal < 4 ? c->pal : 4;
                add_art(c->key, t, has_pal[pi] ? pal[pi] : NULL);
            }
    }
    free(img);
    free(cells);
}

static void load_tile(const char *png, uint64_t k)
{
    int iw, ih;
    uint32_t *img = load_png(png, &iw, &ih);
    if (img && iw == ih && iw % 8 == 0) {
        if (!pack_scale) pack_scale = SDL_clamp(iw / 8, 1, 4);
        add_art(k, cut(img, iw, iw, 0, 0), NULL);
    }
    free(img);
}

static void load_palettes(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        uint64_t k;
        uint32_t pal[16];
        if (strlen(line) < 17 || !parse_hex64(line, &k) || !parse_pal(line + 16, pal)) continue;
        entry *e = find(k);
        if (e && e->ref_pending) { e->t.has_ref = 1; memcpy(e->t.ref, pal, sizeof pal); e->ref_pending = 0; }
    }
    fclose(f);
}

typedef struct { int depth, pass; } walk_ctx;   /* pass 0: sheets, 1: single patterns */
static SDL_EnumerationResult SDLCALL walk(void *ud, const char *dir, const char *name)
{
    walk_ctx *c = ud;
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s%s", dir, name);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        if (c->depth < 4) {
            char sub[1024];
            SDL_snprintf(sub, sizeof sub, "%s/", path);
            c->depth++;
            SDL_EnumerateDirectory(sub, walk, c);
            c->depth--;
        }
        return SDL_ENUM_CONTINUE;
    }
    size_t n = strlen(name);
    if (n < 5 || SDL_strcasecmp(name + n - 4, ".png")) return SDL_ENUM_CONTINUE;
    uint64_t k;
    if (n == 20 && parse_hex64(name, &k)) { if (c->pass == 1) load_tile(path, k); }
    else if ((!SDL_strncasecmp(name, "spr_", 4) || !SDL_strncasecmp(name, "ctx_", 4)) && c->pass == 0) {
        char txt[1024];
        SDL_snprintf(txt, sizeof txt, "%.*s.txt", (int)(strlen(path) - 4), path);
        load_sheet(path, txt);
    }
    return SDL_ENUM_CONTINUE;
}

static SDL_EnumerationResult SDLCALL walk_pal(void *ud, const char *dir, const char *name)
{
    walk_ctx *c = ud;
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s%s", dir, name);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY && c->depth < 4) {
        char sub[1024];
        SDL_snprintf(sub, sizeof sub, "%s/", path);
        c->depth++;
        SDL_EnumerateDirectory(sub, walk_pal, c);
        c->depth--;
    } else if (!SDL_strcasecmp(name, "palettes.txt")) load_palettes(path);
    return SDL_ENUM_CONTINUE;
}

int hdpack_load(const char *dir)
{
    walk_ctx c = { 0 };
    size_t before = tab_n;
    /* sheets first: whole-sprite art wins over loose patterns */
    SDL_EnumerateDirectory(dir, walk, &c);
    c.pass = 1;
    SDL_EnumerateDirectory(dir, walk, &c);
    SDL_EnumerateDirectory(dir, walk_pal, &c);
    copy_valid = 0;
    return (int)(tab_n - before);
}

/* ---------------- per frame ---------------- */
void hdpack_update(vdp_hd *hd)
{
    for (int t = 0; t < 2048; t++) {
        const uint8_t *p = g_vram + t * 32;
        if (!copy_valid || memcmp(p, vram_copy + t * 32, 32)) {
            memcpy(vram_copy + t * 32, p, 32);
            tile_hash[t] = fnv(p, 32, FNV0);
            entry *e = find(tile_hash[t]);
            hd->tile[t] = e ? &e->t : NULL;
        }
    }
    copy_valid = 1;
    hd->scale = pack_scale ? pack_scale : 1;
}

/* ---------------- dumping ---------------- */
static uint64_t *dumped;
static size_t dumped_cap, dumped_n;

static int seen(uint64_t k)
{
    if ((dumped_n + 1) * 2 > dumped_cap) {
        size_t oc = dumped_cap;
        uint64_t *old = dumped;
        dumped_cap = oc ? oc * 2 : 8192;
        dumped = calloc(dumped_cap, sizeof *dumped);
        dumped_n = 0;
        for (size_t i = 0; i < oc; i++) if (old[i]) seen(old[i]);
        free(old);
    }
    size_t i = (size_t)(k ^ (k >> 31)) & (dumped_cap - 1);
    while (dumped[i]) {
        if (dumped[i] == k) return 1;
        i = (i + 1) & (dumped_cap - 1);
    }
    dumped[i] = k;
    dumped_n++;
    return 0;
}

static uint32_t cram_rgb(int i)
{
    return vdp_color(g_cram[i & 63]);
}

static void pattern_rgba(int tile, int pal, uint8_t *dst, int stride)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            uint8_t b = g_vram[(tile * 32 + y * 4 + (x >> 1)) & 0xFFFF];
            int c = (x & 1) ? (b & 15) : (b >> 4);
            uint32_t rgb = cram_rgb(pal * 16 + c);
            uint8_t *o = dst + y * stride + x * 4;
            o[0] = (uint8_t)(rgb >> 16); o[1] = (uint8_t)(rgb >> 8); o[2] = (uint8_t)rgb; o[3] = c ? 255 : 0;
        }
}

static void pal_line(FILE *f, int pal)
{
    for (int i = 0; i < 16; i++) fprintf(f, " %06X", cram_rgb(pal * 16 + i) & 0xFFFFFF);
}

int hdpack_dump(const char *dir, int width)
{
    static uint8_t used[2048];
    memset(used, 0, sizeof used);
    vdp_render_used_tiles(used, width);
    char path[1200];
    int wrote = 0;
    SDL_CreateDirectory(dir);
    SDL_snprintf(path, sizeof path, "%spalettes.txt", dir);
    FILE *pf = NULL;
    for (int t = 0; t < 2048; t++) {
        if (!used[t]) continue;
        const uint8_t *p = g_vram + t * 32;
        uint64_t k = fnv(p, 32, FNV0);
        int blank = 1;
        for (int i = 0; i < 32; i++) if (p[i]) { blank = 0; break; }
        if (blank || seen(k)) continue;
        int pal = 0;
        while (!(used[t] & (1 << pal))) pal++;
        uint8_t img[8 * 8 * 4];
        pattern_rgba(t, pal, img, 32);
        char name[1200];
        SDL_snprintf(name, sizeof name, "%s%016llX.png", dir, (unsigned long long)k);
        stbi_write_png(name, 8, 8, 4, img, 32);
        if (!pf) pf = fopen(path, "a");
        if (pf) { fprintf(pf, "%016llX", (unsigned long long)k); pal_line(pf, pal); fputc('\n', pf); }
        wrote++;
    }
    if (pf) fclose(pf);

    /* whole sprites, as sheets that load back as one image */
    int h40 = (g_vreg[12] & 0x81) != 0;
    uint32_t sat = h40 ? (g_vreg[5] & 0x7E) << 9 : (g_vreg[5] & 0x7F) << 9;
    int link = 0;
    for (int k = 0; k < (h40 ? 80 : 64); k++) {
        uint32_t e = sat + link * 8;
        uint16_t s = (uint16_t)(g_vram[(e + 2) & 0xFFFF] << 8 | g_vram[(e + 3) & 0xFFFF]);
        uint16_t a = (uint16_t)(g_vram[(e + 4) & 0xFFFF] << 8 | g_vram[(e + 5) & 0xFFFF]);
        int w = ((s >> 10) & 3) + 1, h = ((s >> 8) & 3) + 1, pal = (a >> 13) & 3, base = a & 0x7FF;
        link = s & 0x7F;
        if (w * h > 1) {
            uint64_t sk = fnv((const uint8_t *)&w, sizeof w, FNV0);
            sk = fnv((const uint8_t *)&h, sizeof h, sk);
            int any = 0;
            for (int i = 0; i < w * h; i++) {
                const uint8_t *p = g_vram + ((base + i) & 0x7FF) * 32;
                sk = fnv(p, 32, sk);
                for (int j = 0; j < 32; j++) any |= p[j];
            }
            if (any && !seen(sk ^ 0x5350525F5350525Full)) {
                uint8_t *img = calloc((size_t)w * 8 * h * 8 * 4, 1);
                char name[1200];
                SDL_snprintf(name, sizeof name, "%sspr_%016llX.txt", dir, (unsigned long long)sk);
                FILE *f = fopen(name, "w");
                if (f) fprintf(f, "%d %d\n", w, h);
                for (int cy = 0; cy < h; cy++) {
                    for (int cx = 0; cx < w; cx++) {
                        int tile = (base + cx * h + cy) & 0x7FF;   /* sprites are column-major */
                        pattern_rgba(tile, pal, img + (cy * 8 * w * 8 + cx * 8) * 4, w * 8 * 4);
                        if (f) fprintf(f, "%s%016llX", cx ? " " : "", (unsigned long long)fnv(g_vram + tile * 32, 32, FNV0));
                    }
                    if (f) fputc('\n', f);
                }
                if (f) { fputs("pal", f); pal_line(f, pal); fputc('\n', f); fclose(f); }
                SDL_snprintf(name, sizeof name, "%sspr_%016llX.png", dir, (unsigned long long)sk);
                stbi_write_png(name, w * 8, h * 8, 4, img, w * 8 * 4);
                free(img);
                wrote++;
            }
        }
        if (link == 0 || link >= (h40 ? 80 : 64)) break;
    }
    return wrote;
}

/* Context sheets: the visible cells of a world plane as one image, so an
 * upscaler sees each pattern among its neighbours (no seams), written as
 * ctx_<hash>.png/.txt. Skipped when every pattern in it was seen before. */
int hdpack_dump_plane(const char *dir, const vdp_world_plane *wp)
{
    if (!wp || !wp->cells) return 0;
    int wrote = 0;
    for (int x0 = 0; x0 < wp->w; x0 += 64) {
        int w = SDL_min(64, wp->w - x0), h = wp->h, fresh = 0;
        uint64_t sk = FNV0;
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                uint16_t e = wp->cells[j * wp->w + x0 + i];
                if (e == 0xFFFF) continue;
                const uint8_t *p = g_vram + (e & 0x7FF) * 32;
                uint64_t k = fnv(p, 32, FNV0) ^ 0x4354585F43545858ull;
                sk = fnv((const uint8_t *)&k, 8, sk);
                if (!seen(k)) fresh++;
            }
        if (fresh < 4) continue;                     /* nothing much new here */
        uint8_t *img = calloc((size_t)w * 8 * h * 8 * 4, 1);
        char name[1200];
        SDL_snprintf(name, sizeof name, "%sctx_%016llX.txt", dir, (unsigned long long)sk);
        FILE *f = fopen(name, "w");
        if (!f) { free(img); continue; }
        fprintf(f, "%d %d\n", w, h);
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                uint16_t e = wp->cells[j * wp->w + x0 + i];
                if (i) fputc(' ', f);
                if (e == 0xFFFF) { fputc('-', f); continue; }
                int tile = e & 0x7FF, pal = (e >> 13) & 3, hf = (e >> 11) & 1, vf = (e >> 12) & 1;
                uint8_t cellpx[8 * 8 * 4];
                pattern_rgba(tile, pal, cellpx, 32);
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        memcpy(img + (((j * 8 + y) * w * 8) + i * 8 + x) * 4,
                               cellpx + ((vf ? 7 - y : y) * 8 + (hf ? 7 - x : x)) * 4, 4);
                fprintf(f, "%016llX%s%s:p%d", (unsigned long long)fnv(g_vram + tile * 32, 32, FNV0),
                        hf ? ":h" : "", vf ? ":v" : "", pal);
            }
            fputc('\n', f);
        }
        for (int pl = 0; pl < 4; pl++) { fprintf(f, "pal%d", pl); pal_line(f, pl); fputc('\n', f); }
        fclose(f);
        SDL_snprintf(name, sizeof name, "%sctx_%016llX.png", dir, (unsigned long long)sk);
        stbi_write_png(name, w * 8, h * 8, 4, img, w * 8 * 4);
        free(img);
        wrote++;
    }
    return wrote;
}

/* ---------------- starter pack ---------------- */
static uint32_t *scale2x(const uint32_t *s, int w, int h)
{
    uint32_t *o = malloc((size_t)w * h * 16);
    int W = w * 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t p = s[y * w + x];
            uint32_t a = s[(y > 0 ? y - 1 : y) * w + x], d = s[(y < h - 1 ? y + 1 : y) * w + x];
            uint32_t c = s[y * w + (x > 0 ? x - 1 : x)], b = s[y * w + (x < w - 1 ? x + 1 : x)];
            o[(2 * y) * W + 2 * x]         = (c == a && c != d && a != b) ? c : p;
            o[(2 * y) * W + 2 * x + 1]     = (a == b && a != c && b != d) ? a : p;
            o[(2 * y + 1) * W + 2 * x]     = (d == c && d != b && c != a) ? c : p;
            o[(2 * y + 1) * W + 2 * x + 1] = (b == d && b != a && d != c) ? b : p;
        }
    return o;
}

typedef struct { const char *out; int n; } starter_ctx;
static SDL_EnumerationResult SDLCALL starter_one(void *ud, const char *dir, const char *name)
{
    starter_ctx *c = ud;
    char src[1200], dst[1200];
    SDL_snprintf(src, sizeof src, "%s%s", dir, name);
    SDL_snprintf(dst, sizeof dst, "%s%s", c->out, name);
    size_t n = strlen(name);
    if (n > 4 && !SDL_strcasecmp(name + n - 4, ".txt")) {
        size_t len;
        void *d = SDL_LoadFile(src, &len);
        if (d) { SDL_SaveFile(dst, d, len); SDL_free(d); }
    } else if (n > 4 && !SDL_strcasecmp(name + n - 4, ".png")) {
        int w, h;
        uint32_t *img = load_png(src, &w, &h);
        if (img && w <= 256 && h <= 256) {
            uint32_t *a = scale2x(img, w, h), *b = scale2x(a, w * 2, h * 2);
            int W = w * 4, H = h * 4;
            uint8_t *rgba = malloc((size_t)W * H * 4);
            for (int i = 0; i < W * H; i++) {
                rgba[i * 4] = (uint8_t)(b[i] >> 16); rgba[i * 4 + 1] = (uint8_t)(b[i] >> 8);
                rgba[i * 4 + 2] = (uint8_t)b[i]; rgba[i * 4 + 3] = (uint8_t)(b[i] >> 24);
            }
            if (stbi_write_png(dst, W, H, 4, rgba, W * 4)) c->n++;
            free(rgba); free(a); free(b);
        }
        free(img);
    }
    return SDL_ENUM_CONTINUE;
}

int hdpack_make_starter(const char *dump_dir, const char *pack_dir)
{
    starter_ctx c = { pack_dir, 0 };
    SDL_CreateDirectory(pack_dir);
    SDL_EnumerateDirectory(dump_dir, starter_one, &c);
    return c.n;
}
