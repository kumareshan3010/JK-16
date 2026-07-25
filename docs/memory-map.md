# JK16 Memory Map

## Two Independent Address Spaces (Strict Harvard)

JK16 has no unified address space — Program ROM and the data memory (RAM/NVM/GPIO) sit on entirely separate buses that never intersect.

| Bus | Driven by | Destination | Size |
|---|---|---|---|
| Instruction bus | PC (Program Counter) | Program ROM | 128 kB (16-bit words) |
| Data bus | MAR (Memory Address Register) | RAM / NVM / GPIO | 64K addressable 16-bit words |

The PC can *only* address Program ROM; MAR can *only* address the RAM/NVM/GPIO space. There is no instruction that lets code read Program ROM as data or execute out of RAM.

## Data Address Space (via MAR)

The Address Decoder resolves every 16-bit address on the data bus to exactly one memory element, purely from the address bits — no separate chip-select instructions needed.

| Address range (hex) | Unit | Capacity | Decode rule |
|---|---|---|---|
| `0x8000`–`0xFFFF` | RAM | 64 kB (32K × 16-bit words) | bit15 (MSB) = 1 |
| `0x4000`–`0x7FFF` | NVM (EEPROM/flash) | 32 kB (16K × 16-bit words) | bit15 = 0, bit14 = 1 |
| `0x0000`–`0x0003` | GPIO ports 1–4 | 4 × 16-bit ports | bits 15:2 all 0; bits 1:0 select port |
| `0x0004`–`0x3FFF` | *(unused)* | — | bit15 = 0, bit14 = 0, and not a GPIO address — decoder leaves the bus idle |

RAM and NVM are each built from two 8-bit-wide chips joined horizontally to form one 16-bit word per address. All three memory elements share the same address lines and the same memory-in/memory-out data buses — only the element selected by the Address Decoder actually drives or receives data on a given cycle.

### RAM

- `0x8000`–`0xFFFF`, 32K locations, 16-bit words (64 kB total).
- General-purpose read/write memory — variables, the stack (grows downward from `0xFFFF`), and program data.
- `INITSP` sets the stack pointer (`A7`) to `0xFFFF`, the top of RAM, before any `PUSH`/`POP`/`CALL`/`RET` is used.

### NVM

- `0x4000`–`0x7FFF`, 16K locations, 16-bit words (32 kB total).
- Non-volatile storage (EEPROM/flash) — same access instructions as RAM (`LOADA`/`STOREA`/`LOADR`/`STORER`/etc.), distinguished purely by address.

### GPIO

- `0x0000`–`0x0003` — 4 memory-mapped ports, one address per port, 16 bits each (64 pins total).
- Ports are **bidirectional**: a memory write to a port address drives its pins as output; a memory read reads the external pin state as input. There's no separate direction register — direction follows whichever operation (`OUTA`/`OUTB` vs `INA`/`INB`) is issued.
- Accessed via the dedicated I/O instructions `INA`/`OUTA` (bank A) and `INB`/`OUTB` (bank B), specifying the port as `#1`–`#4`.

### Unused space

- `0x0004`–`0x3FFF` currently decodes to nothing — the memory block stays idle if this range is addressed. This is reserved headroom for future memory-mapped peripherals or expanded RAM/NVM.

## Program ROM

- 128 kB, organized as 16-bit words, addressed solely by the PC.
- Holds the program's instruction stream only — no data storage role.
- Two-word instructions (e.g. `LOAD RA, xxxxh`) occupy two consecutive words here; the PC advances an extra step to fetch the second word into the Immediate Register.

## Stack

- Located in RAM, growing **downward** from `0xFFFF`.
- `A7` is the stack pointer; must be initialized via `INITSP` before first use.
- `PUSH`/`POP` operate on general-purpose registers; `PUSHF`/`POPF` save/restore the flag register across calls.
