"""Decode Sega Genesis Game Genie codes (ABCD-EFGH) to ROM address + 16-bit value.

usage: gamegenie.py CODE [CODE...]
Bit layout follows Genesis Plus GX's decoder.
"""
import sys

CHARS = "ABCDEFGHJKLMNPRSTVWXYZ0123456789"


def decode(code):
    code = code.replace("-", "").upper()
    if len(code) != 8 or any(c not in CHARS for c in code):
        raise ValueError(f"{code}: not an 8-character Game Genie code")
    addr = data = 0
    for i, ch in enumerate(code):
        n = CHARS.index(ch)
        if i == 0:   data |= n << 3
        elif i == 1: data |= n >> 2; addr |= (n & 3) << 14
        elif i == 2: addr |= n << 9
        elif i == 3: addr |= ((n & 0xF) << 20) | ((n >> 4) << 8)
        elif i == 4: data |= (n & 1) << 12; addr |= (n >> 1) << 16
        elif i == 5: data |= ((n & 1) << 15) | ((n >> 1) << 8)
        elif i == 6: data |= (n >> 3) << 13; addr |= (n & 7) << 5
        else:        addr |= n
    return addr, data


if __name__ == "__main__":
    for c in sys.argv[1:]:
        a, d = decode(c)
        print(f"{c}: ${a:06X} = ${d:04X}")
