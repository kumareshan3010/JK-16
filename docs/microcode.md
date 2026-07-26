# JK16 Microcode

## Overview

JK16's control unit is fully microprogrammed with **horizontal microcode**: every microstep of every instruction is a single 48-bit control word stored in the control ROM. The control ROM is physically 6 × 8-bit ROM chips (12-bit address lines) wired together horizontally, giving the full 48-bit word — of which **39 bits are wired to control signals** and 9 are reserved for future use.

**Control-store address format**: `address = (opcode << 6) | microstep`

Each of the 64 opcodes gets a fixed 64-slot block in the control ROM (e.g. `ADD` occupies `0x000`–`0x03F`, `SUB` occupies `0x040`–`0x07F`), even though most instructions use only a handful of those slots. The unused slots within a block are simply never reached, since `uSTEP_RESET` returns to fetch before the block's slots are exhausted.

Every instruction's microcode follows the same shape:

1. **Fetch step** (always identical): `PC_EN` (with `PC_SEL=0`, so the PC increments) + `IR_LOAD` — every single instruction starts with the same control word, `0A0000000000`.
2. One or more **execute steps**, specific to the instruction — reading registers/memory, driving the ALU, computing addresses, branching.
3. A final step that sets **`uSTEP_RESET`**, returning the microstep counter to 0 so the next cycle re-enters the fetch step for the next instruction.

## Control Signals

| # | Signal | # | Signal | # | Signal |
|---|---|---|---|---|---|
| 1 | A_RW | 14 | ALU_OP3 | 27 | uSTEP_LOAD1 |
| 2 | B_RW | 15 | ALU_OP2 | 28 | uSTEP_LOAD0 |
| 3 | WB_SEL1 | 16 | ALU_OP1 | 29 | COND_FLAG1 |
| 4 | WB_SEL0 | 17 | ALU_OP0 | 30 | COND_FLAG0 |
| 5 | PC_EN | 18 | FLAGS_LOAD | 31 | COND_VALUE |
| 6 | PC_SEL | 19 | uSTEP_RESET | 32 | JUMP_SEL |
| 7 | IR_LOAD | 20 | USE_CARRY | 33 | MEM_DATA_SEL1 |
| 8 | IMM_LOAD | 21 | CLK_STOP | 34 | MEM_DATA_SEL0 |
| 9 | MAR_LOAD | 22 | uSTEP_LOAD_EN | 35 | FLAGS_LOAD_SEL1 |
| 10 | MAR_SEL1 | 23 | uSTEP_LOAD5 | 36 | FLAGS_LOAD_SEL0 |
| 11 | MAR_SEL0 | 24 | uSTEP_LOAD4 | 37 | DEC_LOAD |
| 12 | MEM_READ | 25 | uSTEP_LOAD3 | 38 | DEC_CLK |
| 13 | MEM_WRITE | 26 | uSTEP_LOAD2 | 39 | MDR_LOAD |

Bits 40–48 are reserved (always zero in the current ISA).

### Multi-bit fields

| Field | Bits | 00 | 01 | 10 | 11 |
|---|---|---|---|---|---|
| **WB_SEL** | WB_SEL1, WB_SEL0 | MEMORY | ALU_OUT | IMM | PC |
| **MAR_SEL** | MAR_SEL1, MAR_SEL0 | IMM | ALU_OUT | RA | RB |
| **COND_FLAG** | COND_FLAG1, COND_FLAG0 | Z | C | N | V |
| **MEM_DATA_SEL** | MEM_DATA_SEL1, MEM_DATA_SEL0 | RA | RB | FLAGS | MDR |
| **FLAGS_LOAD_SEL** | FLAGS_LOAD_SEL1, FLAGS_LOAD_SEL0 | ALU_OUT | MEMORY | DEC_COUNT_OUT | MEMORY |

`ALU_OP3..0` select 1 of 16 ALU operations. `uSTEP_LOAD5..0` gives the 6-bit target address for a microbranch, active only when `uSTEP_LOAD_EN` is set.

## Microbranching

`uSTEP_LOAD_EN` reloads the microstep counter with the 6-bit value on `uSTEP_LOAD[5:0]` instead of letting it increment normally. **`JUMP_SEL`** decides how that reload is gated:

- `JUMP_SEL=1` → **unconditional** microbranch: the jump always happens (used to close loops, e.g. `MUL`/`DIV` looping back to their compute step).
- `JUMP_SEL=0` → **conditional** microbranch: the reload only happens if the flag selected by `COND_FLAG` currently equals `COND_VALUE`.

This is a purely intra-instruction mechanism — microloops and microbranches are distinct from program-level jumps (`JMP`, `JZ`, etc.), which instead load the *Program Counter* from the ALU output via `PC_EN + PC_SEL`.

### Conditional branch pattern (JZ / JNZ / JC / ...)

All the conditional program jumps follow the same microcode shape: fetch → load the target address into IMM → speculatively move it to RA → **microbranch that skips the PC-load step when the condition is *not* met** → PC-load step (loads PC from ALU_OUT, taking the jump) → reset. In other words, the default path *takes* the jump; the microbranch's job is to skip over the PC-load step when the required flag condition fails.

## Worked Examples

### ADD RA, RB (3 steps — arithmetic, shortest form)

| Step | Signals | What happens |
|---|---|---|
| 0 | `PC_INC, IR_LOAD` | Fetch instruction, PC increments |
| 1 | `FLAGS_LOAD<-ALU_OUT, ALU_OP=ADD` | ALU computes RA+RB (op 0), flags updated |
| 2 | `WRITE RA<-ALU_OUT, uSTEP_RESET` | Result written back to RA, ready for next fetch |

### JZ #addr (6 steps — two-word conditional jump)

| Step | Signals | What happens |
|---|---|---|
| 0 | `PC_INC, IR_LOAD` | Fetch first word (opcode + operands) |
| 1 | `PC_INC, IMM_LOAD` | Fetch second word (target address) into IMM |
| 2 | `WRITE RA<-IMM` | Stage the target address via RA (using A6 internally) |
| 3 | `uBRANCH->5 (if Z!=0)` | If Z flag is *not* 1 (condition fails), skip to step 5 (no jump) |
| 4 | `PC_INC(load-ALU), ALU_OP=PASS_A` | PC loaded from ALU output — the jump is taken |
| 5 | `uSTEP_RESET` | Ready for next fetch |

### MUL RA, RB (8 steps — shift-and-add microloop)

| Step | Signals | What happens |
|---|---|---|
| 0 | `PC_INC, IR_LOAD` | Fetch |
| 1 | `FLAGS_LOAD<-ALU_OUT, ALU_OP=PASS_A` | Setup / flags check |
| 2 | `uBRANCH->7 (if Z!=0)` | If loop-done condition met, skip straight to writeback |
| 3 | `(idle)` | Spacer step |
| 4 | `WRITE RA<-ALU_OUT, ALU_OP=SHR_A` | Shift/add step on RA |
| 5 | `WRITE RB<-ALU_OUT, ALU_OP=SHL_B` | Shift step on RB |
| 6 | `uBRANCH->1 (unconditional)` | Loop back to step 1 — this is the shift-and-add microloop |
| 7 | `WRITE RA<-ALU_OUT, uSTEP_RESET` | Final result written back |

The special MUL/DIV hardware and its dedicated write-back muxes (see `architecture.md`) operate underneath this loop — the visible control signals just drive the ALU repeatedly through the shift-and-add steps until the branch condition at step 2 ends the loop.

## Full Decoded ROM Listing

Every instruction's complete microcode, decoded from the raw 48-bit hex control words using the signal table above. Collapsed by instruction — click to expand.

<details>
<summary><strong>Add</strong> (3 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `000` | `0A0000000000` | PC_INC, IR_LOAD |
| `001` | `000040000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=ADD |
| `002` | `900020000000` | WRITE RA<-ALU_OUT, ALU_OP=ADD, uSTEP_RESET |

</details>

<details>
<summary><strong>Sub</strong> (3 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `040` | `0A0000000000` | PC_INC, IR_LOAD |
| `041` | `0000C0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=SUB |
| `042` | `900020000000` | WRITE RA<-ALU_OUT, ALU_OP=ADD, uSTEP_RESET |

</details>

<details>
<summary><strong>Inc</strong> (3 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `080` | `0A0000000000` | PC_INC, IR_LOAD |
| `081` | `0001C0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=DEC_A |
| `082` | `900020000000` | WRITE RA<-ALU_OUT, ALU_OP=ADD, uSTEP_RESET |

</details>

<details>
<summary><strong>Dec</strong> (3 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `0C0` | `0A0000000000` | PC_INC, IR_LOAD |
| `0C1` | `000140000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=INC_A |
| `0C2` | `900020000000` | WRITE RA<-ALU_OUT, ALU_OP=ADD, uSTEP_RESET |

</details>

<details>
<summary><strong>Adc</strong> (3 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `100` | `0A0000000000` | PC_INC, IR_LOAD |
| `101` | `000050000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=ADD, USE_CARRY |
| `102` | `900020000000` | WRITE RA<-ALU_OUT, ALU_OP=ADD, uSTEP_RESET |

</details>

<details>
<summary><strong>Sbc</strong> (3 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `140` | `0A0000000000` | PC_INC, IR_LOAD |
| `141` | `0000D0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=SUB, USE_CARRY |
| `142` | `900020000000` | WRITE RA<-ALU_OUT, ALU_OP=ADD, uSTEP_RESET |

</details>

<details>
<summary><strong>Neg</strong> (4 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `180` | `0A0000000000` | PC_INC, IR_LOAD |
| `181` | `000780000000` | (idle) |
| `182` | `0001C0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=DEC_A |
| `183` | `900020000000` | WRITE RA<-ALU_OUT, ALU_OP=ADD, uSTEP_RESET |

</details>

<details>
<summary><strong>Abs</strong> (6 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `1C0` | `0A0000000000` | PC_INC, IR_LOAD |
| `1C1` | `0005C0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=PASS_A |
| `1C2` | `000004580000` | uBRANCH->5 (if N!=0) |
| `1C3` | `900780000000` | WRITE RA<-ALU_OUT, ALU_OP=NOT_A |
| `1C4` | `9001C0000000` | FLAGS_LOAD<-ALU_OUT, WRITE RA<-ALU_OUT, ALU_OP=DEC_A |
| `1C5` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Mul</strong> (8 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `200` | `0A0000000000` | PC_INC, IR_LOAD |
| `201` | `0005C0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=PASS_A |
| `202` | `000004720000` | uBRANCH->7 (if Z!=1) |
| `203` | `000000000000` | (idle) |
| `204` | `900280000000` | WRITE RA<-ALU_OUT, ALU_OP=SHR_A |
| `205` | `500300000000` | WRITE RB<-ALU_OUT, ALU_OP=SHL_B |
| `206` | `000004110000` | uBRANCH->1 (unconditional) |
| `207` | `900020000000` | WRITE RA<-ALU_OUT, ALU_OP=ADD, uSTEP_RESET |

</details>

<details>
<summary><strong>Div</strong> (8 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `240` | `0A0000000000` | PC_INC, IR_LOAD |
| `241` | `0000C0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=SUB |
| `242` | `0000046A0000` | uBRANCH->6 (if N!=1) |
| `243` | `900080000000` | WRITE RA<-ALU_OUT, ALU_OP=SUB |
| `244` | `000000000000` | (idle) |
| `245` | `000004110000` | uBRANCH->1 (unconditional) |
| `246` | `500000000000` | WRITE RB<-ALU_OUT, ALU_OP=ADD |
| `247` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Mod</strong> (6 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `280` | `0A0000000000` | PC_INC, IR_LOAD |
| `281` | `0000C0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=SUB |
| `282` | `000004540000` | uBRANCH->5 (if C!=0) |
| `283` | `900080000000` | WRITE RA<-ALU_OUT, ALU_OP=SUB |
| `284` | `000004100000` | uBRANCH->1 (if Z!=0) |
| `285` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Min</strong> (6 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `2C0` | `0A0000000000` | PC_INC, IR_LOAD |
| `2C1` | `0000C0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=SUB |
| `2C2` | `0000045A0000` | uBRANCH->5 (if N!=1) |
| `2C3` | `500080000000` | WRITE RB<-ALU_OUT, ALU_OP=SUB |
| `2C4` | `9000A0000000` | WRITE RA<-ALU_OUT, ALU_OP=SUB, uSTEP_RESET |
| `2C5` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Max</strong> (6 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `300` | `0A0000000000` | PC_INC, IR_LOAD |
| `301` | `0000C0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=SUB |
| `302` | `000004580000` | uBRANCH->5 (if N!=0) |
| `303` | `500080000000` | WRITE RB<-ALU_OUT, ALU_OP=SUB |
| `304` | `9000A0000000` | WRITE RA<-ALU_OUT, ALU_OP=SUB, uSTEP_RESET |
| `305` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Cmp</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `340` | `0A0000000000` | PC_INC, IR_LOAD |
| `341` | `0000E0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=SUB, uSTEP_RESET |

</details>

<details>
<summary><strong>Test</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `380` | `0A0000000000` | PC_INC, IR_LOAD |
| `381` | `000460000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=AND, uSTEP_RESET |

</details>

<details>
<summary><strong>Nop</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `3C0` | `0A0000000000` | PC_INC, IR_LOAD |
| `3C1` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>And</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `400` | `0A0000000000` | PC_INC, IR_LOAD |
| `401` | `900460000000` | FLAGS_LOAD<-ALU_OUT, WRITE RA<-ALU_OUT, ALU_OP=AND, uSTEP_RESET |

</details>

<details>
<summary><strong>Or</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `440` | `0A0000000000` | PC_INC, IR_LOAD |
| `441` | `9004E0000000` | FLAGS_LOAD<-ALU_OUT, WRITE RA<-ALU_OUT, ALU_OP=OR, uSTEP_RESET |

</details>

<details>
<summary><strong>Xor</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `480` | `0A0000000000` | PC_INC, IR_LOAD |
| `481` | `900560000000` | FLAGS_LOAD<-ALU_OUT, WRITE RA<-ALU_OUT, ALU_OP=XOR, uSTEP_RESET |

</details>

<details>
<summary><strong>Not</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `4C0` | `0A0000000000` | PC_INC, IR_LOAD |
| `4C1` | `9007E0000000` | FLAGS_LOAD<-ALU_OUT, WRITE RA<-ALU_OUT, ALU_OP=NOT_A, uSTEP_RESET |

</details>

<details>
<summary><strong>Nand</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `500` | `0A0000000000` | PC_INC, IR_LOAD |
| `501` | `900660000000` | FLAGS_LOAD<-ALU_OUT, WRITE RA<-ALU_OUT, ALU_OP=NAND, uSTEP_RESET |

</details>

<details>
<summary><strong>Nor</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `540` | `0A0000000000` | PC_INC, IR_LOAD |
| `541` | `9006E0000000` | FLAGS_LOAD<-ALU_OUT, WRITE RA<-ALU_OUT, ALU_OP=NOR, uSTEP_RESET |

</details>

<details>
<summary><strong>Xnor</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `580` | `0A0000000000` | PC_INC, IR_LOAD |
| `581` | `900760000000` | FLAGS_LOAD<-ALU_OUT, WRITE RA<-ALU_OUT, ALU_OP=XNOR, uSTEP_RESET |

</details>

<details>
<summary><strong>Mov</strong> (5 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `5C0` | `0A0000000000` | PC_INC, IR_LOAD |
| `5C1` | `500080000000` | WRITE RB<-ALU_OUT, ALU_OP=SUB |
| `5C2` | `900080000000` | WRITE RA<-ALU_OUT, ALU_OP=SUB |
| `5C3` | `500000000000` | WRITE RB<-ALU_OUT, ALU_OP=ADD |
| `5C4` | `5005A0000000` | WRITE RB<-ALU_OUT, ALU_OP=PASS_A, uSTEP_RESET |

</details>

<details>
<summary><strong>Shl</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `600` | `0A0000000000` | PC_INC, IR_LOAD |
| `601` | `900260000000` | FLAGS_LOAD<-ALU_OUT, WRITE RA<-ALU_OUT, ALU_OP=SHL_A, uSTEP_RESET |

</details>

<details>
<summary><strong>Shr</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `640` | `0A0000000000` | PC_INC, IR_LOAD |
| `641` | `9002E0000000` | FLAGS_LOAD<-ALU_OUT, WRITE RA<-ALU_OUT, ALU_OP=SHR_A, uSTEP_RESET |

</details>

<details>
<summary><strong>Rol</strong> (6 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `680` | `0A0000000000` | PC_INC, IR_LOAD |
| `681` | `0005C0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=PASS_A |
| `682` | `900200000000` | WRITE RA<-ALU_OUT, ALU_OP=SHL_A |
| `683` | `000004580000` | uBRANCH->5 (if N!=0) |
| `684` | `9001A0000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A, uSTEP_RESET |
| `685` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Ror</strong> (14 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `6C0` | `0A0000000000` | PC_INC, IR_LOAD |
| `6C1` | `500580000000` | WRITE RB<-ALU_OUT, ALU_OP=PASS_A |
| `6C2` | `900780000000` | WRITE RA<-ALU_OUT, ALU_OP=NOT_A |
| `6C3` | `900180000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A |
| `6C4` | `900000000000` | WRITE RA<-ALU_OUT, ALU_OP=ADD |
| `6C5` | `900180000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A |
| `6C6` | `000440000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=AND |
| `6C7` | `000004D20000` | uBRANCH->13 (if Z!=1) |
| `6C8` | `900780000000` | WRITE RA<-ALU_OUT, ALU_OP=NOT_A |
| `6C9` | `900280000000` | WRITE RA<-ALU_OUT, ALU_OP=SHR_A |
| `6CA` | `500380000000` | WRITE RB<-ALU_OUT, ALU_OP=SHR_B |
| `6CB` | `900000000000` | WRITE RA<-ALU_OUT, ALU_OP=ADD |
| `6CC` | `9001A0000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A, uSTEP_RESET |
| `6CD` | `9003A0000000` | WRITE RA<-ALU_OUT, ALU_OP=SHR_B, uSTEP_RESET |

</details>

<details>
<summary><strong>Bitmask</strong> (26 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `700` | `0A0000000000` | PC_INC, IR_LOAD |
| `701` | `500580000000` | WRITE RB<-ALU_OUT, ALU_OP=PASS_A |
| `702` | `900780000000` | WRITE RA<-ALU_OUT, ALU_OP=NOT_A |
| `703` | `900180000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A |
| `704` | `900000000000` | WRITE RA<-ALU_OUT, ALU_OP=ADD |
| `705` | `900180000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A |
| `706` | `900200000000` | WRITE RA<-ALU_OUT, ALU_OP=SHL_A |
| `707` | `900180000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A |
| `708` | `900200000000` | WRITE RA<-ALU_OUT, ALU_OP=SHL_A |
| `709` | `900180000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A |
| `70A` | `900200000000` | WRITE RA<-ALU_OUT, ALU_OP=SHL_A |
| `70B` | `900180000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A |
| `70C` | `500400000000` | WRITE RB<-ALU_OUT, ALU_OP=AND |
| `70D` | `900280000000` | WRITE RA<-ALU_OUT, ALU_OP=SHR_A |
| `70E` | `900280000000` | WRITE RA<-ALU_OUT, ALU_OP=SHR_A |
| `70F` | `900280000000` | WRITE RA<-ALU_OUT, ALU_OP=SHR_A |
| `710` | `500080000000` | WRITE RB<-ALU_OUT, ALU_OP=SUB |
| `711` | `900080000000` | WRITE RA<-ALU_OUT, ALU_OP=SUB |
| `712` | `500000000000` | WRITE RB<-ALU_OUT, ALU_OP=ADD |
| `713` | `0005C0000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=PASS_A |
| `714` | `000005720000` | uBRANCH->23 (if Z!=1) |
| `715` | `500300000000` | WRITE RB<-ALU_OUT, ALU_OP=SHL_B |
| `716` | `900105310000` | WRITE RA<-ALU_OUT, ALU_OP=INC_A, uBRANCH->19 (unconditional) |
| `717` | `500080000000` | WRITE RB<-ALU_OUT, ALU_OP=SUB |
| `718` | `900080000000` | WRITE RA<-ALU_OUT, ALU_OP=SUB |
| `719` | `500020000000` | WRITE RB<-ALU_OUT, ALU_OP=ADD, uSTEP_RESET |

</details>

<details>
<summary><strong>Bitset</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `740` | `0A0000000000` | PC_INC, IR_LOAD |
| `741` | `5004E0000000` | FLAGS_LOAD<-ALU_OUT, WRITE RB<-ALU_OUT, ALU_OP=OR, uSTEP_RESET |

</details>

<details>
<summary><strong>Bitclr</strong> (3 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `780` | `0A0000000000` | PC_INC, IR_LOAD |
| `781` | `900780000000` | WRITE RA<-ALU_OUT, ALU_OP=NOT_A |
| `782` | `500460000000` | FLAGS_LOAD<-ALU_OUT, WRITE RB<-ALU_OUT, ALU_OP=AND, uSTEP_RESET |

</details>

<details>
<summary><strong>Bittest</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `7C0` | `0A0000000000` | PC_INC, IR_LOAD |
| `7C1` | `000460000000` | FLAGS_LOAD<-ALU_OUT, ALU_OP=AND, uSTEP_RESET |

</details>

<details>
<summary><strong>Loada</strong> (4 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `800` | `0A0000000000` | PC_INC, IR_LOAD |
| `801` | `090000000000` | PC_INC, IMM_LOAD |
| `802` | `008000000000` | MAR_LOAD<-IMM |
| `803` | `801020000000` | MEM_READ(RA), WRITE RA<-MEMORY, uSTEP_RESET |

</details>

<details>
<summary><strong>Storea</strong> (4 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `840` | `0A0000000000` | PC_INC, IR_LOAD |
| `841` | `090000000000` | PC_INC, IMM_LOAD |
| `842` | `008000000000` | MAR_LOAD<-IMM |
| `843` | `000820000000` | MEM_WRITE(RA), uSTEP_RESET |

</details>

<details>
<summary><strong>Load</strong> (3 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `880` | `0A0000000000` | PC_INC, IR_LOAD |
| `881` | `090000000000` | PC_INC, IMM_LOAD |
| `882` | `A00020000000` | WRITE RA<-IMM, uSTEP_RESET |

</details>

<details>
<summary><strong>load</strong> (3 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `8C0` | `0A0000000000` | PC_INC, IR_LOAD |
| `8C1` | `090000000000` | PC_INC, IMM_LOAD |
| `8C2` | `600020000000` | WRITE RB<-IMM, uSTEP_RESET |

</details>

<details>
<summary><strong>Storeb</strong> (4 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `900` | `0A0000000000` | PC_INC, IR_LOAD |
| `901` | `090000000000` | PC_INC, IMM_LOAD |
| `902` | `008000000000` | MAR_LOAD<-IMM |
| `903` | `000820004000` | MEM_WRITE(RB), uSTEP_RESET |

</details>

<details>
<summary><strong>Loadr</strong> (3 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `940` | `0A0000000000` | PC_INC, IR_LOAD |
| `941` | `00E000000000` | MAR_LOAD<-RB |
| `942` | `801000000000` | MEM_READ(RA), WRITE RA<-MEMORY |

</details>

<details>
<summary><strong>Storer</strong> (3 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `980` | `0A0000000000` | PC_INC, IR_LOAD |
| `981` | `00E000000000` | MAR_LOAD<-RB |
| `982` | `000820000000` | MEM_WRITE(RA), uSTEP_RESET |

</details>

<details>
<summary><strong>Clr</strong> (5 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `9C0` | `0A0000000000` | PC_INC, IR_LOAD |
| `9C1` | `500580000000` | WRITE RB<-ALU_OUT, ALU_OP=PASS_A |
| `9C2` | `900780000000` | WRITE RA<-ALU_OUT, ALU_OP=NOT_A |
| `9C3` | `900180000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A |
| `9C4` | `900060000000` | FLAGS_LOAD<-ALU_OUT, WRITE RA<-ALU_OUT, ALU_OP=ADD, uSTEP_RESET |

</details>

<details>
<summary><strong>Swap</strong> (4 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `A00` | `0A0000000000` | PC_INC, IR_LOAD |
| `A01` | `500080000000` | WRITE RB<-ALU_OUT, ALU_OP=SUB |
| `A02` | `900080000000` | WRITE RA<-ALU_OUT, ALU_OP=SUB |
| `A03` | `500060000000` | FLAGS_LOAD<-ALU_OUT, WRITE RB<-ALU_OUT, ALU_OP=ADD, uSTEP_RESET |

</details>

<details>
<summary><strong>Push</strong> (4 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `A40` | `0A0000000000` | PC_INC, IR_LOAD |
| `A41` | `900100000000` | WRITE RA<-ALU_OUT, ALU_OP=INC_A |
| `A42` | `00C000000000` | MAR_LOAD<-RA |
| `A43` | `000820004000` | MEM_WRITE(RB), uSTEP_RESET |

</details>

<details>
<summary><strong>Pop</strong> (4 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `A80` | `0A0000000000` | PC_INC, IR_LOAD |
| `A81` | `00C000000000` | MAR_LOAD<-RA |
| `A82` | `401000000000` | MEM_READ(RA), WRITE RB<-MEMORY |
| `A83` | `9001A0000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A, uSTEP_RESET |

</details>

<details>
<summary><strong>Pushf</strong> (4 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `AC0` | `0A0000000000` | PC_INC, IR_LOAD |
| `AC1` | `900100000000` | WRITE RA<-ALU_OUT, ALU_OP=INC_A |
| `AC2` | `00C000000000` | MAR_LOAD<-RA |
| `AC3` | `000820008000` | MEM_WRITE(FLAGS), uSTEP_RESET |

</details>

<details>
<summary><strong>Popf</strong> (4 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `B00` | `0A0000000000` | PC_INC, IR_LOAD |
| `B01` | `00C000000000` | MAR_LOAD<-RA |
| `B02` | `001040002000` | MEM_READ(RA), FLAGS_LOAD<-DEC_COUNT_OUT, ALU_OP=ADD |
| `B03` | `9001A0000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A, uSTEP_RESET |

</details>

<details>
<summary><strong>Movm</strong> (7 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `B40` | `0A0000000000` | PC_INC, IR_LOAD |
| `B41` | `090000000000` | PC_INC, IMM_LOAD |
| `B42` | `098000000000` | PC_INC, IMM_LOAD, MAR_LOAD<-IMM |
| `B43` | `001000000200` | MEM_READ(RA), MDR_LOAD |
| `B44` | `008000000000` | MAR_LOAD<-IMM |
| `B45` | `00080000C000` | MEM_WRITE(MDR) |
| `B46` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Memcpy</strong> (17 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `B80` | `0A0000000000` | PC_INC, IR_LOAD |
| `B81` | `090000000000` | PC_INC, IMM_LOAD |
| `B82` | `090000000800` | PC_INC, IMM_LOAD, DEC_LOAD |
| `B83` | `A90000000000` | PC_INC, IMM_LOAD, WRITE RA<-IMM |
| `B84` | `600000000000` | WRITE RB<-IMM |
| `B85` | `00C000000000` | MAR_LOAD<-RA |
| `B86` | `001000000200` | MEM_READ(RA), MDR_LOAD |
| `B87` | `00E000000000` | MAR_LOAD<-RB |
| `B88` | `00080000C000` | MEM_WRITE(MDR) |
| `B89` | `000000000400` | DEC_CLK |
| `B8A` | `000040002000` | FLAGS_LOAD<-DEC_COUNT_OUT, ALU_OP=ADD |
| `B8B` | `000005020000` | uBRANCH->16 (if Z!=1) |
| `B8C` | `500080000000` | WRITE RB<-ALU_OUT, ALU_OP=SUB |
| `B8D` | `900180000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A |
| `B8E` | `500080000000` | WRITE RB<-ALU_OUT, ALU_OP=SUB |
| `B8F` | `000004510000` | uBRANCH->5 (unconditional) |
| `B90` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Memset</strong> (12 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `BC0` | `0A0000000000` | PC_INC, IR_LOAD |
| `BC1` | `090000000000` | PC_INC, IMM_LOAD |
| `BC2` | `090000000800` | PC_INC, IMM_LOAD, DEC_LOAD |
| `BC3` | `A00000000000` | WRITE RA<-IMM |
| `BC4` | `00C000000000` | MAR_LOAD<-RA |
| `BC5` | `000800004000` | MEM_WRITE(RB) |
| `BC6` | `000000000400` | DEC_CLK |
| `BC7` | `000040002000` | FLAGS_LOAD<-DEC_COUNT_OUT, ALU_OP=ADD |
| `BC8` | `000004B20000` | uBRANCH->11 (if Z!=1) |
| `BC9` | `900180000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A |
| `BCA` | `000004410000` | uBRANCH->4 (unconditional) |
| `BCB` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Jmp</strong> (4 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `C00` | `0A0000000000` | PC_INC, IR_LOAD |
| `C01` | `090000000000` | PC_INC, IMM_LOAD |
| `C02` | `A00000000000` | WRITE RA<-IMM |
| `C03` | `0C05A0000000` | PC_SEL(load-ALU), uSTEP_RESET |

</details>

<details>
<summary><strong>Jz</strong> (6 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `C40` | `0A0000000000` | PC_INC, IR_LOAD |
| `C41` | `090000000000` | PC_INC, IMM_LOAD |
| `C42` | `A00000000000` | WRITE RA<-IMM |
| `C43` | `000004500000` | uBRANCH->5 (if Z!=0) |
| `C44` | `0C0580000000` | PC_SEL(load-ALU) |
| `C45` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Jnz</strong> (6 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `C80` | `0A0000000000` | PC_INC, IR_LOAD |
| `C81` | `090000000000` | PC_INC, IMM_LOAD |
| `C82` | `A00000000000` | WRITE RA<-IMM |
| `C83` | `000004520000` | uBRANCH->5 (if Z!=1) |
| `C84` | `0C0580000000` | PC_SEL(load-ALU) |
| `C85` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Jc</strong> (6 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `CC0` | `0A0000000000` | PC_INC, IR_LOAD |
| `CC1` | `090000000000` | PC_INC, IMM_LOAD |
| `CC2` | `A00000000000` | WRITE RA<-IMM |
| `CC3` | `000004540000` | uBRANCH->5 (if C!=0) |
| `CC4` | `0C0580000000` | PC_SEL(load-ALU) |
| `CC5` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Jnc</strong> (6 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `D00` | `0A0000000000` | PC_INC, IR_LOAD |
| `D01` | `090000000000` | PC_INC, IMM_LOAD |
| `D02` | `A00000000000` | WRITE RA<-IMM |
| `D03` | `000004560000` | uBRANCH->5 (if C!=1) |
| `D04` | `0C0580000000` | PC_SEL(load-ALU) |
| `D05` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Jn</strong> (6 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `D40` | `0A0000000000` | PC_INC, IR_LOAD |
| `D41` | `090000000000` | PC_INC, IMM_LOAD |
| `D42` | `A00000000000` | WRITE RA<-IMM |
| `D43` | `000004580000` | uBRANCH->5 (if N!=0) |
| `D44` | `0C0580000000` | PC_SEL(load-ALU) |
| `D45` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Jnn</strong> (6 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `D80` | `0A0000000000` | PC_INC, IR_LOAD |
| `D81` | `090000000000` | PC_INC, IMM_LOAD |
| `D82` | `A00000000000` | WRITE RA<-IMM |
| `D83` | `0000045A0000` | uBRANCH->5 (if N!=1) |
| `D84` | `0C0580000000` | PC_SEL(load-ALU) |
| `D85` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Jv</strong> (6 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `DC0` | `0A0000000000` | PC_INC, IR_LOAD |
| `DC1` | `090000000000` | PC_INC, IMM_LOAD |
| `DC2` | `A00000000000` | WRITE RA<-IMM |
| `DC3` | `0000045C0000` | uBRANCH->5 (if V!=0) |
| `DC4` | `0C0580000000` | PC_SEL(load-ALU) |
| `DC5` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Call</strong> (14 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `E00` | `0A0000000000` | PC_INC, IR_LOAD |
| `E01` | `090000000000` | PC_INC, IMM_LOAD |
| `E02` | `900100000000` | WRITE RA<-ALU_OUT, ALU_OP=INC_A |
| `E03` | `00C000000000` | MAR_LOAD<-RA |
| `E04` | `700000000000` | WRITE RB<-PC |
| `E05` | `000800004000` | MEM_WRITE(RB) |
| `E06` | `600000000000` | WRITE RB<-IMM |
| `E07` | `500080000000` | WRITE RB<-ALU_OUT, ALU_OP=SUB |
| `E08` | `900080000000` | WRITE RA<-ALU_OUT, ALU_OP=SUB |
| `E09` | `500000000000` | WRITE RB<-ALU_OUT, ALU_OP=ADD |
| `E0A` | `0C0580000000` | PC_SEL(load-ALU) |
| `E0B` | `500080000000` | WRITE RB<-ALU_OUT, ALU_OP=SUB |
| `E0C` | `900080000000` | WRITE RA<-ALU_OUT, ALU_OP=SUB |
| `E0D` | `500020000000` | WRITE RB<-ALU_OUT, ALU_OP=ADD, uSTEP_RESET |

</details>

<details>
<summary><strong>Ret</strong> (11 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `E40` | `0A0000000000` | PC_INC, IR_LOAD |
| `E41` | `00C000000000` | MAR_LOAD<-RA |
| `E42` | `401000000000` | MEM_READ(RA), WRITE RB<-MEMORY |
| `E43` | `500080000000` | WRITE RB<-ALU_OUT, ALU_OP=SUB |
| `E44` | `900080000000` | WRITE RA<-ALU_OUT, ALU_OP=SUB |
| `E45` | `500000000000` | WRITE RB<-ALU_OUT, ALU_OP=ADD |
| `E46` | `0C0580000000` | PC_SEL(load-ALU) |
| `E47` | `500080000000` | WRITE RB<-ALU_OUT, ALU_OP=SUB |
| `E48` | `900080000000` | WRITE RA<-ALU_OUT, ALU_OP=SUB |
| `E49` | `500000000000` | WRITE RB<-ALU_OUT, ALU_OP=ADD |
| `E4A` | `9001A0000000` | WRITE RA<-ALU_OUT, ALU_OP=DEC_A, uSTEP_RESET |

</details>

<details>
<summary><strong>Ina</strong> (5 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `E80` | `0A0000000000` | PC_INC, IR_LOAD |
| `E81` | `090000000000` | PC_INC, IMM_LOAD |
| `E82` | `008000000000` | MAR_LOAD<-IMM |
| `E83` | `801000000000` | MEM_READ(RA), WRITE RA<-MEMORY |
| `E84` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Outa</strong> (5 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `EC0` | `0A0000000000` | PC_INC, IR_LOAD |
| `EC1` | `090000000000` | PC_INC, IMM_LOAD |
| `EC2` | `008000000000` | MAR_LOAD<-IMM |
| `EC3` | `000800000000` | MEM_WRITE(RA) |
| `EC4` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Inb</strong> (5 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `F00` | `0A0000000000` | PC_INC, IR_LOAD |
| `F01` | `090000000000` | PC_INC, IMM_LOAD |
| `F02` | `008000000000` | MAR_LOAD<-IMM |
| `F03` | `401000000000` | MEM_READ(RA), WRITE RB<-MEMORY |
| `F04` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Outb</strong> (5 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `F40` | `0A0000000000` | PC_INC, IR_LOAD |
| `F41` | `090000000000` | PC_INC, IMM_LOAD |
| `F42` | `008000000000` | MAR_LOAD<-IMM |
| `F43` | `000800004000` | MEM_WRITE(RB) |
| `F44` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Initsp</strong> (4 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `F80` | `0A0000000000` | PC_INC, IR_LOAD |
| `F81` | `500780000000` | WRITE RB<-ALU_OUT, ALU_OP=NOT_A |
| `F82` | `900480000000` | WRITE RA<-ALU_OUT, ALU_OP=OR |
| `F83` | `000020000000` | uSTEP_RESET |

</details>

<details>
<summary><strong>Halt</strong> (2 microsteps)</summary>

| Addr | Hex | Decoded control signals |
|---|---|---|
| `FC0` | `000000000000` | (idle) |
| `FC1` | `000020000000` | uSTEP_RESET |

</details>
