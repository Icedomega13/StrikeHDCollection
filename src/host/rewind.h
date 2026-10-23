/*
 * Rewind buffer: a history of save states kept as compressed differences.
 * The newest state is held whole; each older one is stored as the XOR with
 * its successor, run-length coded (most of the 140 KB state is unchanged
 * from one snapshot to the next), so stepping back is cheap and a few MB
 * hold half a minute. The oldest entries are dropped past the byte limit.
 */
#ifndef DS_REWIND_H
#define DS_REWIND_H
#include <stddef.h>
#include <stdint.h>

void rw_init(size_t state_size, size_t max_bytes);
void rw_clear(void);
void rw_push(const uint8_t *state);   /* record the newest state */
int  rw_pop(uint8_t *out);            /* step back: the state before the newest; 0 if none */
int  rw_count(void);                  /* states available to step back to */
size_t rw_bytes(void);

#endif
