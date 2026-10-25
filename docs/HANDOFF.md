# Desert Strike HD — handoff for Sol

Last updated: 2026-10-25 (v0.8.1: difficulty prototype; v0.8.0 released, Amiga edition work in progress; earlier: v0.7.1 released; Amiga ultimate edition in progress: effects, music and intro built, not yet released). All three games are recompiled and lockstep-exact. Next: the user tests the Amiga music and intro build in `dist\StrikeCollectionHD.exe`.

## What this is
A static recompilation of Sega Genesis *Desert Strike: Return to the Gulf* to C for PC. Goals:
- HD sprite/tile replacement;
- ultrawide;
- full key rebinding;
- modern gamepads.

It's the first game of a planned **Strike Remastered Collection HD** (Desert, Jungle and Urban Strike, behind an animated launcher). See `docs/03-collection-plan.md`.

## Where things live
- **GitHub:** https://github.com/Icedomega13/StrikeHDCollection (branch `main`): the source only. `rom/`, `generated/`, `out/` and `dist/` are git-ignored, so no ROMs and no ROM-derived code.
  - **v0.8.1 (2026-10-25):** difficulty prototype (`docs/14-difficulty.md`: Recruit / Remastered default / Original, damage hook at `$00A6F6`, picker on New Game, Settings > System > Difficulty), sound for the silent AA guns and tanks (id 1100 from `$00D678`, fx 19), Build HD Pack from the game picker now starts the highlighted game and builds, and the scene-hang fix below. Lockstep 5/5.
  - **After v0.8.0 (2026-10-25):** a hang on the mission-failed screen ("You destroyed too many Scud Commanders!") from the user's save: the scene text loop's vblank wait at `$005B9C` branches back to its own `bclr`, so a vblank delivered at that instruction is erased; the recompile's fixed timing can phase-lock it forever. Patch at `$005BA4` keeps polling after 120000 spins (the original misses a few vblanks there in normal play, up to ~20000 spins, so the patch stays lockstep-exact: 5/5). Only Desert has this loop shape. Also fixed: the Amiga VM's Musashi build had switched off the instruction hook for `ds_ref` too (directory-wide source properties), which stalled the lockstep suite; it's now a separate object library `avm_m68k`. Test helpers added: `DS_DMG_LOG` (armor hits), `DS_PC_LOG` (resume PC and scene-text RAM every 30 frames). User's test saves: `%APPDATA%\DesertStrikeHD\DesertStrikeHD\balance_test\` (pow_camp_before = just before the POW camp, campaign 2; hang_0644). Measured: flying into the POW camp centre costs 80 armor about every 10 frames (600 in about 1.4 s); scripted inputs after a state load must use the save's frame numbers (`--inputs` is keyed to g_frame, about 36186 in pow_camp_before; load with `--key 900:F7` and no DS_PROMPT).
  - **Release v0.8.0** published 2026-10-24 (latest): https://github.com/Icedomega13/StrikeHDCollection/releases/tag/v0.8.0; main = `827927f`. Amiga edition marked work in progress (sound and Amiga mixing), Continue screen, strafe, minimap options, README screenshots (`docs/screenshots/`, taken headless with `--screen-shot` at 1920x1080 and from the scene tests). Installer compiled in the Inno Setup Compiler GUI on the PC (computer use: open the .iss, Ctrl+F9); portable zip made with `device_bash` (zip into a temp dir, then copy: zip can't replace files in the mounted folder). Tag created by the release form (tag pushes are blocked from the cloud); assets went up through the temporary `release-assets-080` branch, deleted afterwards in the browser.
  - **Release v0.7.1** published 2026-10-23: https://github.com/Icedomega13/StrikeHDCollection/releases/tag/v0.7.1; main = `3d221b3`. Fixes 0.7.0's hidden settings (tab item counts now come from the arrays), merges PR #3 from sxdot (Linux build docs + Renderer setting) with an automatic-renderer fallback, adds a README "How it was made" AI note (issue #1, answered and closed).
  - **Release v0.7.0** published 2026-10-23 (minimap, rewind, autosave, rumble, stats, AI HD packs): https://github.com/Icedomega13/StrikeHDCollection/releases/tag/v0.7.0; main = `f4519b9`. Installer built in Inno Setup on the PC (15.5 MB with the `ai` folder).
  - MIT license. **Release v0.6.0** is published (https://github.com/Icedomega13/StrikeHDCollection/releases/tag/v0.6.0), with the Setup exe and a portable zip; copies are in `release\` on the PC.
  - From the cloud session, branch pushes work, but creating releases, pushing tags and deleting branches are blocked. Do those through the built-in browser on the PC, where the user is signed in to GitHub. Assets reach the page from a temporary branch: fetch them from raw.githubusercontent.com, put them in `#releases-upload` with a DataTransfer, then delete the branch.
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
- **device_commit_files caches by staged path:** re-committing the same `/mnt/user-data/outputs/...` path can write the *old* content (it happened 2026-10-23: the user tested a stale exe). Stage every delivery under a new file name and check the md5 on the PC with `device_bash` afterwards.
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
   - **Minimap (2026-10-22, `docs/11-minimap.md`):** lower-left overlay of the pause map, live: available objectives' units, the selected category, player, Desert's frigate. LB/RB (Q/E) step through the pause map's list (the game's own selection word, so the pause map follows). Terrain = the pause map's picture, captured with hidden frames from a saved state (sandbox in `src/host/main.c`, `mm_capture`) and cached in `<settings>/minimap/`. Marker logic in `games/strike_minimap.c` (ports of each game's icon builder). Settings → Picture → Minimap / Minimap Size. LB/RB were second A/C buttons; old controls.ini files lose those two bindings on load. Lockstep 15/15 MATCH after the change. Policy: only what the pause map shows; enemy units maybe later if it doesn't look busy (the user feels it would make the game too easy).
   - **Next features (user's picks, 2026-10-22):**
     1. **Better HD art: high priority.** The user chose AI upscaling on their PC. **Built (2026-10-22, untested on real hardware):** Real-ESRGAN ncnn-vulkan (portable, BSD-3) ships in `dist\\ai\\` with the x4plus-anime and animevideov3 models. `hdpack_xbrz --ai` redoes the sprite and context sheets on the GPU and falls back to xBRZ when Vulkan is missing; Settings → System → HD Pack Style (`docs/09-hd-packs.md`). The cloud has no GPU, and building ncnn here for CPU testing was blocked by the safety check, so the look hasn't been seen yet: the user's Build HD Pack run is the first test. Possible follow-ups: per-sprite touch-up tools, other models.
     2-5. **Done 2026-10-23 (`docs/12-rewind-autosave-stats.md`):** autosave (Auto 1-3 slots, mission start plus objective completion), rewind (left trigger / Backspace, XOR-delta history in `src/host/rewind.c`), rumble (System slider), stats (`stats.ini`, pause-menu Stats page, game-picker line). Not yet: kills and score (RAM not mapped), remembering unlocked campaigns. Verified headless: rewind, both autosave triggers, the stats file, the menus. Rumble is untested (no pad in the cloud).
     - **Minimap fix 2026-10-23:** Desert sets its objective list (`$FF1030`) and mask only when the pause/briefing screen opens; they're now computed from ROM `$3018C` + campaign `$FF1B10` with the campaign-3 sequential rule (`$005102`). Before this, the minimap appeared only after the first pause.
     Also suggested but not picked yet: interpreter fallback for unrecompiled code (Musashi), a CRT shader, HD briefing text, colorblind colors, code signing, an update check and a Linux/Steam Deck build.
   - **Planned for the next pass (user request, after more testing; not started):** the launcher background should follow the selection. When a game is highlighted with keyboard or gamepad, or the mouse hovers over it, switch the background to that game's live intro instead of the shuffle cycle. Return to the shuffle when nothing is focused or after the mouse leaves. The `att_*` live-intro code in `src/host/main.c` already runs each game's attract mode, so switch which one is shown and keep the 4:3 letterbox; a short crossfade would avoid a hard cut.
   - Installer is built (`dist\StrikeCollectionHD-Setup.exe`); the user still needs to run it end to end. Optional: .zip ROMs, an in-app Manage ROMs screen.
   - **Amiga ultimate edition (2026-10-23, `docs/13-amiga-ultimate.md`):** the player adds their own Amiga `.adf` disks (Settings > Audio > Add Amiga Disks; nothing Amiga ships). Built: Edition presets (Genesis / Amiga / Remaster default / Custom) over three options: Amiga sound effects (63 samples, mapped by the user in the Sound Lab, `amiga_sfx.ini`), Amiga music (11 ProTracker songs rebuilt from the disks, `src/host/modplay.c`, hooked at `$00890E`, defaults in `music_defaults`, `amiga_music.ini`), and the Amiga intro (the three title photographs over the title song when Desert starts; the game picker is unchanged). Lockstep 5/5 after the hook patches. Second pass the same day: the real Amiga effect bank (fx 1-26, disk 2 `$0C1930`; the earlier 47 samples were mostly song instruments), Amiga Extras (objective jingle with ducking, mission start / low fuel / life lost events for voice lines), songs 2/108/10 mapped, and a fix for a briefing hang when a music request was turned into a stop. Audition WAVs in `Downloads\DesertStrikeAmiga\effects` on the PC. The user also asked to look at lola-retro (MIT CRT shader pack, github.com/zoran123456/lola-retro) as a CRT display option: needs SDL's GPU renderer path for custom shaders; prototype after the audio work. Evening: default effect mapping from the user's Codex audio pass (`sfx_defaults`), and a Continue / Load Game / New Game start screen with a resume state written on exit (`docs/12-rewind-autosave-stats.md`). Tester round (2026-10-23 night): strafe button (Y / Left Shift holds A for at least 16 frames, `input_pad_state`), minimap position / hold-Back or M toggle / optional nearby-threat markers (`docs/11-minimap.md`), Amiga rotor loop replacing YM channel 5 and rescue calls (`docs/13-amiga-ultimate.md`). The user wishes for more Amiga content (the animated frigate, etc.) while keeping the Genesis engine's frame rate: art and behaviour would go through the HD pack system and host-side events, not by merging the two codebases. Cutscenes (2026-10-23 night): every Genesis cutscene/briefing picture is shown as its Amiga version (animated where the Amiga animates it), keyed by the scene record the patched scene player reports (`g_scene_rec`); see the Cutscenes section of `docs/13-amiga-ultimate.md`. Later that night the user reported the cutscenes not animating (briefing officer's arm cut off, pilots and smoke still): the briefing now gets the program's bobs, and the frigate, refinery, game over and ending run their own Amiga programs in a built-in Amiga VM (`src/host/amiga_vm.c`, Musashi); exe md5 aab16d7b9c46de7ecfd7775acb2e3a96 delivered. Still to do: ask the user for their `amiga_sfx.ini` to ship as the default mapping, check the music defaults by ear (story/bunker/briefing guesses), the Amiga story animations, voice warnings, Jungle/Urban hooks, then a release.
3. Keep this file updated at each milestone.
