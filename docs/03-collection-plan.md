# Strike Remastered Collection HD: plan

## Goal
Grow Desert Strike HD into a collection of all three Genesis Strike games. Each game is a native recompile with the same PC upgrades:
- HD sprites;
- ultrawide;
- full rebinding;
- modern gamepads.

| Game | Genesis release | Status |
|---|---|---|
| Desert Strike: Return to the Gulf | 1992 | Recompiler done, lockstep-exact |
| Jungle Strike | 1993 | Recompiled, lockstep-exact |
| Urban Strike | 1994 | Recompiled, lockstep-exact |

## Status (2026-10-22)
- **Launcher: done.**
  - A random game with a ROM plays its real intro live behind the picker. It's fast-forwarded past the publisher logos while hidden, kept as an in-memory snapshot so later visits start instantly, shown for 32 s, then crossfaded (0.9 s) to a different game.
  - The intro audio plays at reduced volume and fades with the picture.
  - The title is the app icon plus "STRIKE / REMASTERED COLLECTION HD", with a shine sweep and an amber rule.
  - Three game cards sit in the bottom third. A red dot marks the game being shown.
  - Settings → System → **Launcher Intros** turns the live background off.
  - Intro start frames: Desert 360, Jungle 600, Urban 1560 (`att_start_frame` in `src/host/main.c`).
- **Icon:** `res/icon_256.png` is embedded (`tools/embed_fonts.py`). It's used as the window icon and in the launcher title; `res/desertstrikehd.ico` is used for the exe.
- **Installer: built** with Inno Setup 6.7 on the user's PC (2026-10-22): `dist\StrikeCollectionHD-Setup.exe`, 4.6 MB, compiled with no warnings. Rebuild with `installer\build_installer.bat`, or by opening the `.iss` in the Inno Setup Compiler.
  - Per-user install, no admin rights.
  - A ROM page with one file field per game, each SHA-1-checked when you click Next. At least one game is needed; ones already added are counted.
  - Accepted ROMs are copied to `%APPDATA%\DesertStrikeHD\DesertStrikeHD\roms\<id>.bin`.
  - Start menu and optional desktop shortcuts, and a launch-at-finish option.
  - The uninstaller asks whether to delete settings, save states, packs and ROM copies.
  - Not done: `.zip` ROMs and a "scan a folder" button.

The collection opens on an animated launcher titled **"Strike Remastered Collection HD"**:
- **Background:** the games' intro sequences play full-screen in random shuffle.
- **Foreground:** a game picker built for gamepad navigation.

Players supply their own ROMs, as now. The launcher finds each ROM by SHA-1 and shows any game that's missing as "ROM not found".

## Why this is realistic
All three games run on the same EA engine family. Nothing in the pipeline is specific to Desert Strike:
- the tracer;
- the 68000 → C generator;
- the hardware model;
- the lockstep harness.

Each new game mostly needs:
- its own `hints.json`, for jump tables and RAM entry points;
- a trace;
- lockstep runs until it matches.

The Z80 sound drivers are probably close relatives too, and they'll be emulated rather than recompiled either way.

## Decisions to make now, during step 3
These are cheap to do now and painful to retrofit after the PC host exists.

### 1. One game module per ROM
- `recomp.py` takes a game config: ROM SHA-1, hints file, and a symbol prefix (`ds_`, `js_`, `us_`).
- The output goes to `generated/<game>/`.
- Function names gain the prefix (`ds_f_0089B8`). That way all three games link into one executable with no name clashes.
- Each game exports one descriptor:

```c
typedef struct {
    const char *id, *title;
    const char *rom_sha1;
    void (*boot)(void);                  // reset + start at the game's reset vector
    recomp_fn (*lookup)(uint32_t pc);    // that game's dispatch table
    const game_hd_pack *hd;              // per-game HD replacement set
    const game_enhance *enh;             // widescreen hooks, RAM addresses, etc.
} game_module;
```

- Only one game runs at a time. The shared CPU state and hardware model are reset on every game switch, so they don't need to be per-game.

### 2. The game must be able to stop and give control back
Right now `cpu_run()` never returns. Both the launcher and an in-game "Quit to collection" option need the game to hand control back to the host.

- **Plan:** run the recompiled game on its own fiber or thread.
  - The game yields to the host once per frame, inside the frame callback.
  - The host renders, mixes audio, reads input, then resumes the game.
- **Quit:** the host stops resuming that fiber and starts a fresh one.
- **Tests:** the headless lockstep runner is unaffected, because it never yields.

### 3. Snapshots at frame boundaries
- The launcher wants to jump straight to each game's intro, without the EA logo and boot each time. Save states want the same thing.
- **Plan:** take snapshots only at the frame yield point.
  - A snapshot holds CPU, RAM, VDP, Z80 and FM state.
  - The yield point is always a known resume address, so restoring one never needs the C call stack.
- Lockstep must also cover restoring a snapshot and continuing.

## The launcher

### Background: live intros, not video files
The background runs each game's real intro live, through the recompiled code, into an off-screen texture.

| | Live intro (recommended) | Pre-rendered video |
|---|---|---|
| Ships copyrighted footage | No; it's produced from the player's ROM | Yes, unless captured on the player's PC |
| Resolution | Renders at the display resolution, with HD sprites and widescreen | Fixed when encoded |
| Extra dependencies | None | A video decoder |
| Work | Needs snapshots and the fiber switch (above) | Simpler playback |

**Fallback:** if live switching turns out jerky, capture each intro to a local video cache on first launch. The capture comes from the player's own ROM, so it still ships nothing.

**Shuffle behaviour:**
- Start a random game's intro from its intro snapshot.
- When it finishes, or after a time limit, crossfade to another random game. Don't repeat the same game twice in a row.
- Mute the game audio, or duck it under the launcher music.
- **Option:** when a game's card is in focus, the background switches to that game's intro. Pure shuffle is the default.

### Foreground: built for a gamepad
- **Layout:**
  - animated title logo at the top: "Strike Remastered Collection HD" (original artwork, no EA logos);
  - three large game cards along the bottom third.
- **Card in focus:** scales up and shows the year, a short description, and "ROM found" / "ROM missing".
- **Controls:**

| Input | Action |
|---|---|
| D-pad / left stick | Move focus |
| A / Cross | Play |
| B / Circle | Back |
| Y / Triangle | Game settings |
| Start | Collection settings |
| Arrows + Enter / Esc | Keyboard equivalents |
| Mouse | Optional, never required |

- **Prompts:** button icons follow the connected pad (Xbox, PlayStation, Switch), using SDL3's gamepad type detection.
- **Safe area:** 16:9 base layout, kept inside a safe area on ultrawide. On ultrawide, the background fills the full width using the widescreen render.
- **Settings screens:**
  - **Display:** resolution, aspect ratio, HD packs, scanline/CRT filter.
  - **Audio.**
  - **Controls:** global bindings with per-game overrides.
  - **Per game:** password vault and save-state slots.

## Installer, ROM setup, and icon

### Installer (Windows first)
- **Tool:** Inno Setup. It's free, scriptable and builds one `.exe` setup file. An MSIX package can come later if a Store build is wanted.
- **Pages:**
  1. Welcome, using the collection artwork.
  2. Install folder. Default: `%LOCALAPPDATA%\Programs\Strike Remastered Collection HD`, so no admin rights are needed.
  3. **ROMs:**
     - One row per game, each with a Browse button and a "Scan a folder" option.
     - Each file is checked by SHA-1 as it's picked, with a ✓ or ✗ beside it.
     - Accepted dumps can be `.bin`, `.md`, `.gen` or inside a `.zip`.
     - At least one game is required. The others can be added later.
  4. Shortcuts: Start menu, plus an optional desktop icon.
  5. Finish, with an option to launch.
- **Where ROMs go:** verified ROMs are copied to `%APPDATA%\StrikeCollection\roms\`, under fixed names such as `desert.bin`. The original files are never modified.
- **Uninstaller:** asks whether to keep or delete the copied ROMs, save states and settings.

### The same check inside the app
The installer and the launcher share one ROM checker, so there's only one code path. The launcher uses it:
- for portable/zip builds and Linux/Steam Deck, which have no installer;
- through **Collection settings → Manage ROMs**, to add or replace a game later;
- for a missing game's card: pressing A on it opens the "Add ROM" screen. That screen works with a gamepad, including a file browser.

A wrong dump gets a plain message: "This file is a different version of Jungle Strike (CRC xxxx). Supported: …". Known regional variants can be accepted once they're tested.

### Icon and branding
- **Original artwork** for "Strike Remastered Collection HD": a helicopter silhouette with the collection's name. No EA logos and no art lifted from the games.
- **One source file** (SVG or a high-resolution PNG), exported to:
  - a multi-size `.ico` (16–256 px), used for the exe, the installer, the shortcuts and the taskbar;
  - `SDL_SetWindowIcon` for the window;
  - Steam/Deck library images, if a Steam build happens;
  - a Linux `.desktop` icon.
- The launcher's animated logo is built from the same artwork, so the icon and title screen match.

### Release note: the recompiled code
The ROMs supply the game's art, sound and data at runtime. The executable, however, contains the game's code translated to C. Shipping it that way is the norm for recompilation projects (Zelda64Recomp, for example), but it is a legal grey area. **Decision (2026-10-22):** ship the translated code in the executable. Players provide their own ROMs, and for now this is a personal project. Translating and compiling on the player's PC stays a possible option if that changes.

## In-game HD menu
Each game gets the same PC pause menu: return, save/load states, settings, cheats, quit to collection. See `docs/05-pause-menu-plan.md`. Cheats are per-game data tables.

## Order of work
1. **Step 3, Desert Strike playable on PC:** SDL3 host, renderer, audio, input. Build the game-module, fiber and snapshot design above into it from the start.
2. **Step 4, Desert Strike enhancements:** widescreen, HD replacement, rebinding UI, gamepads. These become shared systems with per-game data.
3. **Jungle Strike:** ROM map, trace, hints, lockstep, enhancement data.
4. **Urban Strike:** same as Jungle Strike.
5. **Launcher:** live intro shuffle, picker, settings, and "Quit to collection" from inside games.
6. **Release:**
   - Inno Setup installer with the ROM page;
   - an in-app Manage ROMs screen;
   - the icon set;
   - credits.

   The icon itself can be made at any time. It's worth doing early, so the dev builds already have it.

The launcher comes after the games on purpose, because it needs working intros from at least two of them. A placeholder picker can exist from step 3, to exercise the game switching.

## Open questions
- **Intro end points:** where each intro starts and ends in its ROM. This gets found while mapping each game.
- **Launcher music:** original, or the games' title themes playing through the emulated FM chip.
- **Other versions:** whether to also accept regional variants of each ROM, such as the European releases.
