/* Amiga disk assets. See amiga.h. RNC unpacking follows the algorithm of
 * lab313ru/rnc_propack_source (free for any use). */
#include "amiga.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

static uint16_t crc_tab[256];

static void crc_init(void)
{
    if (crc_tab[1]) return;
    for (int i = 0; i < 256; i++) {
        uint16_t c = (uint16_t)i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
        crc_tab[i] = c;
    }
}

static uint16_t crc16(const uint8_t *d, size_t n)
{
    uint16_t c = 0;
    while (n--) c = (uint16_t)(crc_tab[(c ^ *d++) & 0xFF] ^ (c >> 8));
    return c;
}

typedef struct {
    const uint8_t *src;
    size_t n, pos;
    uint32_t buf;
    int cnt;
    uint8_t *out;
    size_t len, cap;
    int bad, over;
} rs;

static uint8_t rbyte(rs *s) { return s->pos < s->n ? s->src[s->pos++] : (s->pos++, 0); }
static uint8_t rpeek(rs *s, size_t k) { return s->pos + k < s->n ? s->src[s->pos + k] : 0; }
static void put(rs *s, uint8_t b) { if (s->len < s->cap) s->out[s->len++] = b; else s->over = 1; }
static void copy_back(rs *s, size_t off, int cnt)
{
    while (cnt--) {
        if (off > s->len) { s->bad = 1; return; }
        put(s, s->out[s->len - off]);
    }
}

/* method 1: Huffman, bits LSB first from little-endian words */
static uint32_t bits1(rs *s, int n)
{
    uint32_t r = 0, p = 1;
    while (n--) {
        if (!s->cnt) {
            uint8_t b1 = rbyte(s), b2 = rbyte(s);
            s->buf = (uint32_t)rpeek(s, 1) << 24 | (uint32_t)rpeek(s, 0) << 16 | (uint32_t)b2 << 8 | b1;
            s->cnt = 16;
        }
        if (s->buf & 1) r |= p;
        s->buf >>= 1; p <<= 1; s->cnt--;
    }
    return r;
}

typedef struct { int n; uint8_t depth[16]; uint32_t code[16]; } huf;

static uint32_t inv(uint32_t v, int n) { uint32_t r = 0; while (n--) { r = (r << 1) | (v & 1); v >>= 1; } return r; }

static void table(rs *s, huf *h)
{
    h->n = (int)bits1(s, 5);
    if (h->n > 16) h->n = 16;
    for (int i = 0; i < h->n; i++) h->depth[i] = (uint8_t)bits1(s, 4);
    uint32_t val = 0, div = 0x80000000u;
    for (int bc = 1; bc <= 16; bc++, div >>= 1)
        for (int i = 0; i < h->n; i++)
            if (h->depth[i] == bc) { h->code[i] = inv(val / div, bc); val += div; }
}

static uint32_t decode(rs *s, const huf *h)
{
    for (int i = 0; i < h->n; i++) {
        int d = h->depth[i];
        if (d && h->code[i] == (s->buf & ((1u << d) - 1))) {
            bits1(s, d);
            if (i < 2) return (uint32_t)i;
            return bits1(s, i - 1) | (1u << (i - 1));
        }
    }
    s->bad = 1;
    return 0;
}

static void unpack1(rs *s)
{
    bits1(s, 1); bits1(s, 1);
    while (s->len < s->cap && !s->bad) {
        huf raw, ln, ps;
        table(s, &raw); table(s, &ln); table(s, &ps);
        uint32_t sub = bits1(s, 16);
        while (sub-- && !s->bad) {
            uint32_t n = decode(s, &raw);
            if (n) {
                while (n--) put(s, rbyte(s));
                s->buf = (((uint32_t)rpeek(s, 2) << 16 | (uint32_t)rpeek(s, 1) << 8 | rpeek(s, 0)) << s->cnt) | (s->buf & ((1u << s->cnt) - 1));
            }
            if (sub) {
                uint32_t off = decode(s, &ln) + 1, cnt = decode(s, &ps) + 2;
                copy_back(s, off, (int)cnt);
            }
        }
    }
}

/* method 2: bits MSB first from bytes */
static int bit2(rs *s)
{
    if (!s->cnt) { s->buf = rbyte(s); s->cnt = 8; }
    int r = (s->buf >> 7) & 1;
    s->buf = (s->buf << 1) & 0xFF;
    s->cnt--;
    return r;
}

static uint32_t moff(rs *s)
{
    uint32_t o = 0;
    if (bit2(s)) {
        o = (uint32_t)bit2(s);
        if (bit2(s)) {
            o = ((o << 1) | (uint32_t)bit2(s)) | 4;
            if (!bit2(s)) o = (o << 1) | (uint32_t)bit2(s);
        } else if (!o) o = (uint32_t)bit2(s) + 2;
    }
    return ((o << 8) | rbyte(s)) + 1;
}

static void unpack2(rs *s)
{
    bit2(s); bit2(s);
    while (s->len < s->cap && !s->bad) {
        for (;;) {
            if (s->over) return;              /* damaged stream ran past the end */
            if (s->bad || s->pos > s->n + 4) { s->bad = 1; return; }
            if (!bit2(s)) { put(s, rbyte(s)); continue; }
            int cnt;
            uint32_t off;
            if (bit2(s)) {
                if (bit2(s)) {
                    if (bit2(s)) {
                        cnt = rbyte(s) + 8;
                        if (cnt == 8) { bit2(s); break; }
                    } else cnt = 3;
                    off = moff(s);
                } else { cnt = 2; off = (uint32_t)rbyte(s) + 1; }
            } else {
                cnt = bit2(s) + 4;
                if (bit2(s)) cnt = ((cnt - 1) << 1) + bit2(s);
                if (cnt == 9) {
                    int n = ((bit2(s) << 3) | (bit2(s) << 2) | (bit2(s) << 1) | bit2(s)) * 4 + 12;
                    while (n--) put(s, rbyte(s));
                    continue;
                }
                off = moff(s);
            }
            copy_back(s, off, cnt);
        }
    }
}

static uint8_t *rnc_unpack_ex(const uint8_t *src, size_t avail, size_t *out_len, int *suspect)
{
    crc_init();
    if (avail < 18 || memcmp(src, "RNC", 3) || (src[3] != 1 && src[3] != 2)) return NULL;
    uint32_t usize = (uint32_t)src[4] << 24 | src[5] << 16 | src[6] << 8 | src[7];
    uint32_t psize = (uint32_t)src[8] << 24 | src[9] << 16 | src[10] << 8 | src[11];
    uint16_t ucrc = (uint16_t)(src[12] << 8 | src[13]), pcrc = (uint16_t)(src[14] << 8 | src[15]);
    if (psize > avail - 18 || usize > (1u << 22) || !usize) return NULL;
    int pcrc_bad = crc16(src + 18, psize) != pcrc;
    if (pcrc_bad && !suspect) return NULL;
    rs s = { 0 };
    s.src = src + 18; s.n = psize;
    s.out = malloc(usize); s.cap = usize;
    if (!s.out) return NULL;
    if (src[3] == 1) unpack1(&s); else unpack2(&s);
    if (s.bad || s.len != usize) { free(s.out); return NULL; }
    int crc_bad = pcrc_bad || s.over || crc16(s.out, usize) != ucrc;
    if (crc_bad && !suspect) { free(s.out); return NULL; }
    if (suspect) *suspect = crc_bad;
    *out_len = usize;
    return s.out;
}

uint8_t *rnc_unpack(const uint8_t *src, size_t avail, size_t *out_len)
{
    return rnc_unpack_ex(src, avail, out_len, NULL);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Every block found on the disks, kept while modules are rebuilt. */
typedef struct { uint8_t *d; size_t n; int method, suspect; } block;
static block *blocks;
static int nblocks, cap_blocks;

static void add_block(uint8_t *d, size_t n, int method, int suspect)
{
    if (nblocks == cap_blocks) {
        int c = cap_blocks ? cap_blocks * 2 : 64;
        block *b = realloc(blocks, (size_t)c * sizeof *b);
        if (!b) { free(d); return; }
        blocks = b; cap_blocks = c;
    }
    blocks[nblocks++] = (block){ d, n, method, suspect };
}

static void free_blocks(void)
{
    for (int i = 0; i < nblocks; i++) free(blocks[i].d);
    free(blocks); blocks = NULL; nblocks = cap_blocks = 0;
}

#define MAX_SAMPLES 160
#define MAX_MODULES 24
static amiga_sample samples[MAX_SAMPLES];
static int nsamples;
static amiga_module modules[MAX_MODULES];
static int nmodules;

int amiga_module_count(void) { return nmodules; }
const amiga_module *amiga_module_at(int i) { return i >= 0 && i < nmodules ? &modules[i] : NULL; }
int amiga_module_find(const char *name)
{
    for (int i = 0; i < nmodules; i++) if (!SDL_strcasecmp(modules[i].name, name)) return i;
    return -1;
}

static int is_module(const uint8_t *u, size_t len) { return len > 1084 && !memcmp(u + 1080, "M.K.", 4); }

static uint64_t fnv(const uint8_t *d, size_t n);

/* An instrument stored on its own: an audio block of exactly that length
 * (a byte or two either way for the odd padded copy). */
static const block *find_sample_block(size_t len)
{
    static const int slack[] = { 0, -2, 2 };
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < nblocks; i++)
            if (blocks[i].method != 1 && blocks[i].n == len + slack[k]) return &blocks[i];
    return NULL;
}

/* ProTracker modules sit inside the packed program and music blocks. Some
 * carry their instruments; most have only the song (header and patterns),
 * the instruments being separate blocks that the game loads by length. */
static void rebuild_modules(void)
{
    for (int bi = 0; bi < nblocks; bi++) {
        const block *b = &blocks[bi];
        if (b->method != 1) continue;
        for (size_t p = 1080; p + 4 <= b->n && nmodules < MAX_MODULES; p++) {
            if (memcmp(b->d + p, "M.K.", 4)) continue;
            const uint8_t *h = b->d + p - 1080;
            size_t avail = b->n - (p - 1080);
            int song_len = h[950], npat = 0;
            if (!song_len || song_len > 128) continue;
            for (int i = 0; i < 128; i++) if (h[952 + i] + 1 > npat) npat = h[952 + i] + 1;
            if (npat > 64) continue;
            size_t head = 1084 + (size_t)npat * 1024, total = 0;
            if (head > avail) continue;
            for (int i = 0; i < 31; i++) total += (size_t)(h[42 + i * 30] << 8 | h[43 + i * 30]) * 2;
            char name[24] = { 0 };
            for (int i = 0, k = 0; i < 20 && h[i]; i++)
                if (SDL_isalnum(h[i])) name[k++] = (char)SDL_tolower(h[i]);
            if (!name[0] || amiga_module_find(name) >= 0) continue;
            uint8_t *out = malloc(head + total);
            if (!out) return;
            memcpy(out, h, head);
            size_t off = head;
            int ok = 1;
            if (avail == head + total) memcpy(out + head, h + head, total);   /* complete module */
            else for (int i = 0; i < 31; i++) {
                size_t L = (size_t)(h[42 + i * 30] << 8 | h[43 + i * 30]) * 2;
                if (!L) continue;
                const block *s = find_sample_block(L);
                if (s) {                                  /* name the instrument in the sample list */
                    char id[20];
                    SDL_snprintf(id, sizeof id, "a%016llX", (unsigned long long)fnv(s->d, s->n));
                    int k = amiga_sample_find(id);
                    if (k >= 0 && !samples[k].label[0]) {
                        char ins[23] = { 0 };
                        memcpy(ins, h + 20 + i * 30, 22);
                        SDL_snprintf(samples[k].label, sizeof samples[k].label, "%s:%s", name, ins);
                    }
                }
                if (!s) { ok = 0; memset(out + off, 0, L); }
                else { memcpy(out + off, s->d, L < s->n ? L : s->n); if (L > s->n) memset(out + off + s->n, 0, L - s->n); }
                off += L;
            }
            amiga_module *m = &modules[nmodules++];
            SDL_strlcpy(m->name, name, sizeof m->name);
            m->data = out;
            m->len = (int)(head + total);
            m->complete = ok;
            p += head - 1;
        }
    }
}
int amiga_sample_count(void) { return nsamples; }
const amiga_sample *amiga_sample_at(int i) { return i >= 0 && i < nsamples ? &samples[i] : NULL; }
int amiga_sample_find(const char *id)
{
    for (int i = 0; i < nsamples; i++) if (!strcmp(samples[i].id, id)) return i;
    return -1;
}

static uint64_t fnv(const uint8_t *d, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *d++; h *= 1099511628211ull; }
    return h;
}

static void scan_adf(const uint8_t *d, size_t n)
{
    for (size_t at = 0; at + 18 < n; at++) {
        if (d[at] != 'R' || d[at + 1] != 'N' || d[at + 2] != 'C') continue;
        int method = d[at + 3];
        if (method == 0) {                        /* stored: "RNC\0", size, data */
            size_t len = (size_t)d[at + 4] << 24 | d[at + 5] << 16 | d[at + 6] << 8 | d[at + 7];
            if (len < 256 || len > (1u << 20) || at + 8 + len > n || d[at + 8] || d[at + 9]) continue;
            uint8_t *u = malloc(len);
            if (!u) continue;
            memcpy(u, d + at + 8, len);
            add_block(u, len, 0, 0);
            continue;                             /* a false match must not hide a real block */
        }
        if (method != 1 && method != 2) continue;
        size_t len;
        int suspect = 0;
        uint8_t *u = rnc_unpack_ex(d + at, n - at, &len, &suspect);
        if (!u) continue;
        add_block(u, len, method, suspect);
        at += 17;
    }
}

static amiga_sample *add_sample(const uint8_t *d, size_t n, const char *label, int rate)
{
    char id[20];
    SDL_snprintf(id, sizeof id, "a%016llX", (unsigned long long)fnv(d, n));
    if (nsamples >= MAX_SAMPLES || amiga_sample_find(id) >= 0) return NULL;
    int8_t *pcm = malloc(n);
    if (!pcm) return NULL;
    memcpy(pcm, d, n);
    amiga_sample *s = &samples[nsamples++];
    SDL_strlcpy(s->id, id, sizeof s->id);
    SDL_strlcpy(s->label, label, sizeof s->label);
    s->pcm = pcm;
    s->len = (int)n;
    s->rate = rate;
    s->per_lo = s->per_hi = 0;
    return s;
}

/* The effect bank: a program file with one data hunk that starts with a
 * table of { offset.l, period_lo.w, period_hi.w, length_in_words.w } up to
 * the first sample. */
static void collect_effect_bank(void)
{
    for (int bi = 0; bi < nblocks; bi++) {
        const block *b = &blocks[bi];
        if (b->method != 1 || b->n < 64 || be32(b->d) != 0x3F3 || be32(b->d + 8) != 1) continue;
        const uint8_t *h = b->d + 0x18;                     /* hunk 0's header: 3EA/3E9, size */
        if ((be32(h) & 0x3FFFFFFF) != 0x3EA && (be32(h) & 0x3FFFFFFF) != 0x3E9) continue;
        size_t size = (size_t)(be32(h + 4) & 0x3FFFFFFF) * 4;
        const uint8_t *data = h + 8;
        if (data + size > b->d + b->n) continue;
        uint32_t first = be32(data);
        int count = (int)(first / 10);
        if (first < 20 || first >= size || count > 64) continue;
        int ok = 1;
        for (int i = 0; i < count && ok; i++) {
            const uint8_t *r = data + i * 10;
            uint32_t off = be32(r), len = (uint32_t)(r[8] << 8 | r[9]) * 2;
            if (off < first || !len || off + len > size) ok = 0;
        }
        if (!ok) continue;
        for (int i = 0; i < count; i++) {
            const uint8_t *r = data + i * 10;
            uint32_t off = be32(r), len = (uint32_t)(r[8] << 8 | r[9]) * 2;
            int per = (r[4] << 8 | r[5]) + (r[6] << 8 | r[7]);
            int rate = per > 0 ? (int)(2 * 3546895.0 / per + 0.5) : 0;
            if (rate > 28000) rate = 28000;
            char label[24];
            SDL_snprintf(label, sizeof label, "fx %d", i + 1);
            amiga_sample *sm = add_sample(data + off, len, label, rate);
            if (sm) { sm->per_lo = r[4] << 8 | r[5]; sm->per_hi = r[6] << 8 | r[7]; }
        }
        return;
    }
}

static void collect_samples(void)
{
    collect_effect_bank();                               /* the game's own effects first */
    for (int i = 0; i < nblocks; i++) {
        const block *b = &blocks[i];
        if (b->method == 1 || b->suspect || b->n < 256 || !memcmp(b->d, "FORM", 4) || is_module(b->d, b->n)) continue;
        add_sample(b->d, b->n, "", 0);
    }
}


/* ---- IFF ILBM pictures (the title photographs) ---- */
#define MAX_PICTURES 8
static amiga_picture pictures[MAX_PICTURES];
static int npictures;

int amiga_picture_count(void) { return npictures; }
const amiga_picture *amiga_picture_at(int i) { return i >= 0 && i < npictures ? &pictures[i] : NULL; }


/* Decode a FORM ILBM (planar, ByteRun1, up to 8 planes, EHB) to ARGB. */
static int decode_ilbm(const uint8_t *d, size_t n, amiga_picture *out)
{
    if (n < 12 || memcmp(d, "FORM", 4) || memcmp(d + 8, "ILBM", 4)) return 0;
    size_t end = 8 + be32(d + 4);
    if (end > n) end = n;
    int w = 0, h = 0, depth = 0, masking = 0, comp = 0, ncol = 0;
    uint32_t camg = 0, pal[256] = { 0 };
    const uint8_t *body = NULL;
    size_t body_n = 0;
    for (size_t p = 12; p + 8 <= end;) {
        uint32_t len = be32(d + p + 4);
        const uint8_t *c = d + p + 8;
        if (p + 8 + len > end) break;
        if (!memcmp(d + p, "BMHD", 4) && len >= 20) { w = c[0] << 8 | c[1]; h = c[2] << 8 | c[3]; depth = c[8]; masking = c[9]; comp = c[10]; }
        else if (!memcmp(d + p, "CMAP", 4)) { ncol = (int)(len / 3); if (ncol > 256) ncol = 256; for (int i = 0; i < ncol; i++) pal[i] = 0xFF000000u | (uint32_t)c[i * 3] << 16 | (uint32_t)c[i * 3 + 1] << 8 | c[i * 3 + 2]; }
        else if (!memcmp(d + p, "CAMG", 4) && len >= 4) camg = be32(c);
        else if (!memcmp(d + p, "BODY", 4)) { body = c; body_n = len; }
        p += 8 + len + (len & 1);
    }
    if (!body || w <= 0 || h <= 0 || w > 1024 || h > 1024 || depth < 1 || depth > 8 || (camg & 0x800)) return 0;
    if (camg & 0x80) for (int i = 0; i < 32; i++) pal[32 + i] = 0xFF000000u | ((pal[i] >> 1) & 0x7F7F7F);
    int rb = (w + 15) / 16 * 2, np = depth + (masking == 1);
    size_t need = (size_t)rb * h * np;
    uint8_t *raw = calloc(need, 1);
    out->rgba = malloc((size_t)w * h * 4);
    if (!raw || !out->rgba) { free(raw); free(out->rgba); out->rgba = NULL; return 0; }
    if (comp == 1) {
        size_t i = 0, o = 0;
        while (o < need && i < body_n) {
            int k = body[i++];
            if (k < 128) { for (int j = 0; j <= k && o < need && i < body_n; j++) raw[o++] = body[i++]; }
            else if (k > 128 && i < body_n) { uint8_t v = body[i++]; for (int j = 0; j < 257 - k && o < need; j++) raw[o++] = v; }
        }
    } else memcpy(raw, body, body_n < need ? body_n : need);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = 0, byte = x >> 3, bit = 7 - (x & 7);
            for (int pl = 0; pl < depth; pl++) v |= ((raw[((size_t)y * np + pl) * rb + byte] >> bit) & 1) << pl;
            out->rgba[(size_t)y * w + x] = pal[v] ? pal[v] : 0xFF000000u;
        }
    free(raw);
    out->w = w; out->h = h;
    return 1;
}

/* The title photographs live in the same program block as the title song. */
static void collect_pictures(void)
{
    for (int bi = 0; bi < nblocks && !npictures; bi++) {
        const block *b = &blocks[bi];
        if (b->method != 1) continue;
        int has_title = 0;
        for (size_t p = 1080; p + 4 <= b->n && !has_title; p++)
            if (!memcmp(b->d + p, "M.K.", 4) && !memcmp(b->d + p - 1080, "datitle", 7)) has_title = 1;
        if (!has_title) continue;
        for (size_t p = 0; p + 12 <= b->n && npictures < MAX_PICTURES; p++) {
            if (memcmp(b->d + p, "FORM", 4) || memcmp(b->d + p + 8, "ILBM", 4)) continue;
            if (decode_ilbm(b->d + p, b->n - p, &pictures[npictures])) npictures++;
            p += 11;
        }
    }
}


/* ---- Cutscene pictures and animations (IFF ILBM / ANIM op 5) ---- */
#define MAX_ANIMS 48
static amiga_anim anims[MAX_ANIMS];
static int nanims;

int amiga_anim_count(void) { return nanims; }
const amiga_anim *amiga_anim_at(int i) { return i >= 0 && i < nanims ? &anims[i] : NULL; }
int amiga_anim_find(const char *id)
{
    for (int i = 0; i < nanims; i++) if (!strcmp(anims[i].id, id)) return i;
    return -1;
}

typedef struct { int w, h, depth, rb, ehb; uint8_t *planes; uint32_t pal[256]; } planar;

/* Parse an ILBM's header chunks and body into planar form. */
static int planar_ilbm(const uint8_t *d, size_t start, size_t end, planar *f)
{
    int masking = 0, comp = 0;
    uint32_t camg = 0;
    const uint8_t *body = NULL;
    size_t body_n = 0;
    memset(f, 0, sizeof *f);
    for (size_t p = start; p + 8 <= end;) {
        uint32_t len = be32(d + p + 4);
        const uint8_t *c = d + p + 8;
        if (p + 8 + len > end) break;
        if (!memcmp(d + p, "BMHD", 4) && len >= 20) { f->w = c[0] << 8 | c[1]; f->h = c[2] << 8 | c[3]; f->depth = c[8]; masking = c[9]; comp = c[10]; }
        else if (!memcmp(d + p, "CMAP", 4)) { int n = (int)(len / 3); if (n > 256) n = 256; for (int i = 0; i < n; i++) f->pal[i] = 0xFF000000u | (uint32_t)c[i * 3] << 16 | (uint32_t)c[i * 3 + 1] << 8 | c[i * 3 + 2]; }
        else if (!memcmp(d + p, "CAMG", 4) && len >= 4) camg = be32(c);
        else if (!memcmp(d + p, "BODY", 4)) { body = c; body_n = len; }
        p += 8 + len + (len & 1);
    }
    if (f->w <= 0 || f->h <= 0 || f->w > 1024 || f->h > 1024 || f->depth < 1 || f->depth > 8 || (camg & 0x800)) return 0;
    f->ehb = (camg & 0x80) != 0;
    if (f->ehb) for (int i = 0; i < 32; i++) f->pal[32 + i] = 0xFF000000u | ((f->pal[i] >> 1) & 0x7F7F7F);
    f->rb = (f->w + 15) / 16 * 2;
    f->planes = calloc((size_t)f->rb * f->h * f->depth, 1);
    if (!f->planes) return 0;
    if (!body) return 1;
    int np = f->depth + (masking == 1);
    size_t need = (size_t)f->rb * f->h * np, o = 0, i = 0;
    uint8_t *raw = calloc(need, 1);
    if (!raw) return 1;
    if (comp == 1) {
        while (o < need && i < body_n) {
            int k = body[i++];
            if (k < 128) { for (int j = 0; j <= k && o < need && i < body_n; j++) raw[o++] = body[i++]; }
            else if (k > 128 && i < body_n) { uint8_t v = body[i++]; for (int j = 0; j < 257 - k && o < need; j++) raw[o++] = v; }
        }
    } else memcpy(raw, body, body_n < need ? body_n : need);
    for (int y = 0; y < f->h; y++)
        for (int pl = 0; pl < f->depth; pl++)
            memcpy(f->planes + ((size_t)pl * f->h + y) * f->rb, raw + ((size_t)y * np + pl) * f->rb, (size_t)f->rb);
    free(raw);
    return 1;
}

/* ANIM op 5: byte vertical delta, one opcode list per column per plane. */
static void delta5(planar *f, const uint8_t *d, size_t n)
{
    if (n < 64) return;
    for (int pl = 0; pl < f->depth && pl < 16; pl++) {
        size_t o = be32(d + pl * 4);
        if (!o) continue;
        uint8_t *plane = f->planes + (size_t)pl * f->h * f->rb;
        for (int col = 0; col < f->rb && o < n; col++) {
            int ops = d[o++], y = 0;
            for (int k = 0; k < ops && o < n; k++) {
                int op = d[o++];
                if (op == 0) {
                    if (o + 2 > n) return;
                    int cnt = d[o], val = d[o + 1]; o += 2;
                    for (int j = 0; j < cnt; j++, y++) if (y < f->h) plane[(size_t)y * f->rb + col] = (uint8_t)val;
                } else if (op < 128) y += op;
                else for (int j = 0; j < (op & 0x7F) && o < n; j++, y++) { uint8_t v = d[o++]; if (y < f->h) plane[(size_t)y * f->rb + col] = v; }
            }
        }
    }
}

static uint8_t *planar_to_index(const planar *f)
{
    uint8_t *px = malloc((size_t)f->w * f->h);
    if (!px) return NULL;
    for (int y = 0; y < f->h; y++)
        for (int x = 0; x < f->w; x++) {
            int v = 0, byte = x >> 3, bit = 7 - (x & 7);
            for (int pl = 0; pl < f->depth; pl++) v |= ((f->planes[((size_t)pl * f->h + y) * f->rb + byte] >> bit) & 1) << pl;
            px[(size_t)y * f->w + x] = (uint8_t)v;
        }
    return px;
}

static void add_anim(const uint8_t *d, size_t n)
{
    if (nanims >= MAX_ANIMS) return;
    size_t end = 8 + be32(d + 4);
    if (end > n) end = n;
    amiga_anim a = { 0 };
    SDL_snprintf(a.id, sizeof a.id, "c%016llX", (unsigned long long)fnv(d, end));
    if (amiga_anim_find(a.id) >= 0) return;
    planar cur = { 0 }, back = { 0 };
    int ok = 0;
    if (!memcmp(d + 8, "ILBM", 4)) {
        if (!planar_ilbm(d, 12, end, &cur)) return;
        a.nframes = 1;
        a.frames = malloc(sizeof *a.frames);
        a.delay = malloc(sizeof *a.delay);
        if (a.frames && a.delay && (a.frames[0] = planar_to_index(&cur))) { a.delay[0] = 0; ok = 1; }
    } else {                                                     /* ANIM: FORM ILBM frames */
        int cap = 0;
        for (size_t p = 12; p + 12 <= end;) {
            uint32_t len = be32(d + p + 4);
            if (p + 8 + len > end) break;
            if (!memcmp(d + p, "FORM", 4) && !memcmp(d + p + 8, "ILBM", 4)) {
                size_t s0 = p + 12, s1 = p + 8 + len;
                if (a.nframes == cap) {
                    cap = cap ? cap * 2 : 16;
                    uint8_t **nf = realloc(a.frames, (size_t)cap * sizeof *nf);
                    int *nd = realloc(a.delay, (size_t)cap * sizeof *nd);
                    if (nf) a.frames = nf;
                    if (nd) a.delay = nd;
                    if (!nf || !nd) break;
                }
                if (!a.nframes) {
                    if (!planar_ilbm(d, s0, s1, &cur)) break;
                    back = cur;
                    back.planes = malloc((size_t)cur.rb * cur.h * cur.depth);
                    if (!back.planes) break;
                    memcpy(back.planes, cur.planes, (size_t)cur.rb * cur.h * cur.depth);
                    a.frames[0] = planar_to_index(&cur);
                    a.delay[0] = 6;
                    a.nframes = 1;
                } else {
                    const uint8_t *anhd = NULL, *dlta = NULL;
                    size_t dlta_n = 0;
                    for (size_t q = s0; q + 8 <= s1;) {
                        uint32_t l2 = be32(d + q + 4);
                        if (q + 8 + l2 > s1) break;
                        if (!memcmp(d + q, "ANHD", 4) && l2 >= 24) anhd = d + q + 8;
                        else if (!memcmp(d + q, "DLTA", 4)) { dlta = d + q + 8; dlta_n = l2; }
                        q += 8 + l2 + (l2 & 1);
                    }
                    if (!anhd || !dlta || anhd[0] != 5) { p += 8 + len + (len & 1); continue; }
                    /* double buffered: the delta applies to the frame before last */
                    delta5(&back, dlta, dlta_n);
                    planar t = cur; cur = back; back = t;
                    a.frames[a.nframes] = planar_to_index(&cur);
                    a.delay[a.nframes] = (int)be32(anhd + 14);          /* reltime, jiffies */
                    if (a.frames[a.nframes]) a.nframes++;
                }
            }
            p += 8 + len + (len & 1);
        }
        ok = a.nframes > 0;
    }
    if (ok) {
        a.w = cur.w; a.h = cur.h;
        memcpy(a.pal, cur.pal, sizeof a.pal);
        anims[nanims++] = a;
    } else {
        for (int i = 0; i < a.nframes; i++) free(a.frames[i]);
        free(a.frames); free(a.delay);
    }
    free(cur.planes);
    if (back.planes != cur.planes) free(back.planes);
}

static void collect_anims(void)
{
    for (int bi = 0; bi < nblocks; bi++) {
        const block *b = &blocks[bi];
        if (b->method != 1) continue;
        for (size_t p = 0; p + 12 <= b->n; p++) {
            if (memcmp(b->d + p, "FORM", 4)) continue;
            int anim = !memcmp(b->d + p + 8, "ANIM", 4);
            if (!anim && memcmp(b->d + p + 8, "ILBM", 4)) continue;
            int w = 0, h = 0;                                        /* only screen-sized pictures */
            const uint8_t *bm = NULL;
            for (size_t q = p + (anim ? 24 : 12); q + 28 <= b->n && q < p + 64; q++) if (!memcmp(b->d + q, "BMHD", 4)) { bm = b->d + q + 8; break; }
            if (bm) { w = bm[0] << 8 | bm[1]; h = bm[2] << 8 | bm[3]; }
            if ((w >= 200 && h >= 60) || (w >= 600 && h >= 20)) add_anim(b->d + p, b->n - p);   /* + the wide scrolling strips */
            p += anim ? 11 : 8 + be32(b->d + p + 4) - 1;
        }
    }
}


/* ---- Cutscene "bobs": the small animated overlays the cutscene programs
 * blit onto their pictures (the briefing room's officer, seated pilots and
 * cigarette smoke). Found by the program's own draw routine, which indexes
 * a table of { offset.l, width.w, height.w } with
 *   lea <bob data>,a0 ; adda.l <table>(pc,d3.w),a0
 * and stored masked: each row is mask, then 5 bitplanes. */
#define MAX_BOBS 64
static amiga_bob bobs[MAX_BOBS];
static int nbobs;

int amiga_bob_count(void) { return nbobs; }
const amiga_bob *amiga_bob_at(int i) { return i >= 0 && i < nbobs ? &bobs[i] : NULL; }

typedef struct { const uint8_t *d; size_t n; int type; } hunkview;

static int load_hunks(const uint8_t *d, size_t n, hunkview *h, int max)
{
    if (n < 24 || be32(d) != 0x3F3) return 0;
    size_t p = 4;
    while (p + 4 <= n && be32(d + p)) p += 4 + (size_t)be32(d + p) * 4;   /* resident library names */
    p += 4;
    if (p + 8 > n) return 0;
    uint32_t first = be32(d + p + 4), last = be32(d + p + 8);
    p += 12 + (size_t)(last - first + 1) * 4;
    int k = 0;
    while (p + 8 <= n && k < max) {
        uint32_t t = be32(d + p) & 0x3FFFFFFF;
        if (t == 0x3E9 || t == 0x3EA) {
            size_t len = (size_t)be32(d + p + 4) * 4;
            if (p + 8 + len > n) break;
            h[k].d = d + p + 8; h[k].n = len; h[k].type = (int)t; k++;
            p += 8 + len;
        } else if (t == 0x3EB) { h[k].d = NULL; h[k].n = (size_t)be32(d + p + 4) * 4; h[k].type = (int)t; k++; p += 8; }
        else if (t == 0x3EC) {
            p += 4;
            for (;;) { if (p + 4 > n) return k; uint32_t c = be32(d + p); p += 4; if (!c) break; p += 4 + (size_t)c * 4; }
        } else if (t == 0x3F2) p += 4;
        else break;
    }
    return k;
}

static void collect_bobs(void)
{
    for (int bi = 0; bi < nblocks; bi++) {
        const block *b = &blocks[bi];
        if (b->method != 1) continue;
        hunkview h[8];
        int nh = load_hunks(b->d, b->n, h, 8);
        if (nh < 2 || h[0].type != 0x3E9) continue;
        int data = -1;
        for (int i = 1; i < nh; i++) if (h[i].type == 0x3EA && h[i].d) { data = i; break; }
        if (data < 0) continue;
        const uint8_t *code = h[0].d, *dh = h[data].d;
        size_t cn = h[0].n, dn = h[data].n;
        for (size_t p = 0; p + 10 <= cn; p += 2) {
            if (code[p] != 0x41 || code[p + 1] != 0xF9 || code[p + 6] != 0xD1 || code[p + 7] != 0xFB || code[p + 8] != 0x30) continue;
            uint32_t base = be32(code + p + 2);
            size_t tab = p + 8 + (int8_t)code[p + 9];         /* d8(pc,d3.w): pc = the extension word */
            /* the picture these bobs belong to: the first ILBM in the data hunk */
            char owner[20] = "";
            for (size_t q = 0; q + 12 <= dn; q++)
                if (!memcmp(dh + q, "FORM", 4) && !memcmp(dh + q + 8, "ILBM", 4)) {
                    size_t e = 8 + be32(dh + q + 4);
                    if (q + e <= dn) SDL_snprintf(owner, sizeof owner, "c%016llX", (unsigned long long)fnv(dh + q, e));
                    break;
                }
            for (int i = 0; i < 32 && nbobs < MAX_BOBS && tab + i * 8 + 8 <= cn; i++) {
                uint32_t off = be32(code + tab + i * 8);
                int w = code[tab + i * 8 + 4] << 8 | code[tab + i * 8 + 5], hgt = code[tab + i * 8 + 6] << 8 | code[tab + i * 8 + 7];
                int W = (w + 15) / 16 * 16, rb = W / 8;
                if (w <= 0 || w > 320 || hgt <= 0 || hgt > 256 || base + off + (size_t)rb * 6 * hgt > dn) break;
                amiga_bob *o = &bobs[nbobs];
                memset(o, 0, sizeof *o);
                SDL_strlcpy(o->owner, owner, sizeof o->owner);
                o->index = i; o->w = W; o->h = hgt;
                o->mask = malloc((size_t)W * hgt);
                o->px = malloc((size_t)W * hgt);
                if (!o->mask || !o->px) { free(o->mask); free(o->px); break; }
                const uint8_t *src = dh + base + off;
                for (int y = 0; y < hgt; y++)
                    for (int x = 0; x < W; x++) {
                        int byte = x >> 3, bit = 7 - (x & 7), v = 0;
                        const uint8_t *row = src + (size_t)y * rb * 6;
                        for (int pl = 0; pl < 5; pl++) v |= ((row[rb * (pl + 1) + byte] >> bit) & 1) << pl;
                        o->mask[y * W + x] = (row[byte] >> bit) & 1;
                        o->px[y * W + x] = (uint8_t)v;
                    }
                nbobs++;
            }
            break;
        }
    }
}

/* ---- Cutscene programs, kept whole for the Amiga VM (amiga_vm.c), keyed
 * like the bobs by the hash of the first picture they hold. */
#define MAX_PROGRAMS 48
static amiga_program programs[MAX_PROGRAMS];
static int nprograms;

int amiga_program_count(void) { return nprograms; }
const amiga_program *amiga_program_at(int i) { return i >= 0 && i < nprograms ? &programs[i] : NULL; }
const amiga_program *amiga_program_find(const char *owner)
{
    for (int i = 0; i < nprograms; i++) if (!SDL_strcmp(programs[i].owner, owner)) return &programs[i];
    return NULL;
}

static void collect_programs(void)
{
    for (int bi = 0; bi < nblocks && nprograms < MAX_PROGRAMS; bi++) {
        const block *b = &blocks[bi];
        if (b->method != 1 || b->n < 24 || be32(b->d) != 0x3F3 || b->n > 1500000) continue;
        for (size_t q = 0; q + 12 <= b->n; q++)
            if (!memcmp(b->d + q, "FORM", 4) && !memcmp(b->d + q + 8, "ILBM", 4)) {
                size_t e = 8 + be32(b->d + q + 4);
                if (q + e > b->n) break;
                amiga_program *o = &programs[nprograms];
                if (!(o->data = malloc(b->n))) break;
                memcpy(o->data, b->d, b->n);
                o->n = b->n;
                SDL_snprintf(o->owner, sizeof o->owner, "c%016llX", (unsigned long long)fnv(b->d + q, e));
                nprograms++;
                break;
            }
    }
}

static SDL_EnumerationResult SDLCALL scan_cb(void *ud, const char *dir, const char *name)
{
    (void)ud;
    size_t l = SDL_strlen(name);
    if (l < 4 || SDL_strcasecmp(name + l - 4, ".adf")) return SDL_ENUM_CONTINUE;
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s%s", dir, name);
    size_t n = 0;
    uint8_t *d = SDL_LoadFile(path, &n);
    if (d) { scan_adf(d, n); SDL_free(d); }
    return SDL_ENUM_CONTINUE;
}

int amiga_load_dir(const char *dir)
{
    for (int i = 0; i < nsamples; i++) free(samples[i].pcm);
    for (int i = 0; i < nmodules; i++) free(modules[i].data);
    for (int i = 0; i < npictures; i++) free(pictures[i].rgba);
    for (int i = 0; i < nanims; i++) { for (int k = 0; k < anims[i].nframes; k++) free(anims[i].frames[k]); free(anims[i].frames); free(anims[i].delay); }
    for (int i = 0; i < nbobs; i++) { free(bobs[i].mask); free(bobs[i].px); }
    for (int i = 0; i < nprograms; i++) free(programs[i].data);
    nprograms = 0;
    nsamples = nmodules = npictures = nanims = nbobs = 0;
    SDL_EnumerateDirectory(dir, scan_cb, NULL);
    collect_samples();
    rebuild_modules();
    collect_pictures();
    collect_anims();
    collect_bobs();
    collect_programs();
    free_blocks();
    return nsamples;
}
