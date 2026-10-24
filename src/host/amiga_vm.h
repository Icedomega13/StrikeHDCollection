/*
 * A small Amiga, enough to run Desert Strike's cutscene programs from the
 * player's own disks (docs/13-amiga-ultimate.md): a 68000 (Musashi), 2 MB
 * of chip RAM, the blitter, the copper's display and colour moves, the
 * vertical-blank and copper interrupts, the beam counter and the CIA mouse
 * buttons. The programs take over the hardware, blit their animation into
 * their own screens and point the copper at them; each frame the screen is
 * read back through the copper list, as the Amiga's display would show it.
 * Sound writes are ignored (the host plays the Amiga music itself).
 */
#ifndef DS_AMIGA_VM_H
#define DS_AMIGA_VM_H
#include <stdint.h>
#include <stddef.h>

#define AVM_W 384               /* largest screen read back (overscan) */
#define AVM_H 290

/* Load an AmigaDOS hunk program and start it. Only one runs at a time. */
int  avm_start(const uint8_t *hunks, size_t n);
void avm_stop(void);
int  avm_running(void);
/* Run one PAL frame (1/50 s) and read the display back. */
void avm_frame(void);
/* The last frame: ARGB, AVM_W wide; *w, *h the display size, *x0 the
 * screen's left edge in low-res pixels from the display window start. */
const uint32_t *avm_screen(int *w, int *h);
unsigned avm_debug_pc(void);
void avm_debug_dump(void);
int  avm_finished(void);        /* the program returned (or set its done flag) */

#endif
