/*
 * CRT display (docs/16-crt.md): the game picture through Lola Retro
 * (github.com/zoran123456/lola-retro, MIT), a shader chain that simulates
 * the console's video encoder, the cable, the TV's decoder, the electron
 * beam, the phosphor mask and the glass. Runs on SDL's OpenGL renderer: the
 * passes draw with raw GL between the renderer's own draws, and leave its
 * state as they found it.
 */
#ifndef DS_CRT_H
#define DS_CRT_H
#include <SDL3/SDL.h>

typedef struct {
    int cable;          /* 1 composite, 2 S-Video, 3 RGB */
    int curved;         /* curved glass and rounded corners */
    int mask_type;      /* phosphor mask, -1 = the preset's */
} crt_settings;

int crt_supported(SDL_Renderer *ren);
/* The frame (src_w x src_h at the top left of src) through the chain into
 * dst, a render-target texture the size of the picture on screen. Returns
 * 0 if the chain can't run (then draw the frame as usual). */
int crt_render(SDL_Renderer *ren, SDL_Texture *src, int src_w, int src_h, SDL_Texture *dst,
               const crt_settings *set, unsigned frame);

#endif
