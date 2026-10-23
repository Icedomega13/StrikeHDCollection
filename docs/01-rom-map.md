# Desert Strike HD — Step 1: ROM analysis

## ROM identity
| | |
|---|---|
| File | Desert Strike - Return to the Gulf (USA, Europe) |
| Size | 1,048,576 bytes (8 Mbit) |
| SHA-1 | `d7e7d8c358eb845b84fb08f904cc0b95d0a4053d` |
| CRC32 | `67A9860B` |
| Serial | `GM T-50326 -00` |
| Header checksum | `$D9B1` (verified) |

The recompiled build must check the SHA-1 at startup and refuse other dumps.

## Vectors
- Reset: `$000200` (standard TMSS + VDP/Z80 init, then `jsr $0FE300`).
- V-blank (IRQ 6): `$0089B8`. It calls the sound update at `$F058` (unless `$FF1C70` is set), increments the frame counter `$FF1C6C`, then does `jmp $FF1C72`.
- H-blank / external IRQ: point at `$000080`, which is an `RTE` inside the vector table. Only V-blank does real work.
- **RAM trampoline at `$FF1C72`:** the game writes `jmp abs.l` there at runtime and swaps the target. Known targets: `$3C50`, `$7ECA`, `$89D4`, `$89D6`, `$9206`, `$FF86A`. The recompiler handles this with a dispatch on the long at `$FF1C74`.

## Code map
- **341 functions, ~13,000 instructions, ~54 KB of code** (5% of the ROM). Everything else is graphics, maps and sound data.
- Code lives in `$0200–$11FFF` and `$F7000–$FFFFF`. The end-of-ROM block (`$FE000+`) contains boot and menu helpers, joypad reading (`$FE392`) and a heavy VDP routine (`$FE4CE`).
- 18 jump tables, all resolved:
  - 17 are `jmp 0(pc,Dn.w)` followed by a list of `bra.w` entries.
  - One is a long-offset table at `$F69A`, relative to `$F058`, with 48 entries (sound command dispatch). It is recorded in `tools/hints.json`.
- **0 unresolved indirect jumps, 0 overlapping decodes.**

## Dynamic validation
A Musashi-based harness (`harness/`) boots the ROM with a minimal VDP, pads and Z80 bus stub, and logs every executed PC.
- 12,000 scripted frames (menus, briefing, take-off, flying over the Gulf) executed **61% of the static code**.
- **Zero executed addresses fell outside the static map.** The only RAM execution is the `$FF1C72` trampoline.
- `tools/render_vdp.py` renders the harness's VDP dumps. The main menu and in-game isometric scrolling both render correctly.

## Hardware access
| Device | Sites | Notes |
|---|---|---|
| VDP `$C00000/4` | 221 in 37 functions | Mostly via absolute addresses; DMA used for tiles/sprites |
| Pads `$A10003/5/9/B` | 21 in 4 functions | 3-button protocol, TH toggling |
| Z80 bus/reset `$A11100/200` | 21 in 6 functions | |
| Z80 RAM `$A00000` | 11 in 4 functions | Driver upload + mailbox |

### Z80 / sound
Two Z80 programs:
1. **`$F8000`, 8 KB.** Copied by `$F7670`, called from the front end at `$8702` and `$8938`. It starts with `LD SP,$1F75 / JP $026C`. Commands are sent as one byte to `$A01FFF` via `$F76D6`.
2. **`$11020`, 0x8D8 bytes.** Copied by `$10FD0`, called from the sound routine `$F058`. The 68000 writes commands to a mailbox at `$A000BE–$A000C3` via `$F4FC`.

The 68000 runs the music and effects sequencer every frame (`$F058`). The Z80 drivers handle the low-level chip writes.

**Plan:** emulate the Z80 (an interpreter core is cheap at 3.58 MHz), plus YM2612 (Nuked-OPN2 or ymfm) and SN76489. Static recompilation is not worth it for 10 KB of Z80 code.

## Plan for step 2: the recompiler
1. **Recompiler front end:** reuse `tools/trace.py`'s CFG. Emit one C function per 68000 function, with basic blocks as labels.
2. **Code generation:** a CPU state struct (D0–D7, A0–A7, SR/CCR, USP), and macros for each EA mode and condition-code calculation. Generate flags lazily, only when a later instruction reads them.
3. **Calls and returns:** `bsr/jsr` become C calls when the callee is a well-formed function. Any `rts` that the game manipulates (stack pop, push-and-return tricks) is detected and falls back to dispatch.
4. **Interrupts:** V-blank is triggered at frame boundaries. Main-loop `vsync` wait loops are found and turned into "yield to frame" calls.
5. **Validation:** run the harness (Musashi) and the recompiled build in lockstep on the same inputs and compare RAM, VRAM and registers each frame.
6. **Runtime:** SDL3, an accurate VDP renderer, then the enhancement layers:
   - widescreen by rendering extra columns of the playfield;
   - HD tile/sprite replacement by hashing tile data;
   - rebindable keys and gamepads through SDL.

## Source layout (working tree)
```
rom/              user-supplied ROM (never distributed)
tools/m68k.py     decoder helpers (Capstone for text, opcode bits for control flow)
tools/trace.py    recursive-descent tracer -> out/codemap.json, out/listing.asm
tools/hints.json  manual control-flow annotations
tools/render_vdp.py  VDP state -> PNG
harness/          Musashi reference runner (coverage, VDP dumps, future lockstep oracle)
third_party/      Musashi (MIT), Capstone (BSD)
```
