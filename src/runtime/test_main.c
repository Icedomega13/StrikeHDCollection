/*
 * Headless test runner for the recompiled build. Same arguments and
 * frames.log format as harness/harness.c so the two can be diffed.
 *
 * usage: ds_test rom.bin frames inputs.txt outdir [dump_every]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "game.h"
#include "sound.h"

static uint64_t max_frames;
static uint64_t save_at;          /* --save-at N file: write a state after frame N */
static const char *save_path, *load_path;
static FILE *wav;
static int ram_every;              /* --ram-every N: append RAM to outdir/ram.bin */
static FILE *ramf;                 /* --wav file: record audio */
static uint32_t wav_frames;

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t rate = 53267, bytes = frames * 4;
    uint8_t h[44] = { 'R','I','F','F' };
    uint32_t v;
    v = 36 + bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t s = 1; memcpy(h + 20, &s, 2);
    s = 2; memcpy(h + 22, &s, 2);
    memcpy(h + 24, &rate, 4);
    v = rate * 4; memcpy(h + 28, &v, 4);
    s = 4; memcpy(h + 32, &s, 2);
    s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}
static int dump_every;
static const char *outdir;
static FILE *flog;
extern void (*cpu_fatal_hook)(void);

static void dump_vdp(uint64_t f)
{
    char path[512];
    snprintf(path, sizeof path, "%s/vdp_%06llu.bin", outdir, (unsigned long long)f);
    FILE *o = fopen(path, "wb");
    if (!o) return;
    fwrite(g_vreg, 1, 32, o);
    fwrite(g_vram, 1, sizeof g_vram, o);
    for (int i = 0; i < 64; i++) { uint8_t b[2] = { g_cram[i] >> 8, (uint8_t)g_cram[i] }; fwrite(b, 1, 2, o); }
    for (int i = 0; i < 40; i++) { uint8_t b[2] = { g_vsram[i] >> 8, (uint8_t)g_vsram[i] }; fwrite(b, 1, 2, o); }
    fclose(o);
    snprintf(path, sizeof path, "%s/ram_%06llu.bin", outdir, (unsigned long long)f);
    if ((o = fopen(path, "wb"))) { fwrite(g_ram, 1, sizeof g_ram, o); fclose(o); }
}

static FILE *flightf;
static void log_state(void)
{
    if (flightf) fprintf(flightf, "%llu %d\n", (unsigned long long)g_frame, g_ws_flight_frame + 1 >= g_frame);
    uint32_t c[4];
    gen_state_crcs(c);
    fprintf(flog, "%llu pc=%06X sr=%04X", (unsigned long long)g_frame, cpu_cur_pc, cpu_get_sr());
    for (int i = 0; i < 8; i++) fprintf(flog, " d%d=%08X", i, R.d[i]);
    for (int i = 0; i < 8; i++) fprintf(flog, " a%d=%08X", i, R.a[i]);
    fprintf(flog, " ram=%08X vram=%08X vdp=%08X zram=%08X\n", c[0], c[1], c[2], c[3]);
}

static void on_frame(void)
{
    log_state();
    if (dump_every && g_frame % dump_every == 0) dump_vdp(g_frame);
    {
        int16_t buf[4096 * 2];
        int n;
        while ((n = snd_read(buf, 4096)) > 0)
            if (wav) { fwrite(buf, 4, (size_t)n, wav); wav_frames += (uint32_t)n; }
    }
    { static long zat = -2; if (zat == -2) { const char *e = getenv("DS_ZRAM_AT"); zat = e ? atol(e) : -1; }
      if (zat >= 0 && (long)g_frame == zat) { FILE *z = fopen("/tmp/zram.bin", "wb"); if (z) { fwrite(g_zram, 1, sizeof g_zram, z); fclose(z); } } }
    if (ram_every && g_frame % ram_every == 0 && ramf) fwrite(g_ram, 1, sizeof g_ram, ramf);
    if (save_path && g_frame == save_at) {
        size_t n = cpu_state_size();
        uint8_t *b = malloc(n);
        cpu_state_save(b);
        FILE *o = fopen(save_path, "wb");
        if (o) { fwrite(b, 1, n, o); fclose(o); }
        free(b);
    }
    cpu_frame_yield();
}

static void on_fatal(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/ram_fatal.bin", outdir);
    FILE *o = fopen(path, "wb");
    if (o) { fwrite(g_ram, 1, sizeof g_ram, o); fclose(o); }
    if (flog) fflush(flog);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s rom frames inputs outdir [dump_every] [--save-at N file] [--load file] [--wav file]\n", argv[0]);
        return 1;
    }
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--save-at") && i + 2 < argc) { save_at = strtoull(argv[i+1], 0, 10); save_path = argv[i+2]; i += 2; }
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) { load_path = argv[++i]; }
        else if (!strcmp(argv[i], "--ram-every") && i + 1 < argc) ram_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ws") && i + 1 < argc) { g_ws_ext = atoi(argv[++i]); g_ws_win = g_ws_ext < 56 ? g_ws_ext : 56; }
        else if (!strcmp(argv[i], "--campaign") && i + 1 < argc) g_start_campaign = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frame-insns") && i + 1 < argc) g_frame_insns = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--flight-log") && i + 1 < argc) flightf = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { wav = fopen(argv[++i], "wb"); if (wav) wav_header(wav, 0); }
    }
    if (!gen_load_rom(argv[1])) { perror("rom"); return 1; }
    max_frames = strtoull(argv[2], 0, 10);
    gen_load_inputs(argv[3]);
    outdir = argv[4];
    dump_every = argc > 5 && argv[5][0] != '-' ? atoi(argv[5]) : 0;
    char path[512];
    snprintf(path, sizeof path, "%s/frames.log", outdir);
    flog = fopen(path, "w");
    if (!flog) { perror(path); return 1; }
    const game_module *game = game_identify_rom();
    if (!game) { fprintf(stderr, "%s: not a supported ROM\n", argv[1]); return 1; }
    game_boot(game);
    if (load_path) {
        FILE *in = fopen(load_path, "rb");
        if (!in) { perror(load_path); return 1; }
        size_t n = cpu_state_size();
        uint8_t *b = malloc(n);
        if (fread(b, 1, n, in) != n || !cpu_state_load(b)) { fprintf(stderr, "bad state file\n"); return 1; }
        fclose(in); free(b);
        gen_seek_inputs(g_frame);
    }
    if (ram_every) { snprintf(path, sizeof path, "%s/ram.bin", outdir); ramf = fopen(path, "wb"); }
    gen_frame_cb = on_frame;
    cpu_fatal_hook = on_fatal;
    /* Same frame-stepping loop the PC host uses. */
    while (g_frame < max_frames) cpu_run_frame();
    fclose(flog);
    if (wav) { wav_header(wav, wav_frames); fclose(wav); }
    fprintf(stderr, "%s: ran %llu frames, %llu instructions, PC %06X\n", game->id,
            (unsigned long long)g_frame, (unsigned long long)g_icount, cpu_cur_pc);
    return 0;
}
