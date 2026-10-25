# Difficulty (prototype, v0.8.1)

## Why
The remaster's default Smooth speed removes the console's slowdown. On a Genesis the game slowed down in busy fights, which made enemy fire easier to dodge at exactly the hardest moments; at full speed some areas became near-instant deaths. Measured on the user's save before the POW camp (campaign 2): flying into the centre, a tank hits for 80 about every 10 frames and the full 600 armour is gone in about 1.4 seconds.

The goal (the user's brief): keep the speed and the chaos on screen, with no slower projectiles, fewer shots or fewer enemies, but balance the damage the way a modern game would. An Original setting keeps everything as shipped.

## How it works
- Every hit, on anything, goes through `$00A70C` (`sub.w d0,$32(a1)`): d0 is the damage, read from the weapon entry's `+8` (a4, the weapon table at `$095F86`, 30-byte entries), possibly doubled at `$00A6D2`; a1 is the target; `$32` is the armour.
- A `hints.json` patch at `$00A6F6` (after the god-mode test's `btst`, flags untouched) calls `g_damage_hook(target, weapon, damage)`. NULL in the lockstep tests, so they still match.
- `host_damage_hook` (`src/host/main.c`) only changes hits on the player (`[$FF251C]`), in Desert Strike, when the difficulty isn't Original:
  - **By weapon type:** a weapon whose projectile (`+2`) is the bullet (`$25DA`, the guns, AA guns and tanks) is scaled by `bullet`; everything else (missiles, rockets, crashes) by `missile`.
  - **Per-second cap:** the armour lost in the last 60 frames can't exceed `cap` × maximum armour. A hit over the cap is cut down (to 0 if the cap is already reached).

| | bullets | missiles and rockets | cap per second |
|---|---|---|---|
| Recruit | 35% | 60% | 15% of max armour |
| Remastered (default) | 55% | 80% | 25% |
| Original | 100% | 100% | none, plus Game Speed: Original |

## Measured (POW camp, flying into the centre and hovering)
| | first hit to no armour |
|---|---|
| Original | 1.4 s |
| Remastered | 4.3 s |
| Recruit | 7.1 s |

Test: load `balance_test/pow_camp_before.state` (the user's save) with `--key 900:F7`, inputs keyed to the save's frames (about 36186), `DS_DMG_LOG=1` logs every hit ("hit 80 -> 44 (bullet, last second 120 of 150)") and every armour drop.

## Choosing it
- **New Game** (from the Continue / Load / New Game screen) opens the difficulty screen, Remastered on top. Esc goes back to the game picker. Picking Original also sets Game Speed to Original (the slowdown); the others set Smooth.
- **Settings > System > Difficulty** changes it at any time.
- For now it's one setting, not stored per save, and Desert Strike only. Jungle and Urban Strike need their own damage hook point.

## Not done yet (from the design)
- Edge-of-screen markers for enemies shooting at you from off screen (from the minimap threat code).
- A low-armour warning (the Amiga "Warning" voice and a pulsing armour bar).
- The difficulty stored in each save.
