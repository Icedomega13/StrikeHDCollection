"""List / extract files from an AmigaDOS (OFS/FFS) ADF image."""
import struct, sys, os
def be32(b, o): return struct.unpack('>I', b[o:o+4])[0]
def main(path, out=None):
    d = open(path, 'rb').read()
    boot = d[:4]
    print('boot', boot, 'flags', d[3])
    ffs = d[3] & 1
    root = 880
    rb = d[root*512:(root+1)*512]
    if be32(rb, 0) != 2:
        print('no AmigaDOS root block; custom disk'); return
    name = rb[432+1:432+1+rb[432]].decode('latin1')
    print('volume', name, 'FFS' if ffs else 'OFS')
    def walk(blk, prefix):
        b = d[blk*512:(blk+1)*512]
        for i in range(72):
            h = be32(b, 24 + i*4)
            while h:
                e = d[h*512:(h+1)*512]
                nm = e[432+1:432+1+e[432]].decode('latin1')
                sec = struct.unpack('>i', e[508:512])[0]
                full = prefix + nm
                if sec == 2:
                    print('DIR ', full); walk(h, full + '/')
                else:
                    size = be32(e, 324)
                    print('FILE', full, size)
                    if out:
                        data = read_file(h, size)
                        p = os.path.join(out, full); os.makedirs(os.path.dirname(p) or out, exist_ok=True)
                        open(p, 'wb').write(data)
                h = be32(e, 496)
    def read_file(hdr, size):
        res = b''
        blk = hdr
        while blk:
            e = d[blk*512:(blk+1)*512]
            n = be32(e, 8)
            ptrs = [be32(e, 308 - 4*i) for i in range(n)]
            for p in ptrs:
                if ffs: res += d[p*512:(p+1)*512]
                else:
                    db = d[p*512:(p+1)*512]
                    res += db[24:24+be32(db, 12)]
            blk = be32(e, 504)
        return res[:size]
    walk(root, '')
if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
