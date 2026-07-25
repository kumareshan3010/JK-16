# JK16 Roadmap

## Current Status (v1)

JK16 is complete at the logic-simulator level: the full 64-opcode ISA is defined, the control ROM is fully microcoded, and instructions have been verified executing correctly in the [Digital](https://github.com/hneemann/Digital) simulator. The project has not yet been run through Verilog/HDL simulation, and there's no physical hardware build (PCB) at this stage.

**Working approach**: v1 is being fully validated at the simulator level before any further expansion — correctness and completeness of the current 16-bit design comes first.

## Planned Directions

- **32-bit upgrade** — extending the architecture beyond the current 16-bit data/address/instruction width.
- **Programming hardware** — dedicated hardware for loading programs onto the physical ROM chips.
- **Microcontroller evolution** — growing JK16 toward a microcontroller with integrated peripherals, building on the existing memory-mapped GPIO model.

These are longer-term directions rather than a committed schedule — v1 validation is the current priority, and these will be scoped in more detail once that's complete.
