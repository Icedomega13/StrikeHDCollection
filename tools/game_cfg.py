"""Per-game configuration for the analysis and code-generation tools.

Every tool accepts `--game <id>` (default: desert) and reads
games/<id>/game.json. Outputs go to out/<id>/ and generated/<id>/ so the
games in the collection never overwrite each other.
"""
import hashlib, json, os, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def _game_id():
    argv = sys.argv
    for i, a in enumerate(argv):
        if a == "--game" and i + 1 < len(argv):
            return argv[i + 1]
        if a.startswith("--game="):
            return a.split("=", 1)[1]
    return "desert"


GAME_ID = _game_id()
GAME_DIR = os.path.join(ROOT, "games", GAME_ID)
CFG = json.load(open(os.path.join(GAME_DIR, "game.json")))
PREFIX = CFG["prefix"]
ROM_PATH = os.path.join(ROOT, CFG["rom"])
OUT_DIR = os.path.join(ROOT, "out", GAME_ID)
GEN_DIR = os.path.join(ROOT, "generated", GAME_ID)
HINTS_PATH = os.path.join(GAME_DIR, "hints.json")


def load_game_rom():
    data = open(ROM_PATH, "rb").read()
    got = hashlib.sha1(data).hexdigest()
    if got != CFG["sha1"]:
        sys.exit(f"{ROM_PATH}: SHA-1 {got} does not match {CFG['title']} ({CFG['sha1']})")
    return data
