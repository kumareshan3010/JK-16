# JK16 Architecture

## Overview

JK16 is a custom 16-bit microcoded CISC processor with a completely custom, from-scratch instruction set. It is implemented at the discrete-logic level in the [Digital](https://github.com/hneemann/Digital) logic simulator — every register, mux, and control signal is built from primitive logic gates and simulator components, not behavioral HDL.

| Property | Value |
|---|---|
| Architecture style | Custom 16-bit microcoded CISC |
| Instruction width | 16 bits |
| Data width | 16 bits |
| Address width | 16 bits (data bus) |
| Opcode field | 6 bits (64 opcodes) |
| Register fields | 3-bit RA, 3-bit RB |
| Microinstruction word | 48 bits (39 control bits used, 9 reserved) |
| Microstep counter | 6 bits, loadable |
| Instruction length in microsteps | 2 (minimum) – ~82 (maximum, multiplication) |
| Reserved instruction bits | 4 bits, currently zero |
| Memory model | Strict Harvard |
| Implementation | Digital logic simulator, no physical hardware yet |

## High-Level Block Diagram

Program ROM feeds the Instruction Fetch Unit (PC, IR, IMM), which hands off to the Microcoded Control Unit (Control ROM, Microstep Counter, Condition Checker). The control unit drives the Execution Unit (Register File, ALU, Writeback Selector) and the Memory System (MAR, MDR, Address Decoder, RAM/NVM/GPIO) in parallel, with the Special Hardware (MUL/DIV) feeding back into the Execution Unit's writeback path and Status Flags (Z, C, N, V) feeding back into the control unit for branching.

*(Full gate-level datapath diagram: `docs/images/datapath.png`)*

## Instruction Fetch

- The **Program Counter (PC)** is a dedicated 16-bit register holding the address of the next instruction. During fetch it drives the PC's own dedicated address bus into Program ROM.
- The fetched 16-bit word loads into the **Instruction Register (IR)**, where it stays stable for the entire execution of that instruction so the control unit can decode opcode and operand fields without racing the next fetch.
- After fetch, the PC normally increments. For `JMP`, `CALL`, `RET`, and conditional branches, microcode instead loads the PC with a new target address.

## Instruction Format

Every instruction is a fixed 16-bit word:

- **6-bit opcode** → up to 64 distinct instructions
- **3-bit RA field** → selects 1 of 8 registers in Register Bank A (primary/destination)
- **3-bit RB field** → selects 1 of 8 registers in Register Bank B (secondary/source)
- Remaining bits, depending on format, encode immediate data or other instruction-specific fields
- **4 bits are currently unused**, kept as zero and reserved for future expansion

## Register File

- Two independent banks, **A** and **B**, 8 × 16-bit registers each.
- `A7` is conventionally the stack pointer, but functions as a general-purpose register when the stack is unused.
- RA/RB fields feed the register-file selection logic and write-enable circuitry independently, so an instruction can read from one bank and write to the other in the same cycle.

## Datapath Registers

| Register | Width | Role |
|---|---|---|
| PC | 16-bit | Next-instruction address; own dedicated bus to Program ROM |
| IR | 16-bit | Holds the current instruction stable during decode/execute |
| IMM | 16-bit | Holds the immediate operand extracted from the instruction, extended to 16 bits when needed |
| MAR | 16-bit | Holds the address driven onto the shared RAM/NVM/GPIO data bus |
| MDR | 16-bit | Buffer used **only** for memory-to-memory transfers (e.g. `MEMCPY`) |
| WBS | 16-bit mux | Selects the value written back to the destination register |

**MAR** can be loaded from four sources under microcode control: IMM (direct addressing), Register A (register-indirect), Register B (alternate register-indirect), or the ALU output (computed/indexed addressing).

**MDR** is deliberately narrow in scope: it's loaded only from memory and writes only to memory, used exclusively when a memory-to-memory instruction needs to buffer data between a read and a subsequent write. Ordinary loads bypass the MDR entirely — memory data goes straight to the Writeback Selector.

**Writeback Selector (WBS)** is a 4-input mux choosing what gets written to the destination register: ALU output, memory data, the Immediate Register, or the PC (for instructions like `CALL` that save a return address). One shared writeback path serves arithmetic, logic, memory, immediate, and control-flow instructions, keeping the datapath simple despite a large instruction set.

## ALU

- 16-bit inputs and output.
- 16 primitive operations, selected by a 4-bit **ALUOP** from the control unit.
- Updates four status flags whenever applicable: **Z**ero, **C**arry, **N**egative, **O**verflow.
- Includes dedicated multiplication and division hardware alongside the primitive ALU ops. An 8×8 multiply produces a full result; a 16×16 multiply keeps only the lower 16 bits of the product.

### MUL/DIV writeback path

The **Special Mul Mux** and **Special Div Mux** sit on the bus between the ALU output and the Writeback Selector. During a multiply or divide, the corresponding mux — controlled directly by the multiplication/division hardware — transparently substitutes its result onto that bus in place of the ALU's own output. The rest of the datapath, including the WBS, is unaware of the substitution; as far as writeback is concerned, it's still reading "the ALU output."

## Control Unit

- Fully microprogrammed with **horizontal microcode**: each control ROM word is 48 bits, of which 39 bits are currently assigned to control signals and 9 are reserved for future expansion.
- A **loadable microstep counter** (6 bits) generates the address into the control ROM. It normally increments each cycle, but microcode can load it directly to perform conditional/unconditional microbranches, skip steps, or jump into shared microcode routines.
- Because the microstep counter is only 6 bits wide, long operations — most notably multiplication, the longest instruction at roughly 82 microsteps — are implemented as **microloops**: the counter is repeatedly reloaded to cycle through a shared block of microcode rather than needing a wider counter. Instruction length ranges from 2 microsteps (shortest) to around 82 (multiplication).
- At the end of each instruction, the microstep counter is loaded with the starting microstep of the next fetch cycle.

### Conditional branching

Branching is split across two small dedicated circuits:

- **Condition Checker** takes a 2-bit flag-select code and a 1-bit required value, and outputs 1 if the selected status flag currently matches the required value.
- **Jump Controller** decides whether the Condition Checker is even consulted. For an *unconditional* jump, it asserts the jump-enable signal directly, bypassing the Condition Checker entirely. For a *conditional* jump, it outputs 0 and the jump decision rests entirely on the Condition Checker's result.

## Clocking

A **Frequency Divider** and **Frequency Selector** let the operating clock speed be adjusted by the user. This is a convenience/demo feature (useful for watching execution step-by-step in the simulator) rather than a functional requirement of the architecture.

## Memory System (Strict Harvard)

JK16 keeps instruction and data memory on **completely separate buses** — this is not memory-mapped-ROM-on-a-shared-bus; the PC's address bus never touches the MAR/data path at all.

| Bus | Driven by | Destination | Size |
|---|---|---|---|
| Instruction bus | PC | Program ROM | 128 kB (16-bit words) |
| Data bus | MAR | RAM / NVM / GPIO (shared) | 64K addressable 16-bit words |

The 64K data address space is split by the **Address Decoder**, which enables exactly one unit based on address bit patterns:

| Address pattern | Unit enabled | Capacity |
|---|---|---|
| MSB = 1 | RAM | 32K locations (16-bit words) |
| MSB = 0, next bit = 1 | NVM | 16K locations = 32 kB |
| All bits 0 except lowest 2 | GPIO port select | 4 addresses (1 per port) |
| Top two bits = 0, other pattern | *(unused — memory idle)* | — |

RAM and NVM are both built from two 8-bit-wide chips joined horizontally to form a 16-bit word per location.

## GPIO

- 4 memory-mapped GPIO ports, 16 bits each (64 pins total).
- Each port has its own direction, input, and output registers, addressed via the last two bits of the GPIO address range described above.

## Stack

- Downward-growing, implemented entirely through microcode (no dedicated stack hardware beyond `A7` as the pointer).
- Supported operations: `CALL`, `RET`, `PUSH`, `POP`, `PUSHF`, `POPF`.

## Why CISC

JK16 is classified as CISC rather than RISC because of instruction *complexity*, not just because some instructions touch memory directly. Execution length varies enormously by opcode — from 2 microsteps for the simplest register operations up to roughly 82 microsteps for multiplication — and multi-step, memory-to-memory instructions like `MEMCPY`/`MEMSET` do work in one opcode that a RISC ISA would require a instruction sequence for.
