/*
 * Desert Strike reference harness (Musashi 68000 + shared Genesis model).
 *
 * usage: harness rom.bin frames inputs.txt outdir [dump_every]
 *
 * Writes outdir/frames.log (one line per frame: CPU registers + state CRCs)
 * for lockstep comparison with the recompiled build, outdir/hits.txt
 * (executed PCs) and optional VDP dumps for tools/render_vdp.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "musashi/m68k.h"
#include "../src/hw/genesis.h"

static uint8_t hit_rom[0x400000 / 2];
static uint8_t hit_ram[0x10000 / 2];
static uint32_t cur_pc;
static uint64_t max_frames;
static int dump_every;
static const char *outdir;
static FILE *flog;

unsigned int m68k_read_memory_8(unsigned int a) { return gen_read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return gen_read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return gen_read32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { gen_write8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { gen_write16(a, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { gen_write32(a, v); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return gen_read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return gen_read16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return gen_read32(a); }

static void hit(uint32_t pc)
{
    pc &= 0xFFFFFF;
    if (pc < 0x400000) hit_rom[pc >> 1] = 1;
    else if (pc >= 0xFF0000) hit_ram[(pc & 0xFFFF) >> 1] = 1;
}

static uint64_t cyc_total, cyc_last_run, ins_total;
static int cpi_mode = -1;
void harness_instr_hook(unsigned int pc)
{
    if (cpi_mode < 0) cpi_mode = getenv("DS_CPI") != NULL;
    if (cpi_mode) {
        unsigned int r = (unsigned int)m68k_cycles_run();
        if (r >= cyc_last_run) cyc_total += r - cyc_last_run; else cyc_total += r;
        cyc_last_run = r;
        ins_total++;
    }
    cur_pc = pc & 0xFFFFFF;
    {   /* DS_WATCH_PC=hex: print the address registers each time it runs */
        static int init; static unsigned watch;
        if (!init) { const char *e = getenv("DS_WATCH_PC"); watch = e ? (unsigned)strtoul(e, 0, 16) : 0; init = 1; }
        if (watch && cur_pc == watch)
            fprintf(stderr, "watch %06X frame %llu a0=%06X a3=%06X a4=%06X\n", cur_pc, (unsigned long long)g_frame,
                    m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF, m68k_get_reg(NULL, M68K_REG_A3) & 0xFFFFFF,
                    m68k_get_reg(NULL, M68K_REG_A4) & 0xFFFFFF);
    }
    hit(cur_pc);
    while (gen_instr_tick((m68k_get_reg(NULL, M68K_REG_SR) >> 8) & 7)) {
        /* Take the interrupt before this instruction, exactly as the
         * recompiled build does, then account for the handler's first
         * instruction (Musashi won't call the hook for it). */
        m68k_take_irq_now(6);
        cur_pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
        hit(cur_pc);
    }
}

static void dump_vdp(uint64_t f)
{
    char path[512];
    snprintf(path, sizeof path, "%s/vdp_%06llu.bin", outdir, (unsigned long long)f);
    FILE *o = fopen(path, "wb");
    if (!o) return;
    fwrite(g_vreg, 1, 32, o);
    fwrite(g_vram, 1, sizeof g_vram, o);
    for (int i = 0; i < 64; i++) { uint8_t b[2] = { g_cram[i] >> 8, g_cram[i] }; fwrite(b, 1, 2, o); }
    for (int i = 0; i < 40; i++) { uint8_t b[2] = { g_vsram[i] >> 8, g_vsram[i] }; fwrite(b, 1, 2, o); }
    fclose(o);
}

static void finish(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/hits.txt", outdir);
    FILE *o = fopen(path, "w");
    for (uint32_t i = 0; i < sizeof hit_rom; i++) if (hit_rom[i]) fprintf(o, "%06X\n", i << 1);
    for (uint32_t i = 0; i < sizeof hit_ram; i++) if (hit_ram[i]) fprintf(o, "%06X\n", 0xFF0000 | (i << 1));
    fclose(o);
    fclose(flog);
    fprintf(stderr, "ran %llu frames, %llu instructions, PC %06X\n",
            (unsigned long long)g_frame, (unsigned long long)g_icount, cur_pc);
    exit(0);
}

static void on_frame(void)
{
    uint32_t c[4];
    gen_state_crcs(c);
    fprintf(flog, "%llu pc=%06X sr=%04X",
            (unsigned long long)g_frame, cur_pc, m68k_get_reg(NULL, M68K_REG_SR) & 0xFFFF);
    for (int i = 0; i < 8; i++) fprintf(flog, " d%d=%08X", i, m68k_get_reg(NULL, M68K_REG_D0 + i));
    for (int i = 0; i < 8; i++) fprintf(flog, " a%d=%08X", i, m68k_get_reg(NULL, M68K_REG_A0 + i));
    fprintf(flog, " ram=%08X vram=%08X vdp=%08X zram=%08X\n", c[0], c[1], c[2], c[3]);
    if (dump_every && g_frame % dump_every == 0) dump_vdp(g_frame);
    if (cpi_mode > 0 && g_frame % 500 == 0) {
        static uint64_t c0, i0;
        fprintf(stderr, "frame %llu: %.2f cycles/instruction\n", (unsigned long long)g_frame,
                (double)(cyc_total - c0) / (double)(ins_total - i0 ? ins_total - i0 : 1));
        c0 = cyc_total; i0 = ins_total;
    }
    if (g_frame >= max_frames) finish();
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: %s rom frames inputs outdir [dump_every]\n", argv[0]); return 1; }
    if (!gen_load_rom(argv[1])) { perror("rom"); return 1; }
    max_frames = strtoull(argv[2], 0, 10);
    gen_load_inputs(argv[3]);
    outdir = argv[4];
    dump_every = argc > 5 ? atoi(argv[5]) : 0;
    char path[512];
    snprintf(path, sizeof path, "%s/frames.log", outdir);
    flog = fopen(path, "w");
    if (!flog) { perror(path); return 1; }

    gen_reset();
    gen_frame_cb = on_frame;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    for (;;) m68k_execute(1000000);
}
