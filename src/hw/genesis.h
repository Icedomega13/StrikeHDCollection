/*
 * Shared Genesis hardware model used by both the Musashi reference harness
 * and the recompiled build. Keeping one implementation guarantees both see
 * identical hardware behaviour, which is what makes lockstep testing work.
 *
 * Timing is measured in executed 68000 instructions, not cycles. Each side
 * calls gen_instr_tick() exactly once before every instruction it executes.
 */
#ifndef GENESIS_H
#define GENESIS_H
#include <stddef.h>
#include <stdint.h>

#define GEN_FRAME_INSNS   20000u   /* instructions per frame (default) */
#define GEN_VBLANK_START  17000u   /* vblank begins at this point in a frame */
/* Instructions per frame, applied from the next frame. 20000 = the default
 * (about 2.3x the real console's 68000, so the game never slows down);
 * GEN_FRAME_INSNS_AUTHENTIC approximates the real CPU (128,000 cycles per
 * frame at a measured average of 14.8 cycles per instruction). */
#define GEN_FRAME_INSNS_AUTHENTIC 8650u
extern uint32_t g_frame_insns;
extern uint64_t g_frame_start;
extern uint32_t g_frame_len;
/* vblank starts 85% into a frame (17000 of 20000) */
static inline uint32_t gen_vblank_offset(uint32_t len) { return (uint32_t)((uint64_t)len * GEN_VBLANK_START / GEN_FRAME_INSNS); }

/* pad buttons, bit set = pressed */
enum { GB_UP = 1, GB_DOWN = 2, GB_LEFT = 4, GB_RIGHT = 8,
       GB_B = 16, GB_C = 32, GB_A = 64, GB_START = 128 };

extern uint8_t  g_rom[0x400000];
extern uint32_t g_rom_size;
extern uint8_t  g_ram[0x10000];
extern uint8_t  g_zram[0x2000];
extern uint8_t  g_vram[0x10000];
extern uint16_t g_cram[64];
extern uint16_t g_vsram[40];
extern uint8_t  g_vreg[32];
extern uint16_t g_pad_buttons[2];

extern uint64_t g_icount;      /* instructions executed so far */
extern uint64_t g_frame;       /* frames completed */

int  gen_load_rom(const char *path);
/* Use ROM bytes already in memory (handles odd sizes, pads with $FF). */
int  gen_set_rom(const uint8_t *data, uint32_t size);
void gen_reset(void);

uint32_t gen_read8(uint32_t a);
uint32_t gen_read16(uint32_t a);
uint32_t gen_read32(uint32_t a);
void gen_write8(uint32_t a, uint32_t v);
void gen_write16(uint32_t a, uint32_t v);
void gen_write32(uint32_t a, uint32_t v);

/*
 * Call before every instruction. sr_mask is the CPU interrupt mask (SR bits
 * 8-10). Returns 1 when the CPU must take the level-6 vblank interrupt now,
 * before executing the instruction.
 */
static inline int gen_instr_tick(int sr_mask);

/* Called by gen_instr_tick at the start of every frame (after the previous
 * frame's vblank ends). Set by the host. */
extern void (*gen_frame_cb)(void);
/* Called at the start of vblank, when the frame's picture is complete.
 * The host renders here. Must not touch emulated state. */
extern void (*gen_vblank_cb)(void);

/* Input script: "<frame> <buttons>" lines, applied at frame starts. */
int gen_load_inputs(const char *path);
/* After loading a state: skip script entries up to this frame. */
void gen_seek_inputs(uint64_t frame);

/*
 * Save states. gen_state_size() bytes are written by gen_state_save() and
 * read back by gen_state_load(). Covers memory, VDP, pads, Z80 bus state and
 * timing; the CPU side is saved separately by the runtime. Only valid at a
 * frame boundary.
 */
#define GEN_STATE_VERSION 2
size_t gen_state_size(void);
void gen_state_save(uint8_t *out);
int  gen_state_load(const uint8_t *in);   /* 0 on version mismatch */

/* State checksum line for lockstep comparison (memory side only). */
void gen_state_crcs(uint32_t out[4]);

/* ---- inline timing ---- */
extern uint64_t g_next_vblank, g_next_frame;
extern int g_in_vblank, g_vint_flag;
void gen_on_vblank_start(void);
void gen_on_frame_start(void);

static inline int gen_instr_tick(int sr_mask)
{
    if (g_icount >= g_next_vblank) gen_on_vblank_start();
    if (g_icount >= g_next_frame) gen_on_frame_start();
    g_icount++;
    if (g_vint_flag && (g_vreg[1] & 0x20) && sr_mask < 6) {
        g_vint_flag = 0;
        return 1;
    }
    return 0;
}

/* Fast RAM paths for generated code */
static inline uint32_t gen_ram_r16(uint32_t a) { a &= 0xFFFF; return (g_ram[a] << 8) | g_ram[(a + 1) & 0xFFFF]; }

#endif
