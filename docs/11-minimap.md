# Minimap

An optional overlay in the lower-left corner showing what the pause map shows, live, while flying. It uses the same semi-transparent olive-and-tan style as the status HUD.

- **Settings:** Settings → Picture → **Minimap** (On/Off, default On) and **Minimap Size** (Small/Medium/Large).
- **Controls:** **LB / RB** (keyboard **Q / E**) step through the pause map's list: the mission objectives, then the categories (enemy types, MIAs, fuel, ammo, landing zones). Both actions are under Controls ("Next / Previous Objective (minimap)") and can be rebound.
  - The selection is the game's own, so the pause map opens on the entry picked with the bumpers, and the reverse.
  - LB/RB used to be second A/C buttons. A `controls.ini` from before the minimap has those two bindings removed when it loads.
- It is shown only on the playfield (the same test as widescreen and the HUD), and hidden on the pause screen, in menus and in cutscenes.

## What it shows
The pause map's own picture of the campaign, cropped to the playable area (the camera range plus one screen). On top of it:

| Marker | Meaning |
|---|---|
| Orange diamond | A unit or object of a mission objective that is available now. Shown at all times, like the pause map. |
| Yellow, blinking | The selected objective's markers (the pause map blinks them too). |
| Cyan, blinking | Units of the selected category (only while it is selected, like the pause map). |
| White square | The player. |
| Blue square | Desert Strike's frigate, when the pause map shows it. |

The selected entry's name is written under the map, highlighted for a moment after stepping. "N. ?" is an objective not given yet. "(?)" marks an objective whose location the pause map doesn't reveal; it shows a "?" icon instead of the units.

Enemy units in general are not shown, only the category selected. The pause map does the same.

## How it works
**Terrain.** Each game draws a fixed picture per campaign on the pause map (Jungle and Urban: from a ROM table indexed by campaign). When a level is first seen on the playfield, the host:
1. saves the state in memory;
2. writes a sentinel into the word the map-open routine sets (and the pause page word to the map page);
3. runs hidden frames with Start held for 3 frames, with no host effects (no rendering, audio discarded, no cheats);
4. once the sentinel is set, renders each frame with the sprite list emptied (no icons), until the picture and the palette have held for 12 frames (the pause screen fades in);
5. crops the playable area and restores the state.

This takes 10–70 ms, once per level. The picture is cached as `minimap/<game>_<level>.png` in the settings folder. A fatal error during the hidden frames is caught (longjmp from the fatal hook) and the state restored; a failed capture is retried twice, 10 s apart, and otherwise the minimap draws without terrain.

**Markers** come from `games/strike_minimap.c`, a port of each game's pause-map icon builder:
- **Mission objectives** (entries with a prerequisite number at +8): when available, every object in list 1 and unit in list 2 whose group byte is (index + 1) × factor.
- **The selected entry:** every object or unit of its type (the first byte of its descriptor).
- The game's liveness checks are kept, and so are its limits: one icon per 512 × 512 px cell, 64 per entry.
- **Map transform:** map pixel = world >> shift + origin. Shift is 5 in Desert and Jungle; Urban reads it per campaign from a ROM table.

### Per-game addresses
| | Desert | Jungle | Urban |
|---|---|---|---|
| List pointer | `$FF1030` | ROM `$0EBF84` + 4 × campaign `$FF139E` | ROM `$0C360E` + 4 × campaign `$FF13E4` |
| Selected entry | `$FF103A` | `$FF104C` | `$FF1058` |
| Available mask | `$FF1070` | computed like `$03A9C6` from `$FF10DA` | computed like `$0037E0` from `$FF10EE` |
| "Location unknown" table | `$FF10C2` | `$FF10DA` | `$FF10EE` |
| Group factor | 2 | 1 | 1 |
| List 1 (stride) | `$FF2718` ($28), count `$FF1A34` | `$FF391E` ($1E), count `$FF12C6` | `$FF55DE` ($1E), count `$FF130C` |
| List 2 (grows down) | `$FFFBA8` ($28), count `$FF1A36` | `$FFFBA6` ($E or $2A), count `$FF12C8` | `$FFFB56` ($E or $2A), count `$FF130E` |
| Icon builder | `$003EBC` / `$003F6C` / `$003D1A` | `$039906` / `$0399B6` / `$039784` | `$0026BC` / `$00276C` / `$002532` |
| World origin on screen | (80, 40) | (56, 48) | (72, 32) |
| Shift | 5 | 5 | ROM `$0BBCA2` + 2 × campaign |
| Sentinel | `$FF0286` = 8 | `$FF048A` = 8 | `$FF049A` = 8 |
| Pause page (map = 2) | `$FF103C` | `$FF104E` | `$FF105A` |

Notes:
- Jungle and Urban set the list pointer and the mask only when the pause screen opens, so the minimap computes them the same way while flying.
- Desert's list 2 entries with flag bit 1 are hidden while an entry in the `$FF2522` list refers to them with bit 0 clear (`$0043BC`).
- `ds_test` prints the minimap data for one frame with `DS_MM_AT=<frame>`.
