"""Recursive-descent tracer for the Desert Strike ROM.

Usage: trace.py [--game <id>]

Produces out/<id>/codemap.json with:
  functions   - call targets / vector entries
  labels      - every branch target
  code        - [start, end) ranges of decoded instructions
  unresolved  - indirect jumps/calls we could not resolve
  tables      - jump tables we resolved (address, kind, targets)
"""
import json, os, sys
from collections import deque
from m68k import decode, w, sw, l
from game_cfg import load_game_rom, HINTS_PATH, OUT_DIR

rom = load_game_rom()
ROM_END = len(rom)

insns = {}            # addr -> Insn
owner = {}            # byte addr -> insn addr (overlap detection)
functions = set()
labels = set()
unresolved = {}       # addr -> description
tables = {}           # table addr -> dict
conflicts = []

work = deque()


def add_entry(addr, func=False, why=""):
    if not (0x200 <= addr < ROM_END) or addr & 1:
        return
    if func:
        functions.add(addr)
    labels.add(addr)
    work.append(addr)


def prev_insns(addr, n=4):
    """Walk backward through already-decoded straight-line code."""
    out = []
    a = addr
    for _ in range(n):
        found = None
        for sz in (2, 4, 6, 8, 10):
            p = insns.get(a - sz)
            if p and p.addr + p.size == a:
                found = p
                break
        if not found:
            break
        out.append(found)
        a = found.addr
    return out


def resolve_pc_index_jump(ins):
    """jmp d8(pc,Xn): either a table of BRA instructions or an offset table."""
    base = (ins.addr + 2 + ((w(rom, ins.addr + 2) & 0xFF) ^ 0x80) - 0x80) & 0xFFFFFF
    # Offset-table form: move.w T(pc,Dn.w),Dn ; jmp T(pc,Dn.w)
    for p in prev_insns(ins.addr, 3):
        if p.mnem.startswith("move.w") and "(pc," in p.ops:
            ext = w(rom, p.addr + 2)
            pbase = (p.addr + 2 + ((ext & 0xFF) ^ 0x80) - 0x80) & 0xFFFFFF
            if pbase == base:
                return offset_table(base, base, "pc_offset_table", ins.addr)
    # Common idiom: jmp 0(pc,Dn.w) with Dn = 4,8,12... so entries begin
    # immediately after the jmp instruction rather than at the base.
    return bra_table(max(base, ins.addr + ins.size), ins.addr)


def offset_table(tbl, rel, kind, site, limit=256):
    targets = []
    end = None
    a = tbl
    while len(targets) < limit:
        if end is not None and a >= end:
            break
        off = sw(rom, a)
        t = (rel + off) & 0xFFFFFF
        if t & 1 or not (0x200 <= t < ROM_END) or decode(rom, t) is None:
            break
        targets.append(t)
        if t > tbl:
            end = t if end is None else min(end, t)
        a += 2
    if targets:
        tables[tbl] = {"kind": kind, "site": site, "targets": targets}
        for t in targets:
            add_entry(t)
    return bool(targets)


def bra_table(tbl, site, limit=256):
    targets = []
    a = tbl
    end = None
    while len(targets) < limit:
        if end is not None and a >= end:
            break
        i = decode(rom, a)
        if not i or i.kind != "branch" or i.mnem not in ("bra", "bra.b", "bra.w", "jmp"):
            break
        targets.append(i.target)
        insns[a] = i
        for b in range(a, a + i.size):
            owner[b] = a
        if i.target > tbl:
            end = i.target if end is None else min(end, i.target)
        a += i.size
    if targets:
        tables[tbl] = {"kind": "bra_table", "site": site, "targets": targets}
        for t in targets:
            add_entry(t)
    return bool(targets)


def resolve_areg_indirect(ins):
    """jmp/jsr (An): look back for movea.l T(pc,Dn), An  or  lea T,An; movea.l 0(An,Dn),An."""
    reg = ins.ea[1]
    an = f"a{reg}"
    prev = prev_insns(ins.addr, 4)
    for idx, p in enumerate(prev):
        if p.mnem == "movea.l" and p.ops.endswith(an):
            src = p.ops.rsplit(",", 1)[0].strip()
            if "(pc," in p.ops:
                ext = w(rom, p.addr + 2)
                base = (p.addr + 2 + ((ext & 0xFF) ^ 0x80) - 0x80) & 0xFFFFFF
                return ptr_table(base, ins.addr)
            # movea.l (aX,dn), an  preceded by lea T, aX
            if src.startswith("(a") or src.startswith("$0(a") or "(a" in src:
                for q in prev[idx + 1:]:
                    if q.mnem == "lea.l" and q.ops.endswith(src[src.index("(a") + 1:src.index("(a") + 3]):
                        base = lea_target(q)
                        if base is not None:
                            return ptr_table(base, ins.addr)
            return False
    return False


def lea_target(q):
    op = w(rom, q.addr)
    mode, reg = (op >> 3) & 7, op & 7
    if mode == 7 and reg == 1:
        return l(rom, q.addr + 2) & 0xFFFFFF
    if mode == 7 and reg == 0:
        return sw(rom, q.addr + 2) & 0xFFFFFF
    if mode == 7 and reg == 2:
        return (q.addr + 2 + sw(rom, q.addr + 2)) & 0xFFFFFF
    return None


def ptr_table(tbl, site, limit=256):
    targets = []
    a = tbl
    while len(targets) < limit and a + 4 <= ROM_END:
        t = l(rom, a)
        if t & 1 or not (0x200 <= t < ROM_END) or decode(rom, t) is None:
            break
        targets.append(t)
        a += 4
    if targets:
        tables[tbl] = {"kind": "ptr_table", "site": site, "targets": targets}
        for t in targets:
            add_entry(t, func=True)
    return bool(targets)


HINTS = json.load(open(HINTS_PATH))


def resolve_hint(ins):
    h = HINTS["jump_tables"].get(f"{ins.addr:06X}")
    if not h:
        return False
    tbl, rel, n = int(h["table"], 16), int(h["rel"], 16), h["count"]
    targets = [(rel + l(rom, tbl + 4 * k)) & 0xFFFFFF for k in range(n)]
    tables[tbl] = {"kind": h["kind"], "site": ins.addr, "targets": targets}
    for t in targets:
        add_entry(t)
    return True


def trace():
    while work:
        a = work.popleft()
        while True:
            if a in insns:
                break
            if a in owner:  # lands inside an existing instruction
                conflicts.append(a)
                break
            ins = decode(rom, a)
            if ins is None:
                unresolved[a] = "invalid opcode"
                break
            insns[a] = ins
            for b in range(a, a + ins.size):
                owner[b] = a
            k = ins.kind
            if k in ("branch", "cbranch", "call"):
                add_entry(ins.target, func=(k == "call"))
            if k == "jump_ind" or k == "call_ind":
                mode, reg = ins.ea
                ok = resolve_hint(ins)
                if ok:
                    pass
                elif mode == 7 and reg == 3:
                    ok = resolve_pc_index_jump(ins)
                elif mode == 2:
                    ok = resolve_areg_indirect(ins)
                if not ok:
                    unresolved[a] = f"{ins.mnem} {ins.ops}"
            if k in ("branch", "jump_ind", "ret", "stop"):
                break
            a += ins.size


def main():
    vec = [l(rom, i * 4) for i in range(64)]
    names = {1: "reset", 24 + 4: "hblank", 24 + 6: "vblank", 24 + 2: "extint"}
    for i in range(1, 64):
        if 0x200 <= vec[i] < ROM_END:
            add_entry(vec[i], func=True)
    for e in HINTS.get("extra_entries", []):
        add_entry(int(e, 16), func=True)
    trace()

    # merge code ranges
    ranges = []
    for a in sorted(insns):
        e = a + insns[a].size
        if ranges and ranges[-1][1] >= a:
            ranges[-1][1] = max(ranges[-1][1], e)
        else:
            ranges.append([a, e])
    code_bytes = sum(e - s for s, e in ranges)

    out = {
        "vectors": {str(i): vec[i] for i in range(64)},
        "functions": sorted(functions),
        "labels": sorted(labels),
        "code": ranges,
        "unresolved": {f"{k:06X}": v for k, v in sorted(unresolved.items())},
        "tables": {f"{k:06X}": v for k, v in sorted(tables.items())},
        "conflicts": sorted(set(conflicts)),
    }
    os.makedirs(OUT_DIR, exist_ok=True)
    json.dump(out, open(os.path.join(OUT_DIR, "codemap.json"), "w"), indent=1)
    with open(os.path.join(OUT_DIR, "listing.asm"), "w") as f:
        for a in sorted(insns):
            if a in functions:
                f.write(f"\nsub_{a:06X}:\n")
            elif a in labels:
                f.write(f"loc_{a:06X}:\n")
            f.write(f"    {insns[a]}\n")
    print(f"instructions {len(insns)}  functions {len(functions)}  code bytes {code_bytes} "
          f"({code_bytes*100/ROM_END:.1f}% of ROM)  tables {len(tables)}  "
          f"unresolved {len(unresolved)}  conflicts {len(set(conflicts))}")


if __name__ == "__main__":
    main()
