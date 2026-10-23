/*
 * Genesis sound hardware: the Z80 CPU running the game's sound driver, the
 * YM2612 FM chip, the SN76489 PSG, and the Z80's view of the bus.
 *
 * Everything runs on the same instruction-count clock as the rest of the
 * hardware model (see genesis.h), converted to master clocks, so it is
 * deterministic and identical in the reference and recompiled builds.
 * The Z80 is run lazily: it catches up whenever the 68000 touches sound
 * hardware, at vblank, and at the end of each frame.
 */
#ifndef DS_SOUND_H
#define DS_SOUND_H
#include <stddef.h>
#include <stdint.h>

#define SND_MCLK          53693175u            /* NTSC master clock */
#define SND_FRAME_MCLK    896040u              /* 262 lines x 3420 */
#define SND_SAMPLE_MCLK   1008u                /* YM2612 sample: 7 x 144 */
#define SND_RATE_NUM      SND_MCLK             /* output rate = 53267.04 Hz */
#define SND_RATE_DEN      SND_SAMPLE_MCLK

void snd_init(void);                           /* once */
void snd_reset(void);                          /* console reset */
void snd_sync(void);                           /* run sound hardware up to now */
void snd_vblank(void);                         /* vblank start: Z80 interrupt */

/* 68000 side */
uint8_t snd_68k_read8(uint32_t a);             /* $A00000-$A0FFFF */
void    snd_68k_write8(uint32_t a, uint8_t v);
void    snd_psg_write(uint8_t v);              /* $C00011 */
void    snd_set_busreq(int on);
void    snd_set_reset(int asserted);
int     snd_bus_granted(void);

/* Output: interleaved stereo int16 at 53267 Hz. Returns frames copied. */
int     snd_read(int16_t *out, int max_frames);
int     snd_available(void);
/* Mix levels in percent (100 = default). */
void    snd_set_levels(int fm_pct, int psg_pct);

/* Save states */
size_t  snd_state_size(void);
void    snd_state_save(uint8_t *out);
int     snd_state_load(const uint8_t *in);

#endif
