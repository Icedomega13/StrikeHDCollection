# Jungle Strike and Urban Strike

## Result
Both games are recompiled and run in the same program as Desert Strike (`StrikeCollectionHD.exe`). Both are **lockstep-exact against Musashi**: 5 scenarios × 3 games all MATCH, about 230,000 frames, via `tools/lockstep_suite.sh <build>`.

No game-specific fixes were needed. The tracer, recompiler, hardware model (VDP, Z80, YM2612, PSG) and runtime built for Desert Strike handled both games as-is.

| | Desert Strike | Jungle Strike | Urban Strike |
|---|---|---|---|
| ROM | 1 MB | 2 MB | 2 MB |
| SHA-1 (USA/Europe) | `d7e7d8c3…` | `7a7568e3…` | `897a8f6a…` |
| Instructions traced | 12,969 | 14,100 | 16,520 |
| Functions / jump tables | 341 / 18 | 391 / 27 | 415 / 31 |
| Run-time entries added | 6 | 13 | 12 |
| RAM code | `$FF1C72` trampoline | `$FF1630` | `$FF2F00` |
| Symbol prefix | `ds_` | `js_` | `us_` |

## How a game was added
1. `games/<id>/game.json`: id, symbol prefix, title, year, ROM path, SHA-1.
2. `python tools/trace.py --game <id>`. Both games traced with zero unresolved jumps.
3. Run `ds_ref` (Musashi) on the lockstep scenarios. It writes `hits.txt`, every executed address.
4. `python tools/add_dynamic_entries.py --game <id> <hits.txt...>` adds code that is only reached through run-time pointers (vblank handlers installed into the RAM trampoline, and code right after an `rts`/`jmp` that is jumped to). Re-trace, and repeat until it adds nothing.
5. `python tools/recomp.py --game <id>`, then `games/<id>/<id>.c` (the module) and an entry in `games/registry.c` and in `DS_GAMES` in CMake.
6. Lockstep all scenarios.

## The program
- **Game selector:** on start, it scans for all three ROMs by SHA-1, in the settings folder and next to the exe.
  - Each game card shows READY or ROM NOT FOUND.
  - Selecting a missing game (or "Add ROM") opens a file picker, verifies the file and copies it to `<settings>/roms/<id>.bin`.
  - Works with gamepad, keyboard or mouse. This is a functional first version of the collection launcher (`docs/03-collection-plan.md`).
- **Pause menu:** gains **Switch Game**, which returns to the selector.
- **Shared and per-game data:**
  - Settings and controls are shared.
  - Save states are per game: `states/<id>_quick.state`, `<id>_slotN.state`.
  - The settings folder keeps its original name (`DesertStrikeHD`), so existing settings and states carry over.
- **Widescreen:** all three games, 16:10 and 16:9 (`docs/07-widescreen.md`). With it off, lockstep still matches. 16:9 runs of 20,000 frames complete without errors.

## Known gaps / next
- **Coverage:** the input scripts were written for Desert Strike. Urban Strike's scripts spend a lot of time in its status screens. `harness/inputs_urban_flight.txt` gets into flight: one Start press, because Start toggles the status screen. Scripts made for each game would raise coverage. Any un-traced code would stop with a clear "not recompiled" message, never run wrongly.
- **The animated launcher:** intro shuffle in the background, plus original logo artwork.
