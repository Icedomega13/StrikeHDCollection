/* Genesis sound hardware. See sound.h. */
#include <string.h>
#include "genesis.h"
#include "sound.h"
#include "ym2612.h"
#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef SND_DEBUG
static FILE *dbg;
static void dlog(const char *src, int port, uint8_t v);
#define YMW(src, p, v) (dlog(src, p, v), ym_write(p, v))
#else
#define YMW(src, p, v) ym_write(p, v)
#endif

#define Z80_MCLK  15u                 /* Z80 runs at master / 15 */
#define PSG_MCLK  240u                /* PSG counters tick at master / 15 / 16 */
#define Z80_INT_MCLK (228u * Z80_MCLK) /* /INT is held for about one scanline */

typedef struct {
    uint16_t tone[3];
    uint8_t vol[4], noise, latch;
    int16_t cnt[4];
    uint8_t pol[4];
    uint16_t lfsr;
    uint32_t frac;                    /* master clocks not yet turned into ticks */
} psg_t;

static struct {
    z80 z;
    uint64_t z80_mclk;               /* sound hardware time, master clocks */
    uint64_t ym_next;                /* time of next YM2612 sample */
    uint64_t int_end;                /* /INT deasserts at this time */
    int busreq, reset;               /* 68000 control of the Z80 */
    uint32_t bank;                   /* Z80 $8000 window = 68000 bank << 15 */
    psg_t psg;
    int32_t dc_x[2], dc_y[2];        /* DC blocker state, 16.16 */
} S;

/* Output mix levels (256 = 100%). Host-side only: not part of the
 * emulated state, so they never affect lockstep or save states. */
static int32_t fm_gain = 256, psg_gain = 256;

void snd_set_levels(int fm_pct, int psg_pct)
{
    fm_gain = fm_pct * 256 / 100;
    psg_gain = psg_pct * 256 / 100;
}

/* ---------------- output ring ---------------- */
#define RING 16384                   /* stereo frames, ~0.3 s */
static int16_t ring[RING * 2];
static unsigned ring_w, ring_r;

static void push(int32_t l, int32_t r)
{
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    if (ring_w - ring_r >= RING) ring_r++;           /* nobody is reading: drop oldest */
    unsigned i = ring_w++ % RING;
    ring[2 * i] = (int16_t)l;
    ring[2 * i + 1] = (int16_t)r;
}

int snd_available(void) { return (int)(ring_w - ring_r); }

int snd_read(int16_t *out, int max)
{
    int n = 0;
    while (n < max && ring_r != ring_w) {
        unsigned i = ring_r++ % RING;
        out[2 * n] = ring[2 * i];
        out[2 * n + 1] = ring[2 * i + 1];
        n++;
    }
    return n;
}

/* ---------------- PSG (SN76489) ---------------- */
static int16_t psg_vol[16];

static void psg_reset(psg_t *p)
{
    memset(p, 0, sizeof *p);
    for (int i = 0; i < 4; i++) p->vol[i] = 15;
    p->lfsr = 0x8000;
}

static void psg_write(psg_t *p, uint8_t v)
{
#ifdef SND_DEBUG
    dlog("psg", 9, v);
#endif
    if (v & 0x80) {
        p->latch = (v >> 4) & 7;
        int ch = p->latch >> 1;
        if (p->latch & 1) p->vol[ch] = v & 15;
        else if (ch < 3) p->tone[ch] = (uint16_t)((p->tone[ch] & 0x3F0) | (v & 15));
        else { p->noise = v & 7; p->lfsr = 0x8000; }
    } else {
        int ch = p->latch >> 1;
        if (p->latch & 1) p->vol[ch] = v & 15;
        else if (ch < 3) p->tone[ch] = (uint16_t)((p->tone[ch] & 15) | ((v & 0x3F) << 4));
        else { p->noise = v & 7; p->lfsr = 0x8000; }
    }
}

static int32_t psg_tick(psg_t *p)
{
    for (int ch = 0; ch < 3; ch++) {
        if (--p->cnt[ch] <= 0) {
            p->cnt[ch] = (int16_t)(p->tone[ch] ? p->tone[ch] : 1);
            p->pol[ch] ^= 1;
        }
    }
    if (--p->cnt[3] <= 0) {
        int r = p->noise & 3;
        p->cnt[3] = (int16_t)(r == 3 ? (p->tone[2] ? p->tone[2] : 1) : (0x10 << r));
        p->pol[3] ^= 1;
        if (p->pol[3]) {                              /* shift on rising edge */
            unsigned fb = (p->noise & 4) ? ((p->lfsr ^ (p->lfsr >> 3)) & 1) : (p->lfsr & 1);
            p->lfsr = (uint16_t)((p->lfsr >> 1) | (fb << 15));
        }
    }
    int32_t s = 0;
    for (int ch = 0; ch < 3; ch++) {
        if (p->tone[ch] <= 1) s += psg_vol[p->vol[ch]];   /* tone 0/1: steady level (sample playback) */
        else s += p->pol[ch] ? psg_vol[p->vol[ch]] : -psg_vol[p->vol[ch]];
    }
    s += (p->lfsr & 1) ? psg_vol[p->vol[3]] : -psg_vol[p->vol[3]];
    return s;
}

/* ---------------- Z80 bus ---------------- */
static uint8_t z_read(void *ud, uint16_t a)
{
    (void)ud;
    if (a < 0x4000) return g_zram[a & 0x1FFF];
    if (a < 0x6000) return ym_status();
    if (a < 0x8000) return 0xFF;
    uint32_t ma = (S.bank << 15) | (a & 0x7FFF);
    if (ma >= 0xA00000 && ma < 0xA10000) return 0xFF;
    return (uint8_t)gen_read8(ma);
}

static void bank_write(uint8_t v)
{
    S.bank = ((S.bank >> 1) | ((uint32_t)(v & 1) << 8)) & 0x1FF;
}

static void z_write(void *ud, uint16_t a, uint8_t v)
{
    (void)ud;
    if (a < 0x4000) { g_zram[a & 0x1FFF] = v; return; }
    if (a < 0x6000) { YMW("z80", a & 3, v); return; }
    if (a < 0x6100) { bank_write(v); return; }
    if (a >= 0x7F10 && a < 0x7F18) { if (a & 1) psg_write(&S.psg, v); return; }
    if (a >= 0x8000) {
        uint32_t ma = (S.bank << 15) | (a & 0x7FFF);
        if (ma >= 0xE00000) gen_write8(ma, v);          /* 68000 RAM only */
    }
}

static uint8_t z_in(z80 *z, uint8_t port) { (void)z; (void)port; return 0xFF; }
static void z_out(z80 *z, uint8_t port, uint8_t v) { (void)z; (void)port; (void)v; }

static void z80_hook(void)
{
    S.z.read_byte = z_read;
    S.z.write_byte = z_write;
    S.z.port_in = z_in;
    S.z.port_out = z_out;
    S.z.userdata = 0;
}

static void z80_power(void)
{
    z80_init(&S.z);
    z80_hook();
}

/* ---------------- time ---------------- */
static inline uint64_t now_mclk(void)
{
    /* each frame = 896040 master clocks, whatever its instruction count
     * (at 20000 per frame this equals g_icount * 224010 / 5000) */
    return g_frame * SND_FRAME_MCLK + (g_icount - g_frame_start) * SND_FRAME_MCLK / g_frame_len;
}

static void gen_sample(void)
{
    int32_t fm[2];
    ym_sample(fm);
    int32_t ps = 0, n = 0;
    S.psg.frac += SND_SAMPLE_MCLK;
    while (S.psg.frac >= PSG_MCLK) { ps += psg_tick(&S.psg); n++; S.psg.frac -= PSG_MCLK; }
    if (n) ps /= n;
#ifdef SND_DEBUG
    { static int mode = -1; if (mode < 0) { const char *e = getenv("DS_SND_ONLY"); mode = !e ? 0 : e[0] == 'f' ? 1 : 2; }
      if (mode == 1) ps = 0; if (mode == 2) fm[0] = fm[1] = 0; }
#endif
    int32_t in[2] = { fm[0] * 3 / 4 * fm_gain / 256 + ps * psg_gain / 256,
                      fm[1] * 3 / 4 * fm_gain / 256 + ps * psg_gain / 256 };
    for (int c = 0; c < 2; c++) {
        /* one-pole high-pass at ~5 Hz removes the DAC's DC offset */
        int32_t x = in[c] << 8;
        S.dc_y[c] = x - S.dc_x[c] + (int32_t)(((int64_t)S.dc_y[c] * 65225) >> 16);
        S.dc_x[c] = x;
        in[c] = S.dc_y[c] >> 8;
    }
    push(in[0], in[1]);
    S.ym_next += SND_SAMPLE_MCLK;
}

static int z80_running(void) { return !S.reset && !S.busreq; }

static void run_to(uint64_t target)
{
    while (S.z80_mclk < target) {
        if (z80_running()) {
            unsigned long c0 = S.z.cyc;
            z80_step(&S.z);
            S.z80_mclk += (uint64_t)(S.z.cyc - c0) * Z80_MCLK;
        } else {
#ifdef SND_DEBUG
            { static uint64_t stall, lastf; stall += target - S.z80_mclk;
              if (g_frame / 100 != lastf) { lastf = g_frame / 100; fprintf(stderr, "frame %llu stalled %.1f%%\n", (unsigned long long)g_frame, stall * 100.0 / (100.0 * SND_FRAME_MCLK)); stall = 0; } }
#endif
            S.z80_mclk = target;
        }
        if (S.z.int_pending && S.z80_mclk >= S.int_end) S.z.int_pending = 0;
        while (S.ym_next <= S.z80_mclk) gen_sample();
    }
}

void snd_sync(void) { run_to(now_mclk()); }

void snd_vblank(void)
{
    snd_sync();
    if (z80_running()) {
        z80_gen_int(&S.z, 0xFF);
        S.int_end = S.z80_mclk + Z80_INT_MCLK;
    }
}

/* ---------------- setup ---------------- */
void snd_init(void)
{
    ym_init();
    for (int i = 0; i < 15; i++) {
        double v = 2400.0;
        for (int k = 0; k < i; k++) v *= 0.794328;    /* -2 dB per step */
        psg_vol[i] = (int16_t)v;
    }
    psg_vol[15] = 0;
}

void snd_reset(void)
{
    static int inited;
    if (!inited) { snd_init(); inited = 1; }
    memset(&S, 0, sizeof S);
    z80_power();
    psg_reset(&S.psg);
    ym_reset();
    S.reset = 1;
    S.ym_next = SND_SAMPLE_MCLK;
    ring_w = ring_r = 0;
}

/* ---------------- 68000 side ---------------- */
uint8_t snd_68k_read8(uint32_t a)
{
    snd_sync();
    a &= 0xFFFF;
    if (a < 0x4000) return g_zram[a & 0x1FFF];
    if (a < 0x6000) return ym_status();
    return 0xFF;
}

void snd_68k_write8(uint32_t a, uint8_t v)
{
    snd_sync();
    a &= 0xFFFF;
    if (a < 0x4000) { g_zram[a & 0x1FFF] = v; return; }
    if (a < 0x6000) { YMW("68k", a & 3, v); return; }
    if (a < 0x6100) { bank_write(v); return; }
    if (a >= 0x7F10 && a < 0x7F18 && (a & 1)) psg_write(&S.psg, v);
}

void snd_psg_write(uint8_t v)
{
    snd_sync();
    psg_write(&S.psg, v);
}

void snd_set_busreq(int on)
{
    snd_sync();
    S.busreq = on;
}

void snd_set_reset(int asserted)
{
    snd_sync();
    if (asserted && !S.reset) {
        z80_power();
        ym_reset();
    }
    S.reset = asserted;
}

int snd_bus_granted(void) { return S.busreq && !S.reset; }

/* ---------------- save states ---------------- */
size_t snd_state_size(void) { return sizeof S + 4 + YM_STATE_MAX; }

void snd_state_save(uint8_t *out)
{
    memcpy(out, &S, sizeof S);
    uint32_t n = (uint32_t)ym_state_save(out + sizeof S + 4);
    memcpy(out + sizeof S, &n, 4);
}

int snd_state_load(const uint8_t *in)
{
    memcpy(&S, in, sizeof S);
    z80_hook();
    uint32_t n;
    memcpy(&n, in + sizeof S, 4);
    ring_w = ring_r = 0;
    return ym_state_load(in + sizeof S + 4, n);
}

#ifdef SND_DEBUG
static void dlog(const char *src, int port, uint8_t v)
{
    if (!dbg) dbg = fopen("/tmp/ymlog.txt", "w");
    fprintf(dbg, "%llu %s %d %02X zpc=%04X bank=%03X\n", (unsigned long long)g_frame, src, port, v, S.z.pc, S.bank);
}
#endif
