/* A small Amiga for the cutscene programs. See amiga_vm.h. */
#include "amiga_vm.h"
#include "m68k.h"
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL_log.h>

#define CHIP_SIZE   0x200000u
#define FRAME_CYC   141875              /* 7.09379 MHz / 50 Hz */
#define LINES       313
#define STOP_PC     0x600u              /* the program's return address: STOP there */
#define RTE_PC      0x700u              /* unhandled exceptions return here */

static uint8_t *chip;
static uint16_t cregs[0x100];           /* custom registers as last written */
static uint16_t intena, intreq, dmacon;
static int running, finished;
static uint32_t frame_cycles_done;      /* cycles run in the current frame before this timeslice */
static uint32_t screen[AVM_W * AVM_H];
static int scr_w, scr_h;

static uint32_t rd32(uint32_t a) { a &= CHIP_SIZE - 1; return (uint32_t)chip[a] << 24 | (uint32_t)chip[(a + 1) & (CHIP_SIZE - 1)] << 16 | (uint32_t)chip[(a + 2) & (CHIP_SIZE - 1)] << 8 | chip[(a + 3) & (CHIP_SIZE - 1)]; }
static uint16_t crd16(uint32_t a) { a &= CHIP_SIZE - 2; return (uint16_t)(chip[a] << 8 | chip[a + 1]); }
static void cwr16(uint32_t a, uint16_t v) { a &= CHIP_SIZE - 2; chip[a] = (uint8_t)(v >> 8); chip[a + 1] = (uint8_t)v; }

static void update_irq(void)
{
    int level = 0;
    uint16_t p = intreq & intena;
    if (intena & 0x4000) {
        if (p & 0x2000) level = 6;
        else if (p & 0x1800) level = 5;
        else if (p & 0x0780) level = 4;
        else if (p & 0x0070) level = 3;
        else if (p & 0x0008) level = 2;
        else if (p & 0x0007) level = 1;
    }
    m68k_set_irq((unsigned)level);
}

static int beam_line(void)
{
    uint32_t c = frame_cycles_done + (uint32_t)m68k_cycles_run();
    int l = (int)((uint64_t)c * LINES / FRAME_CYC);
    return l >= LINES ? LINES - 1 : l;
}

/* ---- blitter ---- */
static void blit(uint16_t size)
{
    uint16_t con0 = cregs[0x40 / 2], con1 = cregs[0x42 / 2];
    int h = size >> 6, w = size & 63;
    if (!h) h = 1024;
    if (!w) w = 64;
    if (con1 & 1) return;                                   /* line mode: not used by the cutscenes */
    int ash = con0 >> 12, bsh = con1 >> 12, lf = con0 & 0xFF;
    int useA = con0 & 0x800, useB = con0 & 0x400, useC = con0 & 0x200, useD = con0 & 0x100;
    int desc = con1 & 2, fill = con1 & 0x18, fci = (con1 >> 2) & 1;
    uint32_t pa = ((uint32_t)cregs[0x50 / 2] << 16 | cregs[0x52 / 2]) & 0x1FFFFE;
    uint32_t pb = ((uint32_t)cregs[0x4C / 2] << 16 | cregs[0x4E / 2]) & 0x1FFFFE;
    uint32_t pc = ((uint32_t)cregs[0x48 / 2] << 16 | cregs[0x4A / 2]) & 0x1FFFFE;
    uint32_t pd = ((uint32_t)cregs[0x54 / 2] << 16 | cregs[0x56 / 2]) & 0x1FFFFE;
    int ma = (int16_t)(cregs[0x64 / 2] & 0xFFFE), mb = (int16_t)(cregs[0x62 / 2] & 0xFFFE);
    int mc = (int16_t)(cregs[0x60 / 2] & 0xFFFE), md = (int16_t)(cregs[0x66 / 2] & 0xFFFE);
    uint16_t afwm = cregs[0x44 / 2], alwm = cregs[0x46 / 2];
    uint16_t adat = cregs[0x74 / 2], bdat = cregs[0x72 / 2], cdat = cregs[0x70 / 2];
    uint16_t aold = 0, bold = 0;
    int step = desc ? -2 : 2;
    for (int y = 0; y < h; y++) {
        int carry = fci;
        for (int x = 0; x < w; x++) {
            if (useA) { adat = crd16(pa); pa += step; }
            if (useB) { bdat = crd16(pb); pb += step; }
            if (useC) { cdat = crd16(pc); pc += step; }
            uint16_t a = adat;
            if (x == 0) a &= afwm;
            if (x == w - 1) a &= alwm;
            uint16_t as, bs;
            if (!desc) {
                as = (uint16_t)((((uint32_t)aold << 16) | a) >> ash);
                bs = (uint16_t)((((uint32_t)bold << 16) | bdat) >> bsh);
            } else {
                as = (uint16_t)((((uint32_t)a << 16) | aold) >> (16 - ash));
                bs = (uint16_t)((((uint32_t)bdat << 16) | bold) >> (16 - bsh));
            }
            aold = a; bold = bdat;
            uint16_t c = cdat, d = 0;
            if (lf & 0x80) d |= as & bs & c;
            if (lf & 0x40) d |= as & bs & ~c;
            if (lf & 0x20) d |= as & ~bs & c;
            if (lf & 0x10) d |= as & ~bs & ~c;
            if (lf & 0x08) d |= ~as & bs & c;
            if (lf & 0x04) d |= ~as & bs & ~c;
            if (lf & 0x02) d |= ~as & ~bs & c;
            if (lf & 0x01) d |= ~as & ~bs & ~c;
            if (fill) {                                     /* area fill, right to left */
                uint16_t out = 0;
                for (int bit = 0; bit < 16; bit++) {
                    int v = (d >> bit) & 1, o;
                    if (fill & 0x08) { o = carry | v; carry ^= v; }      /* inclusive */
                    else { carry ^= v; o = carry; }                    /* exclusive */
                    out |= (uint16_t)(o << bit);
                }
                d = out;
            }
            if (useD) { cwr16(pd, d); pd += step; }
        }
        if (useA) pa += desc ? -ma : ma;
        if (useB) pb += desc ? -mb : mb;
        if (useC) pc += desc ? -mc : mc;
        if (useD) pd += desc ? -md : md;
    }
    cregs[0x74 / 2] = adat; cregs[0x72 / 2] = bdat; cregs[0x70 / 2] = cdat;
    cregs[0x50 / 2] = (uint16_t)(pa >> 16); cregs[0x52 / 2] = (uint16_t)pa;
    cregs[0x4C / 2] = (uint16_t)(pb >> 16); cregs[0x4E / 2] = (uint16_t)pb;
    cregs[0x48 / 2] = (uint16_t)(pc >> 16); cregs[0x4A / 2] = (uint16_t)pc;
    cregs[0x54 / 2] = (uint16_t)(pd >> 16); cregs[0x56 / 2] = (uint16_t)pd;
}

/* ---- custom chip registers ---- */
static uint16_t custom_read(uint32_t off)
{
    switch (off & 0x1FE) {
    case 0x002: return dmacon & 0x07FF;                    /* blitter never busy */
    case 0x004: return (uint16_t)((beam_line() >> 8) & 1);
    case 0x006: { int l = beam_line(); return (uint16_t)((l & 0xFF) << 8 | 0x40); }
    case 0x00A: case 0x00C: return 0;
    case 0x010: return cregs[0x9E / 2];
    case 0x016: return 0xFF00;                             /* right mouse button up */
    case 0x01C: return intena;
    case 0x01E: return intreq;
    default: return 0;
    }
}

static void custom_write(uint32_t off, uint16_t v)
{
    off &= 0x1FE;
    switch (off) {
    case 0x096: dmacon = (v & 0x8000) ? (dmacon | (v & 0x7FFF)) : (dmacon & ~v); return;
    case 0x09A: intena = (v & 0x8000) ? (intena | (v & 0x7FFF)) : (intena & ~v); update_irq(); return;
    case 0x09C: intreq = (v & 0x8000) ? (intreq | (v & 0x7FFF)) : (intreq & ~v); update_irq(); return;
    case 0x058: cregs[off / 2] = v; blit(v); return;
    default: cregs[off / 2] = v; return;
    }
}

/* ---- Musashi memory interface ---- */
unsigned int m68k_read_memory_8(unsigned int a)
{
    a &= 0xFFFFFF;
    if (a < CHIP_SIZE) return chip[a];
    if ((a & 0xFFF000) == 0xDFF000) { uint16_t v = custom_read(a & 0xFFE); return (a & 1) ? (v & 0xFF) : (v >> 8); }
    if (a == 0xBFE001) return 0xFF;                        /* mouse / joystick fire buttons up */
    if ((a & 0xFF0000) == 0xBF0000) return 0xFF;
    return 0;
}

unsigned int m68k_read_memory_16(unsigned int a)
{
    a &= 0xFFFFFF;
    if (a < CHIP_SIZE - 1) return (unsigned)(chip[a] << 8 | chip[a + 1]);
    if ((a & 0xFFF000) == 0xDFF000) return custom_read(a & 0xFFE);
    return (m68k_read_memory_8(a) << 8) | m68k_read_memory_8(a + 1);
}

unsigned int m68k_read_memory_32(unsigned int a)
{
    return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2);
}

void m68k_write_memory_8(unsigned int a, unsigned int v)
{
    a &= 0xFFFFFF;
    if (a < CHIP_SIZE) { chip[a] = (uint8_t)v; return; }
    if ((a & 0xFFF000) == 0xDFF000) {                      /* byte writes put the byte on both halves */
        custom_write(a & 0xFFE, (uint16_t)((v & 0xFF) << 8 | (v & 0xFF)));
    }
}

void m68k_write_memory_16(unsigned int a, unsigned int v)
{
    a &= 0xFFFFFF;
    if (a < CHIP_SIZE - 1) { chip[a] = (uint8_t)(v >> 8); chip[a + 1] = (uint8_t)v; return; }
    if ((a & 0xFFF000) == 0xDFF000) custom_write(a & 0xFFE, (uint16_t)v);
}

void m68k_write_memory_32(unsigned int a, unsigned int v)
{
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v & 0xFFFF);
}

/* ---- hunk loading ---- */
static uint32_t be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int load(const uint8_t *d, size_t n, uint32_t *entry)
{
    if (n < 24 || be(d) != 0x3F3) return 0;
    size_t p = 4;
    while (p + 4 <= n && be(d + p)) p += 4 + (size_t)be(d + p) * 4;
    p += 4;
    if (p + 12 > n) return 0;
    uint32_t count = be(d + p), first = be(d + p + 4), last = be(d + p + 8);
    p += 12;
    if (count > 32 || last < first || last - first + 1 > 32) return 0;
    uint32_t base[32], at = 0x10000;
    for (uint32_t i = 0; i <= last - first; i++) {
        uint32_t sz = (be(d + p + i * 4) & 0x3FFFFFFF) * 4;
        base[i] = at;
        at = (at + sz + 15) & ~15u;
        if (at > CHIP_SIZE - 0x10000) return 0;
    }
    p += (size_t)(last - first + 1) * 4;
    int cur = -1;
    while (p + 4 <= n) {
        uint32_t t = be(d + p) & 0x3FFFFFFF;
        p += 4;
        if (t == 0x3E9 || t == 0x3EA) {
            uint32_t len = be(d + p) * 4;
            p += 4;
            cur++;
            if (cur > (int)(last - first) || p + len > n) return 0;
            memcpy(chip + base[cur], d + p, len);
            p += len;
        } else if (t == 0x3EB) { cur++; p += 4; }
        else if (t == 0x3EC) {
            for (;;) {
                if (p + 4 > n) return 0;
                uint32_t c = be(d + p); p += 4;
                if (!c) break;
                uint32_t h = be(d + p); p += 4;
                for (uint32_t k = 0; k < c && p + 4 <= n; k++, p += 4) {
                    uint32_t o = base[cur] + be(d + p);
                    if (h > last - first || o + 4 > CHIP_SIZE) continue;
                    uint32_t v = rd32(o) + base[h];
                    chip[o] = (uint8_t)(v >> 24); chip[o + 1] = (uint8_t)(v >> 16); chip[o + 2] = (uint8_t)(v >> 8); chip[o + 3] = (uint8_t)v;
                }
            }
        } else if (t == 0x3F2) continue;
        else if (t == 0x3F0) {                              /* symbols */
            for (;;) { if (p + 4 > n) break; uint32_t c = be(d + p) & 0xFFFFFF; p += 4; if (!c) break; p += (size_t)c * 4 + 4; }
        } else break;
    }
    *entry = base[0];
    return cur >= 0;
}

int avm_start(const uint8_t *hunks, size_t n)
{
    avm_stop();
    if (!chip && !(chip = malloc(CHIP_SIZE))) return 0;
    memset(chip, 0, CHIP_SIZE);
    memset(cregs, 0, sizeof cregs);
    intena = intreq = dmacon = 0;
    uint32_t entry;
    if (!load(hunks, n, &entry)) return 0;
    for (uint32_t v = 2; v < 64; v++) { uint32_t a = v * 4; chip[a] = 0; chip[a + 1] = 0; chip[a + 2] = (uint8_t)(RTE_PC >> 8); chip[a + 3] = (uint8_t)RTE_PC; }
    cwr16(RTE_PC, 0x4E73);                                 /* rte */
    cwr16(STOP_PC, 0x4E72); cwr16(STOP_PC + 2, 0x2700);    /* stop #$2700 */
    uint32_t sp = CHIP_SIZE - 0x100;
    sp -= 4; chip[sp] = 0; chip[sp + 1] = 0; chip[sp + 2] = (uint8_t)(STOP_PC >> 8); chip[sp + 3] = (uint8_t)STOP_PC;
    chip[0] = 0; chip[1] = 0x1F; chip[2] = 0xFF; chip[3] = 0x00;   /* reset SSP */
    chip[4] = (uint8_t)(entry >> 24); chip[5] = (uint8_t)(entry >> 16); chip[6] = (uint8_t)(entry >> 8); chip[7] = (uint8_t)entry;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_PC, entry);
    m68k_set_reg(M68K_REG_SR, 0x2700);
    memset(chip, 0, 8);                                    /* programs read 0.l as zero (e.g. a blit modulo) */
    m68k_set_reg(M68K_REG_A4, 0x500);                      /* the loader's flag bytes */
    m68k_set_reg(M68K_REG_A5, 0x504);
    m68k_set_reg(M68K_REG_A6, 0x508);
    running = 1; finished = 0;
    scr_w = scr_h = 0;
    memset(screen, 0, sizeof screen);
    return 1;
}

void avm_stop(void) { running = 0; }
int avm_running(void) { return running; }
int avm_finished(void) { return finished; }

/* ---- display ---- */
typedef struct { int line; uint16_t reg, val; } cop_ev;

static int copper_events(cop_ev *ev, int max)
{
    uint32_t pc = ((uint32_t)cregs[0x80 / 2] << 16 | cregs[0x82 / 2]) & (CHIP_SIZE - 2);
    int n = 0, line = 0;
    for (int i = 0; i < 4096 && n < max; i++, pc += 4) {
        uint16_t w1 = crd16(pc), w2 = crd16(pc + 2);
        if (w1 == 0xFFFF && w2 == 0xFFFE) break;
        if (!(w1 & 1)) { ev[n].line = line; ev[n].reg = w1 & 0x1FE; ev[n].val = w2; n++; }
        else if (!(w2 & 1)) {                               /* WAIT */
            int vp = w1 >> 8;
            if (vp < (line & 0xFF) && line >= 0x100 - 8) vp |= 0x100;   /* past line 255 */
            else if (line >= 0x100) vp |= 0x100;
            if (vp > line) line = vp;
        }
    }
    return n;
}

static void render(const cop_ev *ev, int nev)
{
    uint16_t r[0x100];
    memcpy(r, cregs, sizeof r);
    int e = 0;
    while (e < nev && ev[e].line <= 0) { r[ev[e].reg / 2] = ev[e].val; e++; }
    uint16_t diwstrt = r[0x8E / 2] ? r[0x8E / 2] : 0x2C81, diwstop = r[0x90 / 2] ? r[0x90 / 2] : 0x2CC1;
    int vstart = diwstrt >> 8, vstop = (diwstop >> 8) | ((diwstop & 0x8000) ? 0 : 0x100);
    if (vstop <= vstart) vstop = vstart + 256;
    uint32_t pt[6];
    for (int i = 0; i < 6; i++) pt[i] = ((uint32_t)r[(0xE0 + i * 4) / 2] << 16 | r[(0xE2 + i * 4) / 2]) & (CHIP_SIZE - 2);
    int out_y = 0;
    scr_w = 0;
    for (int line = 0; line < LINES && out_y < AVM_H; line++) {
        while (e < nev && ev[e].line <= line) {
            uint16_t reg = ev[e].reg;
            r[reg / 2] = ev[e].val;
            if (reg >= 0xE0 && reg <= 0xF6) { int i = (reg - 0xE0) / 4; pt[i] = ((uint32_t)r[(0xE0 + i * 4) / 2] << 16 | r[(0xE2 + i * 4) / 2]) & (CHIP_SIZE - 2); }
            e++;
        }
        if (line < vstart || line >= vstop) continue;
        int depth = (r[0x100 / 2] >> 12) & 7;
        int ddfstrt = r[0x92 / 2] & 0xFC, ddfstop = r[0x94 / 2] & 0xFC;
        int words = ddfstop > ddfstrt ? (ddfstop - ddfstrt) / 8 + 1 : 20;
        if (words > AVM_W / 16) words = AVM_W / 16;
        int width = words * 16, ehb = depth == 6 && !(r[0x100 / 2] & 0x800);
        uint32_t pal[64];
        for (int i = 0; i < 32; i++) {
            uint16_t c = r[(0x180 + i * 2) / 2];
            uint32_t rr = (c >> 8) & 15, gg = (c >> 4) & 15, bb = c & 15;
            pal[i] = 0xFF000000u | (rr * 17) << 16 | (gg * 17) << 8 | bb * 17;
            pal[i + 32] = 0xFF000000u | ((rr * 17) >> 1) << 16 | ((gg * 17) >> 1) << 8 | ((bb * 17) >> 1);
        }
        uint32_t *row = screen + (size_t)out_y * AVM_W;
        for (int x = 0; x < width; x++) {
            int v = 0, byte = x >> 3, bit = 7 - (x & 7);
            for (int pl = 0; pl < depth && pl < 6; pl++) v |= ((chip[(pt[pl] + byte) & (CHIP_SIZE - 1)] >> bit) & 1) << pl;
            row[x] = ehb ? pal[v & 63] : pal[v & 31];
        }
        for (int x = width; x < AVM_W; x++) row[x] = 0xFF000000u;
        for (int pl = 0; pl < depth && pl < 6; pl++) pt[pl] = (pt[pl] + (uint32_t)(words * 2) + (uint32_t)(int16_t)r[((pl & 1) ? 0x10A : 0x108) / 2]) & (CHIP_SIZE - 2);
        if (width > scr_w) scr_w = width;
        out_y++;
    }
    scr_h = out_y;
}

void avm_frame(void)
{
    if (!running || !chip) return;
    static cop_ev ev[4096];
    int nev = copper_events(ev, 4096);
    intreq |= 0x0020;                                       /* vertical blank */
    update_irq();
    frame_cycles_done = 0;
    /* copper interrupts at the lines the copper list asks for them */
    for (int i = 0; i <= nev; i++) {
        int line = i < nev ? ev[i].line : LINES;
        if (i < nev && !(ev[i].reg == 0x9C && (ev[i].val & 0x8000))) continue;
        uint32_t target = (uint32_t)((uint64_t)line * FRAME_CYC / LINES);
        while (frame_cycles_done < target && running) {
            int ran = m68k_execute((int)(target - frame_cycles_done));
            frame_cycles_done += (uint32_t)(ran > 0 ? ran : (int)(target - frame_cycles_done));
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
            if (pc >= STOP_PC && pc <= STOP_PC + 4) { finished = 1; running = 0; break; }
        }
        if (i < nev) { intreq |= ev[i].val & 0x7FFF; update_irq(); }
    }
    render(ev, nev);
}

const uint32_t *avm_screen(int *w, int *h)
{
    if (w) *w = scr_w;
    if (h) *h = scr_h;
    return screen;
}

unsigned avm_debug_pc(void) { return m68k_get_reg(NULL, M68K_REG_PC); }
void avm_debug_dump(void)
{
    static cop_ev ev[4096];
    int n = copper_events(ev, 4096);
    SDL_Log("regs con0=%04X ddf=%04X-%04X diw=%04X-%04X mod=%04X/%04X cop1=%04X%04X n=%d",
        cregs[0x100/2], cregs[0x92/2], cregs[0x94/2], cregs[0x8E/2], cregs[0x90/2], cregs[0x108/2], cregs[0x10A/2], cregs[0x80/2], cregs[0x82/2], n);
    for (int i = 0; i < n; i++) SDL_Log("  l%d %03X=%04X", ev[i].line, ev[i].reg, ev[i].val);
}
