"""Decode Amiga IFF ILBM pictures and ANIM (op 5) animations to RGB frames."""
import struct
from PIL import Image

def chunks(d, start, end):
    p = start
    while p + 8 <= end:
        cid = d[p:p+4]; n = struct.unpack('>I', d[p+4:p+8])[0]
        yield cid, p + 8, n
        p += 8 + n + (n & 1)

def byterun1(src, size):
    out = bytearray(); i = 0
    while len(out) < size and i < len(src):
        n = src[i]; i += 1
        if n < 128: out += src[i:i+n+1]; i += n + 1
        elif n > 128: out += bytes([src[i]]) * (257 - n); i += 1
    return bytes(out[:size])

class Frame:
    def __init__(s, w, h, depth, cmap, ham=False, ehb=False):
        s.w, s.h, s.depth, s.cmap, s.ham, s.ehb = w, h, depth, cmap, ham, ehb
        s.rb = ((w + 15) // 16) * 2
        s.planes = [bytearray(s.rb * h) for _ in range(depth)]
    def load_body(s, body, comp, masking):
        np = s.depth + (1 if masking == 1 else 0)
        raw = byterun1(body, s.rb * s.h * np) if comp == 1 else body
        for y in range(s.h):
            for p in range(np):
                o = (y * np + p) * s.rb
                if p < s.depth: s.planes[p][y*s.rb:(y+1)*s.rb] = raw[o:o+s.rb]
    def image(s):
        pal = list(s.cmap)
        if s.ehb: pal += [(r//2, g//2, b//2) for r, g, b in pal[:32]]
        while len(pal) < 256: pal.append((0, 0, 0))
        idx = bytearray(s.w * s.h)
        for y in range(s.h):
            for x in range(s.w):
                byte, bit = x >> 3, 7 - (x & 7)
                v = 0
                for p in range(s.depth):
                    v |= ((s.planes[p][y*s.rb+byte] >> bit) & 1) << p
                idx[y*s.w+x] = v
        if s.ham:
            img = Image.new('RGB', (s.w, s.h)); px = img.load()
            for y in range(s.h):
                r, g, b = pal[0]
                for x in range(s.w):
                    v = idx[y*s.w+x]; c, val = v >> 4, v & 15
                    if c == 0: r, g, b = pal[val]
                    elif c == 1: b = val * 17
                    elif c == 2: r = val * 17
                    else: g = val * 17
                    px[x, y] = (r, g, b)
            return img
        img = Image.frombytes('P', (s.w, s.h), bytes(idx))
        img.putpalette([c for rgb in pal[:256] for c in rgb])
        return img.convert('RGB')

def parse_ilbm(d, p, end, prev=None):
    bmhd = cmap = body = None; camg = 0; comp = masking = 0
    for cid, o, n in chunks(d, p, end):
        if cid == b'BMHD':
            w, h, _, _, depth, masking, comp = struct.unpack('>HHhhBBB', d[o:o+11])
            bmhd = (w, h, depth)
        elif cid == b'CMAP': cmap = [tuple(d[o+i:o+i+3]) for i in range(0, n - 2, 3)]
        elif cid == b'CAMG': camg = struct.unpack('>I', d[o:o+4])[0]
        elif cid == b'BODY': body = d[o:o+n]
    if not bmhd: return None
    w, h, depth = bmhd
    f = Frame(w, h, depth, cmap or [(i*17,)*3 for i in range(16)], bool(camg & 0x800), bool(camg & 0x80))
    if body is not None: f.load_body(body, comp, masking)
    return f

def delta5(f, data):
    """ANIM op 5: byte vertical delta, one column list per plane."""
    ptrs = struct.unpack('>16I', data[:64])
    for p in range(f.depth):
        if not ptrs[p]: continue
        o = ptrs[p]; plane = f.planes[p]
        for col in range(f.rb):
            ops = data[o]; o += 1; y = 0
            for _ in range(ops):
                op = data[o]; o += 1
                if op == 0:
                    cnt = data[o]; val = data[o+1]; o += 2
                    for _ in range(cnt): plane[y*f.rb+col] = val; y += 1
                elif op < 128: y += op
                else:
                    for _ in range(op & 0x7F): plane[y*f.rb+col] = data[o]; o += 1; y += 1

def decode(d):
    """Returns a list of PIL images (one for ILBM, all frames for ANIM)."""
    if d[:4] != b'FORM': return []
    kind = d[8:12]; end = 8 + struct.unpack('>I', d[4:8])[0]
    if kind == b'ILBM':
        f = parse_ilbm(d, 12, end); return [f.image()] if f else []
    if kind != b'ANIM': return []
    frames = []; bufs = []; cur = None
    for cid, o, n in chunks(d, 12, end):
        if cid != b'FORM' or d[o:o+4] != b'ILBM': continue
        sub_end = o + n
        anhd = dlta = None
        for c2, o2, n2 in chunks(d, o + 4, sub_end):
            if c2 == b'ANHD': anhd = d[o2:o2+n2]
            elif c2 == b'DLTA': dlta = d[o2:o2+n2]
        if cur is None:
            cur = parse_ilbm(d, o + 4, sub_end)
            bufs = [cur, copy(cur)]
            frames.append(cur.image()); continue
        if anhd and dlta is not None and anhd[0] == 5:
            tgt = bufs[1]                     # double buffered: delta applies to frame n-2
            delta5(tgt, dlta)
            bufs = [tgt, bufs[0]]
            frames.append(tgt.image())
    return frames

def copy(f):
    g = Frame(f.w, f.h, f.depth, f.cmap, f.ham, f.ehb)
    g.planes = [bytearray(p) for p in f.planes]
    return g
