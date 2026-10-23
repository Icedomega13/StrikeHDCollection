/* Runtime support for recompiled 68000 code: dispatch, interrupts, RAM code. */
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include "cpu.h"

cpu_t R;
int32_t g_ws_ext;
int32_t g_ws_win;
uint64_t g_ws_flight_frame;
int32_t g_start_campaign;
uint32_t cpu_cur_pc;
void (*cpu_fatal_hook)(void);
recomp_fn (*cpu_lookup)(uint32_t pc);

void cpu_fatal(const char *msg, uint32_t pc)
{
    fprintf(stderr, "FATAL: %s at %06X (frame %llu, icount %llu)\n", msg, pc,
            (unsigned long long)g_frame, (unsigned long long)g_icount);
    if (cpu_fatal_hook) cpu_fatal_hook();
    exit(2);
}

/* The only RAM-resident code is the vblank trampoline at $FF1C72, a
 * `jmp abs.l` the game rewrites. Interpret the few opcodes it can hold. */
static void ram_exec(uint32_t pc)
{
    TICK(pc);
    uint32_t op = R16(pc);
    if (op == 0x4EF9) {
        cpu_dispatch(R32(pc + 2) & 0xFFFFFF);
        return;
    }
    if (op == 0x4E73) {
        uint32_t sr = POP16();
        R.pc = POP32();
        cpu_set_sr(sr);
        return;
    }
    if (op == 0x4E75) {
        R.pc = POP32();
        return;
    }
    cpu_fatal("unsupported RAM opcode", pc);
}

void cpu_dispatch(uint32_t pc)
{
    pc &= 0xFFFFFF;
    recomp_fn fn = cpu_lookup(pc);
    if (fn) { fn(pc); return; }
    if (pc >= 0xFF0000) { ram_exec(pc); return; }
    cpu_fatal("no recompiled code for address", pc);
}

void cpu_irq(uint32_t pc)
{
    uint16_t sr = cpu_get_sr();
    cpu_set_sr((sr & 0x00FF) | 0x2000 | 0x0600);   /* supervisor, mask 6, trace off */
    PUSH32(pc);
    PUSH16(sr);
    uint32_t vec = R32(0x78) & 0xFFFFFF;
    cpu_dispatch(vec);
    if (R.pc != pc) cpu_fatal("interrupt returned to unexpected address", R.pc);
}

void cpu_reset(void)
{
    for (int i = 0; i < 8; i++) R.d[i] = R.a[i] = 0;
    R.usp = 0;
    R.srh = 0x27;
    R.x = R.n = R.z = R.v = R.c = 0;
    R.a[7] = R32(0);
    R.pc = R32(4) & 0xFFFFFF;
}

void cpu_run(void)
{
    for (;;) cpu_dispatch(R.pc);
}

static jmp_buf frame_jmp;
static int frame_active;

void cpu_run_frame(void)
{
    if (setjmp(frame_jmp)) { frame_active = 0; return; }
    frame_active = 1;
    for (;;) cpu_dispatch(R.pc);
}

void cpu_frame_yield(void)
{
    if (!frame_active) return;          /* plain cpu_run(): keep going */
    R.pc = cpu_cur_pc;
    longjmp(frame_jmp, 1);
}

size_t cpu_state_size(void) { return sizeof R + sizeof cpu_cur_pc + gen_state_size(); }

void cpu_state_save(uint8_t *out)
{
    cpu_t r = R;
    r.pc = cpu_cur_pc;                  /* resume point: the next instruction to run */
    memcpy(out, &r, sizeof r); out += sizeof r;
    memcpy(out, &cpu_cur_pc, sizeof cpu_cur_pc); out += sizeof cpu_cur_pc;
    gen_state_save(out);
}

int cpu_state_load(const uint8_t *in)
{
    if (!gen_state_load(in + sizeof R + sizeof cpu_cur_pc)) return 0;
    memcpy(&R, in, sizeof R);
    memcpy(&cpu_cur_pc, in + sizeof R, sizeof cpu_cur_pc);
    return 1;
}
