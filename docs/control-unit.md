# JK16 Control Unit

This document covers the control unit's own hardware — the ROM structure, timing, and branch-decision circuits that generate and sequence the 48-bit control words described in `microcode.md`. For what those control words actually mean and do, see `microcode.md`; for how they move data around, see `datapath.md`.

## Physical Structure

The control ROM is **6 × 8-bit ROM chips**, each with 12-bit address lines, wired horizontally so they share a single 12-bit address bus and together produce one 48-bit output word per address — 39 bits are wired to control signals, the remaining 9 are unused/reserved.

**Address formation**: the 12-bit control-store address is the 6-bit opcode (from IR) concatenated with the 6-bit microstep counter value:

```
address[11:6] = opcode
address[5:0]  = microstep counter
```

This gives every opcode a fixed 64-slot block in the ROM, regardless of how many microsteps that instruction actually uses (see `microcode.md` for the per-instruction step counts, ranging from 2 to ~82 via microloops).

## Microstep Counter

- 6-bit, **loadable**, with a reset input.
- Normally increments once per clock cycle to advance through an instruction's microsteps.
- Can be **directly loaded** with a 6-bit target value (`uSTEP_LOAD[5:0]`) when `uSTEP_LOAD_EN` is asserted in the current control word — this is how microbranches and microloops (e.g. the `MUL`/`DIV` shift-add loops) are implemented, entirely within the control ROM's own output driving its own next address.
- **Reset** (`uSTEP_RESET`) returns the counter to 0 at the end of every instruction, re-entering the shared fetch microstep for the next instruction.
- **Timing**: unlike every other component in the design (positive-edge triggered), the microstep counter is **negative-edge triggered** — it updates on the falling edge of the clock, so that its output (the next control word) is stable and has settled *before* the following rising edge latches all the positive-edge components (registers, ALU inputs, etc.) that the control word drives. Since the counter is a positive-edge part by default, this is achieved by feeding it an inverted clock.

## Opcode + Microstep → Control Word

On every clock cycle, the concatenation of `{opcode, microstep}` addresses the control ROM, and the ROM's 48-bit output becomes the control word driving that cycle's datapath activity (register reads/writes, ALU operation, memory access, PC behavior, and so on). Because the ROM is purely combinational lookup (no sequencing logic beyond the address itself), the entire instruction set's behavior — all 64 opcodes — is defined by nothing but the contents of this ROM, addressed by opcode and microstep.

## Branch Decision Circuits

Two small dedicated circuits sit alongside the control ROM and gate program-level jumps (as opposed to the microstep counter's own intra-instruction microbranching described above):

- **Condition Checker**: a combinational circuit that takes the flag selected by `COND_FLAG[1:0]` (Z/C/N/V) and a required `COND_VALUE`, and outputs 1 only when the flag's actual current value matches the required value.
- **Jump Controller** (`JUMP_SEL`): decides whether the Condition Checker's output is consulted at all. High (`JUMP_SEL=1`) forces the branch unconditionally; low (`JUMP_SEL=0`) makes the branch depend entirely on the Condition Checker's result.

Together these two signals feed into the same `uSTEP_LOAD_EN` mechanism used for ordinary microloops — a conditional program jump (`JZ`, `JC`, etc.) is implemented as a microbranch that's conditionally skipped, not as separate hardware from the multiply/divide loops.

## Decrement Loop Counter

A hidden counter (distinct from the microstep counter) used internally by block-memory instructions (`MEMCPY`, `MEMSET`) to track how many words remain to be copied/set. It can drive the flag register directly via `FLAGS_LOAD_SEL=DEC_COUNT_OUT`, letting the control unit test "loop finished" as an ordinary flag condition rather than needing dedicated loop-control hardware.

## Clock Generation

- Crystal oscillator → **Frequency Divider** (8-bit counter) → **Frequency Selector** (8-to-1 mux) → distributed processor clock.
- User-adjustable clock speed via the Frequency Selector — a convenience feature for observing execution step-by-step in the simulator, not a functional requirement of the control unit itself.
