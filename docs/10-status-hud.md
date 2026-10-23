# Status HUD

An optional overlay showing **armor, fuel, load, lives and ammo** in the top-left corner of the game image, in the pause menu's style: olive panel, tan labels.

Settings → Picture → **Status HUD** (on by default).
- It is shown only on the scrolling playfield (the same test widescreen uses), and hidden while the pause menu is open.
- Armor and fuel are bars with their value. They turn amber below 50% and red below 25%, and the bar blinks when low.
- Load shows one pip per seat (amber when full) and `n/max`. Lives turn amber at 1.
- Ammo is the bottom row: `G` guns, `HY` hydras, `HF` hellfires; a weapon at 0 turns red.
- It's drawn by the host in screen space, not in the game image. It's placed at the image's corner and sized from the image height, so it keeps its proportions in every window size, widescreen setting and scaling mode. The text is the menu font, rendered crisp at any resolution.
- Nothing is written to game memory.

## Addresses
Each game module has one `strike_hud_cfg` line; reading is in `strike_hud()` (`games/strike_engine.h`).

| | Armor | Armor max | Fuel / max | Load / capacity | Lives |
|---|---|---|---|---|---|
| Desert | `[$FF251C]+$32` | `$FF1040` | `$FF10AA` / `$FF10BC` | `$FF10B2` / 6 (fixed: `cmpi #6` at `$00DEAC`) | `$FF10B6` |
| Jungle | `[$FF29E8]+$2C` | `$FF1054` (per vehicle) | `$FF10C6` / `$FF10D4` | `$FF10CA` / byte `$FF122B` | `$FF10CE` |
| Urban | `[$FF49B4]+$2C` | `$FF1060` (per vehicle) | `$FF10D6` / `$FF10E8` | `$FF10DA` / byte `$FF1245` | `$FF10DE` |

**Ammo:** the player object holds a pointer to its three weapon records (Desert `+$3A`, Jungle/Urban `+$34`). The records are `$40` apart, with rounds left at `+$12`, the word the fire routine decrements (`$00D55A` / `$044BE2` / `$00F446`). The pointer was confirmed by watching that instruction in the reference emulator (`DS_WATCH_PC=<hex> ds_ref ...`). Records seen in test runs: Desert `$FFAE8A`, Jungle `$FFBF34`, Urban `$FFB190`.

These were found by matching the code patterns of Desert Strike's known variables in the other two games:
- fuel: `sub.w d0` burn, `cmpi #$E` low-fuel warning, refill from the max;
- lives: `subq`/`addq`, reset from the start value;
- load: `addq` at pickup, `sub` at drop-off, compared against the capacity;
- armor: refilled from the max into the player object.

All read plausible values in flight dumps of all three games. The load counter was checked from the code; it hasn't yet been seen non-zero in a test run.

These addresses also serve the cheats tab later (`docs/06-cheat-addresses.md`).
