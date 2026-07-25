# JK16 Datapath

This document describes how data actually moves between components on each clock edge — the buses, muxes, and control points that the microcode (see `microcode.md`) drives. For what each instruction *does*, see `instruction-set.md`; for the high-level component roles, see `architecture.md`.

*(Full gate-level datapath diagram: `docs/images/datapath.png`)*

## Buses

| Bus | Width | Driven by | Read by |
|---|---|---|---|
| Instruction bus | 16-bit | PC | Program ROM (address in), IR (data out) |
| Data/address bus | 16-bit | MAR | Address Decoder → RAM/NVM/GPIO |
| Memory-in bus | 16-bit | RAM / NVM / GPIO (on read) | Writeback Selector, MDR |
| Memory-out bus | 16-bit | Writeback source (on write) | RAM / NVM / GPIO |
| ALU output bus | 16-bit | ALU (combinational) | Special Mul/Div Muxes → Writeback Selector, MAR |
| Writeback bus | 16-bit | Writeback Selector | Register File (A and/or B bank) |

## Instruction Fetch Path

`PC → Program ROM → IR`

- PC drives its own dedicated address bus into Program ROM every cycle it's enabled.
- The fetched word loads into IR (`IR_LOAD`), staying stable through decode/execute.
- For two-word instructions, PC increments again and the second word loads into IMM (`IMM_LOAD`) instead of IR.
- PC itself is loadable: `PC_SEL=1` switches its input from "increment" to "load from ALU output," used by every taken jump/call/return.

## Register Read Path

`Register Bank A / Register Bank B → ALU inputs`

- IR's RA/RB fields (3 bits each) independently select one register from each bank.
- Both banks feed the ALU's two operand inputs directly — every ALU operation reads one operand from bank A and one from bank B (even single-operand ops like `INC`/`NOT` route through this same path, using B7's hidden internal value where the encoding doesn't need a real RB).

## ALU and Special Hardware Path

`Register File → ALU → (Special Mul/Div Mux) → Writeback Selector`

- The ALU is purely combinational — no clock needed to produce `ALU_OUT`.
- `ALU_OP[3:0]` (4 bits from the control word) select 1 of 16 operations.
- The **Special Mul Mux** and **Special Div Mux** sit directly on the ALU-output-to-Writeback-Selector path. During multiply/divide sequences, the mux — driven by the multiplication/division hardware itself, not by the normal control word — silently substitutes `mul_result` or the division quotient/remainder onto this bus in place of the raw ALU output. Everything downstream (the Writeback Selector, `WB_SEL` decoding) is unaware of the substitution.
- Multiplication hardware (shift-and-add) and division hardware (repeated subtraction) both feed their own registers into this substitution point, and both loop via microbranches (see `microcode.md`) rather than dedicated datapath hardware for the iteration itself.

## Writeback Path

`{ALU_OUT, MEMORY, IMM, PC} → Writeback Selector → Register File`

- The Writeback Selector (`WB_SEL[1:0]`) is a 4-to-1 mux choosing which value reaches the register file's write data input.
- `A_RW` / `B_RW` are the actual write-enable signals — independent of `WB_SEL`, they decide *whether* bank A and/or bank B latch the writeback value this cycle, and the RA/RB fields decide *which* register in each enabled bank.
- Because `WB_SEL` and the write-enables are decoupled, the same mux serves arithmetic writeback (`ALU_OUT`), memory loads (`MEMORY`), immediate loads (`IMM`), and return-address saves for `CALL` (`PC`).

## Memory Address Path

`{IMM, ALU_OUT, RA, RB} → MAR_SEL mux → MAR → Address Decoder → RAM/NVM/GPIO`

- `MAR_LOAD` latches a new address into MAR from one of four sources selected by `MAR_SEL[1:0]`: `IMM` (direct addressing, e.g. `LOADA`), `ALU_OUT` (computed addressing, used internally by block-memory ops), `RA` or `RB` (register-indirect, e.g. `LOADR`/`STORER`).
- MAR's output is the sole input to the Address Decoder, which enables exactly one of RAM/NVM/GPIO per the ranges in `memory-map.md`.

## Memory Data Path

`Register/Flags/MDR → MEM_DATA_SEL mux → memory-out bus → memory (on MEM_WRITE)`
`memory (on MEM_READ) → memory-in bus → Writeback Selector or MDR`

- `MEM_DATA_SEL[1:0]` chooses what's being written to memory: `RA`, `RB`, `FLAGS` (for `PUSHF`), or `MDR` (for memory-to-memory copies).
- On a read (`MEM_READ`), the fetched word normally goes straight to the Writeback Selector's `MEMORY` input. The **MDR** is the one exception: it's loaded only from memory and its output only feeds back to a memory write — it exists purely as a buffer so `MOVM`/`MEMCPY`/`MEMSET` can read one address and write another without disturbing any general-purpose register.

## Flag Path

`{ALU_OUT, MEMORY, DEC_COUNT_OUT} → FLAGS_LOAD_SEL mux → Flag Register → Condition Checker / stack`

- `FLAGS_LOAD` latches a new value into the flag register (Z, C, N, V) from the source chosen by `FLAGS_LOAD_SEL[1:0]` — normally `ALU_OUT`, but memory-sourced or decrement-loop-counter-sourced updates are used internally by block-memory and loop instructions.
- The Flag Register feeds the **Condition Checker**, which compares one flag (chosen by `COND_FLAG[1:0]`) against `COND_VALUE` to gate conditional microbranches and conditional program jumps (see `microcode.md`).
- Flag Register contents can themselves be pushed/popped via the memory data path (`PUSHF`/`POPF`), routing through `MEM_DATA_SEL=FLAGS`.

## Stack Path

- No dedicated stack hardware — `PUSH`/`POP`/`CALL`/`RET`/`PUSHF`/`POPF` are built entirely from the paths above: `A7` (as an ALU operand) computes the new stack address, that address goes to MAR, and the pushed/popped value travels the normal memory-data and writeback paths.

## Clock Distribution

- Crystal oscillator → Frequency Divider (8-bit counter) → Frequency Selector (8-to-1 mux) → distributed clock.
- All components are positive-edge triggered except the microstep counter, which runs on the inverted clock (negative edge), so its outputs — the control word driving every mux and register in this document — settle before the next positive edge latches everything else.
