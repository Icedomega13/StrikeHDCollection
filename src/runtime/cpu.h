/*
 * CPU state and helpers for recompiled 68000 code.
 * Semantics follow the real 68000 (and Musashi, our reference) exactly,
 * including condition codes, so lockstep comparison is meaningful.
 */
#ifndef DS_CPU_H
#define DS_CPU_H
#include <stdint.h>
#include "../hw/genesis.h"

typedef struct {
    uint32_t d[8], a[8];
    uint32_t usp;           /* inactive stack pointer */
    uint32_t pc;            /* set on return/unwind; target for dispatch */
    uint8_t x, n, z, v, c;  /* condition codes, 0/1 */
    uint8_t srh;            /* SR bits 8-15: T . S . . I2 I1 I0 */
} cpu_t;

extern cpu_t R;

/* Widescreen: extra pixels the game keeps valid on each side of its 320 px
 * view (0 = original game). Read by patched instructions (hints.json). */
extern int32_t g_ws_ext;
/* Extra width the game's 512 px plane window keeps loaded per side:
 * min(g_ws_ext, 56). Wider views are painted from map data. */
extern int32_t g_ws_win;
/* Last frame in which the game wrote the scrolling-map camera to the VDP;
 * the host only draws the extra columns while this is current. */
extern uint64_t g_ws_flight_frame;
/* Campaign a new game starts at (0 = the first, as on the console). Read by
 * a patched instruction; used by the graphics extractor and campaign select. */
extern int32_t g_start_campaign;
/* Sound effect requests (hints.json patch at the game's sound-effect call):
 * return 1 to skip the Genesis effect (the host plays a replacement). */
extern int (*g_sfx_hook)(int id);
/* Music / digital sample requests (0 stop, 1-99 song, 100+ sample): 1 = skip
 * the Genesis one, 2 = stop the Genesis music instead. g_music_ctx says where
 * the request came from (1 = campaign briefing); cleared after each request. */
extern int (*g_music_hook)(int id);
extern int g_music_ctx;
/* The scene record of the cutscene / briefing screen being shown (Desert:
 * hints.json patches on the scene player $005994), 0 when none. */
extern uint32_t g_scene_rec;

typedef void (*recomp_fn)(uint32_t entry);
extern recomp_fn (*cpu_lookup)(uint32_t pc); /* the running game's table (generated) */
void cpu_dispatch(uint32_t pc);            /* run code at pc until it returns/unwinds */
void cpu_irq(uint32_t pc);                 /* take vblank interrupt before pc */
void cpu_fatal(const char *msg, uint32_t pc);
extern void (*cpu_fatal_hook)(void);     /* called before exiting on a fatal error */
void cpu_reset(void);
void cpu_run(void);                        /* run forever (no frame yields) */

/*
 * Frame stepping. cpu_run_frame() runs the game until the next frame
 * boundary and returns. The frame callback must call cpu_frame_yield(),
 * which unwinds the C stack back to cpu_run_frame(). The next call resumes
 * at the exact instruction, which is possible because every instruction is
 * a dispatch entry. Hardware time is unaffected: re-running the tick for
 * the resume instruction is a no-op until its counter increment.
 */
void cpu_run_frame(void);
void cpu_frame_yield(void);

/* Whole-machine save state (CPU + hardware). Take only between frames. */
size_t cpu_state_size(void);
void cpu_state_save(uint8_t *out);
int  cpu_state_load(const uint8_t *in);

#define R8(a)  gen_read8(a)
#define R16(a) gen_read16(a)
#define R32(a) gen_read32(a)
#define W8(a, v)  gen_write8((a), (v))
#define W16(a, v) gen_write16((a), (v))
#define W32(a, v) gen_write32((a), (v))

extern uint32_t cpu_cur_pc;
#define TICK(pc) do { cpu_cur_pc = (pc); while (gen_instr_tick(R.srh & 7)) { cpu_irq(pc); cpu_cur_pc = (pc); } } while (0)

static inline uint16_t cpu_get_sr(void)
{
    return (uint16_t)((R.srh << 8) | (R.x << 4) | (R.n << 3) | (R.z << 2) | (R.v << 1) | R.c);
}

static inline void cpu_set_ccr(uint32_t v)
{
    R.x = (v >> 4) & 1; R.n = (v >> 3) & 1; R.z = (v >> 2) & 1; R.v = (v >> 1) & 1; R.c = v & 1;
}

static inline void cpu_set_sr(uint32_t v)
{
    uint8_t nh = (uint8_t)((v >> 8) & 0xA7);
    if ((nh ^ R.srh) & 0x20) { uint32_t t = R.a[7]; R.a[7] = R.usp; R.usp = t; }
    R.srh = nh;
    cpu_set_ccr(v);
}

static inline void PUSH32(uint32_t v) { R.a[7] -= 4; W32(R.a[7], v); }
static inline void PUSH16(uint32_t v) { R.a[7] -= 2; W16(R.a[7], v); }
static inline uint32_t POP32(void) { uint32_t v = R32(R.a[7]); R.a[7] += 4; return v; }
static inline uint32_t POP16(void) { uint32_t v = R16(R.a[7]); R.a[7] += 2; return v; }

/* ---- condition codes ---- */
#define CC_T  1
#define CC_F  0
#define CC_HI (!R.c && !R.z)
#define CC_LS (R.c || R.z)
#define CC_CC (!R.c)
#define CC_CS (R.c)
#define CC_NE (!R.z)
#define CC_EQ (R.z)
#define CC_VC (!R.v)
#define CC_VS (R.v)
#define CC_PL (!R.n)
#define CC_MI (R.n)
#define CC_GE (R.n == R.v)
#define CC_LT (R.n != R.v)
#define CC_GT (!R.z && R.n == R.v)
#define CC_LE (R.z || R.n != R.v)

/* ---- flag helpers, generated per size ---- */
#define DEF_SIZE(S, MASK, MSB)                                                          \
static inline void NZ##S(uint32_t r) { R.n = (r & MSB) != 0; R.z = (r & MASK) == 0; }   \
static inline void LOGIC##S(uint32_t r) { NZ##S(r); R.v = 0; R.c = 0; }                 \
static inline uint32_t ADD##S(uint32_t s, uint32_t d) {                                 \
    s &= MASK; d &= MASK; uint32_t r = (d + s) & MASK;                                  \
    R.c = R.x = (((s & d) | (~r & (s | d))) & MSB) != 0;                                \
    R.v = (((s ^ r) & (d ^ r)) & MSB) != 0; NZ##S(r); return r; }                       \
static inline uint32_t SUB##S(uint32_t s, uint32_t d) {                                 \
    s &= MASK; d &= MASK; uint32_t r = (d - s) & MASK;                                  \
    R.c = R.x = (((s & ~d) | (r & ~d) | (s & r)) & MSB) != 0;                           \
    R.v = (((s ^ d) & (r ^ d)) & MSB) != 0; NZ##S(r); return r; }                       \
static inline void CMP##S(uint32_t s, uint32_t d) {                                     \
    s &= MASK; d &= MASK; uint32_t r = (d - s) & MASK;                                  \
    R.c = (((s & ~d) | (r & ~d) | (s & r)) & MSB) != 0;                                 \
    R.v = (((s ^ d) & (r ^ d)) & MSB) != 0; NZ##S(r); }                                 \
static inline uint32_t ADDX##S(uint32_t s, uint32_t d) {                                \
    s &= MASK; d &= MASK; uint32_t r = (d + s + R.x) & MASK;                            \
    R.c = R.x = (((s & d) | (~r & (s | d))) & MSB) != 0;                                \
    R.v = (((s ^ r) & (d ^ r)) & MSB) != 0; R.n = (r & MSB) != 0;                      \
    if (r) R.z = 0; return r; }                                                         \
static inline uint32_t SUBX##S(uint32_t s, uint32_t d) {                                \
    s &= MASK; d &= MASK; uint32_t r = (d - s - R.x) & MASK;                            \
    R.c = R.x = (((s & ~d) | (r & ~d) | (s & r)) & MSB) != 0;                           \
    R.v = (((s ^ d) & (r ^ d)) & MSB) != 0; R.n = (r & MSB) != 0;                      \
    if (r) R.z = 0; return r; }                                                         \
static inline uint32_t NEG##S(uint32_t d) { return SUB##S(d, 0); }                      \
static inline uint32_t NEGX##S(uint32_t d) { return SUBX##S(d, 0); }                    \
static inline uint32_t ASL##S(uint32_t v, int n) {                                      \
    v &= MASK; uint32_t c = 0; int vf = 0;                                              \
    for (int i = 0; i < n; i++) { c = (v & MSB) != 0; uint32_t nv = (v << 1) & MASK;    \
        if ((nv ^ v) & MSB) vf = 1; v = nv; }                                           \
    if (n) { R.c = R.x = (uint8_t)c; } else R.c = 0;                                    \
    R.v = (uint8_t)vf; NZ##S(v); return v; }                                            \
static inline uint32_t LSL##S(uint32_t v, int n) {                                      \
    v &= MASK; uint32_t c = 0;                                                          \
    for (int i = 0; i < n; i++) { c = (v & MSB) != 0; v = (v << 1) & MASK; }            \
    if (n) { R.c = R.x = (uint8_t)c; } else R.c = 0;                                    \
    R.v = 0; NZ##S(v); return v; }                                                      \
static inline uint32_t LSR##S(uint32_t v, int n) {                                      \
    v &= MASK; uint32_t c = 0;                                                          \
    for (int i = 0; i < n; i++) { c = v & 1; v >>= 1; }                                 \
    if (n) { R.c = R.x = (uint8_t)c; } else R.c = 0;                                    \
    R.v = 0; NZ##S(v); return v; }                                                      \
static inline uint32_t ASR##S(uint32_t v, int n) {                                      \
    v &= MASK; uint32_t c = 0;                                                          \
    for (int i = 0; i < n; i++) { c = v & 1; v = (v >> 1) | (v & MSB); }                \
    if (n) { R.c = R.x = (uint8_t)c; } else R.c = 0;                                    \
    R.v = 0; NZ##S(v); return v; }                                                      \
static inline uint32_t ROL##S(uint32_t v, int n) {                                      \
    v &= MASK; uint32_t c = 0;                                                          \
    for (int i = 0; i < n; i++) { c = (v & MSB) != 0; v = ((v << 1) | c) & MASK; }      \
    R.c = n ? (uint8_t)c : 0; R.v = 0; NZ##S(v); return v; }                            \
static inline uint32_t ROR##S(uint32_t v, int n) {                                      \
    v &= MASK; uint32_t c = 0;                                                          \
    for (int i = 0; i < n; i++) { c = v & 1; v = (v >> 1) | (c ? MSB : 0); }            \
    R.c = n ? (uint8_t)c : 0; R.v = 0; NZ##S(v); return v; }                            \
static inline uint32_t ROXL##S(uint32_t v, int n) {                                     \
    v &= MASK;                                                                          \
    for (int i = 0; i < n; i++) { uint32_t c = (v & MSB) != 0;                          \
        v = ((v << 1) | R.x) & MASK; R.x = (uint8_t)c; }                                \
    R.c = R.x; R.v = 0; NZ##S(v); return v; }                                           \
static inline uint32_t ROXR##S(uint32_t v, int n) {                                     \
    v &= MASK;                                                                          \
    for (int i = 0; i < n; i++) { uint32_t c = v & 1;                                   \
        v = (v >> 1) | (R.x ? MSB : 0); R.x = (uint8_t)c; }                             \
    R.c = R.x; R.v = 0; NZ##S(v); return v; }

DEF_SIZE(8, 0xFFu, 0x80u)
DEF_SIZE(16, 0xFFFFu, 0x8000u)
DEF_SIZE(32, 0xFFFFFFFFu, 0x80000000u)

#define SET_DB(n, v)  (R.d[n] = (R.d[n] & 0xFFFFFF00u) | ((v) & 0xFFu))
#define SET_DW(n, v)  (R.d[n] = (R.d[n] & 0xFFFF0000u) | ((v) & 0xFFFFu))
#define SET_DL(n, v)  (R.d[n] = (uint32_t)(v))
#define SX8(v)  ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v) ((uint32_t)(int32_t)(int16_t)(v))

static inline void DIVU(int dn, uint32_t src, uint32_t pc)
{
    src &= 0xFFFF;
    if (!src) { cpu_fatal("divide by zero", pc); return; }
    uint32_t q = R.d[dn] / src, rem = R.d[dn] % src;
    if (q < 0x10000) {
        R.z = q == 0; R.n = (q & 0x8000) != 0; R.v = 0; R.c = 0;
        R.d[dn] = (q & 0xFFFF) | (rem << 16);
    } else R.v = 1;
}

static inline void DIVS(int dn, uint32_t src, uint32_t pc)
{
    int32_t s = (int16_t)src;
    if (!s) { cpu_fatal("divide by zero", pc); return; }
    if (R.d[dn] == 0x80000000u && s == -1) { R.z = 0; R.n = 0; R.v = 0; R.c = 0; R.d[dn] = 0; return; }
    int32_t q = (int32_t)R.d[dn] / s, rem = (int32_t)R.d[dn] % s;
    if (q == (int16_t)q) {
        R.z = q == 0; R.n = (q & 0x8000) != 0; R.v = 0; R.c = 0;
        R.d[dn] = ((uint32_t)q & 0xFFFF) | ((uint32_t)rem << 16);
    } else R.v = 1;
}

#endif
