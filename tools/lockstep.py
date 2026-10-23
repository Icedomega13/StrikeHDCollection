"""Compare frames.log from the reference harness and the recompiled build.

usage: lockstep.py ref/frames.log recomp/frames.log
Reports the first frame where any register or state checksum differs.
"""
import sys


def parse(path):
    out = []
    for line in open(path):
        parts = line.split()
        fr = int(parts[0])
        kv = dict(p.split("=") for p in parts[1:])
        out.append((fr, kv))
    return out


def main():
    a, b = parse(sys.argv[1]), parse(sys.argv[2])
    n = min(len(a), len(b))
    for i in range(n):
        fa, ka = a[i]
        fb, kb = b[i]
        diff = [k for k in ka if ka.get(k) != kb.get(k)]
        if diff or fa != fb:
            print(f"DIVERGED at frame {fa}: " + ", ".join(f"{k}: ref={ka[k]} recomp={kb.get(k)}" for k in diff))
            if i:
                print(f"last matching frame: {a[i-1][0]}")
            return 1
    print(f"MATCH: {n} frames identical (ref {len(a)}, recomp {len(b)})")
    return 0 if len(a) == len(b) else 1


if __name__ == "__main__":
    sys.exit(main())
