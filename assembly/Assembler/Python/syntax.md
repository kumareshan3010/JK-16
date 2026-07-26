# JK16 Assembler Syntax

Reference for the assembly language accepted by `asm_check.py`, `asm_machinecode.py`, and `asm_listing.py` (all three share the same parser/encoder in `assembler_core.py`, so anything valid for one is valid for all).

## File Structure

A program is a plain text file, one statement per line. Blank lines are ignored. There is no requirement for statements to be indented or aligned.

```asm
INITSP
LOAD A0, 0005h
LOAD B0, 0003h
ADD  A0, B0
STOREA A0, #result
HALT

VAR result
```

## Comments

- `//` — line comment, runs to end of line.
- `/* ... */` — block comment, may span multiple lines.
- An unterminated `/*` is a hard error — everything after it is still treated as a comment (never silently assembled), and the assembler reports the line the comment opened on.

```asm
ADD A0, B0   // inline comment
/* this whole
   block is ignored */
```

## Labels

A label declaration is an identifier followed by a colon, alone on its line:

```asm
loop:
    DEC A0
    JNZ #loop
```

- Names are case-sensitive, must start with a letter or underscore, and may contain letters, digits, and underscores.
- A label marks the *instruction word address* of the next instruction line — it does not consume any space itself.
- Declaring the same label twice is an error.
- A name cannot be used as both a label and a variable.

## Variables (`VAR`)

Variables reserve a data-memory address for use with `#Name` in memory-referencing instructions.

```asm
VAR counter                 // auto-allocated address
VAR result = #8000h         // explicit address
```

- **Auto-allocated**: `VAR Name` — the assembler assigns the next free data address starting at `0004h`, skipping the reserved port range (`0000h`–`0003h`) and stopping with an error if it runs into the reserved stack region (`FA00h`–`FFFFh`).
- **Explicit**: `VAR Name = #xxxxh` — you choose the address. It's an error if that address falls inside the port range or the stack region, or is already used by another variable.
- `VAR` declarations can appear anywhere in the file (commonly grouped at the end); they don't need to precede their use.

## Registers

- Bank A: `A0`–`A6` (general purpose). `A7` is the stack pointer and cannot be named explicitly in an instruction.
- Bank B: `B0`–`B6` (general purpose). `B7` is a hidden internal register and cannot be named explicitly.
- Register names are case-insensitive (`a0` and `A0` are the same).
- Two-operand instructions (`ADD`, `SUB`, `AND`, etc.) always take **an A-bank register first, then a B-bank register** — `ADD A0, B0` is valid; `ADD A0, A1` is not, even though A1 is a real register, because the second operand must come from bank B.

## Immediates, Addresses, Ports

| Form | Meaning | Example |
|---|---|---|
| `xxxxh` | 16-bit immediate (1–4 hex digits, **no** `#`) | `LOAD A0, 00FFh` |
| `#xxxxh` | 16-bit literal memory address | `STOREA A0, #8000h` |
| `#Name` | reference to a label or variable | `JMP #loop`, `STOREA A0, #result` |
| `#1`–`#4` | GPIO port number | `OUTA A0, #1` |

Notes:

- A literal address like `#FB00h` is always read as the literal address `FB00h`, never as a symbol named `FB00h`, even though `FB00h` also looks like a valid identifier — the strict `#xxxxh` pattern is checked first.
- Referencing an address that happens to match a declared variable's address (e.g. `#8000h` where `VAR result = #8000h`) still works, but produces a warning suggesting you use `#result` instead.
- Using a label where a data address is expected (or a variable where a jump target is expected) is an error — labels are instruction addresses, variables are data addresses, and the two are not interchangeable.
- Addresses inside the reserved GPIO range (`0000h`–`0003h`) or the reserved stack range (`FA00h`–`FFFFh`) are rejected wherever a data address is expected.

## Instruction Reference by Operand Shape

See `docs/instruction-set.md` in the main repo for the full opcode table. Operand shapes as accepted by this assembler:

| Shape | Syntax | Instructions |
|---|---|---|
| Two-register | `OP RA, RB` | `ADD SUB ADC SBB MUL DIV MOD MIN MAX CMP TEST AND OR XOR NAND NOR XNOR MOV LOADR STORER SWAP BITSET BITCLR BITTEST` |
| One-register | `OP RA` | `INC DEC NEG ABS NOT SHL SHR ROL ROR BITMASK CLR` |
| No operands | `OP` | `NOP PUSHF POPF RET INITSP HALT` |
| RB only | `OP RB` | `PUSH POP` |
| Load immediate | `LOAD RA, xxxxh` or `LOAD RB, xxxxh` | `LOAD` (register bank picked from which register you name) |
| Register + address | `OP RA, #addr` / `OP RB, #addr` | `LOADA STOREA` (A-bank), `STOREB` (B-bank) |
| Register + port | `OP RA, #port` / `OP RB, #port` | `INA OUTA` (A-bank), `INB OUTB` (B-bank) |
| Jump target | `OP #addr` | `JMP JZ JNZ JC JNC JN JNN JV CALL` |
| Move memory-to-memory | `MOVM #src, #dst` | `MOVM` |
| Memory copy | `MEMCPY xxxxh, #src, #dst` (length is a plain immediate, no `#`) | `MEMCPY` |
| Memory set | `MEMSET RB, xxxxh, #dst` | `MEMSET` |

`BITSET`/`BITCLR`/`BITTEST` take `RA, RB` like other two-register ops, but semantically expect a bitmask already generated into RA by a prior `BITMASK RA` — the assembler warns if it doesn't see a `BITMASK` for that register earlier in the file.

## Semantic Checks

Beyond syntax, the assembler enforces some ISA-level rules and warns about common mistakes:

- **`INITSP` must run before any stack-using instruction** (`PUSH`, `POP`, `PUSHF`, `POPF`, `CALL`, `RET`) — using one first is an error.
- **`INITSP` can only appear once** — a second occurrence is an error.
- **A6-clobbering instructions** (`MEMSET`, all the jump/call family, `RET`) produce a warning that A6 will be overwritten, as a reminder not to rely on A6 across them.
- **Missing `HALT`** — if the program has no `HALT` instruction anywhere, a warning is issued at the last instruction, since execution would otherwise run past the end of program memory.
- **Missing `BITMASK`** — using `BITSET`/`BITCLR`/`BITTEST` on a register the assembler hasn't seen a `BITMASK` generated for yet produces a warning (not an error, since the mask could have been set up in a way the assembler can't statically track, e.g. via a jump).
- **Program size** — the whole program (including operand words) must fit in the 65536-word instruction address space; exceeding it is an error.

## Reserved Address Ranges (data memory)

| Range | Reserved for |
|---|---|
| `0000h`–`0003h` | GPIO ports |
| `FA00h`–`FFFFh` | Stack |

`VAR` auto-allocation starts at `0004h` and skips over the port range automatically; explicit `VAR` addresses and literal `#addr` operands are rejected if they fall in either reserved range.

## Output Files

Depending on which tool you run:

- **`asm_check.py`** — writes nothing; prints diagnostics and exits 0 (clean) or 1 (errors found).
- **`asm_machinecode.py`** — on a clean assemble, writes `<name>_rom0.hex` (high byte) and `<name>_rom1.hex` (low byte) as Intel HEX; with `--digital`, also writes `<name>_digital.hex` (combined 16-bit-word format) and can push it straight into a running Digital simulator instance over TCP.
- **`asm_listing.py`** — on a clean assemble, writes `<name>_listing.txt`: a human-readable address/binary/source listing plus a variable address map, for debugging.

All three refuse to write any output if the program has at least one error.
