/* ProTracker module player. See modplay.h. */
#include "modplay.h"
#include <math.h>
#include <string.h>

typedef struct {
    const int8_t *data;
    int len, loop_start, loop_len;     /* bytes */
    int finetune, volume;
} smp;

typedef struct {
    const smp *s;
    int sample;                        /* 1-based, 0 none */
    double pos, step;
    int period, target, porta_speed;
    int volume;
    int note_period;                   /* period of the note (for arpeggio) */
    int vib_pos, vib_speed, vib_depth;
    int trem_pos, trem_speed, trem_depth;
    int eff, param;
    int arp_out;                       /* period offset this tick */
    int vol_out;
    int loop_row, loop_count;
    int cut_tick, delay_tick, retrig;
    int delayed_note, delayed_smp;
    int offset_mem;
    int active;
} chan;

static struct {
    const uint8_t *d;
    int len, rate, loop, playing;
    smp s[32];
    int song_len, npat;
    const uint8_t *order, *pat;
    int ord, row, tick, speed, bpm;
    double tick_left;                  /* output frames left in this tick */
    int next_ord, next_row, jump;
    int pattern_delay, pdelay_left;
    chan c[4];
    float volume;
} m;

static const int8_t sine[32] = { 0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
                                 255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24 };

static void row_start(void);
static void tick_effects(void);
static int r16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static int fine_period(int period, int ft)
{
    if (!ft) return period;
    return (int)(period * pow(2.0, -ft / 96.0) + 0.5);
}

static void set_freq(chan *c, int period)
{
    if (period < 28) period = 28;
    c->step = (3546894.6 / period) / m.rate;
}

int mod_start(const uint8_t *data, int len, int out_rate, int loop)
{
    mod_stop();
    if (len < 1084 + 1024 || memcmp(data + 1080, "M.K.", 4)) return 0;
    memset(&m.s, 0, sizeof m.s);
    m.d = data; m.len = len; m.rate = out_rate; m.loop = loop;
    m.song_len = data[950];
    m.order = data + 952;
    m.npat = 0;
    for (int i = 0; i < 128; i++) if (m.order[i] + 1 > m.npat) m.npat = m.order[i] + 1;
    m.pat = data + 1084;
    int off = 1084 + m.npat * 1024;
    for (int i = 1; i <= 31; i++) {
        const uint8_t *h = data + 20 + (i - 1) * 30;
        smp *s = &m.s[i];
        s->len = r16(h + 22) * 2;
        s->finetune = h[24] & 15; if (s->finetune > 7) s->finetune -= 16;
        s->volume = h[25] > 64 ? 64 : h[25];
        s->loop_start = r16(h + 26) * 2;
        s->loop_len = r16(h + 28) * 2;
        if (off + s->len > len) s->len = len - off > 0 ? len - off : 0;
        s->data = (const int8_t *)data + off;
        off += r16(h + 22) * 2;
        if (s->loop_len <= 2) s->loop_len = 0;
        if (s->loop_start + s->loop_len > s->len) s->loop_len = s->len - s->loop_start > 2 ? s->len - s->loop_start : 0;
    }
    memset(m.c, 0, sizeof m.c);
    m.ord = 0; m.row = 0; m.tick = 0; m.speed = 6; m.bpm = 125;
    m.tick_left = 0; m.jump = 0; m.pattern_delay = 0; m.pdelay_left = 0;
    if (m.volume == 0) m.volume = 1;
    m.playing = 1;
    row_start();
    tick_effects();
    m.tick_left = m.rate * 2.5 / m.bpm;
    return 1;
}

void mod_stop(void) { m.playing = 0; }
int mod_playing(void) { return m.playing; }
void mod_set_volume(float v) { m.volume = v < 0 ? 0 : v > 2 ? 2 : v; }

static void trigger(chan *c, int period)
{
    if (!c->s) return;
    c->pos = 0;
    c->active = 1;
    c->period = period;
    c->note_period = period;
    set_freq(c, period);
    if (c->vib_depth) c->vib_pos = 0;
}

static void row_start(void)
{
    const uint8_t *p = m.pat + m.order[m.ord] * 1024 + m.row * 16;
    for (int ch = 0; ch < 4; ch++, p += 4) {
        chan *c = &m.c[ch];
        int sn = (p[0] & 0xF0) | (p[2] >> 4);
        int per = ((p[0] & 0x0F) << 8) | p[1];
        int eff = p[2] & 0x0F, prm = p[3];
        c->eff = eff; c->param = prm;
        c->arp_out = 0; c->cut_tick = -1; c->delay_tick = -1; c->retrig = 0;
        if (sn && sn <= 31) {
            c->sample = sn; c->s = &m.s[sn];
            c->volume = c->s->volume;
        }
        if (per && c->s) per = fine_period(per, c->s->finetune);
        if (eff == 0xE && (prm >> 4) == 0xD && (prm & 15)) {        /* note delay */
            c->delay_tick = prm & 15; c->delayed_note = per; per = 0;
        } else if (per) {
            if (eff == 3 || eff == 5) { c->target = per; }
            else {
                trigger(c, per);
                if (eff == 9) {
                    if (prm) c->offset_mem = prm;
                    c->pos = c->offset_mem * 256.0;
                }
            }
        }
        switch (eff) {
        case 0x3: if (prm) c->porta_speed = prm; break;
        case 0x4: if (prm >> 4) c->vib_speed = prm >> 4; if (prm & 15) c->vib_depth = prm & 15; break;
        case 0x7: if (prm >> 4) c->trem_speed = prm >> 4; if (prm & 15) c->trem_depth = prm & 15; break;
        case 0xB: m.next_ord = prm; m.next_row = 0; m.jump = 1; break;
        case 0xC: c->volume = prm > 64 ? 64 : prm; break;
        case 0xD: if (!m.jump) m.next_ord = m.ord + 1; m.next_row = (prm >> 4) * 10 + (prm & 15); m.jump = 1; break;
        case 0xE:
            switch (prm >> 4) {
            case 0x1: c->period -= prm & 15; set_freq(c, c->period); break;
            case 0x2: c->period += prm & 15; set_freq(c, c->period); break;
            case 0x6:
                if (!(prm & 15)) c->loop_row = m.row;
                else {
                    if (!c->loop_count) c->loop_count = prm & 15;
                    else c->loop_count--;
                    if (c->loop_count) { m.next_ord = m.ord; m.next_row = c->loop_row; m.jump = 1; }
                }
                break;
            case 0x9: c->retrig = prm & 15; break;
            case 0xA: c->volume += prm & 15; if (c->volume > 64) c->volume = 64; break;
            case 0xB: c->volume -= prm & 15; if (c->volume < 0) c->volume = 0; break;
            case 0xC: c->cut_tick = prm & 15; break;
            case 0xE: m.pattern_delay = prm & 15; break;
            }
            break;
        case 0xF:
            if (prm == 0) break;
            if (prm < 32) m.speed = prm; else m.bpm = prm;
            break;
        }
        c->vol_out = c->volume;
    }
}

static void vol_slide(chan *c)
{
    int up = c->param >> 4, dn = c->param & 15;
    c->volume += up ? up : -dn;
    if (c->volume < 0) c->volume = 0;
    if (c->volume > 64) c->volume = 64;
}

static void tone_porta(chan *c)
{
    if (!c->target) return;
    if (c->period < c->target) { c->period += c->porta_speed; if (c->period > c->target) c->period = c->target; }
    else if (c->period > c->target) { c->period -= c->porta_speed; if (c->period < c->target) c->period = c->target; }
    set_freq(c, c->period);
}

static void vibrato(chan *c)
{
    int d = sine[c->vib_pos & 31] * c->vib_depth / 128;
    if (c->vib_pos & 32) d = -d;
    set_freq(c, c->period + d);
    c->vib_pos += c->vib_speed;
}

static void tick_effects(void)
{
    for (int ch = 0; ch < 4; ch++) {
        chan *c = &m.c[ch];
        c->vol_out = c->volume;
        if (c->delay_tick == m.tick) { trigger(c, c->delayed_note); c->vol_out = c->volume; }
        if (c->cut_tick == m.tick) { c->volume = 0; c->vol_out = 0; }
        if (c->retrig && m.tick && m.tick % c->retrig == 0) c->pos = 0;
        if (!m.tick) continue;
        switch (c->eff) {
        case 0x0:
            if (c->param) {
                int k = m.tick % 3, semis = k == 1 ? c->param >> 4 : k == 2 ? c->param & 15 : 0;
                set_freq(c, (int)(c->period * pow(2.0, -semis / 12.0) + 0.5));
            }
            break;
        case 0x1: c->period -= c->param; if (c->period < 113) c->period = 113; set_freq(c, c->period); break;
        case 0x2: c->period += c->param; if (c->period > 856) c->period = 856; set_freq(c, c->period); break;
        case 0x3: tone_porta(c); break;
        case 0x4: vibrato(c); break;
        case 0x5: tone_porta(c); vol_slide(c); break;
        case 0x6: vibrato(c); vol_slide(c); break;
        case 0x7: {
            int d = sine[c->trem_pos & 31] * c->trem_depth / 64;
            if (c->trem_pos & 32) d = -d;
            int v = c->volume + d;
            c->vol_out = v < 0 ? 0 : v > 64 ? 64 : v;
            c->trem_pos += c->trem_speed;
            break;
        }
        case 0xA: vol_slide(c); break;
        }
        if (c->eff != 0x7) c->vol_out = c->volume;
    }
}

static void advance(void)
{
    if (++m.tick < m.speed * (1 + m.pdelay_left)) { tick_effects(); return; }
    m.tick = 0;
    if (m.pattern_delay) { m.pdelay_left = m.pattern_delay; m.pattern_delay = 0; }
    else m.pdelay_left = 0;
    if (m.jump) {
        m.jump = 0;
        m.ord = m.next_ord; m.row = m.next_row;
        if (m.row > 63) m.row = 0;
    } else if (++m.row >= 64) { m.row = 0; m.ord++; }
    if (m.ord >= m.song_len) {
        if (!m.loop) { m.playing = 0; return; }
        m.ord = 0;
    }
    row_start();
    tick_effects();
}

void mod_mix(int16_t *out, int frames)
{
    if (!m.playing) return;
    for (int f = 0; f < frames && m.playing; f++) {
        if (m.tick_left <= 0) {
            advance();
            m.tick_left += m.rate * 2.5 / m.bpm;
        }
        m.tick_left -= 1;
        int l = 0, r = 0;
        for (int ch = 0; ch < 4; ch++) {
            chan *c = &m.c[ch];
            if (!c->active || !c->s || !c->s->len) continue;
            int ip = (int)c->pos;
            if (c->s->loop_len) {
                int end = c->s->loop_start + c->s->loop_len;
                while (ip >= end) { c->pos -= c->s->loop_len; ip = (int)c->pos; }
            } else if (ip >= c->s->len - 1) { c->active = 0; continue; }
            double fr = c->pos - ip;
            int a = c->s->data[ip], b = ip + 1 < c->s->len ? c->s->data[ip + 1] : a;
            int x = (int)((a + (b - a) * fr) * 256) * c->vol_out / 64;
            if (ch == 0 || ch == 3) { l += x * 3 / 4; r += x / 4; } else { r += x * 3 / 4; l += x / 4; }
            c->pos += c->step;
        }
        l = (int)(l * m.volume * 0.55f); r = (int)(r * m.volume * 0.55f);
        int L = out[f * 2] + l, R = out[f * 2 + 1] + r;
        out[f * 2] = (int16_t)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
        out[f * 2 + 1] = (int16_t)(R > 32767 ? 32767 : R < -32768 ? -32768 : R);
    }
}
