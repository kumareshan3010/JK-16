# JK16 Architecture

## Overview

JK16 is a custom 16-bit microcoded CISC processor with a completely custom, from-scratch instruction set. It is implemented at the discrete-logic level in the [Digital](https://github.com/hneemann/Digital) logic simulator — every register, mux, and control signal is built from primitive logic gates and simulator components, not behavioral HDL.

All control signals in the design use **active-high logic** throughout.

| Property | Value |
|---|---|
| Architecture style | Custom 16-bit microcoded CISC |
| Instruction width | 16 bits |
| Data width | 16 bits |
| Address width | 16 bits (data bus) |
| Opcode field | 6 bits (64 opcodes) |
| Register fields | 3-bit RA, 3-bit RB |
| Microinstruction word | 48 bits (39 control bits used, 9 reserved) |
| Microstep counter | 6 bits, loadable, negative-edge triggered |
| Instruction length in microsteps | 2 (minimum) – ~82 (maximum, multiplication) |
| Reserved instruction bits | 4 bits, currently zero |
| Memory model | Strict Harvard |
| Implementation | Digital logic simulator, no physical hardware yet |

## High-Level Block Diagram

Program ROM feeds the Instruction Fetch Unit (PC, IR, IMM), which hands off to the Microcoded Control Unit (Control ROM, Microstep Counter, Condition Checker). The control unit drives the Execution Unit (Register File, ALU, Writeback Selector) and the Memory System (MAR, MDR, Address Decoder, RAM/NVM/GPIO) in parallel, with the Special Hardware (MUL/DIV) feeding back into the Execution Unit's writeback path and Status Flags (Z, C, N, V) feeding back into the control unit for branching.

*(Full gate-level datapath diagram: `docs/images/datapath.png`)*

## Clocking

- The clock signal originates from a crystal oscillator, then passes through a **Frequency Divider** (an 8-bit counter) and a **Frequency Selector** (an 8-to-1 multiplexer); the mux output is the actual processor clock. This lets the operating speed be adjusted by the user — a convenience/demo feature for watching execution step-by-step in the simulator, not a functional requirement.
- Every component in the design is **positive-edge triggered**, except the **microstep counter**, which is **negative-edge triggered** so that control signals settle before the next rising clock edge drives the rest of the datapath. Since the counter itself is a positive-edge part, this is achieved by inverting the clock into its clock input.
- The clock is distributed to every component that needs it.

## Instruction Fetch

- The **Program Counter (PC)** is a loadable 16-bit counter with its own dedicated address bus to Program ROM — it is the *only* register that can address Program ROM.
- PC behavior is controlled by two bits, **pc_enable** and **pc_sel**:
  - `pc_enable=1, pc_sel=1` → PC loads from the ALU output on the next clock edge (used for jumps/calls/returns)
  - `pc_enable=1, pc_sel=0` → PC increments (normal sequential fetch)
  - `pc_enable=0` → PC holds its value
- The fetched 16-bit word loads into the **Instruction Register (IR)**, which holds the instruction stable for the entire duration of its execution so the control unit can decode opcode and operand fields without racing the next fetch.
- **Two-word instructions**: some instructions (e.g. load-immediate) span two 16-bit words — the first word is the instruction itself, and the PC increments a second time to fetch the following word, which loads into the Immediate Register (IMM) as the immediate value or address.

## Instruction Format

Every instruction is a fixed 16-bit word, read MSB first:

- **6-bit opcode** → up to 64 distinct instructions
- **3-bit RA field** → selects 1 of the registers in Register Bank A
- **3-bit RB field** → selects 1 of the registers in Register Bank B
- **4 bits, currently unused** — always zero, reserved for future expansion

## Register File

- Two independent banks, **A** and **B**.
- **A0–A6** and **B0–B6** are general-purpose (7 registers per bank).
- **A7** is the stack pointer for a downward-growing stack (usable as general-purpose if the stack is unused).
- **B7** is a hidden register, inaccessible to the programmer, used internally for calculations; **A6** is also occasionally borrowed for internal use.
- RA/RB fields drive the register-file selection and write-enable logic independently, so an instruction can read from one bank and write to the other in the same cycle.

## Datapath Registers

| Register | Width | Role |
|---|---|---|
| PC | 16-bit | Loadable counter; sole address source for Program ROM |
| IR | 16-bit | Holds the current instruction stable during decode/execute |
| IMM | 16-bit | Holds the immediate value/address from a two-word instruction |
| MAR | 16-bit | Holds the address driven onto the shared RAM/NVM/GPIO data bus |
| MDR | 16-bit | Buffer used **only** for memory-to-memory transfers (e.g. `MEMCPY`, `MEMSET`) |
| WBS | 16-bit mux | Selects the value written back to the destination register |

**MAR** can be loaded from four sources under microcode control: IMM (direct addressing), Register A (register-indirect), Register B (alternate register-indirect), or the ALU output (computed/indexed addressing). It is the only register that can supply an address to memory.

**MDR** is deliberately narrow in scope: loaded only from memory, and its output goes only to a memory write. It exists purely as temporary storage for memory-to-memory operations. Ordinary loads bypass it entirely — memory data goes straight to the Writeback Selector.

**Writeback Selector (WBS)** is a 16-bit, 4-to-1 mux that controls only the *datapath* of what gets written into the register file (not which register — that's the RA/RB decode logic). Its four sources: Immediate Register (IMM), ALU output, Memory (RAM/NVM/GPIO), or the Program Counter (for instructions like `CALL` that save a return address). One shared writeback path serves arithmetic, logic, memory, immediate, and control-flow instructions, keeping the datapath simple despite a large instruction set.

## ALU

- 16-bit inputs and output, purely **combinational** — no clock pulse needed to produce a result.
- 16 operations available, operating on the A and B register fields.
- Includes a **pass** operation, which produces no arithmetic result but lets the status flags be loaded directly from a register's value.
- Result writes back to the selected Register A field via the Writeback Selector.
- Dedicated multiplication and division hardware sits alongside the primitive ALU ops (see below).

### Multiplication

- Implemented via **shift-and-add**.
- Result is stored in a hidden 16-bit **mul_result** register: holds the full result for 8-bit operands, but only the lower 16 bits for a 16×16 multiply (the upper half is discarded).
- Writeback is handled by the **Special Mul Mux**, a 16-bit 2-to-1 mux that temporarily substitutes `mul_result` onto the ALU-output input of the Writeback Selector during a multiply; at all other times, the true ALU output passes through unchanged.

### Division

- Implemented via **repeated subtraction**.
- The remainder is held in a register in the **A** field; the quotient is written back to a register in the **B** field.
- Writeback follows the same pattern as multiplication, via a dedicated **Special Div Mux** feeding the Writeback Selector.

## Control Unit

- Fully microprogrammed with **horizontal microcode**. Physically, the control ROM is **6 × 8-bit ROM chips**, each with 12-bit address lines, wired horizontally and sharing the same address bus — together producing the 48-bit microinstruction word (39 bits currently used as control signals, 9 reserved for future use).
- The **microstep counter** (6-bit, loadable, with reset) combines with the opcode to form the address into the control ROM. It normally increments each cycle, but microcode can reload it directly to perform microbranches, skip steps, or jump into shared microcode routines (microsequencing).
- Because the counter is only 6 bits wide, long operations — most notably multiplication, the longest instruction at roughly 82 microsteps — are implemented as **microloops**: the same block of microcode is revisited by repeatedly reloading the counter, rather than requiring a wider counter. Instruction length overall ranges from 2 microsteps (shortest) to ~82 (multiplication).
- **Important distinction**: these microcode jumps happen *within* a single instruction's execution (microbranches/microsequencing/microloops) — they are not the same thing as program-level `JMP` instructions.

### Conditional branching

Program-level branching is split across two small dedicated circuits:

- **Condition Checker**: a combinational circuit taking a 2-bit flag-select code (via the **Flag Selector**, choosing among Z/C/N/V) and a 1-bit required value; outputs 1 only if the selected flag's actual value matches the required value.
- **Jump Controller**: a control bit that decides whether the Condition Checker is even consulted — low means the jump is conditional (decision rests entirely on the Condition Checker), high means the jump is unconditional (jump proceeds regardless).

## Status Flags

- Four flags: **Z**ero, **C**arry, **N**egative, **O**verflow, held in a dedicated flag register.
- A **flag load selector** mux chooses which of three sources loads the flag register: ALU output (the primary source), memory, or the **decrement loop counter** (a hidden counter used internally for operations like `MEMCPY`).
- Flag register contents can be pushed to and popped from the stack (`PUSHF`/`POPF`), so flag state can be preserved across subroutine calls.

## Memory System (Strict Harvard)

JK16 keeps instruction and data memory on **completely separate buses** — the PC's address bus never touches the MAR/data path at all.

| Bus | Driven by | Destination | Size |
|---|---|---|---|
| Instruction bus | PC | Program ROM | 128 kB (16-bit words) |
| Data bus | MAR | RAM / NVM / GPIO (shared) | 64K addressable 16-bit words |

The 64K data address space is split by the **Address Decoder**, which enables exactly one unit based on the address bits presented by MAR:

| Address pattern | Unit enabled | Capacity | Address lines needed |
|---|---|---|---|
| MSB = 1 | RAM | 64 kB (32K x 16-bit words) | 15 |
| MSB = 0, next bit = 1 | NVM (EEPROM/flash) | 32 kB (16K x 16-bit words) | 14 |
| Higher 14 bits all 0 | GPIO port select (low 2 bits choose port) | 4 ports | 2 |
| Any other top-bits-zero pattern | *(unused — memory idle)* | — | — |

RAM and NVM are each built from two 8-bit-wide chips joined horizontally to form one 16-bit word per address. All three memory elements share the same address lines and the same memory-in/memory-out data buses; only the element selected by the Address Decoder actually drives or receives data.

## GPIO

- 4 memory-mapped ports, 16 bits each (64 pins total), each addressed as a single memory location (0x0000–0x0003).
- Ports are bidirectional with no separate direction register: a memory write to a port (OUTA/OUTB) drives its pins as output; a memory read (INA/INB) reads the external pin state as input. Direction simply follows whichever operation is issued.

## Stack

- Downward-growing, implemented entirely through microcode (no dedicated stack hardware beyond `A7` as the pointer).
- Supported operations: `CALL`, `RET`, `PUSH`, `POP`, `PUSHF`, `POPF`.

## Why CISC

JK16 is classified as CISC rather than RISC because of instruction *complexity*, not just because some instructions touch memory directly. Execution length varies enormously by opcode — from 2 microsteps for the simplest register operations up to roughly 82 microsteps for multiplication — and multi-step, memory-to-memory instructions like `MEMCPY`/`MEMSET` do work in one opcode that a RISC ISA would require an instruction sequence for.
