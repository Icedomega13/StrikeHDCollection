# Desert Strike HD — Step 3: the PC host

## Result
`DesertStrikeHD.exe` plays the recompiled game in a window, with:
- picture;
- full sound (Z80 sound driver, YM2612 FM, PSG);
- keyboard and gamepad input;
- save states;
- fullscreen.

Lockstep against Musashi still matches exactly in all five scenarios (about 76,500 frames). That now includes the sound hardware, because the Z80's mailbox in RAM is part of the compared state.

| Check | Result |
|---|---|
| Lockstep: menu 1,500, gameplay 15,000, seeds 11/22/33 × 20,000 | All identical |
| Save at frames 300, 500, 1,234, 2,222, 7,777, 11,111; restore; continue | Identical to the unbroken run, including the audio samples |
| C renderer vs Python renderer, 31 dumps | Identical except where the Python one was wrong (vscroll register) or incomplete (window plane) |
| Windows exe | Statically linked; only Windows system DLLs (UCRT, Win10+); icon embedded |
| Headless speed (with sound, no video) | ~536 frames/s, about 9× real time |

## Layout
```
games/game.h            game_module: id, title, ROM SHA-1, dispatch table
games/registry.c        list of games, ROM identification, game_boot()
games/desert/           desert.c (module), game.json (tool config), hints.json
generated/desert/       recompiled code, symbols prefixed ds_ (git-ignored)
src/hw/genesis.c        bus, VDP ports, pads, timing, save-state fields
src/hw/vdp_render.c     software renderer (+ extra columns for widescreen)
src/hw/sound.c          Z80 + bus, PSG, mixing, DC filter, output ring
src/hw/ym2612_ymfm.cpp  YM2612 via ymfm, timers on the shared clock
src/runtime/cpu.c       dispatch, interrupts, frame stepping, CPU save state
src/host/main.c         SDL3 app: window, pacing, audio, ROM discovery, states
src/host/input.c        bindings (controls.ini), keyboard + gamepads
src/util/sha1.c         ROM identification
res/                    icon (tools/make_icon.py), Windows .rc
third_party/ymfm, z80   vendored (BSD-3, MIT); SDL3 fetched by CMake
```

## Design

### Running one frame at a time
- The host calls `cpu_run_frame()`. The game runs until the next frame boundary.
- At the boundary the frame callback calls `cpu_frame_yield()`. It saves the resume PC and `longjmp`s back to the host.
- The next call simply dispatches that PC. There are no threads or fibers.

This works because:
- `recomp.py` now makes **every instruction a dispatch entry**. The lookup table has 12,969 entries, which cost about a third of the headless speed.
- The game's return addresses live on the emulated stack, so throwing away the C stack loses nothing.
- Hardware time doesn't change. The frame boundary fires inside `gen_instr_tick()` after the timing counters have moved on but before the instruction counter increments, so repeating the tick on resume is a no-op.
- `TICK` restores `cpu_cur_pc` after an interrupt returns, so a boundary that lands just after an interrupt resumes at the right instruction.

### Save states
- `cpu_state_save()` covers:
  - the CPU, with the resume PC;
  - RAM, VRAM, CRAM, VSRAM and VDP registers;
  - VDP port latches, pads and timing;
  - the Z80, PSG and YM2612 (ymfm's own save/restore) and the timers.
- Size is about 148 KB. Files go to `<settings>/states/<game>_slot1.state`, with a header carrying a magic number and the game id.
- `GEN_STATE_VERSION` is bumped whenever the layout changes. Old states are refused with a message.

### Sound
- **Timing:** everything runs on the shared instruction clock, converted to master clocks: 20,000 instructions = 896,040 mclk per frame.
  - The Z80 runs at mclk/15.
  - A YM2612 sample is produced every 1,008 mclk (53,267 Hz).
  - PSG counters tick every 240 mclk.
- **Lazy Z80:** the Z80 runs only when needed, catching up whenever the 68000 touches:
  - Z80 RAM, the YM2612 or the PSG;
  - bus request or reset.

  It also catches up at vblank, when the Z80 interrupt is raised for one scanline, and at the end of each frame.
- **Bank window:** the Z80's `$8000` window into 68000 memory is supported (9-bit bank register).
- **YM2612:** ymfm's `ym2612` class, which models the discrete chip's DAC "ladder" distortion. Timers are counted in chip clocks per sample, with overshoot carried across re-arms, so the driver's timer-B tempo is exact: 208 Hz, 3–4 ticks per frame while music plays.
- **Mix:** FM × 0.75 plus PSG, then a one-pole high-pass at about 5 Hz to remove the DAC's DC offset.
- **Host side:** samples go into an SDL audio stream, which resamples to the device rate. The host keeps about 50 ms queued, nudging the playback ratio by up to ±1% so it neither drifts nor builds up delay.

### Video
- **When it draws:** `gen_vblank_cb` renders the frame at the start of vblank. The game has no raster effects (its HBlank vector is an RTE), so a whole-frame render is exact.
- **Renderer features:**
  - planes A and B with all scroll modes;
  - the window plane;
  - sprites with the per-line sprite and pixel limits, plus masking;
  - shadow/highlight;
  - the console's DAC colour levels.
- **Extra columns:** `ext_left` and `ext_right` draw beyond the 320-pixel display. That's the basis for widescreen, but the game still culls objects outside its 320-pixel view (see step 4).
- **On screen:** 4:3 shape (slightly narrow pixels, as on a TV), using "sharp bilinear" scaling: nearest-neighbour up to an integer multiple, then linear down to the window size.

### Input
- `controls.ini` lives in the settings folder and is created with defaults on first run.
- Each action takes up to 4 keyboard keys and 4 gamepad inputs.
- The actions are the 8 Genesis buttons plus host hotkeys: save state, load state, fullscreen, fast forward, pause and quit.
- **Gamepad names are positional:** `south`/`east`/`west`/`north`, d-pad, shoulders, `+leftx`/`-lefty` and so on. This keeps Xbox, PlayStation and Switch pads consistent.
- **Defaults:** Genesis A/B/C → west/south/east, which sit left to right like the Genesis buttons.
- Any connected pad controls player 1. Hot-plugging works.

### ROM handling
On start the game looks for a matching ROM by SHA-1, in this order:
1. the command-line path;
2. `<settings>/roms/`;
3. next to the exe: `roms/`, `rom/`, the exe folder itself, `../rom/`.

If none is found, it shows a welcome message and a file picker. A verified ROM is copied to `<settings>/roms/desert.bin`.

**Settings folder:** `%APPDATA%\DesertStrikeHD\DesertStrikeHD\` on Windows.

## Controls (defaults)
| Genesis | Keyboard | Gamepad |
|---|---|---|
| D-pad | Arrows / WASD | D-pad / left stick |
| A | J / Z | West (X on Xbox, □ on PlayStation), left shoulder |
| B | K / X | South (A on Xbox, ✕ on PlayStation) |
| C | L / C | East (B on Xbox, ○ on PlayStation), right shoulder |
| Start | Enter | Start |

| Host | Keyboard | Gamepad |
|---|---|---|
| Save / load state | F5 / F7 | — |
| Fullscreen | F11 or Alt+Enter | — |
| Fast forward (hold) | Tab | Right trigger |
| Pause | P | — |
| Quit | Esc | — |

## Building
- **Windows, Visual Studio or MinGW:** `cmake -S . -B build`, then `cmake --build build --config Release`. SDL3 is downloaded automatically if `third_party/SDL3` is missing.
- **Cross-compiling from Linux:**
  ```
  cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DMINGW_ROOT=/path/to/llvm-mingw
  ```
- **Lockstep suite:** `tools/lockstep_suite.sh <build dir>`.
- **Host test mode:**
  ```
  DesertStrikeHD rom.bin --frames N --inputs script.txt --shot-every N dir
  ```
  With `SDL_VIDEO_DRIVER=offscreen` and `SDL_AUDIO_DRIVER=dummy`, it runs headless and still draws every frame.

## Fixes after first Windows test
- **Garbled EA logo and intro** (reported by the user). On real hardware, reading or writing the VDP data port cancels a half-written control command. EA's intro code at `$0FE754` relies on this: it sets the low address bits with a single control word and then writes data. The model only cancelled it on status reads, so the next lone word was taken as the second half. The "increment = 2" register write was swallowed and tile data landed in the plane and scroll tables. Fixed in `vdp_write_data` and `vdp_read_data`. The whole attract sequence was checked frame by frame: EA logo, title, credits, story scenes, briefing and map. Lockstep still matches.

## Not yet verified
- Real-time feel and audio on actual Windows hardware. The exe was built and checked statically; it hasn't been run on Windows yet.
- Whether the music sounds right by ear. It was checked by spectrum and timing only.

## Next (step 4)
1. A settings menu in the game, with a gamepad-navigable rebinding screen on top of `controls.ini`.
2. **Widescreen:** find the game's view and culling routines and widen them, so objects and map columns exist outside 320 px. Then turn on `ext_left`/`ext_right`.
3. **HD replacement:** hash each 8×8 tile's pattern data and sprite frame, and swap in high-res images from a pack. This needs a GPU or layer-based renderer path.
4. **Performance:** win back what the every-instruction entry points cost, with lazy flags and fewer ticks inside basic blocks. Lockstep must still pass.
5. **Options:** turn off sprite flicker (`sprite_limits = 0`), plus a CRT/scanline filter.
