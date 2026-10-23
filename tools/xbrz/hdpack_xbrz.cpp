// Build an HD pack from a graphics dump with xBRZ (GPL-3, by Zenju; see
// License.txt). Kept as a separate program so the game itself stays free of
// GPL code. Every .png in <dump dir> is scaled 4x (2..6 with -s) and written
// with the same name to <pack dir>; .txt files (sheet layouts, palettes) are
// copied. Context sheets (ctx_*) and sprite sheets (spr_*) are scaled whole,
// so patterns are upscaled among their neighbours.
//
// Busy, dithered 8x8 cells (foliage, noise, checkerboard shading) don't
// suit xBRZ: it turns them into blobs that differ from tile to tile. Cells
// whose neighbouring pixels differ more often than the threshold are scaled
// nearest-neighbour instead, so textures stay crisp and seamless.
//
//   hdpack_xbrz <dump dir> <pack dir> [-s N] [-n threshold 0..1, default 0.5]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include "xbrz.h"
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

namespace fs = std::filesystem;

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <dump dir> <pack dir> [-s N]\n", argv[0]); return 1; }
    int scale = 4;
    double busy = 0.5;
    for (int i = 3; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "-s")) scale = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "-n")) busy = atof(argv[i + 1]);
    }
    if (scale < 2 || scale > 6) scale = 4;
    fs::path in = fs::u8path(argv[1]), out = fs::u8path(argv[2]);
    fs::create_directories(out);
    int n = 0, copied = 0;
    for (auto &e : fs::directory_iterator(in)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        for (auto &c : ext) c = (char)tolower(c);
        fs::path dst = out / e.path().filename();
        if (ext == ".txt") { fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing); copied++; continue; }
        if (ext != ".png") continue;
        int w, h, ch;
        unsigned char *px = stbi_load(e.path().string().c_str(), &w, &h, &ch, 4);
        if (!px) continue;
        std::vector<uint32_t> src((size_t)w * h), dst_px((size_t)w * h * scale * scale);
        memcpy(src.data(), px, src.size() * 4);
        stbi_image_free(px);
        /* transparent pixels: one colour so they don't create false edges */
        for (auto &p : src) if ((p >> 24) == 0) p = 0;
        xbrz_scale_defaults(scale, src.data(), dst_px.data(), w, h, ColorFormat::RGBA);
        /* busy cells: nearest-neighbour */
        int W = w * scale;
        for (int cy = 0; cy + 8 <= h; cy += 8)
            for (int cx = 0; cx + 8 <= w; cx += 8) {
                int diff = 0, n = 0;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        uint32_t a = src[(size_t)(cy + y) * w + cx + x];
                        if (x < 7) { n++; diff += a != src[(size_t)(cy + y) * w + cx + x + 1]; }
                        if (y < 7) { n++; diff += a != src[(size_t)(cy + y + 1) * w + cx + x]; }
                    }
                if ((double)diff / n < busy) continue;
                for (int y = 0; y < 8 * scale; y++)
                    for (int x = 0; x < 8 * scale; x++)
                        dst_px[(size_t)(cy * scale + y) * W + cx * scale + x] = src[(size_t)(cy + y / scale) * w + cx + x / scale];
            }
        stbi_write_png(dst.string().c_str(), w * scale, h * scale, 4, dst_px.data(), w * scale * 4);
        if (++n % 500 == 0) printf("%d images...\n", n);
    }
    printf("%d images scaled %dx, %d text files copied into %s\n", n, scale, copied, argv[2]);
    return 0;
}
