# Desert Strike HD — handoff for Sol

Last updated: 2026-10-22. All three games are recompiled and lockstep-exact; widescreen up to 32:9, HD packs and the speed option work; everything on the original list is done, cheats included. Next: whatever the user asks for.

## What this is
A static recompilation of Sega Genesis *Desert Strike: Return to the Gulf* to C for PC. Goals:
- HD sprite/tile replacement;
- ultrawide;
- full key rebinding;
- modern gamepads.

It's the first game of a planned **Strike Remastered Collection HD** (Desert, Jungle and Urban Strike, behind an animated launcher). See `docs/03-collection-plan.md`.

## Where things live
- **GitHub:** https://github.com/Icedomega13/StrikeHDCollection (branch `main`): the source only. `rom/`, `generated/`, `out/` and `dist/` are git-ignored, so no ROMs and no ROM-derived code.
- **Source of truth:** the user's PC at `C:\Users\dac30\source\DesertStrikeHD`, reached through the remote-devices bridge. It has no git repo yet.
  - The latest Windows build is in `dist\StrikeCollectionHD.exe` there. (`dist\DesertStrikeHD.exe` is an older single-game build.)
- **Cloud working copy:** `/home/claude/dshd`. It may be gone in a new session; the PC copy is complete.
- **Cloud extras:** llvm-mingw is at `/home/claude/tc/llvm-mingw-20250924-ucrt-ubuntu-22.04-x86_64` (from the mstorsjo/llvm-mingw GitHub release). Builds were in `/tmp/build-linux` and `/tmp/build-win`; both are temporary.
- **ROM:** `rom/desert_strike.bin` (git-ignored), SHA-1 `d7e7d8c358eb845b84fb08f904cc0b95d0a4053d`. Original at `C:\Users\dac30\Downloads\Desert Strike - Return to the Gulf (USA Europe)`. Stage the file via the bridge, never through chat upload: an upload once corrupted it.
- **Project docs:** `claude/01-rom-map.md`, `claude/02-recompiler.md`, `claude/03-collection-plan.md`, `claude/04-pc-host.md`, plus this file. Mirrored in `docs/`.

## Status
| Step | What | State |
|---|---|---|
| 1 | ROM analysis, tracer, code map | Done |
| 2 | 68000 → C recompiler | Done. Lockstep-exact vs Musashi. |
| 3 | SDL3 host: window, VDP renderer, Z80 + YM2612 (ymfm) + PSG, input, save states, icon | **Done, but not yet run on real Windows by the user** |
| 4 | Settings/rebinding menu, widescreen, HD replacement, performance, no-flicker option | Done (cheats last) |
| 5+ | Jungle Strike, Urban Strike, launcher, installer | Collection plan |

**Verification at the end of step 3:**
- The lockstep suite (`tools/lockstep_suite.sh`, 5 scenarios, ~76.5k frames) all MATCH.
- Save/restore is identical, audio included.
- The host was smoke-tested headless with the SDL offscreen driver. The Windows exe was cross-built and checked: imports only system DLLs, icon embedded.

## Environment gotchas
- **Cloud:**
  - apt and pypi are blocked; GitHub is reachable (clones and release downloads both work).
  - Capstone is built from source into `tools/vendor`.
  - `third_party/SDL3` is a clone of release-3.2.24 (git-ignored); CMake falls back to FetchContent if it's missing.
- **PC device VM:**
  - Linux; gcc and python3.10; no cmake, no network.
  - It cannot run the Windows exe.
  - device_bash was still "downloading" during step 3. Writes via device_commit_files work.
- **Clang:** treats implicit function declarations as errors. That's why `harness/musashi/m68kconf.h` declares `harness_instr_hook`.

## Key design facts
- **Games are modules:**
  - Each game has `games/<id>/game.json` and `hints.json`.
  - Run the tools with `--game <id>`; output goes to `out/<id>/` and `generated/<id>/`.
  - Symbols are prefixed (`ds_f_XXXXXX`, `ds_recomp_lookup`), and `cpu_lookup` points at the running game's table.
  - Games are identified by ROM SHA-1 (`games/registry.c`).
- **Frame stepping:**
  - `cpu_run_frame()` runs one frame; the frame callback calls `cpu_frame_yield()`, which longjmps back to the host.
  - Resume works because every instruction is a dispatch entry, and re-running the boundary tick is a no-op.
- **Timing:**
  - Instruction-count: 20,000 per frame, vblank at 17,000 = 896,040 mclk.
  - Z80 = mclk/15; YM sample = 1,008 mclk (53,267 Hz); PSG tick = 240 mclk.
- **Save state** = CPU (with resume PC) + `gen_state` fields + sound (Z80 struct, PSG, ymfm save_restore, timers). Bump `GEN_STATE_VERSION` whenever the layout changes; it's currently 2.
- **Flags** exactly match Musashi:
  - DIVU/DIVS overflow sets only V;
  - CLR doesn't read its destination;
  - abs.w/abs.l keep the full 32 bits.
- **The VDP renderer runs at vblank start.** The game has no raster effects. `ext_left`/`ext_right` exist for widescreen, but the game still culls objects outside 320 px.

## The user's standing instructions
- When running low on tokens, write a handoff for Sol: this file, also in the project.
- The installer will ask for ROMs; there will be a launch icon. The current icon is a placeholder from `tools/make_icon.py`.
- They want an HD pause menu with settings and cheats; see `docs/05-pause-menu-plan.md`.
- They like the smooth, no-slowdown feel. It is the default; System → Game Speed → Original gives the console's slowdown.
- Translated code ships in the exe (a decided grey zone; the user provides ROMs; a personal project).

## Next actions
1. **Done since step 3:**
   - Fixed the VDP control-port pending flag (EA intro garbage).
   - Built the HD pause menu (`docs/05-pause-menu-plan.md`, Status).
   - Mapped the cheat addresses (`docs/06-cheat-addresses.md`). The user wants cheats **last**.
   - Widescreen 16:10 and 16:9 (`docs/07-widescreen.md`), done with recompiler patches driven by `g_ws_ext`.
   - **Jungle Strike and Urban Strike** (`docs/08-jungle-urban.md`):
     - The user supplied the ROMs: `C:\Users\dac30\Downloads\Jungle Strike\` and `...\Urban Strike\`, both `.md` files.
     - Both are recompiled and lockstep-exact.
     - The program is now `StrikeCollectionHD.exe`, with a game selector and "Switch Game" in the pause menu.
   - Widescreen 16:10/16:9 now works in all three games. The shared playfield test is in `games/strike_engine.h`.
   - **Game picker:** the Add ROM button was replaced by **Settings** (opens the settings menu in launcher mode: Settings / Controls / Back) and **Quit**. Selecting a game without a ROM still opens the file picker. The final installer is built (`dist\StrikeCollectionHD-Setup.exe`, 5.0 MB), and the source is published to GitHub.
   - **Cheats menu done** (`docs/06-cheat-addresses.md`): god mode, unlimited fuel/ammo/lives, campaign select, in all three games. Settings rows now fit any item count.
   - **HD pack builder (Settings → System → Build HD Pack):**
     - The extractor starts every campaign through the `g_start_campaign` imm patch: Desert `$002EC2` → `$FF1098`, Jungle `$038C90` → `$FF10B0`, Urban `$0018CE` → `$FF10C0`. That gives 4 / 9 / 10 campaigns.
     - It tours each map by writing the player position: `[[player]+$28]` Desert, `+$22` Jungle/Urban; x at +0, y at +4, height at +8. The player is kept alive.
     - It dumps tiles, sprite sheets and context sheets (`ctx_*`).
     - `hdpack_xbrz(.exe)` (GPL, a separate program from `tools/xbrz/`) scales them 4× with xBRZ. Busy, dithered cells stay nearest-neighbour.
     - A full cloud run took about 15 minutes for all three games: 8.4k / 17.7k / 17.1k images, about 400 MB of packs.
     - Cutscenes always play at 4:3: the playfield latch, plus a ground-vs-map check, plus a 30-frame settle delay.
   - **Bug fixes after user testing (2026-10-22):**
     - The Z80 core wrote back on `BIT n,(HL)`. Urban's sound driver polls YM status with `bit 0,(hl)` between register-address and data writes, so the write-back re-latched the YM address and lost FM writes. The continuous helicopter sound was silent. Fixed in `third_party/z80/z80.c` (local patch).
     - Widescreen dropped to 4:3 when hovering: replaced with a latch plus per-frame camera markers.
     - The map check guards cutscenes drawn in the planes (`docs/07-widescreen.md`).
     - The user's "first cutscene cut in half at 21:9" was not reproduced: the Desert/Jungle/Urban openings and attract modes are all fine. Ask which game and scene, or get a save state from `%APPDATA%\DesertStrikeHD\DesertStrikeHD\states`.
   - **Launcher with live intro shuffle** and the app icon, plus an **Inno Setup installer script** (`installer/`, built on Windows with `build_installer.bat`). See `docs/03-collection-plan.md` → Status.
   - **Status HUD** (`docs/10-status-hud.md`): armor/fuel/load/lives overlay, Picture tab toggle, addresses for all three games in each module's `strike_hud_cfg`.
   - **Game Speed setting** (System tab): Smooth = 20000 instructions/frame (default, ~2.3x the real 68000), Original = 8650 (128k cycles at a measured 14.8 cycles/instruction via `DS_CPI=1 ds_ref`). `g_frame_insns` is latched at each frame start; sound time is per-frame mclk so audio is unaffected; lockstep unchanged. `ds_test --frame-insns N`.
   - **Performance measured** (2-core cloud VM): CPU + sound ~1.8 ms/frame, 1x render 2.8 ms at 32:9, HD 4x compose ~8 ms at 32:9 with 2 threads. No further optimisation needed for now.
   - **HD graphics packs** (`docs/09-hd-packs.md`): art keyed by pattern hash, HD compose on worker threads (`src/hw/vdp_render.c` layer refs, `src/host/hdpack.c`), dumper, built-in starter-pack builder (Scale2x ×2). Verified: 1× output byte-identical with packs off.
   - **Ultrawide 21:9 / 32:9 / "Match window"** in all three games: planes painted from map data in `games/strike_world.c`, signed sprite X. See `docs/07-widescreen.md`.
2. **Next:**
   - Per-game input scripts for better coverage.
   - Installer is built (`dist\StrikeCollectionHD-Setup.exe`); the user still needs to run it end to end. Optional: .zip ROMs, an in-app Manage ROMs screen, focus-follows background.
3. Keep this file updated at each milestone.
