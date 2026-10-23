/* Shared Genesis hardware model. See genesis.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "genesis.h"
#include "sound.h"

uint8_t  g_rom[0x400000];
uint32_t g_rom_size;
uint8_t  g_ram[0x10000];
uint8_t  g_zram[0x2000];
uint8_t  g_vram[0x10000];
uint16_t g_cram[64], g_vsram[40];
uint8_t  g_vreg[32];
uint16_t g_pad_buttons[2];

uint64_t g_icount, g_frame;
uint64_t g_next_vblank = GEN_VBLANK_START, g_next_frame = GEN_FRAME_INSNS;
uint32_t g_frame_insns = GEN_FRAME_INSNS;     /* setting; latched at each frame start */
uint64_t g_frame_start;                       /* icount at the start of this frame */
uint32_t g_frame_len = GEN_FRAME_INSNS;       /* this frame's length */
int g_in_vblank, g_vint_flag;
void (*gen_frame_cb)(void);
void (*gen_vblank_cb)(void);

static int ctl_pending;
static uint32_t vaddr;
static int vcode;
static int dma_fill_pending;
static uint8_t pad_ctrl[2], pad_data[2];

/* input script */
static uint64_t in_frame[8192];
static uint16_t in_btn[8192];
static int n_in, in_idx;

int gen_load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    g_rom_size = (uint32_t)fread(g_rom, 1, sizeof g_rom, f);
    fclose(f);
    return g_rom_size > 0;
}

int gen_set_rom(const uint8_t *data, uint32_t size)
{
    if (!size || size > sizeof g_rom) return 0;
    memset(g_rom, 0xFF, sizeof g_rom);
    memcpy(g_rom, data, size);
    g_rom_size = size;
    return 1;
}

void gen_reset(void)
{
    memset(g_ram, 0, sizeof g_ram);
    memset(g_vram, 0, sizeof g_vram);
    memset(g_cram, 0, sizeof g_cram);
    memset(g_vsram, 0, sizeof g_vsram);
    memset(g_vreg, 0, sizeof g_vreg);
    g_icount = 0; g_frame = 0;
    g_frame_start = 0;
    g_frame_len = g_frame_insns;
    g_next_vblank = gen_vblank_offset(g_frame_len); g_next_frame = g_frame_len;
    g_in_vblank = 0; g_vint_flag = 0;
    in_idx = 0;
    while (in_idx < n_in && in_frame[in_idx] == 0) g_pad_buttons[0] = in_btn[in_idx++];
    snd_reset();
}

static uint16_t parse_buttons(const char *s)
{
    uint16_t b = 0;
    for (; *s; s++) switch (*s) {
        case 'U': b |= GB_UP; break;    case 'D': b |= GB_DOWN; break;
        case 'L': b |= GB_LEFT; break;  case 'R': b |= GB_RIGHT; break;
        case 'A': b |= GB_A; break;     case 'B': b |= GB_B; break;
        case 'C': b |= GB_C; break;     case 'S': b |= GB_START; break;
    }
    return b;
}

int gen_load_inputs(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    n_in = 0;
    while (fgets(line, sizeof line, f) && n_in < 8192) {
        unsigned long long fr; char btn[64] = "";
        if (line[0] == '#') continue;
        if (sscanf(line, "%llu %63s", &fr, btn) >= 1) {
            in_frame[n_in] = fr; in_btn[n_in] = parse_buttons(btn); n_in++;
        }
    }
    fclose(f);
    return 1;
}

void gen_seek_inputs(uint64_t frame)
{
    in_idx = 0;
    while (in_idx < n_in && in_frame[in_idx] <= frame) in_idx++;
}

void gen_on_vblank_start(void)
{
    g_in_vblank = 1;
    g_vint_flag = 1;
    g_next_vblank = UINT64_MAX;               /* set when the next frame starts */
    if (gen_vblank_cb) gen_vblank_cb();
    snd_vblank();
}

void gen_on_frame_start(void)
{
    g_in_vblank = 0;
    g_frame++;
    g_frame_start = g_next_frame;
    g_frame_len = g_frame_insns;
    g_next_frame = g_frame_start + g_frame_len;
    g_next_vblank = g_frame_start + gen_vblank_offset(g_frame_len);
    while (in_idx < n_in && in_frame[in_idx] <= g_frame) g_pad_buttons[0] = in_btn[in_idx++];
    snd_sync();
    if (gen_frame_cb) gen_frame_cb();
}

/* ---------------- VDP ---------------- */
static void vdp_write_data(uint16_t v)
{
    /* Any data-port access ends a half-written control command, so a lone
     * first control word can be used to set the low address bits. EA's
     * intro code relies on this. */
    ctl_pending = 0;
    if (dma_fill_pending) {
        uint32_t len = g_vreg[19] | (g_vreg[20] << 8);
        if (!len) len = 0x10000;
        g_vram[vaddr & 0xFFFF] = v & 0xFF;
        while (len--) {
            g_vram[(vaddr ^ 1) & 0xFFFF] = v >> 8;
            vaddr += g_vreg[15];
        }
        dma_fill_pending = 0;
        return;
    }
    switch (vcode & 0xF) {
    case 1:
        g_vram[vaddr & 0xFFFF] = v >> 8;
        g_vram[(vaddr ^ 1) & 0xFFFF] = v & 0xFF;
        break;
    case 3: g_cram[(vaddr >> 1) & 63] = v; break;
    case 5: if (((vaddr >> 1) & 63) < 40) g_vsram[(vaddr >> 1) & 63] = v; break;
    }
    vaddr += g_vreg[15];
}

static uint16_t vdp_read_data(void)
{
    uint16_t v = 0;
    ctl_pending = 0;
    switch (vcode & 0xF) {
    case 0: v = (g_vram[vaddr & 0xFFFE] << 8) | g_vram[(vaddr & 0xFFFE) + 1]; break;
    case 8: v = g_cram[(vaddr >> 1) & 63]; break;
    case 4: v = g_vsram[((vaddr >> 1) & 63) % 40]; break;
    }
    vaddr += g_vreg[15];
    return v;
}

static void vdp_do_dma(void)
{
    if (!(g_vreg[1] & 0x10)) return;
    uint32_t len = g_vreg[19] | (g_vreg[20] << 8);
    if (!len) len = 0x10000;
    int mode = g_vreg[23] >> 6;
    if (mode < 2) {
        uint32_t src = ((g_vreg[21] | (g_vreg[22] << 8) | ((g_vreg[23] & 0x7F) << 16)) << 1);
        while (len--) {
            vdp_write_data((uint16_t)gen_read16(src));
            src = (src & 0xFE0000) | ((src + 2) & 0x1FFFF);
        }
    } else if (mode == 2) {
        dma_fill_pending = 1;
    } else {
        uint32_t src = g_vreg[21] | (g_vreg[22] << 8);
        while (len--) {
            g_vram[vaddr & 0xFFFF] = g_vram[src & 0xFFFF];
            src++;
            vaddr += g_vreg[15];
        }
    }
}

static void vdp_write_ctl(uint16_t v)
{
    if (!ctl_pending) {
        if ((v & 0xC000) == 0x8000) {
            g_vreg[(v >> 8) & 0x1F] = v & 0xFF;
            vcode = 0;
            return;
        }
        ctl_pending = 1;
        vaddr = (vaddr & 0xC000) | (v & 0x3FFF);
        vcode = (vcode & 0x3C) | (v >> 14);
        return;
    }
    ctl_pending = 0;
    vaddr = (vaddr & 0x3FFF) | ((v & 3) << 14);
    vcode = (vcode & 3) | ((v >> 2) & 0x3C);
    if (vcode & 0x20) vdp_do_dma();
}

static uint16_t vdp_status(void)
{
    uint16_t s = 0x3400 | 0x0200;
    if (g_in_vblank || !(g_vreg[1] & 0x40)) s |= 0x0008;
    if (g_vint_flag) s |= 0x0080;
    if ((g_icount >> 2) & 1) s |= 0x0004;
    ctl_pending = 0;
    return s;
}

static uint16_t vdp_hv(void)
{
    uint32_t pos = (uint32_t)((g_icount - g_frame_start) % g_frame_len);
    uint32_t line = pos * 262 / g_frame_len;
    uint32_t h = (uint32_t)((uint64_t)pos * 262 * 171 / g_frame_len) % 171;
    return (uint16_t)(((line & 0xFF) << 8) | (h & 0xFF));
}

/* ---------------- pads ---------------- */
static uint8_t pad_read(int p)
{
    uint16_t b = g_pad_buttons[p];
    uint8_t v;
    if (pad_data[p] & 0x40) {
        v = 0x7F;
        if (b & GB_UP) v &= ~0x01;
        if (b & GB_DOWN) v &= ~0x02;
        if (b & GB_LEFT) v &= ~0x04;
        if (b & GB_RIGHT) v &= ~0x08;
        if (b & GB_B) v &= ~0x10;
        if (b & GB_C) v &= ~0x20;
    } else {
        v = 0x33;
        if (b & GB_UP) v &= ~0x01;
        if (b & GB_DOWN) v &= ~0x02;
        if (b & GB_A) v &= ~0x10;
        if (b & GB_START) v &= ~0x20;
    }
    return (v & ~pad_ctrl[p]) | (pad_data[p] & pad_ctrl[p]) | 0x80;
}

/* ---------------- bus ---------------- */
uint32_t gen_read8(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a < 0x400000) return a < g_rom_size ? g_rom[a] : 0xFF;
    if (a >= 0xE00000) return g_ram[a & 0xFFFF];
    if (a >= 0xA00000 && a < 0xA10000) return snd_68k_read8(a);
    switch (a) {
    case 0xA10001: return 0xA0;
    case 0xA10003: return pad_read(0);
    case 0xA10005: return pad_read(1);
    case 0xA10007: return 0xFF;
    case 0xA10009: return pad_ctrl[0];
    case 0xA1000B: return pad_ctrl[1];
    case 0xA1000D: return 0;
    case 0xA11100: return snd_bus_granted() ? 0 : 1;
    case 0xA11101: return 0;
    }
    if (a >= 0xC00000 && a < 0xC00020) {
        uint16_t v = 0xFFFF;
        switch (a & 0x1E) {
        case 0x00: case 0x02: v = vdp_read_data(); break;
        case 0x04: case 0x06: v = vdp_status(); break;
        case 0x08: case 0x0A: v = vdp_hv(); break;
        }
        return (a & 1) ? (v & 0xFF) : (v >> 8);
    }
    return 0xFF;
}

uint32_t gen_read16(uint32_t a)
{
    a &= 0xFFFFFE;
    if (a < 0x400000) return a < g_rom_size ? (g_rom[a] << 8) | g_rom[a + 1] : 0xFFFF;
    if (a >= 0xE00000) return (g_ram[a & 0xFFFF] << 8) | g_ram[(a + 1) & 0xFFFF];
    if (a >= 0xC00000 && a < 0xC00020) {
        switch (a & 0x1E) {
        case 0x00: case 0x02: return vdp_read_data();
        case 0x04: case 0x06: return vdp_status();
        case 0x08: case 0x0A: return vdp_hv();
        }
        return 0xFFFF;
    }
    if (a == 0xA11100) return snd_bus_granted() ? 0x0000 : 0x0100;
    return (gen_read8(a) << 8) | gen_read8(a + 1);
}

uint32_t gen_read32(uint32_t a)
{
    uint32_t hi = gen_read16(a);
    return (hi << 16) | gen_read16(a + 2);
}

void gen_write8(uint32_t a, uint32_t v)
{
    a &= 0xFFFFFF; v &= 0xFF;
    if (a >= 0xE00000) { g_ram[a & 0xFFFF] = (uint8_t)v; return; }
    if (a >= 0xA00000 && a < 0xA10000) { snd_68k_write8(a, (uint8_t)v); return; }
    switch (a) {
    case 0xA10003: pad_data[0] = (uint8_t)v; return;
    case 0xA10005: pad_data[1] = (uint8_t)v; return;
    case 0xA10009: pad_ctrl[0] = (uint8_t)v; return;
    case 0xA1000B: pad_ctrl[1] = (uint8_t)v; return;
    case 0xA11100: snd_set_busreq(v & 1); return;
    case 0xA11200: snd_set_reset(!(v & 1)); return;
    }
    if (a >= 0xC00010 && a < 0xC00018) { if (a & 1) snd_psg_write((uint8_t)v); return; }
    if (a >= 0xC00000 && a < 0xC00020) {
        uint16_t w = (uint16_t)((v << 8) | v);
        if ((a & 0x1E) < 4) vdp_write_data(w);
        else if ((a & 0x1E) < 8) vdp_write_ctl(w);
    }
}

void gen_write16(uint32_t a, uint32_t v)
{
    a &= 0xFFFFFE; v &= 0xFFFF;
    if (a >= 0xE00000) { g_ram[a & 0xFFFF] = (uint8_t)(v >> 8); g_ram[(a + 1) & 0xFFFF] = (uint8_t)v; return; }
    if (a >= 0xC00010 && a < 0xC00018) { snd_psg_write((uint8_t)v); return; }
    if (a >= 0xC00000 && a < 0xC00020) {
        if ((a & 0x1E) < 4) vdp_write_data((uint16_t)v);
        else if ((a & 0x1E) < 8) vdp_write_ctl((uint16_t)v);
        return;
    }
    if (a == 0xA11100) { snd_set_busreq((v >> 8) & 1); return; }
    if (a == 0xA11200) { snd_set_reset(!((v >> 8) & 1)); return; }
    gen_write8(a, v >> 8);
    gen_write8(a + 1, v);
}

void gen_write32(uint32_t a, uint32_t v)
{
    gen_write16(a, v >> 16);
    gen_write16(a + 2, v);
}

/* ---------------- save states ---------------- */
#define STATE_FIELDS(X) \
    X(g_ram) X(g_zram) X(g_vram) X(g_cram) X(g_vsram) X(g_vreg) X(g_pad_buttons) \
    X(g_icount) X(g_frame) X(g_next_vblank) X(g_next_frame) X(g_in_vblank) X(g_vint_flag) \
    X(ctl_pending) X(vaddr) X(vcode) X(dma_fill_pending) \
    X(pad_ctrl) X(pad_data)

size_t gen_state_size(void)
{
    size_t n = 4 + snd_state_size();
#define X(f) n += sizeof f;
    STATE_FIELDS(X)
#undef X
    return n;
}

void gen_state_save(uint8_t *out)
{
    uint32_t ver = GEN_STATE_VERSION;
    memcpy(out, &ver, 4); out += 4;
#define X(f) memcpy(out, &f, sizeof f); out += sizeof f;
    STATE_FIELDS(X)
#undef X
    snd_state_save(out);
}

int gen_state_load(const uint8_t *in)
{
    uint32_t ver;
    memcpy(&ver, in, 4); in += 4;
    if (ver != GEN_STATE_VERSION) return 0;
#define X(f) memcpy(&f, in, sizeof f); in += sizeof f;
    STATE_FIELDS(X)
#undef X
    /* The frame length is not stored (keeps old states loadable): take the
     * current setting for the rest of this frame. */
    g_frame_len = g_frame_insns;
    g_frame_start = g_next_frame >= g_frame_len ? g_next_frame - g_frame_len : 0;
    if (g_frame_start > g_icount) g_frame_start = g_icount;
    if (g_next_frame - g_frame_start > g_frame_len) g_frame_len = (uint32_t)(g_next_frame - g_frame_start);
    return snd_state_load(in);
}

/* ---------------- state checksums ---------------- */
static uint32_t crc32(uint32_t c, const void *p, size_t n)
{
    const uint8_t *b = p;
    c = ~c;
    while (n--) {
        c ^= *b++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

void gen_state_crcs(uint32_t out[4])
{
    out[0] = crc32(0, g_ram, sizeof g_ram);
    out[1] = crc32(0, g_vram, sizeof g_vram);
    uint32_t c = crc32(0, g_cram, sizeof g_cram);
    c = crc32(c, g_vsram, sizeof g_vsram);
    out[2] = crc32(c, g_vreg, sizeof g_vreg);
    out[3] = crc32(0, g_zram, sizeof g_zram);
}
