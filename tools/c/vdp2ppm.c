/* Render a VDP dump (as written by ds_test/ds_ref) to a PPM image.
 * usage: vdp2ppm dump.bin out.ppm [ext_left ext_right] */
#include <stdio.h>
#include <stdlib.h>
#include "genesis.h"
#include "vdp_render.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s dump.bin out.ppm [extl extr]\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    uint8_t b[208];
    fread(g_vreg, 1, 32, f);
    fread(g_vram, 1, 0x10000, f);
    fread(b, 1, 208, f);
    fclose(f);
    for (int i = 0; i < 64; i++) g_cram[i] = (uint16_t)(b[2*i] << 8 | b[2*i+1]);
    for (int i = 0; i < 40; i++) g_vsram[i] = (uint16_t)(b[128+2*i] << 8 | b[128+2*i+1]);
    vdp_render_opts o = { argc > 3 ? atoi(argv[3]) : 0, argc > 4 ? atoi(argv[4]) : 0, 1 };
    int w = o.ext_left + vdp_display_width() + o.ext_right;
    static uint32_t fb[VDP_MAX_W * VDP_H];
    vdp_render(fb, VDP_MAX_W, &o);
    FILE *o2 = fopen(argv[2], "wb");
    fprintf(o2, "P6\n%d %d\n255\n", w, VDP_H);
    for (int y = 0; y < VDP_H; y++)
        for (int x = 0; x < w; x++) {
            uint32_t p = fb[y * VDP_MAX_W + x];
            uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
            fwrite(rgb, 1, 3, o2);
        }
    fclose(o2);
    return 0;
}
