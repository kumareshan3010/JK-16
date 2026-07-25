# JK16 — A Custom 16-bit Microcoded CISC Processor

JK16 is a completely custom-designed 16-bit CISC processor with a from-scratch instruction set, built entirely at the discrete-logic level and implemented in the [Digital](https://github.com/hneemann/Digital) logic simulator — every register, mux, ALU, and control signal is hand-designed from primitive logic gates, not generated from behavioral HDL.

The processor, its instruction set, its microcode, and its full datapath are an original hardware design built from the ground up.

## Highlights

- **Strict Harvard architecture** — separate 128 kB instruction ROM and a 64K-word data space (RAM + NVM + memory-mapped GPIO), on completely independent buses.
- **64-opcode custom ISA** — arithmetic, logic/bitwise, data movement, stack, block-memory (`MEMCPY`/`MEMSET`/`MOVM`), control flow, and I/O instructions, all fully defined.
- **Fully microcoded control unit** — horizontal microcode, 48-bit control words, physically built from 6 × 8-bit ROM chips, with microloops/microbranches driving everything from simple 2-step instructions up to an ~82-step multiply.
- **Dedicated multiply/divide hardware** — shift-and-add multiplication and repeated-subtraction division, transparently muxed into the normal ALU writeback path.
- **Dual-bank register file** — banks A and B, 7 general-purpose registers each, plus a stack pointer and a hidden internal-use register.
- **Memory-mapped GPIO** — 4 bidirectional 16-bit ports for interfacing with the outside world.
- **User-adjustable clock** — frequency divider/selector for step-by-step observation in the simulator.

## Repository Structure

```
your-cpu/
├── README.md
├── LICENSE
├── CHANGELOG.md
│
├── docs/
│   ├── architecture.md       High-level design: datapath registers, ALU, control unit, memory system
│   ├── instruction-set.md    Full 64-opcode reference table, categories, usage notes
│   ├── memory-map.md         Address space layout: RAM / NVM / GPIO / Program ROM
│   ├── microcode.md          Control signal reference + fully decoded microcode for every instruction
│   ├── datapath.md           Bus-level signal flow between every component
│   ├── control-unit.md       Control ROM structure, microstep counter, branch circuits, clocking
│   ├── roadmap.md            Planned directions beyond v1
│   └── images/                Diagrams
│
├── digital/                   Digital simulator design files (.dig)
├── rom/                        Generated control-ROM and boot-ROM hex files
├── assembler/                  Two-pass Python assembler for the JK16 ISA
├── programs/                   Example assembly programs
├── tools/                      Utility scripts (hex generation, etc.)
└── sim-screenshots/            Simulator and waveform captures
```

## Getting Started

1. Install [Digital](https://github.com/hneemann/Digital) (requires Java).
2. Open `digital/Processor.dig` to load the top-level CPU design.
3. Assemble a program from `programs/` (or write your own) using the assembler in `assembler/` — see `assembler/syntax.md`.
4. Load the resulting hex output into the simulator's ROM and run.

See `docs/instruction-set.md` for the full opcode reference while writing programs.

## Documentation

| Doc | Covers |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | Overall design: registers, ALU, control unit, memory system, GPIO, stack |
| [`docs/instruction-set.md`](docs/instruction-set.md) | Every opcode, its encoding, and usage notes |
| [`docs/memory-map.md`](docs/memory-map.md) | Address ranges for RAM, NVM, GPIO, and Program ROM |
| [`docs/datapath.md`](docs/datapath.md) | How data moves between components on each cycle |
| [`docs/microcode.md`](docs/microcode.md) | Control signal reference and the fully decoded microcode for all 64 opcodes |
| [`docs/control-unit.md`](docs/control-unit.md) | Control ROM hardware, microstep counter, branch circuits |
| [`docs/roadmap.md`](docs/roadmap.md) | Where the project goes from here |

## Toolchain

- **Digital simulator** — the CPU design itself, at gate/component level.
- **Assembler** (`assembler/`) — a two-pass Python assembler supporting the full ISA, multi-word instructions, and variable auto-allocation, built as supporting tooling around the hardware design (developed with AI assistance).

## Status

v1 is functionally complete at the simulator level — all 64 opcodes are defined and microcoded, and instructions have been verified executing correctly in Digital. It has not yet been run through Verilog/HDL simulation or built as physical hardware. See [`docs/roadmap.md`](docs/roadmap.md) for planned next steps.

## License

MIT — see [`LICENSE`](LICENSE).
