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
//               [--ai <realesrgan-ncnn-vulkan> [-m model dir] [--model name]]
//
// --ai: after the xBRZ pass, the sprite and context sheets are redone with
// Real-ESRGAN (ncnn/Vulkan, BSD-3; a separate program run on the GPU). Its
// RGB output is used with the xBRZ alpha, so transparent edges stay crisp.
// Transparent pixels are filled from their neighbours and every sheet is
// padded before it goes in, so edges get no halo. If the program can't run
// (no Vulkan GPU), the xBRZ result stays. Progress goes to <pack>/progress.txt
// as "<done> <total> <message>".
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include "xbrz.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

namespace fs = std::filesystem;

static void progress(const fs::path &out, int done, int total, const char *msg)
{
    FILE *f = fopen((out / "progress.txt").string().c_str(), "w");
    if (f) { fprintf(f, "%d %d %s\n", done, total, msg); fclose(f); }
}

static std::string quote(const std::string &s) { return "\"" + s + "\""; }

/* Run a command line without a console window; returns its exit code. */
static int run(const std::string &cmd)
{
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, NULL, 0);
    std::vector<wchar_t> w((size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, w.data(), n);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, w.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
#else
    return system(cmd.c_str());
#endif
}

/* Redo the sheets in `out` with the AI upscaler; returns sheets replaced. */
static int ai_pass(const fs::path &in, const fs::path &out, const std::string &exe, const std::string &models, const std::string &model)
{
    const int S = 4, PAD = 8;
    fs::path tmp = out / "ai_tmp";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    std::vector<fs::path> sheets;
    for (auto &e : fs::directory_iterator(in)) {
        std::string name = e.path().filename().string();
        if (e.path().extension() != ".png") continue;
        if (name.rfind("spr_", 0) == 0 || name.rfind("ctx_", 0) == 0) sheets.push_back(e.path());
    }
    int total = (int)sheets.size(), done = 0, replaced = 0;
    const int CHUNK = 150;
    for (int c0 = 0; c0 < total; c0 += CHUNK) {
        fs::path ti = tmp / "in", to = tmp / "out";
        fs::remove_all(tmp, ec);
        fs::create_directories(ti);
        fs::create_directories(to);
        int c1 = c0 + CHUNK < total ? c0 + CHUNK : total;
        /* opaque, padded inputs */
        for (int i = c0; i < c1; i++) {
            int w, h, ch;
            unsigned char *px = stbi_load(sheets[i].string().c_str(), &w, &h, &ch, 4);
            if (!px) continue;
            std::vector<uint32_t> p((size_t)w * h);
            memcpy(p.data(), px, p.size() * 4);
            stbi_image_free(px);
            std::vector<uint8_t> known((size_t)w * h);
            for (size_t k = 0; k < p.size(); k++) known[k] = (p[k] >> 24) >= 128;
            for (int pass = 0; pass < 12; pass++) {      /* grow colours into transparency */
                std::vector<uint8_t> nk = known;
                std::vector<uint32_t> np = p;
                int grew = 0;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        size_t k = (size_t)y * w + x;
                        if (known[k]) continue;
                        int r = 0, g = 0, b = 0, n = 0;
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                int xx = x + dx, yy = y + dy;
                                if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                                size_t q = (size_t)yy * w + xx;
                                if (!known[q]) continue;
                                r += p[q] & 255; g += p[q] >> 8 & 255; b += p[q] >> 16 & 255; n++;
                            }
                        if (n) { np[k] = 0xFF000000u | (uint32_t)(b / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(r / n); nk[k] = 1; grew = 1; }
                    }
                p.swap(np); known.swap(nk);
                if (!grew) break;
            }
            int W = w + PAD * 2, H = h + PAD * 2;
            std::vector<uint8_t> rgb((size_t)W * H * 3);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    int sx = x - PAD < 0 ? 0 : x - PAD >= w ? w - 1 : x - PAD;
                    int sy = y - PAD < 0 ? 0 : y - PAD >= h ? h - 1 : y - PAD;
                    size_t k = (size_t)sy * w + sx;
                    uint32_t c = known[k] ? p[k] : 0xFF808080u;
                    uint8_t *d = &rgb[((size_t)y * W + x) * 3];
                    d[0] = (uint8_t)c; d[1] = (uint8_t)(c >> 8); d[2] = (uint8_t)(c >> 16);
                }
            stbi_write_png((ti / sheets[i].filename()).string().c_str(), W, H, 3, rgb.data(), W * 3);
        }
        progress(out, done, total, "AI upscaling");
        std::string cmd = quote(exe) + " -i " + quote(ti.string()) + " -o " + quote(to.string()) +
                          " -m " + quote(models) + " -n " + model + " -s 4 -f png";
        int rc = run(cmd);
        int got = 0;
        for (int i = c0; i < c1; i++) {
            fs::path ai = to / sheets[i].filename(), xb = out / sheets[i].filename();
            int aw, ah, ac, xw, xh, xc;
            unsigned char *a = stbi_load(ai.string().c_str(), &aw, &ah, &ac, 3);
            unsigned char *x = a ? stbi_load(xb.string().c_str(), &xw, &xh, &xc, 4) : NULL;
            if (a && x && aw == xw + PAD * 2 * S && ah == xh + PAD * 2 * S) {
                for (int y = 0; y < xh; y++)
                    for (int xx = 0; xx < xw; xx++) {
                        const unsigned char *s3 = &a[((size_t)(y + PAD * S) * aw + xx + PAD * S) * 3];
                        unsigned char *d = &x[((size_t)y * xw + xx) * 4];
                        d[0] = s3[0]; d[1] = s3[1]; d[2] = s3[2];      /* alpha from xBRZ */
                    }
                stbi_write_png(xb.string().c_str(), xw, xh, 4, x, xw * 4);
                replaced++; got++;
            }
            if (a) stbi_image_free(a);
            if (x) stbi_image_free(x);
        }
        done = c1;
        if (got == 0) {                                  /* the program didn't run */
            fprintf(stderr, "AI upscaler failed (exit %d); keeping xBRZ\n", rc);
            progress(out, total, total, "AI upscaler could not run; xBRZ used");
            fs::remove_all(tmp, ec);
            return -1;
        }
    }
    fs::remove_all(tmp, ec);
    return replaced;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <dump dir> <pack dir> [-s N]\n", argv[0]); return 1; }
    int scale = 4;
    double busy = 0.5;
    std::string ai, models, model = "realesrgan-x4plus-anime";
    for (int i = 3; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "-s")) scale = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "-n")) busy = atof(argv[i + 1]);
        if (!strcmp(argv[i], "--ai")) ai = argv[i + 1];
        if (!strcmp(argv[i], "-m")) models = argv[i + 1];
        if (!strcmp(argv[i], "--model")) model = argv[i + 1];
    }
    if (scale < 2 || scale > 6) scale = 4;
    fs::path in = fs::u8path(argv[1]), out = fs::u8path(argv[2]);
    fs::create_directories(out);
    int n = 0, copied = 0, total = 0;
    for (auto &e : fs::directory_iterator(in)) total += e.path().extension() == ".png";
    progress(out, 0, total, "Upscaling with xBRZ");
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
        if (++n % 500 == 0) { printf("%d images...\n", n); progress(out, n, total, "Upscaling with xBRZ"); }
    }
    printf("%d images scaled %dx, %d text files copied into %s\n", n, scale, copied, argv[2]);
    if (!ai.empty() && scale == 4) {
        if (models.empty()) models = (fs::u8path(ai).parent_path() / "models").string();
        int r = ai_pass(in, out, ai, models, model);
        if (r < 0) return 3;                             /* pack is fine, but xBRZ only */
        printf("%d sheets redone with %s\n", r, model.c_str());
    }
    progress(out, 1, 1, "done");
    return 0;
}
