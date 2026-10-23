/* C interface to the YM2612 (OPN2) emulation, implemented with ymfm. */
#ifndef DS_YM2612_H
#define DS_YM2612_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void    ym_init(void);
void    ym_reset(void);
void    ym_write(int port, uint8_t v);     /* port 0-3 = $4000-$4003 */
uint8_t ym_status(void);
/* One output sample at the chip rate (clock / 144), stereo, about +-32k. */
void    ym_sample(int32_t out[2]);

/* Save states: ym_state_save returns the byte count written (max YM_STATE_MAX). */
#define YM_STATE_MAX 8192
size_t  ym_state_save(uint8_t *out);
int     ym_state_load(const uint8_t *in, size_t n);

#ifdef __cplusplus
}
#endif
#endif
