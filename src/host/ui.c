/* HD menu drawing. See ui.h. */
#include <stdlib.h>
#include <string.h>
#include "ui.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

extern const unsigned char font_body[], font_title[];

static SDL_Renderer *R;
static float S = 1, OX, OY;                  /* scale and offset: pixel = O + virtual * S */
static int out_w, out_h;

static const float font_px[UI_FONT_COUNT] = { 18, 25, 33, 58 };   /* virtual pixel heights */

typedef struct {
    SDL_Texture *tex;
    int aw, ah;
    float px;                                /* baked size in real pixels */
    float ascent;                            /* in real pixels */
    stbtt_packedchar ch[96];
} font_t;
static font_t fonts[UI_FONT_COUNT];

static void bake(font_t *f, const unsigned char *ttf, float px)
{
    if (f->tex) { SDL_DestroyTexture(f->tex); f->tex = 0; }
    int a = 256;
    while ((float)a * a < px * px * 140.0f) a *= 2;
    unsigned char *alpha = calloc((size_t)a * a, 1);
    stbtt_pack_context pc;
    stbtt_PackBegin(&pc, alpha, a, a, 0, 1, 0);
    stbtt_PackSetOversampling(&pc, 2, 1);
    stbtt_PackFontRange(&pc, ttf, 0, px, 32, 96, f->ch);
    stbtt_PackEnd(&pc);
    stbtt_fontinfo info;
    stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0));
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    f->ascent = asc * stbtt_ScaleForPixelHeight(&info, px);
    uint32_t *rgba = malloc((size_t)a * a * 4);
    for (int i = 0; i < a * a; i++) rgba[i] = 0x00FFFFFFu | ((uint32_t)alpha[i] << 24);
    f->tex = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, a, a);
    SDL_UpdateTexture(f->tex, 0, rgba, a * 4);
    SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(f->tex, SDL_SCALEMODE_LINEAR);
    free(rgba);
    free(alpha);
    f->aw = a; f->ah = a; f->px = px;
}

void ui_init(SDL_Renderer *r) { R = r; }

void ui_shutdown(void)
{
    for (int i = 0; i < UI_FONT_COUNT; i++) if (fonts[i].tex) SDL_DestroyTexture(fonts[i].tex);
    memset(fonts, 0, sizeof fonts);
}

void ui_begin(int w, int h)
{
    out_w = w; out_h = h;
    S = SDL_min(w / UI_W, h / UI_H);
    OX = (w - UI_W * S) / 2;
    OY = (h - UI_H * S) / 2;
    for (int i = 0; i < UI_FONT_COUNT; i++) {
        float px = SDL_floorf(font_px[i] * S + 0.5f);
        if (px < 8) px = 8;
        if (!fonts[i].tex || fonts[i].px != px) bake(&fonts[i], i == UI_TITLE ? font_title : font_body, px);
    }
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
}

void ui_to_virtual(float px, float py, float *vx, float *vy)
{
    *vx = (px - OX) / S;
    *vy = (py - OY) / S;
}

static float measure(font_t *f, const char *s)
{
    float x = 0;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < 32 || c > 127) c = '?';
        x += f->ch[c - 32].xadvance;
    }
    return x / S;
}

float ui_text_w(ui_font f, const char *s) { return measure(&fonts[f], s); }
float ui_line_h(ui_font f) { return font_px[f] * 1.15f; }

float ui_text(ui_font fi, float x, float y, SDL_FColor c, const char *s)
{
    font_t *f = &fonts[fi];
    size_t n = strlen(s);
    if (!n) return 0;
    SDL_Vertex *v = SDL_malloc(n * 4 * sizeof *v);
    int *idx = SDL_malloc(n * 6 * sizeof *idx);
    float px = SDL_floorf(OX + x * S), py = SDL_floorf(OY + y * S + f->ascent);
    float start = px;
    int q = 0;
    for (size_t i = 0; i < n; i++) {
        int ch = (unsigned char)s[i];
        if (ch < 32 || ch > 127) ch = '?';
        stbtt_aligned_quad aq;
        stbtt_GetPackedQuad(f->ch, f->aw, f->ah, ch - 32, &px, &py, &aq, 0);
        SDL_Vertex *qv = v + q * 4;
        float xs[4] = { aq.x0, aq.x1, aq.x1, aq.x0 }, ys[4] = { aq.y0, aq.y0, aq.y1, aq.y1 };
        float us[4] = { aq.s0, aq.s1, aq.s1, aq.s0 }, vs[4] = { aq.t0, aq.t0, aq.t1, aq.t1 };
        for (int k = 0; k < 4; k++) {
            qv[k].position.x = xs[k]; qv[k].position.y = ys[k];
            qv[k].color = c;
            qv[k].tex_coord.x = us[k]; qv[k].tex_coord.y = vs[k];
        }
        int *qi = idx + q * 6;
        qi[0] = q * 4; qi[1] = q * 4 + 1; qi[2] = q * 4 + 2;
        qi[3] = q * 4; qi[4] = q * 4 + 2; qi[5] = q * 4 + 3;
        q++;
    }
    SDL_RenderGeometry(R, f->tex, v, q * 4, idx, q * 6);
    SDL_free(v);
    SDL_free(idx);
    return (px - start) / S;
}

void ui_text_center(ui_font f, float cx, float y, SDL_FColor c, const char *s)
{
    ui_text(f, cx - ui_text_w(f, s) / 2, y, c, s);
}

void ui_text_right(ui_font f, float rx, float y, SDL_FColor c, const char *s)
{
    ui_text(f, rx - ui_text_w(f, s), y, c, s);
}

static void set_color(SDL_FColor c) { SDL_SetRenderDrawColorFloat(R, c.r, c.g, c.b, c.a); }

void ui_rect(float x, float y, float w, float h, SDL_FColor c)
{
    set_color(c);
    SDL_FRect r = { OX + x * S, OY + y * S, w * S, h * S };
    SDL_RenderFillRect(R, &r);
}

void ui_frame(float x, float y, float w, float h, float t, SDL_FColor c)
{
    ui_rect(x, y, w, t, c);
    ui_rect(x, y + h - t, w, t, c);
    ui_rect(x, y + t, t, h - 2 * t, c);
    ui_rect(x + w - t, y + t, t, h - 2 * t, c);
}

static void poly(const float *pts, int n, SDL_FColor c)
{
    SDL_Vertex v[16];
    int idx[48], k = 0;
    for (int i = 0; i < n; i++) {
        v[i].position.x = OX + pts[2 * i] * S;
        v[i].position.y = OY + pts[2 * i + 1] * S;
        v[i].color = c;
        v[i].tex_coord.x = v[i].tex_coord.y = 0;
    }
    for (int i = 1; i + 1 < n; i++) { idx[k++] = 0; idx[k++] = i; idx[k++] = i + 1; }
    SDL_RenderGeometry(R, 0, v, n, idx, k);
}

void ui_triangle(float x0, float y0, float x1, float y1, float x2, float y2, SDL_FColor c)
{
    float p[6] = { x0, y0, x1, y1, x2, y2 };
    poly(p, 3, c);
}

void ui_panel(float x, float y, float w, float h, float cut, SDL_FColor fill, SDL_FColor edge)
{
    float p[12] = { x + cut, y, x + w, y, x + w, y + h - cut, x + w - cut, y + h, x, y + h, x, y + cut };
    poly(p, 6, fill);
    if (edge.a > 0) {
        float t = 2.0f;
        /* edges as thin quads following the outline */
        for (int i = 0; i < 6; i++) {
            float ax = p[2 * i], ay = p[2 * i + 1], bx = p[(2 * i + 2) % 12], by = p[(2 * i + 3) % 12];
            float dx = bx - ax, dy = by - ay, len = SDL_sqrtf(dx * dx + dy * dy);
            if (len <= 0) continue;
            float nx = -dy / len * t / 2, ny = dx / len * t / 2;
            float q[8] = { ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny };
            poly(q, 4, edge);
        }
    }
}

void ui_image(SDL_Texture *t, float x, float y, float w, float h)
{
    SDL_FRect r = { OX + x * S, OY + y * S, w * S, h * S };
    SDL_RenderTexture(R, t, 0, &r);
}

void ui_dim(float a)
{
    SDL_SetRenderDrawColorFloat(R, 0, 0, 0, a);
    SDL_FRect r = { 0, 0, (float)out_w, (float)out_h };
    SDL_RenderFillRect(R, &r);
}
