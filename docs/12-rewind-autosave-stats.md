# Rewind, autosave, rumble and stats

Quality-of-life features handled entirely by the host. They read the game's status (the same values as the status HUD and the minimap) and never change the game's code path. Settings → System has a switch or slider for each: **Rewind**, **Autosave**, **Rumble**.

## Rewind
- **Hold the left trigger, or Backspace** (action "Rewind (hold)" under Controls), to step back through roughly the last 30 seconds. Steps run at 60 a second and each covers 2 game frames, so the replay moves at twice normal speed. A REWIND badge shows at the top; sound is muted until you let go.
- Every 2nd frame the state is saved into `src/host/rewind.c`:
  - The newest state is held whole; each older one is kept as the XOR with its successor, run-length coded. A state is about 140 KB, but a step is typically a few KB.
  - The buffer is capped at 48 MB, and the oldest steps are dropped first.
  - Stepping back applies one delta, so it costs almost nothing.
- Each step loads the state and renders it straight away (`on_vblank`), so widescreen, the HD pack and the HUD all work while rewinding.
- The history is cleared when a state is loaded, the game restarts or the game changes.
- Test: `--rewind-at <tick>:<steps>`.

## Autosave
Three rolling slots, **Auto 1–3**, in a fourth row of the Load State page. Auto 1 is the newest. You can't save into them by hand. A save happens when:
- **a mission starts**: a new level, after 2 seconds on the playfield;
- **a mission objective is completed**: its units (the minimap's markers for it) have been gone for 45 frames while the objective is still "available".

The message names the objective ("Autosaved: 1.RADAR SITES complete"). Loading a state or rewinding re-baselines the watcher, so that alone never triggers an autosave.

Objectives with no units on the map (some rescues) are not detected. Files: `states/<game>_auto1..3.state`.

Test: `--poke <tick>:<RAM addr>:<word>`, for example zeroing the radar sites' alive words (`FFF77C` and `FFEC3C` in Desert's first campaign) triggers the objective autosave.

## Rumble
Strength 0–100 % (default 70; 0 turns it off). It goes to every connected pad through `SDL_RumbleGamepad`, and never while a menu is open.

| Event | Motors |
|---|---|
| Gun round fired | light buzz, 45 ms |
| Hydra | medium, 90 ms |
| Hellfire | strong, 180 ms |
| Hit | scaled by the armour lost |
| Life lost | full, 0.7 s |

## Stats
Kept per game in `stats.ini` in the settings folder (`src/host/stats.c`). The file is saved every minute in flight, when leaving a game and on quit. Counted from frame-to-frame changes in the HUD values:
- time flying;
- missions started;
- furthest campaign;
- objectives completed;
- rescued/unloaded (load going down without a life lost);
- winched aboard (load going up);
- rounds fired per weapon;
- armour lost;
- lives lost.

The pause menu has a **Stats** page, and each game card on the game picker shows a career line. Kills and score aren't counted yet; their RAM locations haven't been mapped.

## Continue / Load Game / New Game (2026-10-23)
Picking a game in the game picker that has any saves opens a start screen: **Continue** (the state written when the game was last left mid-play, `states/<game>_resume.state`, or else the newest save of any kind, shown with its thumbnail and time), **Load Game** (the pause menu's Load State page) and **New Game** (with the Amiga intro for Desert, when on). Esc / B goes back to the picker. The resume state is written when the game is left (Quit, Switch Game, closing the window) if the playfield was reached that session. Settings > System > Continue Prompt turns the screen off. Loading a state no longer counts the level it lands in as a new mission (no "mission start" autosave after Continue). Tests: `DS_PROMPT=1` shows it under `--frames`.
