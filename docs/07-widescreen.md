# Widescreen

**All three games** support 16:10, 16:9, 21:9 and 32:9, plus "Match window". Jungle and Urban Strike use the same engine: 14 patches each at their own addresses (see each game's `hints.json`). The playfield test is shared in `games/strike_engine.h`.

| Game | Layer records | Scroll writer | Playfield test |
|---|---|---|---|
| Desert | `$FF1A50`, `$FF1AA8` | `$00BCBC` | camera `$FF1AF2`/`$FF1AF0` vs plane B |
| Jungle | `$FF12E4`, `$FF133C` | `$0405AE` | camera `$FF1386`/`$FF1384` vs plane A |
| Urban | `$FF132A`, `$FF1382` | `$009376` | camera `$FF13CC`/`$FF13CA` vs plane A |

Differences from Desert Strike:
- Jungle and Urban's camera clamp subtracts 321/225 (`$141`/`$E1`) and reads the map bounds from `$10(a0)`.
- The layer record that Desert Strike uses for plane B drives plane A in these two.

## Result
Settings → Picture → **Widescreen**: Off / 16:10 / 16:9 / 21:9 / 32:9 / Match window.
- The scrolling playfield draws more on each side: 32 px (16:10), 56 (16:9), 120 (21:9), 272 (32:9). "Match window" picks the width that fills the window's shape.
- Terrain, roads and objects are valid all the way to the edges.
- Menus, the briefing and the map/status screen stay 4:3 with black bars, so the picture size never jumps.
- Widescreen off = the original game. Lockstep still matches in all scenarios, because every patch reduces to the original instruction when `g_ws_ext` = 0.
- With 16:9 on, three 20,000-frame random-play runs complete without errors.

## How the game streams its map
Each scroll layer has a record (`$FF1A50` = plane A, the overlay; `$FF1AA8` = plane B, the ground):

| Offset | Meaning |
|---|---|
| `+$10..+$17` | loaded window in tiles (64×64 = the whole plane) |
| `+$18..+$1F` | loaded window in pixels (512×512) |
| `+$30..+$37` | camera clamp: map bounds − (320, 224) |
| `+$38..+$3F` | free-scroll range: the camera can move here without reloading |
| `+$40..+$47` | active-object rectangle: camera −64 … +384 (x), +288 (y) |
| `+$48`, `+$4A` | camera y, x |

- `$00B394` sets a new window and its free range.
- `$00BA0A` scrolls. When the camera leaves the free range, it re-windows:
  - moving left: window starts at the camera;
  - moving right: window starts at camera − 184, which leaves only 8 px past the view.
- `$00BD16` draws tile blocks into the plane.
- `$00BCBC` writes the camera to the VDP scroll registers once per frame.

## The patches (`games/desert/hints.json` → `patches`)
`tools/recomp.py` supports two per-instruction patch kinds, both reading the runtime variable `g_ws_ext` (pixels per side):
- `imm`: replace an immediate with a C expression.
- `post`: run C after an instruction.

| Address | Change |
|---|---|
| `$00B3F4`, `$00B3FA` | free range shrinks by `ext` on both sides |
| `$00BA54`, `$00BA6C` | re-window leaves `ext` px beyond the view on both sides |
| `$00BA22`/`$00BA2E`, `$00BB84`/`$00BB90`, `$00BBA6`/`$00BBB2` | active-object rect widened by `ext` |
| `$00BB48` | initial window starts at camera − `ext` |
| `$00BB2C`, `$00BB38` | camera clamp: never shows past the map edge |
| `$00BCBC` | records the frame (`g_ws_flight_frame`) for playfield detection |

## Playfield detection (updated)
- **Latch:** `strike_ws_active()` turns on when the scroll registers match the layer camera in a frame where the game's camera code ran. It stays on while the match holds, even when the camera stops. Before, it required the scroll writer within the last 600 frames. That writer only runs when the camera moves, so hovering in place for 10 s dropped back to 4:3 (user report).
- **Markers:** the per-frame camera routine now marks the frame even when the camera doesn't move: Desert `$00764E`, Jungle `$03D412`, Urban `$006ABC` (`post` patches in each `hints.json`). The scroll-writer markers stay.
- **Map check:** `strike_ws_world()` compares the ground it paints with what the game drew in VRAM inside the drawn window. In the playfield these match exactly. If more than 1/8 of at least 64 cells differ, something else is in the plane (a cutscene drawn over the map), and the frame is shown 4:3. The host runs this check at every widescreen setting, and the HUD uses the same result.

## Playfield detection (original notes)
`games/desert/desert.c` → `ds_ws_active()`: plane B's scroll registers equal the layer-B camera (within 24 px, since the camera can move between the scroll write and vblank), and `$00BCBC` ran in the last 600 frames. The status screen resets scroll while the camera keeps its flight value, so it fails the test. Each game module provides its own `ws_active` and `ws_max_ext`.

## Ultrawide (more than 56 px per side)
Two hardware limits stop at 16:9: the plane holds a 512 px window of the map, and sprite X is 9 bits. Beyond 56 px:

- **Planes are painted from the game's map data** (`games/strike_world.c`, `strike_ws_world`). For each frame the module builds both planes in world coordinates for the whole view:
  - **Ground:** the same lookup the strip drawers use. Desert: a grid of 64×64-tile sectors (level descriptor `+$62`). Jungle/Urban: a grid of 32×16-tile blocks (level descriptor `+$5C` / `+$68`). Each block's tile map is word or byte format (`+$10` = `$1A`), with its tile set's VRAM base from the tile-set table.
  - **Overlay** (buildings and other map objects): the game's map-object list, each object's tile block blitted at its rectangle. Jungle/Urban add a per-object tile offset.
  - **Inside the window the game has finished drawing** (the intersection of the layer record's `+0` and `+$10` rects), the real VRAM cells replace the painted ones, so anything the game changed there is exact.
  - A tile set that isn't loaded in VRAM is skipped, as the game itself does.
  - The screen origin comes from the scroll registers, unwrapped to the copy nearest the layer camera, so planes and sprites stay in step.
  - Validated against VRAM: every cell inside the window matched for all three games (ground and overlay, dozens of frames each).
- **Sprite X is read as signed 16-bit** while painting (the game writes full positions; masking uses the full value).
- **Patches:** the active-object rectangle and camera clamp use the full `g_ws_ext`; the plane-window patches use `g_ws_win` = min(ext, 56), so the game's own window logic stays inside the 512 px plane.
- The renderer takes the painted planes through `vdp_render_opts.world[]` (`vdp_world_plane`).

Each game's addresses are one `strike_cfg` line in its module:

| | Level ptr | Ground table | Tile sets | Ground / overlay record | Object list | Mask |
|---|---|---|---|---|---|---|
| Desert | `$FF24BA` | sectors `+$62` | `$FF255C` | `$FF1AA8` / `$FF1A50` | `$FF2508` | `$19` bit 0 |
| Jungle | `$FF283C` | `+$5C` | `$FF2982` | `$FF133C` / `$FF12E4` | `$FF29CC` | `$19` bits 3+6 |
| Urban | `$FF4542` | `+$68` | `$FF4690` | `$FF1382` / `$FF132A` | `$FF4998` | `$19` bits 3+6 |

The ground is plane B and the overlay plane A in all three.

### Notes
- More objects are active in a wider view, so the game does more work per frame and the 80-sprite table fills sooner. Very wide views can show more slowdown and flicker than the original.
- The renderer's line buffer is 1024 px, so the maximum is 352 px per side; modules allow 272 (32:9).

## Leaving the pause map (2026-10-26, issue #7)
After any non-playfield frame the picture used to stay 4:3 for half a second (the cutscene guard), so closing the pause map showed pillarboxing for about 30 frames. Now only a failed map check (a cutscene drawn over the map) arms the half-second wait; after 20 or more frames simply off the playfield (pause map, status screens) the wait is 6 frames. 6, not less: a respawn shows the playfield for 2 frames between black screens. Closing the map now widens within a frame or two of the playfield coming back. `DS_WS_LOG=1` logs the decision.
