"""Minimal 68000 helpers shared by the analysis tools.

Capstone gives us instruction length and text; control-flow targets are
decoded directly from opcode bits, which is simpler and exact for the 68000.
"""
import os, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "vendor"))
import capstone as cs  # noqa: E402

ROM_PATH = os.path.join(os.path.dirname(__file__), "..", "rom", "desert_strike.bin")


def load_rom(path=ROM_PATH):
    with open(path, "rb") as f:
        return f.read()


_md = cs.Cs(cs.CS_ARCH_M68K, cs.CS_MODE_BIG_ENDIAN | cs.CS_MODE_M68K_000)


class Insn:
    __slots__ = ("addr", "size", "mnem", "ops", "op", "kind", "target", "ea")

    def __str__(self):
        return f"{self.addr:06X}  {self.mnem:<8} {self.ops}"


def w(rom, a):
    return struct.unpack_from(">H", rom, a)[0]


def sw(rom, a):
    return struct.unpack_from(">h", rom, a)[0]


def l(rom, a):
    return struct.unpack_from(">I", rom, a)[0]


def decode(rom, addr):
    """Decode one instruction. Returns Insn or None if invalid / out of ROM."""
    if addr & 1 or addr < 0 or addr + 2 > len(rom):
        return None
    try:
        ci = next(_md.disasm(rom[addr:addr + 16], addr), None)
    except Exception:
        ci = None
    if ci is None:
        return None
    ins = Insn()
    ins.addr, ins.size, ins.mnem, ins.ops = addr, ci.size, ci.mnemonic, ci.op_str
    op = w(rom, addr)
    ins.op = op
    ins.kind = "normal"   # normal | branch | cbranch | call | jump_ind | call_ind | ret | stop
    ins.target = None
    ins.ea = None

    hi = op >> 12
    if hi == 6:  # Bcc / BRA / BSR
        cond = (op >> 8) & 0xF
        d8 = op & 0xFF
        if d8 == 0:
            disp = sw(rom, addr + 2)
        elif d8 == 0xFF:
            return None  # 32-bit disp is 68020+
        else:
            disp = d8 - 256 if d8 & 0x80 else d8
        ins.target = (addr + 2 + disp) & 0xFFFFFF
        ins.kind = {0: "branch", 1: "call"}.get(cond, "cbranch")
    elif (op & 0xF0F8) == 0x50C8:  # DBcc
        ins.target = (addr + 2 + sw(rom, addr + 2)) & 0xFFFFFF
        ins.kind = "cbranch"
    elif op in (0x4E75, 0x4E73, 0x4E77):  # RTS RTE RTR
        ins.kind = "ret"
    elif op == 0x4E72 or op == 0x4AFC:  # STOP / ILLEGAL
        ins.kind = "stop"
    elif (op & 0xFFC0) in (0x4EC0, 0x4E80):  # JMP / JSR
        is_call = (op & 0xFFC0) == 0x4E80
        mode, reg = (op >> 3) & 7, op & 7
        tgt = None
        if mode == 7 and reg == 0:
            tgt = sw(rom, addr + 2) & 0xFFFFFF
        elif mode == 7 and reg == 1:
            tgt = l(rom, addr + 2) & 0xFFFFFF
        elif mode == 7 and reg == 2:
            tgt = (addr + 2 + sw(rom, addr + 2)) & 0xFFFFFF
        if tgt is not None:
            ins.target = tgt
            ins.kind = "call" if is_call else "branch"
        else:
            ins.kind = "call_ind" if is_call else "jump_ind"
            ins.ea = (mode, reg)
    return ins
