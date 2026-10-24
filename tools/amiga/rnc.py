"""Rob Northen ProPack (RNC) method 1 and 2 decompressor (port of the
algorithm in lab313ru/rnc_propack_source, which allows any use)."""
import struct

def _crc_table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
        t.append(c)
    return t
CRC = _crc_table()

def crc16(data):
    c = 0
    for b in data:
        c = CRC[(c ^ b) & 0xFF] ^ (c >> 8)
    return c

class _M1:
    def __init__(s, src, pos):
        s.src, s.pos, s.buf, s.cnt = src, pos, 0, 0
    def byte(s):
        b = s.src[s.pos] if s.pos < len(s.src) else 0
        s.pos += 1
        return b
    def peek(s, k):
        return s.src[s.pos + k] if s.pos + k < len(s.src) else 0
    def bits(s, n):
        r, p = 0, 1
        for _ in range(n):
            if not s.cnt:
                b1 = s.byte(); b2 = s.byte()
                s.buf = (s.peek(1) << 24) | (s.peek(0) << 16) | (b2 << 8) | b1
                s.cnt = 16
            if s.buf & 1: r |= p
            s.buf >>= 1; p <<= 1; s.cnt -= 1
        return r

def _inv(v, n):
    r = 0
    for _ in range(n):
        r = (r << 1) | (v & 1); v >>= 1
    return r

def _table(b):
    n = b.bits(5)
    t = []
    if n:
        n = min(n, 16)
        depth = [b.bits(4) for _ in range(n)]
        val, div = 0, 0x80000000
        code = [0] * n
        for bc in range(1, 17):
            for i in range(n):
                if depth[i] == bc:
                    code[i] = _inv(val // div, bc); val += div
            div >>= 1
        t = list(zip(depth, code))
    return t

def _decode(b, t):
    for i, (d, c) in enumerate(t):
        if d and c == (b.buf & ((1 << d) - 1)):
            b.bits(d)
            if i < 2: return i
            return b.bits(i - 1) | (1 << (i - 1))
    raise ValueError('bad huffman code')

def unpack_m1(src, pos, size, out):
    b = _M1(src, pos)
    b.bits(1); b.bits(1)                      # lock / key flags
    while len(out) < size:
        raw, ln, ps = _table(b), _table(b), _table(b)
        sub = b.bits(16)
        while sub:
            sub -= 1
            n = _decode(b, raw)
            if n:
                out += src[b.pos:b.pos + n]; b.pos += n
                b.buf = (((b.peek(2) << 16) | (b.peek(1) << 8) | b.peek(0)) << b.cnt) | (b.buf & ((1 << b.cnt) - 1))
            if sub:
                off = _decode(b, ln) + 1
                cnt = _decode(b, ps) + 2
                for _ in range(cnt): out.append(out[-off])
    return out

class _M2:
    def __init__(s, src, pos):
        s.src, s.pos, s.buf, s.cnt = src, pos, 0, 0
    def byte(s):
        b = s.src[s.pos] if s.pos < len(s.src) else 0
        s.pos += 1
        return b
    def bit(s):
        if not s.cnt:
            s.buf = s.byte(); s.cnt = 8
        r = (s.buf >> 7) & 1
        s.buf = (s.buf << 1) & 0xFF; s.cnt -= 1
        return r

def unpack_m2(src, pos, size, out):
    b = _M2(src, pos)
    b.bit(); b.bit()
    def moff():
        o = 0
        if b.bit():
            o = b.bit()
            if b.bit():
                o = ((o << 1) | b.bit()) | 4
                if not b.bit(): o = (o << 1) | b.bit()
            elif not o:
                o = b.bit() + 2
        return ((o << 8) | b.byte()) + 1
    while len(out) < size:
        while True:
            if not b.bit():
                out.append(b.byte()); continue
            if b.bit():
                if b.bit():
                    if b.bit():
                        cnt = b.byte() + 8
                        if cnt == 8:
                            b.bit(); break
                    else:
                        cnt = 3
                    off = moff()
                else:
                    cnt = 2; off = b.byte() + 1
            else:
                cnt = b.bit() + 4
                if b.bit(): cnt = ((cnt - 1) << 1) + b.bit()
                if cnt == 9:
                    n = (b.bits4() if False else (b.bit() << 3 | b.bit() << 2 | b.bit() << 1 | b.bit())) * 4 + 12
                    out += src[b.pos:b.pos + n]; b.pos += n
                    continue
                off = moff()
            for _ in range(cnt): out.append(out[-off])
    return out

def header(d, at):
    if d[at:at+3] != b'RNC' or d[at+3] not in (1, 2): return None
    usize, psize = struct.unpack('>II', d[at+4:at+12])
    ucrc, pcrc = struct.unpack('>HH', d[at+12:at+16])
    return d[at+3], usize, psize, ucrc, pcrc

def unpack(d, at=0):
    h = header(d, at)
    if not h: raise ValueError('not RNC')
    m, usize, psize, ucrc, pcrc = h
    body = d[at+18:at+18+psize]
    if crc16(body) != pcrc: raise ValueError('packed CRC mismatch')
    out = bytearray()
    (unpack_m1 if m == 1 else unpack_m2)(body, 0, usize, out)
    out = bytes(out[:usize])
    if crc16(out) != ucrc: raise ValueError('unpacked CRC mismatch')
    return out
