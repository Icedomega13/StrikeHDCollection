"""Generate a random-play input script for lockstep testing.

usage: make_inputs.py <seed> <out.txt> [frames]
Presses Start through the menus, then flies around and fires at random,
occasionally pausing. Format: "<frame> <buttons>" (UDLRABCS).
"""
import random, sys


def main():
    seed, out = int(sys.argv[1]), sys.argv[2]
    end = int(sys.argv[3]) if len(sys.argv) > 3 else 20000
    random.seed(seed)
    lines = ["0 "]
    for f in range(420, 1000, 90):
        lines += [f"{f} S", f"{f + 6} "]
    moves = ["U", "D", "L", "R", "UL", "UR", "DL", "DR", "", "U", "U", "U"]
    fires = ["", "", "A", "B", "C", "AB", "A", "A"]
    f = 1100
    while f < end:
        b = random.choice(moves) + random.choice(fires)
        if random.random() < 0.004:
            b = "S"
        lines.append(f"{f} {b}")
        f += random.randint(4, 40)
    open(out, "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
