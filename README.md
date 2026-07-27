# JK16 — A Custom 16-bit Microcoded CISC Processor

JK16 is a completely custom-designed 16-bit CISC processor featuring a from-scratch instruction set architecture (ISA), built entirely at the discrete-logic level and implemented in the [Digital](https://github.com/hneemann/Digital) logic simulator. Nothing in this project is generated from behavioural HDL or a synthesis tool. The processor, its ISA, microcode, control unit, and complete datapath are all original designs, constructed from fundamental digital logic components.

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Status: v1 simulator-verified](https://img.shields.io/badge/status-v1%20simulator--verified-brightgreen.svg)
[![Platform: Digital](https://img.shields.io/badge/simulator-Digital-orange.svg)](https://github.com/hneemann/Digital)

---

## Table of Contents

- [Overview](#overview)
- [Key Specifications](#key-specifications)
- [Architecture Summary](#architecture-summary)
- [Instruction Set Summary](#instruction-set-summary)
- [Memory Map](#memory-map)
- [Microcode](#microcode)
- [Repository Structure](#repository-structure)
- [Getting Started](#getting-started)
- [Example Program](#example-program)
- [Toolchain](#toolchain)
- [Documentation](#documentation)
- [Status & Roadmap](#status--roadmap)
- [Design Notes](#design-notes)
- [License](#license)

---

## Overview

JK16 was designed and built as a way to learn computer architecture from first principles — not by following a tutorial CPU, but by designing an original ISA, an original microcode format, and an original datapath, then implementing all of it gate-by-gate in a logic simulator. The result is a fully working CISC processor: 64 custom-defined opcodes, a horizontally microcoded control unit, dedicated multiply/divide hardware, a strict Harvard memory architecture, and memory-mapped GPIO — all verified executing real programs in the simulator.

Nothing in this design comes from an existing ISA (no x86, ARM, or MIPS influence in the encoding) — the instruction format, opcode assignments, register model, and microcode were all designed from scratch specifically for this processor.

## Key Specifications

| Property | Value |
|---|---|
| Architecture style | Custom 16-bit microcoded CISC |
| Memory model | Strict Harvard (separate instruction and data buses) |
| Instruction width | 16 bits (some instructions span 2–4 words) |
| Data width | 16 bits |
| Opcode space | 6 bits — 64 opcodes, all defined, no reserved slots |
| Register file | 2 banks (A, B), 7 general-purpose registers each, plus SP and 1 hidden register |
| ALU | 16-bit, 16 operations, combinational, with dedicated MUL/DIV hardware |
| Control unit | Horizontal microcode, 48-bit control words, 39 signals used / 9 reserved |
| Microstep counter | 6-bit, loadable, negative-edge triggered |
| Instruction length | 2 to ~82 microsteps (via microloops) |
| Program ROM | 128 kB, dedicated bus (PC only) |
| Data memory | 64K-word address space: 64 kB RAM + 32 kB NVM + 4×16-bit GPIO |
| Stack | Downward-growing, in RAM, from `0xFFFF` |
| Implementation | [Digital](https://github.com/hneemann/Digital) logic simulator (gate-level) — no physical hardware yet |

## Architecture Summary

JK16's datapath centers on a small set of dedicated registers — **PC**, **IR**, **IMM**, **MAR**, **MDR** — feeding a dual-bank register file through a 16-bit combinational **ALU** and a 4-way **Writeback Selector**. Every instruction's execution is entirely microcode-driven: there's no hardwired sequencing logic beyond the control ROM's own address formation (`opcode × 64 + microstep`).

A few design choices define the character of the processor:

- **A shared, decoupled writeback path.** Whether a value comes from the ALU, from memory, from an immediate, or from the PC (for `CALL`'s return address), it all funnels through one 4-to-1 Writeback Selector into the register file. This keeps a large, varied instruction set from needing a proliferation of dedicated datapath wiring.
- **Transparent MUL/DIV substitution.** Multiplication (shift-and-add) and division (repeated subtraction) have their own hardware, but their results are quietly muxed onto the ALU-output bus by dedicated Special Mul/Div Muxes — the Writeback Selector never needs to know the difference.
- **Everything is a microloop.** There's no special-case hardware for looping constructs like multiply or block-memory copy — they're built from the same microbranch mechanism (`uSTEP_LOAD_EN` + `JUMP_SEL`) used for ordinary conditional jumps, just looping back into the same instruction's own microcode block instead of jumping to a new one.
- **Pure address-based memory decoding.** RAM, NVM, and GPIO share one 16-bit address space and one physical data bus; an Address Decoder reads only the address bits to decide which unit responds — no explicit chip-select instructions exist in the ISA.

Full details, including every control register's exact load-source options and the ALU's operation set, are in [`docs/architecture.md`](docs/architecture.md).

## Instruction Set Summary

All 64 opcodes (`0x00`–`0x3F`) are defined — there are no reserved/unused slots. They break down as:

| Category | Examples | Count |
|---|---|---|
| Arithmetic | `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `CMP`, `ABS` | 15 |
| Logic / Bitwise | `AND`, `XOR`, `SHL`, `ROR`, `BITMASK`, `BITTEST` | 15 |
| Data Movement | `MOV`, `LOADA`, `STORER`, `CLR`, `SWAP` | 10 |
| Stack | `PUSH`, `POP`, `PUSHF`, `POPF`, `INITSP` | 5 |
| Block Memory | `MOVM`, `MEMCPY`, `MEMSET` | 3 |
| Control Flow | `JMP`, `JZ`/`JNZ`/`JC`/`JNC`/`JN`/`JNN`/`JV`, `CALL`, `RET` | 10 |
| I/O | `INA`, `OUTA`, `INB`, `OUTB` | 4 |
| Misc | `NOP`, `HALT` | 2 |

Every instruction is `[6-bit opcode][3-bit RA][3-bit RB][4 reserved bits]`, with some instructions extending to 2–4 words for immediates and addresses. Block-memory and control-flow instructions have full support for register-indirect and immediate addressing, conditional branching on all four status flags (Z/C/N/V), and safe subroutine calls with a dedicated downward-growing stack.

The complete opcode table, per-instruction operand formats, and usage notes (e.g. why `INITSP` must run first, how `BITMASK`/`BITSET`/`BITCLR`/`BITTEST` chain together) are in [`docs/instruction-set.md`](docs/Instruction-set.md).

## Memory Map

```
0xFFFF ┐
       │  RAM (64 kB, 32K words)         stack grows downward from here
0x8000 ┘
0x7FFF ┐
       │  NVM (32 kB, 16K words)
0x4000 ┘
0x3FFF ┐
       │  (unused / reserved)
0x0004 ┘
0x0003 ┐
       │  GPIO ports 1–4
0x0000 ┘
```

Program ROM (128 kB) lives on a completely separate bus, addressed only by the Program Counter — it's never visible in this map because the CPU has no instruction that can address it as data. Full decode rules and per-region details are in [`docs/memory-map.md`](docs/memory-map.md).

## Microcode

The control unit is horizontally microcoded: every microstep of every instruction is one 48-bit word in a control ROM physically built from 6×8-bit ROM chips. [`docs/microcode.md`](docs/microcode.md) documents:

- The full 39-signal control-word bit map and all multi-bit field decodings (`WB_SEL`, `MAR_SEL`, `COND_FLAG`, `MEM_DATA_SEL`, `FLAGS_LOAD_SEL`).
- Worked, step-by-step examples (`ADD`, a conditional jump, and the `MUL` shift-and-add microloop).
- A **fully decoded appendix covering all 64 opcodes and all 326 microsteps** — every control word translated from raw hex into human-readable signal names, not just the raw ROM dump.

For the control ROM's own hardware (address formation, the negative-edge microstep counter, the branch-decision circuits), see [`docs/control-unit.md`](docs/control-unit.md).

## Repository Structure

```
JK16_Custom_CISC_Processor-main/
├── .gitignore
├── LICENSE
├── README.md
│
├── assembly/
│   ├── syntax.md
│   └── Assembler/
│       ├── C/
│       │   ├── Makefile
│       │   ├── README.md
│       │   ├── assembler_check.c
│       │   ├── assembler_full.c
│       │   ├── assembler_hex.c
│       │   ├── assembler_txt.c
│       │   ├── core.c
│       │   ├── core.h
│       │   ├── syntax.md
│       │   └── single_file/
│       │       ├── Makefile
│       │       └── assembler.c
│       └── Python/
│           ├── asm_check.py
│           ├── asm_listing.py
│           ├── asm_machinecode.py
│           ├── assembler_core.py
│           ├── digital_remote.py
│           ├── syntax.md
│           └── single_file/
│               └── assembler.py
│
├── control-rom/
│   └── CONTROL_ROM_48BIT_(DIGITAL).hex
│
├── digital/                          (Digital simulator .dig files — gate-level)
│   ├── ALU.dig
│   ├── COND_CHECKER.dig
│   ├── CONTROL_UNIT.dig
│   ├── DEC_COUNTER.dig
│   ├── FLAGS.dig
│   ├── IR_and_IMM.dig
│   ├── MEMORY.dig
│   ├── MICROSTEP_COUNTER.dig
│   ├── PROGRAM_COUNTER.dig
│   ├── PROG_MEMORY.dig
│   ├── REG_BANK.dig
│   ├── SPECIAL_DIV.dig
│   ├── SPECIAL_MUL.dig
│   ├── WB_SELECTOR.dig
│   └── processor/
│       └── FULL_PROCESSOR.dig
│
├── docs/
│   ├── Instruction-set.md
│   ├── architecture.md
│   ├── control-unit.md
│   ├── datapath.md
│   ├── memory-map.md
│   ├── microcode.md
│   └── roadmap.md
│
├── images/
│   ├── architecture/
│   │   ├── EXECUTION.jpg
│   │   ├── FETCH.jpg
│   │   ├── FLAGS.jpg
│   │   └── MEMORY.jpg
│   └── schematic/
│       ├── ALU.jpg
│       ├── COND_CHECKER.jpg
│       ├── CONTROL_UNIT.jpg
│       ├── DEC_COUNTER.jpg
│       ├── FLAGS.jpg
│       ├── IR_and_IMM.jpg
│       ├── MEMORY.jpg
│       ├── MICROSTEP_COUNTER.jpg
│       ├── PROGRAM_COUNTER.jpg
│       ├── PROG_MEMORY.jpg
│       ├── REG_BANK.jpg
│       ├── SPECIAL_DIV.jpg
│       ├── SPECIAL_MUL.jpg
│       └── WB_SELECTOR.jpg
│
├── programs/
│   ├── blink.asm
│   ├── block_memory.asm
│   ├── factorial.asm
│   ├── fibonacci.asm
│   └── subroutine_calls.asm
│
├── verilog/                          (RTL translation of the design)
│   ├── ALU.v
│   ├── COND_CHECKER.v
│   ├── CONTROL_UNIT.v
│   ├── DEC_COUNTER.v
│   ├── FLAGS.v
│   ├── IR_and_IMM.v
│   ├── MEMORY.v
│   ├── MICROSTEP_COUNTER.v
│   ├── PROG_MEMORY.v
│   ├── REG_BANK.v
│   ├── SPECIAL_DIV.v
│   ├── SPECIAL_MUL.v
│   ├── WB_SELECTOR.v
│   └── processor/
│       └── PROCESSOR.v
│
└── vhdl/                             (VHDL translation of the design)
    ├── ALU.vhdl
    ├── COND_CHECKER.vhdl
    ├── CONTROL_UNIT.vhdl
    ├── DEC_COUNTER.vhdl
    ├── FLAGS.vhdl
    ├── IR_and_IMM.vhdl
    ├── MEMORY.vhdl
    ├── MICROSTEP_COUNTER.vhdl
    ├── PROGRAM_COUNTER.vhdl
    ├── PROG_MEMORY.vhdl
    ├── REG_BANK.vhdl
    ├── SPECIAL_DIV.vhdl
    ├── SPECIAL_MUL.vhdl
    └── processor/
        └── PROCESSOR.vhdl

```

## Getting Started

**Prerequisites:** [Digital](https://github.com/hneemann/Digital) (requires a Java runtime), Python 3 (for the assembler).

1. **Clone the repository** and open `digital/Processor.dig` in Digital to load the top-level CPU design.
2. **Write or choose a program.** Sample programs are in `programs/`; instruction syntax is documented in `assembler/syntax.md` and the full opcode reference in `docs/instruction-set.md`.
3. **Assemble it:**
   ```bash
   python assembler/assembler.py programs/fibonacci.asm -o rom/program.hex
   ```
4. **Load the hex output** into the simulator's Program ROM component and run — adjust the clock speed via the Frequency Selector to step through execution as slowly or quickly as you like.

## Example Program

A minimal program that adds two immediate values and stores the result:

```asm
INITSP                  ; required before any stack-using instruction
LOAD    A0, 0005h       ; A0 = 5
LOAD    A1, 0003h       ; A1 = 3
ADD     A0, A1          ; A0 = A0 + A1  ->  8
STOREA  A0, #8000h      ; store result to the start of RAM
HALT
```

More complete examples — including loops, function calls, and GPIO I/O — are in `programs/`.

## Toolchain

- **[Digital](https://github.com/hneemann/Digital)** — the CPU design itself, built and simulated entirely at gate/component level.
- **Assembler** (`assembler/`) — a two-pass Python assembler with full ISA support: multi-word instruction handling, `VAR` auto-allocation, strict hex-literal enforcement, and warnings for common mistakes (missing `BITMASK` before bit ops, missing `INITSP`, missing `HALT`). Built as supporting tooling around the hardware design, developed with AI assistance.
- **ROM generation** (`tools/`, `rom/`) — scripts that emit Intel HEX files for the parallel ROM chips (both the boot/program ROM and the six-chip microcode control ROM).

## Documentation

| Doc | Covers |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | Overall design: registers, ALU, control unit, memory system, GPIO, stack, clocking |
| [`docs/instruction-set.md`](docs/instruction-set.md) | Every opcode, its encoding, and usage notes |
| [`docs/memory-map.md`](docs/memory-map.md) | Address ranges and decode rules for RAM, NVM, GPIO, and Program ROM |
| [`docs/datapath.md`](docs/datapath.md) | How data moves between components, bus by bus, on every cycle |
| [`docs/microcode.md`](docs/microcode.md) | Control signal reference and the fully decoded microcode for all 64 opcodes |
| [`docs/control-unit.md`](docs/control-unit.md) | Control ROM hardware, microstep counter, branch circuits, clock generation |
| [`docs/roadmap.md`](docs/roadmap.md) | Where the project goes from here |

## Status & Roadmap

**v1 is functionally complete at the simulator level.** All 64 opcodes are defined and microcoded, and instructions have been verified executing correctly in Digital. It has not yet been run through Verilog/HDL simulation or built as physical hardware — the current priority is fully validating v1 before extending it further.

Planned future directions (see [`docs/roadmap.md`](docs/roadmap.md) for more):

- Upgrading the architecture to 32-bit
- Building dedicated programming hardware for loading programs onto physical ROM chips
- Evolving toward a full microcontroller with integrated peripherals, building on the existing memory-mapped GPIO model

## Design Notes

A few things worth knowing if you're reading the source or the microcode appendix:

- **Active-high logic throughout** — every control signal in the design is active-high, with no exceptions.
- **A6 is a working register for block/control-flow instructions.** Instructions like `MEMCPY`, `MEMSET`, and the jump/call family use A6 internally for intermediate values; don't rely on A6 holding useful data across those instructions.
- **B7 is hidden.** It's used internally by certain instructions and isn't programmer-accessible.
- **The microstep counter runs on the inverted clock**, so control signals are stable before the rest of the (positive-edge) datapath latches on the next rising edge.

## License

MIT — see [`LICENSE`](LICENSE).
