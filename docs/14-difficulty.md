# Difficulty (prototype in v0.8.1, finished 2026-11-12)

## Why
The remaster's default Smooth speed removes the console's slowdown. On a Genesis the game slowed down in busy fights, which made enemy fire easier to dodge at exactly the hardest moments; at full speed some areas became near-instant deaths. Measured on the user's save before the POW camp (campaign 2): flying into the centre, a tank hits for 80 about every 10 frames and the full 600 armour is gone in about 1.4 seconds.

The goal (the user's brief): keep the speed and the chaos on screen, with no slower projectiles, fewer shots or fewer enemies, but balance the damage the way a modern game would. An Original setting keeps everything as shipped.

## How it works
- **Desert Strike:** every hit, on anything, goes through `$00A70C` (`sub.w d0,$32(a1)`): d0 is the damage, read from the weapon entry's `+8` (a4, the weapon table at `$095F86`, 30-byte entries), possibly doubled at `$00A6D2`; a1 is the target; `$32` is the armour. A `hints.json` patch at `$00A6F6` (after the god-mode test's `btst`, flags untouched) calls `g_damage_hook(target, weapon, damage)`.
- **Jungle and Urban Strike** share one damage routine each (`$04192E` / `$00B7B2`: `sub.w d0,$2C(a5)` at `$041940` / `$00B7C4`), reached from several callers: weapon hits (`$0429BA` / `$00CAAE`, damage from the weapon entry's `+8`, doubled or quartered just before as in Desert), collisions (`$FF122C`/`$FF122E` in Jungle, `$FF125A`/`$FF125C` in Urban) and the `$7FFF` instant kills. The patch sits on the weapon-hit caller only (`$0429B8` / `$00CAAC`, after `movea.l a1,a5`): a5 the target, a4 the weapon entry, d0 the damage. Collisions and instant kills are left as the Genesis has them.
- The hook is NULL in the lockstep tests, so they still match. `DS_DMG_PROBE=1` in `ds_test` logs every weapon hit with its weapon entry (how the hook points and the bullet objects were found).
- `host_damage_hook` (`src/host/main.c`) only changes hits on the player, when the difficulty isn't Original; the game module says what the hit is (`hit_class`, `games/strike_engine.h`):
  - **By weapon type:** a weapon whose projectile (the entry's first long) is the game's bullet object (Desert `$FF25DA`, Jungle `$FF2F44`, Urban `$FF4F20`: the guns, AA guns and tanks) is scaled by `bullet`; everything else (missiles, rockets; Desert's crashes) by `missile`. In Desert the entry must also be in the weapon table.
  - **Per-second cap:** the armour lost in the last 60 frames can't exceed `cap` × maximum armour. A hit over the cap is cut down (to 0 if the cap is already reached).
  - A hit of the whole maximum armour or more is never scaled (instant kills stay instant).

| | bullets | missiles and rockets | cap per second |
|---|---|---|---|
| Recruit | 35% | 60% | 15% of max armour |
| Remastered (default) | 55% | 80% | 25% |
| Original | 100% | 100% | none, plus Game Speed: Original |

Maximum armour: Desert 600, Jungle and Urban 1000 (the cap scales with it). Jungle and Urban collisions (15 a bump in Jungle) are not weapon hits and stay as they are.

## Measured (POW camp, flying into the centre and hovering)
| | first hit to no armour |
|---|---|
| Original | 1.4 s |
| Remastered | 4.3 s |
| Recruit | 7.1 s |

Test: load `balance_test/pow_camp_before.state` (the user's save) with `--key 900:F7`, inputs keyed to the save's frames (about 36186), `DS_DMG_LOG=1` logs every hit ("hit 80 -> 44 (bullet, last second 120 of 150)") and every armour drop. Urban, seed 33 inputs: "hit 5 -> 3 (bullet ...)", armour 1000 -> 997.

## Choosing it, and saves
- **New Game** (from the Continue / Load / New Game screen) opens the difficulty screen in all three games, Remastered on top. Esc goes back to the game picker. Picking Original also sets Game Speed to Original (the slowdown); the others set Smooth.
- **Each save keeps its difficulty:** byte 15 of the state header (after the 7-byte game id; 0 in older states) holds difficulty + 1. Loading a state, Continue included, switches to the save's difficulty; an older state keeps the current one. The Load State grid shows each save's difficulty under its time.
- `g_difficulty` is the difficulty being played; `g_set.difficulty` (settings.ini) is the one a New Game starts with and preselects, the last one chosen. **Settings > System > Difficulty** changes the game being played (and the next New Game).

## Threat markers
- **Settings > System > Threat Markers** (on by default; never in Original): a red arrow at the edge of the picture toward each enemy gun or missile site that is off screen but within reach (about a screen and a half), brighter and larger the closer it is, with a slight flicker.
- The threats are the minimap's (`watch_threats`: the pause map's enemy entries, not pick-ups, rescues or numbered objectives), now with each unit's live position: `game_mm_mark.live/lx/ly` (`games/strike_minimap.c`). The pause map (and so the minimap) places units where they started; Desert's unit entries hold the unit's current position at `+$E` / `+$12` and its object at `+4` once spawned; Jungle and Urban's big entries point to the object. Only spawned units get a marker (a unit not yet in play can't shoot).
- Screen position: `ext_left + x - camera x`, `y - camera y`, the camera being the ground layer's `+$4A` / `+$48` (`game->camera`). Checked against a tank on screen (`DS_THREAT_DEBUG=1` also frames the on-screen ones and the player's ground point; `DS_THREAT_WARP=1` moves the Desert helicopter next to the first threat, test only).

## Low armour warning
- **Settings > System > Low Armor Warning** (on by default): at a quarter of the maximum armour or less, the HUD's armour bar pulses red (1.6 beats a second, a glow around the bar). With the status HUD off, a small armour bar appears on its own in the HUD's corner while the armour is low.
- With Amiga Extras on and the Amiga disks added, the Amiga's computer voice says "Warning" (fx 25, event 1007 `low armour`, in all three games) once each time the armour drops below the quarter; it re-arms above 40% or with a new life. The Sound Lab can change or turn it off like the other events.

## Strafe speed (2026-10-26)
A tester asked whether strafing is slow on the Genesis too. It is: the flight model in use during missions (`$00C626`, when `$FF1088` >= 2) turns the strafe state into sideways velocity with `muls.w #$80` at `$00C74C`, against `#$160` for forward flight, and halves it again while turning. Measured: about 0.54 px a frame strafing against 2.6 flying forward. **Settings > System > Strafe Speed** (Original, Fast 1.5x, Faster 2x) scales that constant (`g_strafe_mul`) and the other flight model's strafe thrust (`$00CA4E` / `$00CA66`, 0xC00, `g_strafe_speed`); measured 0.54 / 0.80 / 1.07 px a frame. The launcher background and the graphics extractor always use the original.

**Strafe and Hellfire together (issue #9):** a Hellfire fires on the *release* of a short A tap (pressed under 16 frames); a long hold strafes. While the strafe button holds A, pressing A sends: release 1 frame, press 3 frames, release (fires), then hold again (`input_pad_state`). No 6-button pad is needed.
