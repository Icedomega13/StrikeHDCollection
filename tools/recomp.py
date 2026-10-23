"""68000 -> C static recompiler for Desert Strike.

Reads out/codemap.json (from trace.py) and the ROM, and writes
generated/<id>/recomp_*.c plus recomp_table.c. Usage: recomp.py [--game <id>]
Symbols get the game's prefix (ds_f_XXXXXX, ds_recomp_lookup) so several
games can link into one executable.

Model
-----
* Each traced function becomes `void <prefix>f_XXXXXX(uint32_t entry)`. Its body is
  every instruction reachable from the entry without following calls; a
  branch or fallthrough into another function's entry becomes a tail call.
* Every instruction starts with TICK(pc), which advances shared hardware
  time by one instruction and takes the vblank interrupt if due.
* Calls push the real return address on the emulated stack, call the C
  function, then check R.pc (set by RTS) matches. A mismatch means the game
  manipulated its stack, so we unwind to the dispatcher in cpu_run().
* Functions can be entered mid-way (unwinding, interrupts, computed jumps)
  via a switch on `entry`.
"""
import json, os, struct, sys
from collections import deque

sys.path.insert(0, os.path.dirname(__file__))
from m68k import decode, w, sw, l  # noqa: E402
from game_cfg import load_game_rom, OUT_DIR, GEN_DIR, PREFIX, CFG, HINTS_PATH  # noqa: E402

rom = load_game_rom()
cmap = json.load(open(os.path.join(OUT_DIR, "codemap.json")))
# Per-instruction patches from hints.json "patches": {"00B3F4": {"imm": "...", "post": "..."}}
PATCHES = {int(k, 16): v for k, v in json.load(open(HINTS_PATH)).get("patches", {}).items()}
P = PREFIX + "f_"
FUNCS = set(cmap["functions"])
TABLE_SITES = {}
for tbl, t in cmap["tables"].items():
    TABLE_SITES[t["site"]] = (int(tbl, 16), t)

SIZES = {1: ("8", "0xFFu", "B"), 2: ("16", "0xFFFFu", "W"), 4: ("32", "0xFFFFFFFFu", "L")}
CC = ["T", "F", "HI", "LS", "CC", "CS", "NE", "EQ", "VC", "VS", "PL", "MI", "GE", "LT", "GT", "LE"]


class Unsupported(Exception):
    pass


def h(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


# ---------------------------------------------------------------------------
# Effective addresses
# ---------------------------------------------------------------------------
class Cursor:
    def __init__(self, addr):
        self.addr = addr
        self.p = addr + 2
        self.ntmp = 0

    def ext16(self):
        v = w(rom, self.p); self.p += 2; return v

    def ext32(self):
        v = l(rom, self.p); self.p += 4; return v

    def tmp(self, base="ea"):
        self.ntmp += 1
        return f"{base}{self.ntmp}"


class Op:
    """An operand. setup() emits address computation (once); read()/write()."""

    def __init__(self, cx, mode, reg, size):
        self.cx, self.mode, self.reg, self.size = cx, mode, reg, size
        self.var = None
        self.addr_lines = []
        bits, mask, _ = SIZES.get(size, ("32", "0xFFFFFFFFu", "L"))
        self.bits, self.mask = bits, mask
        step = size
        if mode == 0:
            self.kind = "d"
        elif mode == 1:
            self.kind = "a"
        elif mode == 2:
            self.kind = "m"; self._expr = f"R.a[{reg}]"
        elif mode == 3:
            self.kind = "m"
            if reg == 7 and size == 1:
                step = 2
            self._post = (reg, step)
        elif mode == 4:
            self.kind = "m"
            if reg == 7 and size == 1:
                step = 2
            self._pre = (reg, step)
        elif mode == 5:
            self.kind = "m"; d = sw(rom, cx.p); cx.p += 2
            self._expr = f"(R.a[{reg}] + {h(d)})"
        elif mode == 6:
            self.kind = "m"; ext = cx.ext16()
            self._expr = f"(R.a[{reg}] + {h(self._sx8(ext))} + {self._idx(ext)})"
        elif mode == 7:
            if reg == 0:
                self.kind = "m"; self.const = sw(rom, cx.p) & 0xFFFFFFFF; cx.p += 2
                self._expr = h(self.const)
            elif reg == 1:
                self.kind = "m"; self.const = cx.ext32()
                self._expr = h(self.const)
            elif reg == 2:
                self.kind = "m"; base = cx.p; self.const = (base + sw(rom, cx.p)) & 0xFFFFFF; cx.p += 2
                self._expr = h(self.const)
            elif reg == 3:
                self.kind = "m"; base = cx.p; ext = cx.ext16()
                self.pcbase = (base + self._sx8(ext)) & 0xFFFFFF
                self._expr = f"({h(self.pcbase)} + {self._idx(ext)})"
            elif reg == 4:
                self.kind = "i"
                if size == 1:
                    self.imm = cx.ext16() & 0xFF
                elif size == 2:
                    self.imm = cx.ext16()
                else:
                    self.imm = cx.ext32()
            else:
                raise Unsupported(f"ea mode 7/{reg}")

    @staticmethod
    def _sx8(ext):
        d = ext & 0xFF
        return d - 256 if d & 0x80 else d

    @staticmethod
    def _idx(ext):
        r = (ext >> 12) & 7
        reg = f"R.a[{r}]" if ext & 0x8000 else f"R.d[{r}]"
        return reg if ext & 0x800 else f"SX16({reg})"

    def setup(self, out):
        if self.kind != "m" or self.var:
            return
        self.var = self.cx.tmp()
        if hasattr(self, "_post"):
            r, st = self._post
            out.append(f"{self.var} = R.a[{r}]; R.a[{r}] += {st};")
        elif hasattr(self, "_pre"):
            r, st = self._pre
            out.append(f"R.a[{r}] -= {st}; {self.var} = R.a[{r}];")
        else:
            out.append(f"{self.var} = {self._expr};")

    def addr(self, out):
        self.setup(out)
        return self.var

    def read(self, out):
        """Expression for the operand value (zero-extended to its size)."""
        if self.kind == "d":
            return f"(R.d[{self.reg}] & {self.mask})" if self.size != 4 else f"R.d[{self.reg}]"
        if self.kind == "a":
            return f"(R.a[{self.reg}] & {self.mask})" if self.size != 4 else f"R.a[{self.reg}]"
        if self.kind == "i":
            pt = PATCHES.get(self.cx.addr, {})
            if "imm" in pt:
                # runtime-adjustable immediate (e.g. widescreen); equals the
                # original value when the controlling variable is 0
                return f"((uint32_t)({pt['imm']}) & {self.mask})"
            return h(self.imm)
        self.setup(out)
        return f"R{self.bits}({self.var})"

    def write(self, out, expr):
        if self.kind == "d":
            out.append({1: f"SET_DB({self.reg}, {expr});", 2: f"SET_DW({self.reg}, {expr});",
                        4: f"SET_DL({self.reg}, {expr});"}[self.size])
        elif self.kind == "a":
            out.append(f"R.a[{self.reg}] = {expr};")
        elif self.kind == "m":
            self.setup(out)
            out.append(f"W{self.bits}({self.var}, {expr});")
        else:
            raise Unsupported("write to immediate")


def size_bits(s):
    return {0: 1, 1: 2, 2: 4}[s]


# ---------------------------------------------------------------------------
# Instruction semantics (non-control-flow part)
# ---------------------------------------------------------------------------
def gen(addr):
    """Return list of C statements for the data/flag effects of the instruction."""
    cx = Cursor(addr)
    op = w(rom, addr)
    o = []
    line = op >> 12
    my, ry = (op >> 3) & 7, op & 7          # EA field
    rx = (op >> 9) & 7
    ssz = (op >> 6) & 3

    def bits(sz):
        return SIZES[sz][0]

    # ---- line 0: immediates, bit ops ----
    if line == 0:
        if op in (0x003C, 0x023C, 0x0A3C):           # ORI/ANDI/EORI to CCR
            imm = cx.ext16() & 0xFF
            fn = {0x003C: "|", 0x023C: "&", 0x0A3C: "^"}[op]
            o.append(f"cpu_set_ccr((cpu_get_sr() & 0xFF) {fn} {h(imm)});")
            return o, cx
        if op in (0x007C, 0x027C, 0x0A7C):           # ORI/ANDI/EORI to SR
            imm = cx.ext16()
            fn = {0x007C: "|", 0x027C: "&", 0x0A7C: "^"}[op]
            o.append(f"cpu_set_sr(cpu_get_sr() {fn} {h(imm)});")
            return o, cx
        if op & 0x0100:                               # dynamic bit op Dn,<ea>
            if my == 1:
                raise Unsupported("movep")
            kind = (op >> 6) & 3
            dst = Op(cx, my, ry, 4 if my == 0 else 1)
            o.append(bitop(kind, dst, f"R.d[{rx}]", o))
            return o, cx
        if (op & 0x0F00) == 0x0800:                   # static bit op #n,<ea>
            kind = (op >> 6) & 3
            bit = cx.ext16() & 0xFF
            dst = Op(cx, my, ry, 4 if my == 0 else 1)
            o.append(bitop(kind, dst, str(bit), o))
            return o, cx
        sub = (op >> 9) & 7
        if ssz == 3:
            raise Unsupported("line0 size3")
        sz = size_bits(ssz)
        imm = Op(cx, 7, 4, sz)
        dst = Op(cx, my, ry, sz)
        b = bits(sz)
        s = imm.read(o)
        if sub == 6:                                  # CMPI
            o.append(f"CMP{b}({s}, {dst.read(o)});")
            return o, cx
        dst.setup(o)
        d = dst.read(o)
        expr = {0: f"({d} | {s})", 1: f"({d} & {s})", 5: f"({d} ^ {s})",
                2: f"SUB{b}({s}, {d})", 3: f"ADD{b}({s}, {d})"}.get(sub)
        if expr is None:
            raise Unsupported(f"line0 sub {sub}")
        o.append(f"r = {expr};")
        if sub in (0, 1, 5):
            o.append(f"LOGIC{b}(r);")
        dst.write(o, "r")
        return o, cx

    # ---- move ----
    if line in (1, 2, 3):
        sz = {1: 1, 3: 2, 2: 4}[line]
        b = bits(sz)
        src = Op(cx, my, ry, sz)
        dmode = (op >> 6) & 7
        o.append(f"s = {src.read(o)};")
        if dmode == 1:                                # MOVEA
            o.append(f"R.a[{rx}] = {'SX16(s)' if sz == 2 else 's'};")
            return o, cx
        dst = Op(cx, dmode, rx, sz)
        o.append(f"LOGIC{b}(s);")
        dst.write(o, "s")
        return o, cx

    # ---- line 4: misc ----
    if line == 4:
        if op == 0x4E71:
            return o, cx                              # NOP
        if op in (0x4E75, 0x4E73, 0x4E77):
            return o, cx                              # handled as control flow
        if (op & 0xFFF0) == 0x4E60:                   # MOVE An,USP / USP,An
            if op & 8:
                o.append(f"R.a[{ry}] = R.usp;")
            else:
                o.append(f"R.usp = R.a[{ry}];")
            return o, cx
        if (op & 0xFFC0) in (0x4EC0, 0x4E80):
            return o, cx                              # JMP/JSR: control flow
        if (op & 0xFFF8) == 0x4840:                   # SWAP
            o.append(f"R.d[{ry}] = (R.d[{ry}] >> 16) | (R.d[{ry}] << 16); LOGIC32(R.d[{ry}]);")
            return o, cx
        if (op & 0xFFF8) == 0x4880:                   # EXT.W
            o.append(f"SET_DW({ry}, SX8(R.d[{ry}])); LOGIC16(R.d[{ry}]);")
            return o, cx
        if (op & 0xFFF8) == 0x48C0:                   # EXT.L
            o.append(f"R.d[{ry}] = SX16(R.d[{ry}]); LOGIC32(R.d[{ry}]);")
            return o, cx
        if (op & 0xFB80) == 0x4880:                   # MOVEM
            return movem(cx, op, o)
        if (op & 0xF1C0) == 0x41C0:                   # LEA
            src = Op(cx, my, ry, 4)
            o.append(f"R.a[{rx}] = {src.addr(o)};")
            return o, cx
        if (op & 0xFFC0) == 0x4840:                   # PEA
            src = Op(cx, my, ry, 4)
            o.append(f"s = {src.addr(o)}; PUSH32(s);")
            return o, cx
        if (op & 0xFFC0) == 0x40C0:                   # MOVE from SR
            dst = Op(cx, my, ry, 2)
            dst.write(o, "cpu_get_sr()")
            return o, cx
        if (op & 0xFFC0) == 0x44C0:                   # MOVE to CCR
            src = Op(cx, my, ry, 2)
            o.append(f"cpu_set_ccr({src.read(o)});")
            return o, cx
        if (op & 0xFFC0) == 0x46C0:                   # MOVE to SR
            src = Op(cx, my, ry, 2)
            o.append(f"cpu_set_sr({src.read(o)});")
            return o, cx
        if (op & 0xFF00) in (0x4000, 0x4200, 0x4400, 0x4600, 0x4A00) and ssz != 3:
            sz = size_bits(ssz); b = bits(sz)
            dst = Op(cx, my, ry, sz)
            kind = op & 0xFF00
            if kind == 0x4A00:                        # TST
                o.append(f"LOGIC{b}({dst.read(o)});")
            elif kind == 0x4200:                      # CLR (no read, as Musashi)
                o.append(f"R.n = 0; R.z = 1; R.v = 0; R.c = 0;")
                dst.write(o, "0")
            else:
                dst.setup(o)
                d = dst.read(o)
                if kind == 0x4400:
                    o.append(f"r = NEG{b}({d});")
                elif kind == 0x4000:
                    o.append(f"r = NEGX{b}({d});")
                else:
                    o.append(f"r = ~{d} & {SIZES[sz][1]}; LOGIC{b}(r);")
                dst.write(o, "r")
            return o, cx
        if (op & 0xFFC0) == 0x4AC0:                   # TAS
            dst = Op(cx, my, ry, 1); dst.setup(o)
            o.append(f"r = {dst.read(o)}; LOGIC8(r);")
            dst.write(o, "r | 0x80")
            return o, cx
        if (op & 0xFFF8) == 0x4E50 or (op & 0xFFF8) == 0x4E58:
            raise Unsupported("link/unlk")
        raise Unsupported(f"line4 {op:04X}")

    # ---- line 5: addq/subq/scc/dbcc ----
    if line == 5:
        if ssz == 3:
            if my == 1:
                return o, cx                          # DBcc: control flow
            dst = Op(cx, my, ry, 1)
            cond = CC[(op >> 8) & 15]
            dst.write(o, f"(CC_{cond} ? 0xFF : 0)")
            return o, cx
        q = rx or 8
        sz = size_bits(ssz); b = bits(sz)
        dst = Op(cx, my, ry, sz)
        if my == 1:                                   # to An: whole register, no flags
            sign = "-" if op & 0x100 else "+"
            o.append(f"R.a[{ry}] {sign}= {q};")
            return o, cx
        dst.setup(o)
        fn = "SUB" if op & 0x100 else "ADD"
        o.append(f"r = {fn}{b}({q}, {dst.read(o)});")
        dst.write(o, "r")
        return o, cx

    if line == 6:
        return o, cx                                  # Bcc/BRA/BSR

    if line == 7:                                     # MOVEQ
        v = op & 0xFF
        v = v - 256 if v & 0x80 else v
        o.append(f"R.d[{rx}] = {h(v)}; LOGIC32(R.d[{rx}]);")
        return o, cx

    # ---- line 8: OR / DIV ----
    if line == 8:
        if ssz == 3:
            src = Op(cx, my, ry, 2)
            fn = "DIVS" if op & 0x100 else "DIVU"
            o.append(f"{fn}({rx}, {src.read(o)}, {h(addr)});")
            return o, cx
        if (op & 0x1F0) == 0x100:
            raise Unsupported("sbcd")
        return logic_op(cx, op, o, "|")

    # ---- line 9 / D: SUB / ADD ----
    if line in (9, 0xD):
        fn = "SUB" if line == 9 else "ADD"
        if ssz == 3:                                  # SUBA/ADDA
            sz = 4 if op & 0x100 else 2
            src = Op(cx, my, ry, sz)
            s = src.read(o)
            if sz == 2:
                s = f"SX16({s})"
            sign = "-" if line == 9 else "+"
            o.append(f"R.a[{rx}] {sign}= {s};")
            return o, cx
        sz = size_bits(ssz); b = bits(sz)
        if (op & 0x130) == 0x100:                     # ADDX/SUBX
            if op & 8:                                # -(Ay),-(Ax)
                src = Op(cx, 4, ry, sz); dst = Op(cx, 4, rx, sz)
                o.append(f"s = {src.read(o)};")
                dst.setup(o)
                o.append(f"r = {fn}X{b}(s, {dst.read(o)});")
                dst.write(o, "r")
            else:
                o.append(f"r = {fn}X{b}(R.d[{ry}], R.d[{rx}]);")
                Op(cx, 0, rx, sz).write(o, "r")
            return o, cx
        if op & 0x100:                                # Dn op <ea> -> <ea>
            dst = Op(cx, my, ry, sz); dst.setup(o)
            o.append(f"r = {fn}{b}(R.d[{rx}], {dst.read(o)});")
            dst.write(o, "r")
        else:                                         # <ea> op Dn -> Dn
            src = Op(cx, my, ry, sz)
            o.append(f"s = {src.read(o)};")
            o.append(f"r = {fn}{b}(s, R.d[{rx}]);")
            Op(cx, 0, rx, sz).write(o, "r")
        return o, cx

    # ---- line B: CMP / CMPA / EOR / CMPM ----
    if line == 0xB:
        if ssz == 3:                                  # CMPA
            sz = 4 if op & 0x100 else 2
            src = Op(cx, my, ry, sz)
            s = src.read(o)
            if sz == 2:
                s = f"SX16({s})"
            o.append(f"CMP32({s}, R.a[{rx}]);")
            return o, cx
        sz = size_bits(ssz); b = bits(sz)
        if op & 0x100:
            if my == 1:                               # CMPM (Ay)+,(Ax)+
                src = Op(cx, 3, ry, sz); dst = Op(cx, 3, rx, sz)
                o.append(f"s = {src.read(o)};")
                o.append(f"CMP{b}(s, {dst.read(o)});")
                return o, cx
            dst = Op(cx, my, ry, sz); dst.setup(o)    # EOR Dn,<ea>
            o.append(f"r = (R.d[{rx}] ^ {dst.read(o)}) & {SIZES[sz][1]}; LOGIC{b}(r);")
            dst.write(o, "r")
            return o, cx
        src = Op(cx, my, ry, sz)
        o.append(f"CMP{b}({src.read(o)}, R.d[{rx}]);")
        return o, cx

    # ---- line C: AND / MUL / EXG ----
    if line == 0xC:
        if ssz == 3:
            src = Op(cx, my, ry, 2)
            if op & 0x100:
                o.append(f"R.d[{rx}] = (uint32_t)((int32_t)(int16_t){src.read(o)} * (int32_t)(int16_t)R.d[{rx}]);")
            else:
                o.append(f"R.d[{rx}] = (uint32_t)({src.read(o)}) * (R.d[{rx}] & 0xFFFFu);")
            o.append(f"LOGIC32(R.d[{rx}]);")
            return o, cx
        if (op & 0x1F8) in (0x140, 0x148, 0x188):     # EXG
            mode = op & 0x1F8
            if mode == 0x140:
                a, b2 = f"R.d[{rx}]", f"R.d[{ry}]"
            elif mode == 0x148:
                a, b2 = f"R.a[{rx}]", f"R.a[{ry}]"
            else:
                a, b2 = f"R.d[{rx}]", f"R.a[{ry}]"
            o.append(f"s = {a}; {a} = {b2}; {b2} = s;")
            return o, cx
        if (op & 0x1F0) == 0x100:
            raise Unsupported("abcd")
        return logic_op(cx, op, o, "&")

    # ---- line E: shifts / rotates ----
    if line == 0xE:
        names = ["AS", "LS", "ROX", "RO"]
        dirn = "L" if op & 0x100 else "R"
        if ssz == 3:                                  # memory, by 1
            kind = names[(op >> 9) & 3]
            dst = Op(cx, my, ry, 2); dst.setup(o)
            o.append(f"r = {kind}{dirn}16({dst.read(o)}, 1);")
            dst.write(o, "r")
            return o, cx
        sz = size_bits(ssz); b = bits(sz)
        kind = names[(op >> 3) & 3]
        if op & 0x20:
            cnt = f"(int)(R.d[{rx}] & 63)"
        else:
            cnt = str(rx or 8)
        o.append(f"r = {kind}{dirn}{b}(R.d[{ry}], {cnt});")
        Op(cx, 0, ry, sz).write(o, "r")
        return o, cx

    raise Unsupported(f"opcode {op:04X}")


def bitop(kind, dst, bitexpr, o):
    """kind 0=btst 1=bchg 2=bclr 3=bset. dst size 4 for Dn (mod 32) else byte (mod 8)."""
    m = 31 if dst.kind == "d" else 7
    bexpr = f"(1u << (({bitexpr}) & {m}))"
    if dst.kind == "d":
        reg = f"R.d[{dst.reg}]"
        s = f"R.z = ({reg} & {bexpr}) == 0;"
        if kind == 1:
            s += f" {reg} ^= {bexpr};"
        elif kind == 2:
            s += f" {reg} &= ~{bexpr};"
        elif kind == 3:
            s += f" {reg} |= {bexpr};"
        return s
    dst.setup(o)
    val = dst.read(o)
    if dst.kind == "i":
        return f"R.z = ({val} & {bexpr}) == 0;"
    s = f"s = {val}; R.z = (s & {bexpr}) == 0;"
    if kind == 1:
        s += f" W8({dst.var}, s ^ {bexpr});"
    elif kind == 2:
        s += f" W8({dst.var}, s & ~{bexpr});"
    elif kind == 3:
        s += f" W8({dst.var}, s | {bexpr});"
    return s


def logic_op(cx, op, o, sym):
    my, ry, rx, ssz = (op >> 3) & 7, op & 7, (op >> 9) & 7, (op >> 6) & 3
    sz = size_bits(ssz); b = SIZES[sz][0]
    if op & 0x100:                                    # Dn op <ea> -> <ea>
        dst = Op(cx, my, ry, sz); dst.setup(o)
        o.append(f"r = (R.d[{rx}] {sym} {dst.read(o)}) & {SIZES[sz][1]}; LOGIC{b}(r);")
        dst.write(o, "r")
    else:
        src = Op(cx, my, ry, sz)
        o.append(f"r = ({src.read(o)} {sym} R.d[{rx}]) & {SIZES[sz][1]}; LOGIC{b}(r);")
        Op(cx, 0, rx, sz).write(o, "r")
    return o, cx


def movem(cx, op, o):
    to_reg = bool(op & 0x400)
    sz = 4 if op & 0x40 else 2
    mask = cx.ext16()
    my, ry = (op >> 3) & 7, op & 7
    regs = [f"R.d[{i}]" for i in range(8)] + [f"R.a[{i}]" for i in range(8)]
    if not to_reg and my == 4:                        # predecrement: mask reversed, a7..d0
        o.append(f"ea1 = R.a[{ry}];")
        for i in range(15, -1, -1):
            if mask & (1 << (15 - i)):
                o.append(f"ea1 -= {sz}; W{sz*8}(ea1, {regs[i]});")
        o.append(f"R.a[{ry}] = ea1;")
        return o, cx
    if my == 3:
        o.append(f"ea1 = R.a[{ry}];")
    else:
        src = Op(cx, my, ry, sz)
        o.append(f"ea1 = {src.addr(o)};")
    for i in range(16):
        if mask & (1 << i):
            if to_reg:
                v = f"R16(ea1)" if sz == 2 else "R32(ea1)"
                if sz == 2:
                    v = f"SX16({v})"
                o.append(f"{regs[i]} = {v}; ea1 += {sz};")
            else:
                o.append(f"W{sz*8}(ea1, {regs[i]}); ea1 += {sz};")
    if my == 3:
        o.append(f"R.a[{ry}] = ea1;")
    return o, cx


# ---------------------------------------------------------------------------
# Control flow and function assembly
# ---------------------------------------------------------------------------
insn_cache = {}


def ins_at(a):
    if a not in insn_cache:
        insn_cache[a] = decode(rom, a)
    return insn_cache[a]


def table_entries(site):
    """Addresses a computed jump at `site` can land on."""
    if site not in TABLE_SITES:
        return None
    tbl, t = TABLE_SITES[site]
    if t["kind"] == "bra_table":
        out, a = [], tbl
        for _ in t["targets"]:
            out.append(a)
            a += ins_at(a).size
        return out
    return list(t["targets"])


def successors(i):
    """Intra-procedural successors (excluding call targets)."""
    out = []
    if i.kind in ("normal", "call", "call_ind", "cbranch"):
        out.append(i.addr + i.size)
    if i.kind in ("branch", "cbranch") and i.target is not None and i.target < 0xFF0000:
        out.append(i.target)
    if i.kind == "jump_ind":
        out += table_entries(i.addr) or []
    return out


def function_body(entry):
    body = {}
    q = deque([entry])
    while q:
        a = q.popleft()
        if a in body or (a != entry and a in FUNCS) or a >= 0xFF0000:
            continue
        i = ins_at(a)
        if i is None:
            raise Unsupported(f"bad decode at {a:06X} in {entry:06X}")
        body[a] = i
        for s in successors(i):
            q.append(s)
    return body


def goto_code(t, body):
    if t in body:
        return f"goto L_{t:06X};"
    if t in FUNCS:
        return f"{{ {P}{t:06X}({h(t)}); return; }}"
    return f"{{ cpu_dispatch({h(t)}); return; }}"


def emit_function(entry):
    body = function_body(entry)
    addrs = sorted(body)
    # Every instruction is an entry point. The host stops the game at frame
    # boundaries by unwinding the C stack and later resumes at the exact
    # instruction (also how save states restore), so any address must be
    # dispatchable.
    entries = set(addrs) | {entry}
    for a in addrs:
        i = body[a]
        for s in successors(i):
            if s in body and s != i.addr + i.size:
                entries.add(s)
        if i.kind in ("call", "call_ind"):
            entries.add(a + i.size)
        if i.kind == "jump_ind":
            for t in table_entries(a) or []:
                if t in body:
                    entries.add(t)
    lines = [f"void {P}{entry:06X}(uint32_t entry)", "{",
             "    uint32_t s, r, ea1, ea2, ea3; (void)s; (void)r; (void)ea1; (void)ea2; (void)ea3;",
             "    switch (entry) {"]
    for e in sorted(entries):
        lines.append(f"    case {h(e)}: goto L_{e:06X};")
    lines.append(f"    default: cpu_fatal(\"bad entry\", entry); return;")
    lines.append("    }")
    errors = []
    for idx, a in enumerate(addrs):
        i = body[a]
        lines.append(f"L_{a:06X}: TICK({h(a)}); /* {i.mnem} {i.ops} */")
        try:
            stmts, cx = gen(a)
        except Unsupported as e:
            errors.append((a, str(e)))
            lines.append(f"    cpu_fatal(\"unsupported: {e}\", {h(a)}); return;")
            continue
        if stmts:
            lines.append("    { " + " ".join(stmts) + " }")
        if "post" in PATCHES.get(a, {}):
            lines.append("    { " + PATCHES[a]["post"] + " } /* patch */")
        if cx.p != a + i.size and i.kind not in ("branch", "cbranch", "call", "jump_ind", "call_ind", "ret") \
                and w(rom, a) != 0x4E71:
            errors.append((a, f"size mismatch {cx.p - a} vs {i.size}"))
        ft = a + i.size
        nxt = addrs[idx + 1] if idx + 1 < len(addrs) else None
        op = w(rom, a)
        k = i.kind
        if k == "ret":
            if op == 0x4E73:
                lines.append("    { uint32_t sr = POP16(); R.pc = POP32(); cpu_set_sr(sr); return; }")
            elif op == 0x4E77:
                lines.append("    { cpu_set_ccr(POP16()); R.pc = POP32(); return; }")
            else:
                lines.append("    R.pc = POP32(); return;")
            continue
        if k == "stop":
            lines.append(f"    cpu_fatal(\"stop/illegal\", {h(a)}); return;")
            continue
        if k == "branch":
            lines.append("    " + goto_code(i.target, body))
            continue
        if k == "cbranch":
            if (op & 0xF0F8) == 0x50C8:               # DBcc
                n = op & 7
                cond = CC[(op >> 8) & 15]
                lines.append(f"    if (!CC_{cond}) {{ uint32_t c_ = (R.d[{n}] - 1) & 0xFFFFu; SET_DW({n}, c_);"
                             f" if (c_ != 0xFFFFu) {goto_code(i.target, body)} }}")
            else:
                cond = CC[(op >> 8) & 15]
                lines.append(f"    if (CC_{cond}) {goto_code(i.target, body)}")
        elif k == "call":
            t = i.target
            call = f"{P}{t:06X}({h(t)})" if t in FUNCS else f"cpu_dispatch({h(t)})"
            lines.append(f"    PUSH32({h(ft)}); {call}; if (R.pc != {h(ft)}) return;")
        elif k in ("call_ind", "jump_ind"):
            mode, reg = i.ea
            cx2 = Cursor(a)
            tmp = []
            opnd = Op(cx2, mode, reg, 4)
            tgt = opnd.addr(tmp)
            pre = " ".join(tmp)
            if k == "call_ind":
                lines.append(f"    {{ {pre} uint32_t t_ = {tgt} & 0xFFFFFFu; PUSH32({h(ft)}); cpu_dispatch(t_);"
                             f" if (R.pc != {h(ft)}) return; }}")
            else:
                ents = sorted({t for t in (table_entries(a) or []) if t in body})
                cases = " ".join(f"case {h(t)}: goto L_{t:06X};" for t in ents)
                lines.append(f"    {{ {pre} uint32_t t_ = {tgt} & 0xFFFFFFu; switch (t_) {{ {cases}"
                             f" default: cpu_dispatch(t_); return; }} }}")
                continue
        # fallthrough
        if nxt != ft:
            lines.append("    " + goto_code(ft, body))
    lines.append("}")
    return lines, body, entries, errors


def main():
    outdir = GEN_DIR
    os.makedirs(outdir, exist_ok=True)
    funcs = sorted(FUNCS)
    all_entries = {}
    errors = []
    chunks = [[]]
    total_lines = 0
    for f in funcs:
        lines, body, entries, errs = emit_function(f)
        errors += errs
        for e in entries:
            all_entries.setdefault(e, f)
        chunks[-1].append(lines)
        total_lines += len(lines)
        if sum(len(x) for x in chunks[-1]) > 20000:
            chunks.append([])
    hdr = ['#include "runtime/cpu.h"', '#include "recomp_decls.h"', ""]
    for n, ch in enumerate(chunks):
        with open(os.path.join(outdir, f"recomp_{n}.c"), "w") as fo:
            fo.write("\n".join(hdr) + "\n")
            for fl in ch:
                fo.write("\n".join(fl) + "\n\n")
    with open(os.path.join(outdir, "recomp_decls.h"), "w") as fo:
        fo.write("#pragma once\n#include <stdint.h>\n")
        for f in funcs:
            fo.write(f"void {P}{f:06X}(uint32_t entry);\n")
    with open(os.path.join(outdir, "recomp_table.c"), "w") as fo:
        fo.write('#include "runtime/cpu.h"\n#include "recomp_decls.h"\n\n')
        fo.write("static const struct { uint32_t pc; recomp_fn fn; } tbl[] = {\n")
        for e in sorted(all_entries):
            fo.write(f"    {{ {h(e)}, {P}{all_entries[e]:06X} }},\n")
        fo.write(f"}};\n\nrecomp_fn {PREFIX}recomp_lookup(uint32_t pc)\n{{\n"
                 "    int lo = 0, hi = (int)(sizeof tbl / sizeof tbl[0]) - 1;\n"
                 "    while (lo <= hi) { int m = (lo + hi) / 2;\n"
                 "        if (tbl[m].pc == pc) return tbl[m].fn;\n"
                 "        if (tbl[m].pc < pc) lo = m + 1; else hi = m - 1; }\n"
                 "    return 0;\n}\n")
    print(f"{len(funcs)} functions, {len(all_entries)} entry points, {total_lines} lines, "
          f"{len(chunks)} files, {len(errors)} issues")
    for a, e in errors[:40]:
        print(f"  {a:06X}: {e}")


if __name__ == "__main__":
    main()
