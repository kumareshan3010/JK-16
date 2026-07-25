# CPU Assembler (C port)

A C port of the Python two-pass assembler for JK16. See [`syntax.md`](syntax.md) for the assembly
language reference.

All four tools share one core implementation (`core.c`/`core.h`) - the
same lexer, symbol resolution, and instruction encoder - so their
diagnostics and encoding are identical. They only differ in what they do
with the result.

## Build

```sh
make
```

Produces four binaries: `assembler_full`, `assembler_check`,
`assembler_hex`, `assembler_txt`.

## Tools

### `assembler_full` - the full assembler

Matches the original Python CLI exactly.

```sh
./assembler_full program.asm             # check only, writes nothing
./assembler_full program.asm --build     # writes rom0.hex, rom1.hex, listing.txt
./assembler_full program.asm --digital [--digital-mode start|debug]
                                          # also writes a combined hex file and
                                          # sends it to Digital (hneemann) over TCP
```

### `assembler_check` - syntax/semantic check only

Runs full symbol resolution and encoding so it catches everything the
real assembler would, but **never writes a file**, no matter what.

```sh
./assembler_check program.asm
```

### `assembler_hex` - hex output only

Builds and, on success, writes only `program_rom0.hex` and
`program_rom1.hex` (no listing).

```sh
./assembler_hex program.asm
```

### `assembler_txt` - listing output only

Builds and, on success, writes only `program_listing.txt` (no hex).

```sh
./assembler_txt program.asm
```

## Exit codes

All four tools exit `0` if there are zero errors, `1` otherwise (warnings
never cause a nonzero exit).
