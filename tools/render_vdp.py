"""Render a harness VDP dump (regs, VRAM, CRAM, VSRAM) to PNG.

Handles H32/H40, planes A/B with per-screen scrolling, the window plane is
ignored, sprites with priority ordering by link list. Good enough to check
what is on screen; the real renderer lives in the C runtime.
"""
import struct, sys
from PIL import Image


def load(path):
    d = open(path, "rb").read()
    regs = d[:32]
    vram = d[32:32 + 0x10000]
    cram = struct.unpack(">64H", d[32 + 0x10000:32 + 0x10000 + 128])
    vsram = struct.unpack(">40H", d[32 + 0x10000 + 128:32 + 0x10000 + 208])
    return regs, vram, cram, vsram


def color(c):
    r = (c >> 1) & 7; g = (c >> 5) & 7; b = (c >> 9) & 7
    return (r * 36, g * 36, b * 36)


def tile_pixel(vram, tile, x, y, hf, vf):
    if hf: x = 7 - x
    if vf: y = 7 - y
    b = vram[(tile * 32 + y * 4 + (x >> 1)) & 0xFFFF]
    return (b >> 4) if not (x & 1) else (b & 15)


def render(path, out):
    regs, vram, cram, vsram = load(path)
    pal = [color(c) for c in cram]
    h40 = regs[12] & 0x81
    W = 320 if h40 else 256
    H = 224
    sz = regs[16]
    pw = [32, 64, 32, 128][sz & 3]
    ph = [32, 64, 32, 128][(sz >> 4) & 3]
    na = (regs[2] & 0x38) << 10
    nb = (regs[4] & 7) << 13
    hs = (regs[13] & 0x3F) << 10
    sat = (regs[5] & (0x7E if h40 else 0x7F)) << 9
    bg = pal[regs[7] & 0x3F]
    img = Image.new("RGB", (W, H), bg)
    px = img.load()
    hmode = regs[11] & 3
    vmode = (regs[11] >> 2) & 1
    layers = [[[None] * W for _ in range(H)] for _ in range(4)]  # B lo, A lo, B hi, A hi

    for li, base in ((0, nb), (1, na)):
        for y in range(H):
            if hmode == 0:
                hsc = struct.unpack_from(">H", vram, hs + li * 2 ^ 2)[0] if False else \
                    struct.unpack_from(">H", vram, hs + (2 if li == 0 else 0))[0]
            elif hmode == 2:
                hsc = struct.unpack_from(">H", vram, hs + (y & ~7) * 4 + (2 if li == 0 else 0))[0]
            else:
                hsc = struct.unpack_from(">H", vram, hs + y * 4 + (2 if li == 0 else 0))[0]
            hsc &= 0x3FF
            for x in range(W):
                vs = vsram[(x // 16) * 2 + li] if vmode else vsram[li]
                sy = (y + vs) % (ph * 8)
                sx = (x - hsc) % (pw * 8)
                ent = struct.unpack_from(">H", vram, (base + ((sy // 8) * pw + sx // 8) * 2) & 0xFFFF)[0]
                c = tile_pixel(vram, ent & 0x7FF, sx & 7, sy & 7, ent & 0x800, ent & 0x1000)
                if c:
                    pri = 1 if ent & 0x8000 else 0
                    layers[li + 2 * pri][y][x] = pal[((ent >> 13) & 3) * 16 + c]

    # sprites
    spr = []
    link = 0
    for _ in range(80):
        e = sat + link * 8
        y, s, t, x = struct.unpack_from(">HHHH", vram, e & 0xFFFF)
        spr.append((y & 0x3FF, s, t, x & 0x1FF))
        link = s & 0x7F
        if link == 0:
            break
    sprite_layer = [[None] * W for _ in range(H)]
    sprite_pri = [[0] * W for _ in range(H)]
    for (sy, s, t, sx) in spr:
        w = ((s >> 10) & 3) + 1; h = ((s >> 8) & 3) + 1
        hf = t & 0x800; vf = t & 0x1000; pri = 1 if t & 0x8000 else 0
        for ty in range(h * 8):
            yy = sy - 128 + ty
            if not 0 <= yy < H: continue
            for tx in range(w * 8):
                xx = sx - 128 + tx
                if not 0 <= xx < W or sprite_layer[yy][xx] is not None: continue
                cx = (w * 8 - 1 - tx) if hf else tx
                cy = (h * 8 - 1 - ty) if vf else ty
                tile = (t & 0x7FF) + (cx // 8) * h + cy // 8
                c = tile_pixel(vram, tile, cx & 7, cy & 7, 0, 0)
                if c:
                    sprite_layer[yy][xx] = pal[((t >> 13) & 3) * 16 + c]
                    sprite_pri[yy][xx] = pri

    for y in range(H):
        for x in range(W):
            c = None
            sp = sprite_layer[y][x]
            for cand in (sp if sp and sprite_pri[y][x] else None, layers[3][y][x], layers[2][y][x],
                         sp if sp and not sprite_pri[y][x] else None, layers[1][y][x], layers[0][y][x]):
                if cand is not None:
                    c = cand
                    break
            if c: px[x, y] = c
    img = img.resize((W * 2, H * 2), Image.NEAREST)
    img.save(out)


if __name__ == "__main__":
    render(sys.argv[1], sys.argv[2])
