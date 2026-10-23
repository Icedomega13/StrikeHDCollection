/*
 * Software VDP renderer. Draws the current VDP state (g_vreg, g_vram,
 * g_cram, g_vsram) into a 32-bit XRGB framebuffer.
 *
 * The frame is rendered once per frame at the start of vblank. The game
 * uses no raster effects (its HBlank vector is a bare RTE), so a whole-
 * frame render is exact.
 *
 * ext_left / ext_right add extra columns either side of the 320 (H40) or
 * 256 (H32) pixel display. Planes wrap and sprites extend into them, which
 * is the basis for widescreen. Use 0, 0 for the original image.
 */
#ifndef VDP_RENDER_H
#define VDP_RENDER_H
#include <stdint.h>

#define VDP_MAX_W 1024
#define VDP_H     224

/* A plane supplied in world coordinates instead of from VRAM (ultrawide).
 * cells[w*h] are name-table entries for world cells (cx0.., cy0..);
 * 0xFFFF = transparent. (ox, oy) = world pixel at screen (0, 0). */
typedef struct {
    const uint16_t *cells;
    int cx0, cy0, w, h;
    int ox, oy;
} vdp_world_plane;

/* HD replacement art for one 8x8 pattern: (8*scale)^2 ARGB texels.
 * ref = the 16 colours the art was drawn for (has_ref), so palette changes
 * (fades, flashes) can be followed. */
typedef struct {
    const uint32_t *rgba;
    int has_ref;
    uint32_t ref[16];
} vdp_hd_tile;

/* HD output: when fb is set, the frame is also composed at `scale` times
 * the size into fb (pitch in pixels), sampling tile[] art where present. */
typedef struct {
    int scale;
    uint32_t *fb;
    int pitch;
    const vdp_hd_tile *tile[2048];
} vdp_hd;

typedef struct {
    int ext_left, ext_right;   /* extra pixels each side (widescreen) */
    int sprite_limits;         /* 1: hardware per-line sprite limits (flicker); 0: unlimited */
    /* Ultrawide: planes A (0) and B (1) from world buffers when non-NULL,
     * and sprite X read as a signed 16-bit value instead of 9 bits. */
    const vdp_world_plane *world[2];
    int wide_sprites;
    const vdp_hd *hd;          /* optional HD composition */
    /* optional: run fn(ctx, l0, l1) over lines [0, n) split across threads */
    void (*parallel)(void (*fn)(void *ctx, int l0, int l1), void *ctx, int n);
    uint32_t *fb_lo; int pitch_lo;   /* internal */
} vdp_render_opts;

/* Width of the active display in pixels (256 or 320), without extensions. */
int vdp_display_width(void);

/* Render into fb (pitch in pixels). Output size is
 * (ext_left + vdp_display_width() + ext_right) x VDP_H. */
void vdp_render(uint32_t *fb, int pitch, const vdp_render_opts *o);

/* After vdp_render: OR into used[tile] a bit per palette line (1 << pal)
 * for every pattern visible in the last frame (n = rendered width). */
void vdp_render_used_tiles(uint8_t used[2048], int n);

/* Genesis 9-bit colour to XRGB8888 using the console's DAC levels. */
uint32_t vdp_color(uint16_t c);

#endif
