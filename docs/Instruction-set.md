# JK16 Instruction Set

## Instruction Encoding

Every instruction is a 16-bit word: **6-bit opcode | 3-bit RA | 3-bit RB | 4 reserved bits (zero)**.

Some instructions span additional 16-bit words for immediate values or addresses (see **Words** column below).

## Register Notes

- `A0–A6` — general-purpose registers, register bank A
- `B0–B6` — general-purpose registers, register bank B
- `A7` — stack pointer for the downward-growing stack
- `B7` — hidden/inaccessible register, used internally by certain instructions
- `xxxxh` — 16-bit hex immediate value; `#xxxxh` — 16-bit hex address; `#port` — GPIO port number (1–4 in hex)

## Full Opcode Table

| Opcode (hex) | RA | RB | Words | Instruction | Description |
|---|---|---|---|---|---|
| 00 | A0-A6 | B0-B6 | 1 | `ADD RA, RB` | Addition |
| 01 | A0-A6 | B0-B6 | 1 | `SUB RA, RB` | Subtraction |
| 02 | A0-A6 | x | 1 | `INC RA` | Increment |
| 03 | A0-A6 | x | 1 | `DEC RA` | Decrement |
| 04 | A0-A6 | B0-B6 | 1 | `ADC RA, RB` | Addition with carry |
| 05 | A0-A6 | B0-B6 | 1 | `SBB RA, RB` | Subtraction with borrow |
| 06 | A0-A6 | x | 1 | `NEG RA` | Negation |
| 07 | A0-A6 | x | 1 | `ABS RA` | Absolute value (\|RA\|) |
| 08 | A0-A6 | B0-B6 | 1 | `MUL RA, RB` | Multiplication |
| 09 | A0-A6 | B0-B6 | 1 | `DIV RA, RB` | Division |
| 0A | A0-A6 | B0-B6 | 1 | `MOD RA, RB` | Modulo |
| 0B | A0-A6 | B0-B6 | 1 | `MIN RA, RB` | Minimum |
| 0C | A0-A6 | B0-B6 | 1 | `MAX RA, RB` | Maximum |
| 0D | A0-A6 | B0-B6 | 1 | `CMP RA, RB` | Compare |
| 0E | A0-A6 | B0-B6 | 1 | `TEST RA, RB` | Test |
| 0F | x | x | 1 | `NOP` | No operation |
| 10 | A0-A6 | B0-B6 | 1 | `AND RA, RB` | Bitwise AND |
| 11 | A0-A6 | B0-B6 | 1 | `OR RA, RB` | Bitwise OR |
| 12 | A0-A6 | B0-B6 | 1 | `XOR RA, RB` | Bitwise XOR |
| 13 | A0-A6 | x | 1 | `NOT RA` | Bitwise NOT |
| 14 | A0-A6 | B0-B6 | 1 | `NAND RA, RB` | Bitwise NAND |
| 15 | A0-A6 | B0-B6 | 1 | `NOR RA, RB` | Bitwise NOR |
| 16 | A0-A6 | B0-B6 | 1 | `XNOR RA, RB` | Bitwise XNOR |
| 17 | A0-A6 | B0-B6 | 1 | `MOV RA, RB` | Move value in RB to RA |
| 18 | A0-A6 | x | 1 | `SHL RA` | Bit shift left |
| 19 | A0-A6 | x | 1 | `SHR RA` | Bit shift right |
| 1A | A0-A6 | x | 1 | `ROL RA` | Bit rotate left |
| 1B | A0-A6 | x | 1 | `ROR RA` | Bit rotate right |
| 1C | A0-A6 | x | 1 | `BITMASK RA` | Generate a bitmask for RA |
| 1D | A0-A6 | B0-B6 | 1 | `BITSET RB` | Set a specific bit |
| 1E | A0-A6 | B0-B6 | 1 | `BITCLR RB` | Clear a specific bit |
| 1F | A0-A6 | B0-B6 | 1 | `BITTEST RB` | Test a specific bit |
| 20 | A0-A6 | x | 2 | `LOADA RA, #xxxxh` | Load RA from memory at address `#xxxxh` |
| 21 | A0-A6 | x | 2 | `STOREA RA, #xxxxh` | Store RA to memory at address `#xxxxh` |
| 22 | A0-A6 | x | 2 | `LOAD RA, xxxxh` | Load immediate to RA |
| 23 | x | B0-B6 | 2 | `LOAD RB, xxxxh` | Load immediate to RB |
| 24 | x | B0-B6 | 2 | `STOREB RB, #xxxxh` | Store RB to memory at address `#xxxxh` |
| 25 | A0-A6 | B0-B6 | 1 | `LOADR RA, RB` | Load RA from memory, RB holds the address |
| 26 | A0-A6 | B0-B6 | 1 | `STORER RA, RB` | Store RA to memory, RB holds the address |
| 27 | A0-A6 | x | 1 | `CLR RA` | Set RA to zero |
| 28 | A0-A6 | B0-B6 | 1 | `SWAP RA, RB` | Swap values between RA and RB |
| 29 | x | B0-B6 | 1 | `PUSH RB` | Push RB to stack |
| 2A | x | B0-B6 | 1 | `POP RB` | Pop RB from stack |
| 2B | x | x | 1 | `PUSHF` | Push flags to stack |
| 2C | x | x | 1 | `POPF` | Pop flags from stack |
| 2D | x | x | 3 | `MOVM #xxxxh, #xxxxh` | Move value from source address to destination address |
| 2E | x | x | 4 | `MEMCPY xxxxh, #xxxxh, #xxxxh` | Copy memory of given length from source to destination |
| 2F | A6 | B0-B6 | 3 | `MEMSET RB, xxxxh, #xxxxh` | Set RB's value into memory for a given length from a starting address |
| 30 | A6 | x | 2 | `JMP #xxxxh` | Jump to address |
| 31 | A6 | x | 2 | `JZ #xxxxh` | Jump if zero |
| 32 | A6 | x | 2 | `JNZ #xxxxh` | Jump if not zero |
| 33 | A6 | x | 2 | `JC #xxxxh` | Jump if carry |
| 34 | A6 | x | 2 | `JNC #xxxxh` | Jump if not carry |
| 35 | A6 | x | 2 | `JN #xxxxh` | Jump if negative |
| 36 | A6 | x | 2 | `JNN #xxxxh` | Jump if not negative |
| 37 | A6 | x | 2 | `JV #xxxxh` | Jump if overflow |
| 38 | A6 | x | 2 | `CALL #xxxxh` | Call a function at address |
| 39 | A6 | x | 1 | `RET` | Return from function |
| 3A | A0-A6 | x | 2 | `INA RA, #port` | Load RA from GPIO port |
| 3B | A0-A6 | x | 2 | `OUTA RA, #port` | Output RA to GPIO port |
| 3C | x | B0-B6 | 2 | `INB RB, #port` | Load RB from GPIO port |
| 3D | x | B0-B6 | 2 | `OUTB RB, #port` | Output RB to GPIO port |
| 3E | x | x | 1 | `INITSP` | Initialize the stack pointer to FFFFh |
| 3F | x | x | 1 | `HALT` | Halt execution |

## Category Summary

| Category | Opcodes | Count |
|---|---|---|
| Arithmetic | ADD, SUB, INC, DEC, ADC, SBB, NEG, ABS, MUL, DIV, MOD, MIN, MAX, CMP, TEST | 15 |
| Logic / Bitwise | AND, OR, XOR, NOT, NAND, NOR, XNOR, SHL, SHR, ROL, ROR, BITMASK, BITSET, BITCLR, BITTEST | 15 |
| Data Movement | MOV, LOADA, STOREA, LOAD (RA), LOAD (RB), STOREB, LOADR, STORER, CLR, SWAP | 10 |
| Stack | PUSH, POP, PUSHF, POPF, INITSP | 5 |
| Block Memory | MOVM, MEMCPY, MEMSET | 3 |
| Control Flow | JMP, JZ, JNZ, JC, JNC, JN, JNN, JV, CALL, RET | 10 |
| I/O | INA, OUTA, INB, OUTB | 4 |
| Misc | NOP, HALT | 2 |

Total: 64 opcodes (all defined, no unused/reserved slots).

## Usage Notes

- `INITSP` (3E) **must** appear at the start of any program that uses `PUSH`/`POP` (29–2C), to set the stack pointer to `FFFFh` before first use.
- `BITMASK` (1C) generates a mask with only the *n*th bit set, where *n* is the value held in RA (only the lower 4 bits are considered) at the start of the operation; the mask overwrites RA's original value.
- `BITSET`, `BITCLR`, `BITTEST` (1D–1F) expect a bitmask (as produced by `BITMASK`) to already be present in RA.
- In `MEMSET`/`MEMCPY`/the jump and call family (2F–39), **A6** is used internally to hold intermediate values during execution and will be overwritten — do not rely on A6 holding useful data across these instructions; save it to memory first if needed.
- `MOVM` syntax: `MOVM #SourceAddress, #DestinationAddress`
- `MEMCPY` syntax: `MEMCPY Length, #SourceStartAddress, #DestinationStartAddress`
- `MEMSET` syntax: `MEMSET RB, Length, #DestinationStartAddress`
- GPIO ports are addressed as `#1`–`#4` (hex) via `#port`.
