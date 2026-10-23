#!/usr/bin/env python3
"""Make a starter HD pack from a graphics dump (Settings > System > Dump Graphics).

Upscales every dumped pattern and sprite sheet 4x with Scale2x applied twice
(EPX), which rounds diagonal edges without inventing colours. It is a
starting point for hand-made art, and a test pack for the HD renderer.

  python tools/hdpack_upscale.py <dump dir> <pack dir>
e.g. %APPDATA%/DesertStrikeHD/DesertStrikeHD/hd_dump/desert -> .../packs/desert
"""
import os, shutil, sys
from PIL import Image

def scale2x(im):
    w, h = im.size
    src = im.load()
    out = Image.new('RGBA', (w * 2, h * 2))
    dst = out.load()
    for y in range(h):
        for x in range(w):
            p = src[x, y]
            a = src[x, max(y - 1, 0)]; b = src[min(x + 1, w - 1), y]
            c = src[max(x - 1, 0), y]; d = src[x, min(y + 1, h - 1)]
            e0 = c if (c == a and c != d and a != b) else p
            e1 = a if (a == b and a != c and b != d) else p
            e2 = c if (d == c and d != b and c != a) else p
            e3 = b if (b == d and b != a and d != c) else p
            dst[2 * x, 2 * y] = e0; dst[2 * x + 1, 2 * y] = e1
            dst[2 * x, 2 * y + 1] = e2; dst[2 * x + 1, 2 * y + 1] = e3
    return out

def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    n = 0
    for name in sorted(os.listdir(src)):
        p = os.path.join(src, name)
        if name.lower().endswith('.png'):
            im = Image.open(p).convert('RGBA')
            scale2x(scale2x(im)).save(os.path.join(dst, name))
            n += 1
        elif name.lower().endswith('.txt'):
            shutil.copy(p, os.path.join(dst, name))
    print(f'{n} images upscaled into {dst}')

if __name__ == '__main__':
    main()
