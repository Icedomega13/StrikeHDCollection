# Plan: the HD pause menu, settings and cheats

## Status (2026-10-22)
**Built**, apart from the cheats, which the user asked to do last; their address map is in `docs/06-cheat-addresses.md`.
- **Code:** `src/host/menu.c` (screens), `src/host/ui.c` (drawing, with the Chakra Petch font via stb_truetype), `src/host/settings.c` (`settings.ini`).
- **Main entries:** Resume, Save State, Load State, Settings, Controls, Restart Game, Quit to Desktop.
- **Save/Load:** 9 slots (Quick + 1–8), each with a 160×112 thumbnail and a date.
  - State file v2: `DSHDST02` + game id + time + thumbnail + state. Version-1 files still load.
  - F5/F7 use the Quick slot.
- **Settings tabs:**
  - Display: window mode, exclusive-fullscreen resolution, VSync, FPS.
  - Picture: aspect 4:3 / square / stretch; scaling sharp / integer / smooth / pixelated; scanlines; sprite flicker.
  - Audio: master volume, FM and PSG levels, mute when unfocused.
  - System: fast-forward speed, pause when unfocused, open the settings folder.
- **Controls:** a 14-action × 4-column table (2 keyboard and 2 gamepad bindings per action), with capture, clear and reset to defaults.
  - Binding an input removes it from any other action.
  - The menu action always keeps a binding.
- **Opening it:** Esc or the pad's Back/View button. Quit moved into the menu, with a confirmation.
- **Tested headless:** with injected keys and full-window screenshots (`--key T:NAME`, `--screen-shot T:path`).
- **Not built yet:**
  - "authentic slowdown": needs a variable instructions-per-frame setting;
  - widescreen and HD-pack options: these arrive with their features.


This is a PC-side menu drawn over the game. The game's own pause/map screen (Start) stays as it is. The menu opens with:
- **Esc** on the keyboard;
- **Back/View/Select** on a gamepad (the button left of the guide button).

Both are rebindable. The emulated game is frozen between frames while the menu is open; the frame-stepping design makes this free. Sound fades out.

The menu is built for a gamepad first:

| Input | Action |
|---|---|
| D-pad / stick | Move |
| A / Cross | Select |
| B / Circle | Back |
| LB / RB | Switch tabs |

The mouse and keyboard also work. The same menu code becomes the collection launcher's settings screens later.

## Top level
1. **Return to Game**
2. **Save State** — slots 1–8, each with a thumbnail and a date
3. **Load State** — same slot view
4. **Settings**
5. **Cheats**
6. **Controls** — shortcut to Settings → Controls
7. **Quit to Collection** (once the launcher exists) / **Quit to Desktop**

## Settings
| Tab | Options |
|---|---|
| **Display** | Window mode (windowed / borderless fullscreen / exclusive fullscreen); resolution (exclusive mode) or window size; monitor; VSync; frame-rate limit |
| **Picture** | Aspect: 4:3 original / pixel-perfect square / stretch / widescreen 16:9 / ultrawide 21:9 and 32:9. Scaling: sharp / integer only / smooth. Filters: none / scanlines / CRT. HD sprite pack on/off. |
| **Game feel** | Sprite flicker: original / off. Slowdown: none (current) / authentic console speed. |
| **Audio** | Master volume; FM / PSG balance; output device; mute when unfocused |
| **Controls** | Rebinding for every Genesis button and hotkey, keyboard and gamepad (several per action); stick deadzone; button prompts follow the pad type; reset to defaults |
| **System** | Save-state slot count; fast-forward speed; show FPS; open the settings folder |

Settings are saved to `settings.ini` next to `controls.ini`. Every option applies immediately, without a restart, where possible.

## Cheats
The cheat set fits Desert Strike's gameplay: fuel, armour, three weapon types, lives, copilots and campaigns. Each cheat is a toggle, and every cheat is off by default.

| Cheat | Effect |
|---|---|
| **God mode** | Armour never drops (no damage, no crash from damage) |
| **Unlimited fuel** | Fuel stays full |
| **Unlimited guns** | Chain gun ammo stays full |
| **Unlimited Hydras** | Hydra rockets stay full |
| **Unlimited Hellfires** | Hellfire missiles stay full |
| **Unlimited lives** | Lives never decrease |
| **Max lives** | Sets lives to the maximum once |
| **One-hit kills** | Enemies destroyed by any hit (if the damage code allows it cleanly) |
| **Campaign select** | Jump to any of the four campaigns, using the game's own password system |
| **Best copilot** | Always fly with the top copilot (better aim and winch) |
| **No slowdown / no flicker** | Shortcuts to the Game feel settings |

**How cheats work:**
- Most cheats write a value into game RAM at the frame boundary, between frames. That keeps them deterministic and safe with save states.
- God mode and one-hit kills may instead hook the game's damage routine. The recompiled C makes it easy to wrap one function cleanly.
- RAM addresses are found with a small RAM-search tool. It runs the game with lockstep inputs and finds the bytes that track the HUD values. The step-3 runs already show the starting values to search for: GUNS 1178, HYDRAS 38, HELLFIRES 8, FUEL 100, ARMOR 600, LIVES 3.
- Cheats are defined per game in `games/desert/cheats.json`: name, address, size, value, kind. Jungle and Urban Strike can then reuse the same menu with their own tables.
- An "active cheats" icon shows in the corner while any cheat is on.

## Build order
1. **Menu framework:** an overlay renderer with text, panels, focus and transitions, and gamepad navigation. Settings save/load.
2. **Return / Save / Load / Quit**, and the Display, Picture and Audio settings.
3. **Controls tab:** rebinding with "press a key/button" capture.
4. **RAM-search tool**, the address map, then the cheats tab.
5. **Widescreen and HD-pack options:** the settings appear as each feature lands (step 4 work).
