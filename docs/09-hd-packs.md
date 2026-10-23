# HD graphics packs

Replacement art for the games' graphics, drawn at up to 4× resolution. Packs work in all three games, in every screen mode including ultrawide.

## Using a pack
Put the pack in `packs/<game>/`, either in the settings folder (`%APPDATA%\DesertStrikeHD\DesertStrikeHD\packs\desert\`) or next to the program. The game ids are `desert`, `jungle` and `urban`. Any subfolders are scanned.

The pack loads when the game starts. A message shows how many graphics it holds and at what scale. Settings → Picture → **HD Graphics** turns it off and on.

## How it works
- Art is keyed by a hash of an 8×8 pattern's 32 bytes of pixel data. It follows the graphic wherever the game loads it in video memory and whatever palette it is drawn with.
- The renderer records, for every screen pixel of every layer, which pattern pixel it shows. The 1× image is made from that exactly as before. With a pack active, the frame is also composed at the pack scale: each layer samples the art where there is some, and falls back to the original pixel (enlarged) where there is none. Priorities between sprites and planes are worked out per HD pixel, so smooth edges and transparency work.
- **Palette changes:** a pattern's art is drawn for one palette (recorded in `palettes.txt` or in the sheet's `.txt`). When the game shows it with different colours (screen fades, damage flashes, colour variants of an enemy), each HD pixel is shifted by the difference between the current colour and the recorded colour of the original pixel underneath.
- Art may be opaque where the original pixel was transparent, for example an anti-aliased edge. For sprites, such a pixel is used where no other sprite covers it.
- Shadow/highlight mode is applied per original pixel.
- The HD compose runs on several threads. Without a pack nothing changes, and the 1× path is byte-identical to before.

## Pack files
| File | Contents |
|---|---|
| `<hash>.png` | One pattern: 8S×8S pixels, S = 1 to 4. `<hash>` is 16 hex digits. |
| `spr_<id>.png` + `spr_<id>.txt` | A sheet: any block of patterns, usually a whole sprite. The `.txt` holds `w h` (in patterns), then `h` rows of `w` hashes (`-` for none), then optionally `pal` and 16 colours (`RRGGBB`). |
| `palettes.txt` | Lines of `<hash> c0 … c15`: the colours each `<hash>.png` was drawn for. |

- Sheets are loaded before single patterns, so whole-sprite art wins. Otherwise the first art found for a hash is used.
- The pack scale is set by the first image loaded. Other images are resampled to it.
- Alpha below 128 is transparent.

## Building a pack automatically (recommended)
**Settings → System → Build HD Pack** does everything for the running game in a few minutes. Esc cancels it, and the game restarts afterwards.

1. **Extract.** The game plays itself, headless:
   - It shows the intro, title and menus.
   - It starts every campaign, using a patched start-campaign value (`g_start_campaign`, the same one a campaign select will use).
   - It flies the player in rows across the whole map. The player is kept alive and fuelled (`strike_tour` in `games/strike_engine.h`).
   - It uses the 32:9 view, so units across the map wake up and get drawn.
   - Every new pattern and sprite is dumped. Every 8 frames, the visible ground and overlay cells are also written as **context sheets** (`ctx_*.png`/`.txt`). These let the upscaler see each pattern among its real neighbours, so ground tiles come out without seams.
2. **Upscale.** `hdpack_xbrz.exe` (next to the game) scales every dumped image 4× with **xBRZ**, the edge-aware pixel-art scaler used by emulators. It's a separate program because xBRZ is GPL-3; its licence ships as `hdpack_xbrz-LICENSE.txt`, and the game itself contains no GPL code. If the tool is missing, the built-in Scale2x is used instead.
3. **Load.** The result goes to `packs/<game>/` and loads immediately.

The same extraction runs headless with `StrikeCollectionHD --extract [campaign]`, for all games with ROMs. The tool also runs on its own: `hdpack_xbrz <dump dir> <pack dir> [-s 2..6]`.

Sheet tokens: a context sheet's cells are `HASH[:h][:v]:pN`, meaning the cell is shown mirrored (its art is mirrored back on load) and uses palette line `palN`. Cells shown as "-" are empty.

## Making a pack by hand
1. Settings → System → **Dump Graphics: On**, then play. Every new pattern and sprite on screen is written to `hd_dump/<game>/` in the settings folder, as files in exactly the pack format (1× size, with their palettes).
2. Redraw or upscale the images at 2×, 3× or 4×. Keep the file names and the `.txt` files. Sprite sheets are the easiest to work on, since they are whole objects.
3. Copy the results into `packs/<game>/`.

`tools/hdpack_upscale.py <dump dir> <pack dir>` is a simple Scale2x alternative to the xBRZ tool. Hand-drawn or AI-upscaled art can replace any file in a pack: keep the name and size.

## Notes
- Single 8×8 patterns of the ground are tiny, so per-pattern upscaling can't see across their edges. Making seamless ground art means working on larger areas and cutting them into patterns.
- A pattern shared by several graphics gets the same art everywhere.
- Performance: the cloud test machine (2 cores) composes 32:9 at 4× in about 10 ms per frame. The 16:9 view is half the size, and a typical desktop has more cores.
