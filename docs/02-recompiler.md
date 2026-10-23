# Desert Strike HD — Step 2: the 68000 → C recompiler

## Result
The recompiled build matches the Musashi reference exactly in long runs.
- Every frame compared: all CPU registers (D0–D7, A0–A7, SR), stopping PC, and CRC32s of RAM, VRAM, CRAM+VSRAM+VDP registers and Z80 RAM.
- Runs that match:

| Run | Frames | What it covers |
|---|---|---|
| Menu script | 1,500 | Boot, EA logo, title, menus, password screen |
| Gameplay script | 15,000 | Briefing, frigate take-off, flying to the coast |
| Random play, 3 seeds | 20,000 each | Flying, all fire buttons, pausing/map screens |

- Combined coverage of these runs is **78% of all traced instructions**. The rest is content not yet reached: later campaigns, options, game over.
- Speed: ~1,000+ frames/s headless in a Release build, before any optimization.

## How to build and test
```
python tools/trace.py          # ROM -> out/codemap.json
python tools/recomp.py         # codemap -> generated/*.c
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
build/ds_ref  rom/desert_strike.bin 15000 harness/inputs_gameplay.txt out/ref
build/ds_test rom/desert_strike.bin 15000 harness/inputs_gameplay.txt out/rc
python tools/lockstep.py out/ref/frames.log out/rc/frames.log
```
The `out/ref` and `out/rc` folders must exist first. `generated/` is git-ignored because it is derived from the ROM.

## Design

### Shared hardware model (`src/hw/genesis.c`)
Both the reference harness and the recompiled build use the same bus, VDP, pad, and Z80-bus model. Any difference between them can only come from the CPU, so a mismatch always points at the recompiler.

### Instruction-count time
Hardware time advances one step per executed 68000 instruction:
- 20,000 instructions per frame;
- V-blank starts at instruction 17,000 of each frame.

Both sides call `gen_instr_tick()` once before each instruction, so the V-blank interrupt is taken at exactly the same instruction boundary.

The reference needed one small addition to Musashi, `m68k_take_irq_now()`. Musashi's normal `m68k_set_irq()` only checks for interrupts at the next `m68k_execute()` call, which is too coarse for lockstep.

Real-time pacing for the PC build will come from the host's frame timer, not cycle counts. The game syncs on V-blank, so this only affects slowdown, which can be removed.

### Code generation (`tools/recomp.py`)
- **Functions:** each traced 68000 function becomes `void f_XXXXXX(uint32_t entry)`. Its body is everything reachable without following calls.
- **Branches:**
  - inside the function: `goto`;
  - to another function's entry: tail call;
  - anywhere else: go through the dispatcher.
- **Calls:** `jsr`/`bsr` push the real return address, call the C function, then check `R.pc` (set by `rts`). If the game manipulated the stack and the return address differs, the C stack unwinds back to `cpu_run()`, which dispatches the new address. Stack tricks therefore stay correct without special cases.
- **Mid-function entry:** a function can be entered partway through (after unwinding, or from computed jumps) via a `switch (entry)`.
- **Computed jumps:** the 18 jump tables become C `switch` statements.
- **RAM trampoline:** `$FF1C72` (`jmp abs.l`) is handled by a tiny interpreter in `cpu.c`.
- **Flags:** computed exactly, matching Musashi. That includes divide overflow, which sets only V, the ASL overflow rule, and ROX/RO with a count of zero.
- **Other details that mattered:**
  - Absolute short addresses sign-extend to a full 32-bit register value (e.g. `lea $FFF1.w,a0` → `$FFFFFFF1`).
  - CLR does not read the destination, matching Musashi.

### Interrupts
`TICK(pc)` precedes every instruction. When the shared model signals V-blank, `cpu_irq()` pushes PC and SR, raises the mask to 6, dispatches the vector at `$78`, and checks that `RTE` came back to the same PC.

## Next steps
1. **Performance:** compute flags only when a later instruction reads them, and skip per-instruction ticks inside blocks with no hardware access. Re-verify with lockstep after each change.
2. **SDL3 host:**
   - window and input;
   - a real-time VDP renderer (the Python renderer is the reference);
   - Z80 + YM2612/PSG audio (Nuked-OPN2 / ymfm).
3. **Enhancements:**
   - widescreen by drawing more of the playfield;
   - HD tile/sprite replacement keyed by tile-data hash;
   - rebinding and gamepads via SDL.
4. **Coverage:** longer scripted playthroughs (later campaigns, game over, options) to push lockstep coverage toward 100%.
