"""Add code that the reference executed but the static trace missed.

usage: add_dynamic_entries.py --game <id> hits.txt [hits.txt...]
Reads hits files from ds_ref runs, finds executed ROM addresses outside the
traced code, and adds the start of each such block to hints.json
"extra_entries". Re-run trace.py afterwards; repeat until nothing is added.
"""
import bisect, json, os, sys

sys.path.insert(0, os.path.dirname(__file__))
from game_cfg import OUT_DIR, HINTS_PATH  # noqa: E402


def main():
    files = [a for a in sys.argv[1:] if not a.startswith("--") and a != sys.argv[sys.argv.index("--game") + 1]] \
        if "--game" in sys.argv else sys.argv[1:]
    cmap = json.load(open(os.path.join(OUT_DIR, "codemap.json")))
    code = cmap["code"]
    starts = [s for s, e in code]

    def inside(a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and a < code[i][1]

    hits = sorted({int(l, 16) for f in files for l in open(f) if l.strip()})
    miss = [a for a in hits if a < 0xFF0000 and not inside(a)]
    # block starts: missed addresses not preceded by another missed address
    # (code right after an rts/jmp is a separate entry even though the
    # instruction before it ran)
    missset = set(miss)
    new = [a for a in miss if not any((a - k) in missset for k in (2, 4, 6, 8, 10))]
    hints = json.load(open(HINTS_PATH))
    have = {int(x, 16) for x in hints.get("extra_entries", [])}
    add = [a for a in new if a not in have]
    hints["extra_entries"] = [f"{a:06X}" for a in sorted(have | set(add))]
    hints.setdefault("comment_entries", "Code reached only through run-time pointers (found by tools/add_dynamic_entries.py)")
    json.dump(hints, open(HINTS_PATH, "w"), indent=2)
    print(f"executed {len(hits)}, outside static code {len(miss)}, new entries {len(add)}")


if __name__ == "__main__":
    main()
