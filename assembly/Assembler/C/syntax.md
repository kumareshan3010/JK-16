# JK16 Assembly Language Reference

This is the syntax reference for the JK16 assembler — the two-pass Python
tool that turns `.asm` source into the hex files JK16 loads from its
Program ROM. `docs/instruction-set.md` covers what each opcode does; this
document covers how it's written down: file layout, operand syntax,
labels, variables, and the diagnostics the assembler produces when
something is wrong.

## Table of contents

- [File layout](#file-layout)
- [Comments](#comments)
- [Registers](#registers)
- [Numbers and operands](#numbers-and-operands)
- [Labels](#labels)
- [Variables](#variables)
- [Memory map](#memory-map)
- [Instruction set](#instruction-set)
- [Assembler diagnostics](#assembler-diagnostics)

## File layout

A source file is a plain text `.asm` file. Each line contains **one** of:

- a blank line (ignored)
- a comment (ignored)
- a label declaration: `Name:`
- a variable declaration: `VAR Name` or `VAR Name = #xxxxh`
- an instruction: `MNEMONIC operand1, operand2, ...`

Whitespace at the start/end of a line is ignored. Instruction mnemonics
are case-insensitive (`add`, `ADD`, `Add` are all equivalent); label and
variable names are case-sensitive.

## Comments

```asm
// a line comment - everything to end of line is ignored

/* a block comment
   can span multiple lines */
```

An unterminated `/*` is an error: everything after it is treated as
comment (it is never silently assembled), and the assembler reports the
line the comment opened on.

## Registers

JK16 has two independent register banks, **A** and **B**, referenced as
`A0`-`A7` and `B0`-`B7`. RA and RB are decoded independently in hardware,
so a single instruction can read one bank and write the other in the
same cycle.

- **`A0`-`A6`** and **`B0`-`B6`** are general-purpose (7 usable registers
  per bank).
- **`A7`** is the **stack pointer** for JK16's downward-growing stack. It
  is usable as an ordinary register only if the stack isn't in use in
  that program.
- **`B7`** is a **hidden register**, used internally by the hardware; it
  is not programmer-accessible.
- `A7` and `B7` may **not** be referenced explicitly in an instruction —
  the assembler rejects it.
- **`A6`** is occasionally borrowed internally as scratch space by
  block-memory and control-flow instructions (`MEMSET`, all jumps,
  `CALL`, `RET`) — its value should not be relied on across one of these.
- Register letters are case-insensitive (`a0` == `A0`), but the bank
  letter has to match what the instruction expects (e.g. `ADD` takes an
  `A` register first, a `B` register second).

## Numbers and operands

| Syntax        | Meaning                                    | Example      |
|---------------|---------------------------------------------|--------------|
| `xxxxh`       | 16-bit immediate (hex, 1-4 digits, no `#`) | `0005h`      |
| `#xxxxh`      | literal data/jump address (hex)            | `#8000h`     |
| `#Name`       | reference to a label or variable by name    | `#counter`   |
| `#n` / `#nh`  | GPIO port number, 1-2 hex digits            | `#1`, `#2h`  |

Hex digits `A`-`F` may be upper or lowercase, but the trailing `h` suffix
on immediates/addresses must be **lowercase**.

Operands are separated by commas: `MOV A0, B1`.

## Labels

```asm
loop:
    INC A0
    JNZ #loop
```

A label marks the current program-memory address (in 16-bit words) on
JK16's dedicated Program ROM bus. A label is referenced with `#Name`.
Labels are valid as jump/call targets only, not as data addresses. Each
name can be declared once; labels and variables share one namespace, so
a label name can't be reused as a variable, or vice versa.

## Variables

```asm
VAR total              ; auto-assigned a free data address
VAR counter = #8000h   ; fixed at a specific data address
```

`VAR Name` reserves a data-memory cell and auto-assigns it a free
address; `VAR Name = #xxxxh` pins it to a specific address instead. A
variable is referenced with `#Name` wherever an instruction expects a
data address (never as a jump target). Variables belong in RAM
(`8000h`-`FFFFh`) or, for non-volatile storage, NVM (`4000h`-`7FFFh`) —
see [Memory map](#memory-map) below for what each region is for.

## Memory map

JK16 has **two completely separate address spaces** by design (strict
Harvard): the Program Counter addresses Program ROM only, and the Memory
Address Register (MAR) addresses the data space only. Nothing in the ISA
can read Program ROM as data or execute code out of RAM.

Data-space instructions (`LOADA`/`STOREA`/`LOADR`/`STORER`/`MOVM`/
`MEMCPY`/`MEMSET`, `PUSH`/`POP`, `INA`/`OUTA`/`INB`/`OUTB`) all address
this single 64K-word (16-bit) data space:

| Address range   | Region             | Size               | Notes |
|------------------|--------------------|---------------------|-------|
| `0000h`-`0003h` | GPIO ports 1-4     | 4 x 16-bit ports    | Bidirectional: a write drives pins as output, a read returns pin state. Accessed via `INA`/`OUTA`/`INB`/`OUTB` with `#1`-`#4`, not via `#xxxxh` data addresses. |
| `0004h`-`3FFFh` | *(unused)*         | -                   | Not decoded by any memory element — reads/writes here go nowhere. Reserved for future peripherals/expansion. Variables should not be placed here. |
| `4000h`-`7FFFh` | NVM (EEPROM/flash) | 32 kB (16K words)   | Non-volatile storage, same access instructions as RAM. |
| `8000h`-`FFFFh` | RAM                | 64 kB (32K words)   | General-purpose read/write memory: variables and the stack. |

**Stack:** downward-growing, lives in RAM, starts at `FFFFh`. `INITSP`
sets the stack pointer (`A7`) to `FFFFh` and has to run exactly once,
before any of `PUSH`, `POP`, `PUSHF`, `POPF`, `CALL`, or `RET` — the
assembler flags a stack operation used before `INITSP`, and flags
`INITSP` used more than once.

**Program ROM** (128 kB, 16-bit words) sits on its own bus, addressed
only by the PC — it never appears in the data-space table above, since no
instruction can address it as data.

## Instruction set

Operand columns: `A`/`B` = register in that bank, `imm` = `xxxxh`
immediate, `addr` = `#xxxxh`/`#Name` (variable, in RAM or NVM), `label` =
`#xxxxh`/`#Name` (label), `port` = `#n`/`#nh` (GPIO port 1-4).

Every instruction is one fixed 16-bit word — `[6-bit opcode][3-bit RA]
[3-bit RB][4 reserved bits]` — with some instructions extending to 2-4
words total for immediates/addresses (the extra words are consumed from
Program ROM right after the opcode word). All 64 opcodes (`0x00`-`0x3F`)
are defined; there are no reserved/unused opcode slots.

### Arithmetic / logic (register, register)

| Mnemonic | Operands | Effect |
|---|---|---|
| `ADD`, `SUB`, `ADC`, `SBB` | `A, B` | add / subtract (with/without carry) |
| `MUL`, `DIV`, `MOD` | `A, B` | multiply (shift-and-add) / divide (repeated subtraction) / remainder |
| `MIN`, `MAX` | `A, B` | min / max |
| `CMP`, `TEST` | `A, B` | compare / bitwise test (flags only) |
| `AND`, `OR`, `XOR`, `NAND`, `NOR`, `XNOR` | `A, B` | bitwise ops |
| `MOV` | `A, B` | copy `B` into `A` |
| `SWAP` | `A, B` | exchange register contents |
| `LOADR`, `STORER` | `A, B` | load/store using `B` as a pointer register |
| `BITSET`, `BITCLR`, `BITTEST` | `A, B` | set/clear/test a bit (needs a prior `BITMASK` on the same `A` register) |

### Arithmetic / logic (single register)

| Mnemonic | Operands | Effect |
|---|---|---|
| `INC`, `DEC` | `A` | increment / decrement |
| `NEG`, `ABS` | `A` | negate / absolute value |
| `NOT` | `A` | bitwise complement |
| `SHL`, `SHR`, `ROL`, `ROR` | `A` | shift / rotate |
| `CLR` | `A` | clear register to 0 |
| `BITMASK` | `A` | prepare a bit mask for a later `BITSET`/`BITCLR`/`BITTEST` |

### No-operand

`NOP`, `PUSHF`, `POPF`, `RET`, `INITSP`, `HALT`

### Stack

| Mnemonic | Operands | Effect |
|---|---|---|
| `PUSH`, `POP` | `B` | push/pop a `B` register |

### Memory / immediate

| Mnemonic | Operands | Effect |
|---|---|---|
| `LOAD` | `A or B, imm` | load an immediate into a register |
| `LOADA`, `STOREA` | `A, addr` | load/store via bank-A register and a data address |
| `STOREB` | `B, addr` | store via bank-B register and a data address |
| `MOVM` | `addr, addr` | move memory-to-memory (source, dest) |
| `MEMCPY` | `imm, addr, addr` | copy `imm` words (length, source, dest) |
| `MEMSET` | `B, imm, addr` | fill `imm` words at `addr` with register `B`'s value |

### Jumps / calls

| Mnemonic | Operands | Effect |
|---|---|---|
| `JMP` | `label` | unconditional jump |
| `JZ`, `JNZ` | `label` | jump if zero / not zero |
| `JC`, `JNC` | `label` | jump if carry / not carry |
| `JN`, `JNN` | `label` | jump if negative / not negative |
| `JV` | `label` | jump if overflow |
| `CALL` | `label` | call subroutine (requires `INITSP` first) |

### I/O

| Mnemonic | Operands | Effect |
|---|---|---|
| `INA`, `OUTA` | `A, port` | read/write GPIO port via bank-A register |
| `INB`, `OUTB` | `B, port` | read/write GPIO port via bank-B register |

By category: 15 arithmetic, 15 logic/bitwise, 10 data movement, 5 stack,
3 block-memory, 10 control-flow, 4 I/O, 2 misc = 64 opcodes total.

> Instructions that clobber `A6` (`MEMSET`, all jumps, `CALL`, `RET`) use
> it as scratch space for that operation — its value should not be
> relied on across one of these.

## Assembler diagnostics

Every problem is reported as `file:line: level: message`, where `level`
is `error` or `warning`. The common ones:

- `unknown instruction 'X'` — mnemonic not recognized
- `label 'X' already declared` / `variable 'X' already declared`
- `'X' is already declared as a label/variable` — names have to be
  unique across both label and variable namespaces
- `expected A/B register, got 'X'` — wrong bank or malformed register
  token
- `explicit use of A7/B7 is not allowed` — reserved registers
- `expected immediate value 'xxxxh' (no '#'), got 'X'`
- `expected '#xxxxh' or '#Name', got 'X'`
- `undefined symbol 'X'`
- `'X' is a label, not valid as a data address here` / `'X' is a
  variable, not valid as a jump target`
- `address X is reserved for ports` / `... inside the reserved stack
  region`
- `MNEMONIC expects N operand(s), got M`
- `MNEMONIC used before INITSP has been executed` / `INITSP executed
  more than once`
- `no BITMASK generated for AN before BITSET/BITCLR/BITTEST` (warning)
- `MNEMONIC will overwrite A6` (warning)
- `program contains no HALT instruction` (warning)
- `program exceeds addressable program memory (65536 words)`

A build (hex/listing output) is refused if there is at least one `error`
diagnostic; `warning` diagnostics do not block a build.
