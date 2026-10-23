# Cheats

## Status: done (2026-10-22), in all three games
Pause menu → **Cheats**:
- God Mode, Unlimited Fuel, Unlimited Ammo, Unlimited Lives.
- **Campaign** (1–4 / 1–9 / 1–10) and **Start Campaign**.

Cheats aren't saved, so every session starts clean, and they reset when you switch games.

How they work (`strike_cheats()` in `games/strike_engine.h`): RAM writes at the frame boundary, only while the playfield is up.
- **God mode:** armour = max each frame.
- **Fuel:** fuel = max.
- **Ammo:** each weapon is refilled to its learnt full load, the highest count seen since the campaign started.
- **Lives:** held at their value when enabled (or any higher value gained since).
- **Campaign select:** sets `g_start_campaign`, which a recompiler `imm` patch puts into the campaign variable when a new game is set up. Desert `$002EC2` → `$FF1098`; Jungle `$038C90` → `$FF10B0`; Urban `$0018CE` → `$FF10C0`. "Start Campaign" reboots the game; a new game then starts at that campaign.

Addresses for all three games are in each module's `strike_hud_cfg` (see `docs/10-status-hud.md`). The rest of this file is the original Desert research.

# Desert Strike cheat map (original research)

## How these were found
- Published Game Genie codes, decoded with `tools/gamegenie.py`. The decoder is checked: every code's address holds the original value its description implies.
- RAM traces from `ds_test --ram-every N`.
- The disassembly in `out/desert/listing.asm`.

The game code is compiled into the exe, so **Game Genie patches to instructions do nothing**. Only patches to data the game reads from ROM at runtime take effect. Cheats therefore work by writing RAM at the frame boundary, or by hooking a recompiled function.

## Live RAM
| What | Address | Notes |
|---|---|---|
| Fuel (current) | `$FF10AA` w | Burned by `$FF103E` (=2) at `$00689C`, only in flight state `$FF1A20 = $12` (over land: the coastline test at `$006836`). It doesn't burn over the sea near the frigate. |
| Fuel capacity | `$FF10BC` w | 100, set at `$004900`; refuel copies it into `$FF10AA`. |
| Fuel burn rate | `$FF103E` w | 2, set at `$0049BC`. 0 = no fuel use (same as GG `ABET-AAF8`). |
| Armour (current) | player object `+$32` = `$FFACDC` w | Player object pointer is at `$FF251C` (= `$FFACAA` so far). |
| Armour max | `$FF1040` w | 600, set at `$0049CE`. |
| Lives | `$FF10B6` w | `subq` at `$006AA4` (death), `addq` at `$00DC8E` (red-cross box). Start value from `$FF100E`. |
| Gun rounds | `$FFAE9C` w | Weapon records are 0x40 bytes apart; the count is at `+$14`. Decremented at `$00D55A`. |
| Hydras | `$FFAEDC` w | |
| Hellfires | `$FFAF1C` w | |
| Ammo capacities (ROM) | `$095F8C` (1178), `$095FAA` (38), `$095FC8` (8) | Read these for "refill to max". |
| Flight state | `$FF1A20` w | Jump table at `$006614`: `$0E` = near the frigate, `$12` = flying over land. |
| Campaign at start | immediate at `$002EC6` | GG `AE1A-AAGG` = campaign 2. Needs a RAM variable or password path. |

## Planned cheats
| Cheat | How |
|---|---|
| Unlimited fuel | Each frame: `$FF10AA` = `$FF10BC` |
| God mode | Each frame: armour (`[$FF251C]+$32`) = `$FF1040`. If one-frame deaths slip through, hook the damage routine instead. |
| Unlimited lives | Hold `$FF10B6` at its value when enabled, never lower |
| Unlimited ammo (×3) | Each frame: counts = ROM capacities |
| Campaign select | Find the campaign variable written near `$002EC6`, or go through the password system |
| Best copilot | Copilot index at `$FF107E`? Stats tables at `$000374`/`$00037E`. To check. |

Published codes, for reference: cyberly.org "Desert Strike" Genesis cheats page (43 Game Genie codes).
