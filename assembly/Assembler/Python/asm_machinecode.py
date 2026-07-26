#!/usr/bin/env python3
"""
asm_machinecode.py - machine-code generator for the JK16 ISA.

Assembles a program and, if there are zero errors, writes ONLY the
machine code:
    <name>_rom0.hex   high byte of every instruction word (Intel HEX)
    <name>_rom1.hex   low byte of every instruction word (Intel HEX)

No listing file is produced - use asm_listing.py for that. Refuses to
write any file if there is at least one error.

Usage:
    python3 asm_machinecode.py program.asm
    python3 asm_machinecode.py program.asm --digital [--digital-mode start|debug]

--digital additionally writes a combined 16-bit-word hex file
(<name>_digital.hex) and sends it to a running Digital (hneemann)
simulator instance over TCP, the same as the original combined
assembler's --digital flag.

Exit code: 0 on a successful build, 1 if there were errors or the
Digital load failed.
"""

import sys
import os
import argparse

from assembler_core import Assembler
from digital_remote import digital_send, DigitalRemoteError


def main():
    parser = argparse.ArgumentParser(description="JK16 machine-code assembler")
    parser.add_argument('source', help='input .asm file')
    parser.add_argument('--digital', action='store_true',
                         help='also write a combined 16-bit-word hex file and '
                              'send it to Digital (hneemann) over TCP')
    parser.add_argument('--digital-mode', choices=['start', 'debug'], default='debug',
                         help="'debug' loads without starting the clock (default); "
                              "'start' loads and begins free-running clocking immediately")
    parser.add_argument('--digital-host', default='127.0.0.1')
    parser.add_argument('--digital-port', type=int, default=41114)
    args = parser.parse_args()

    path = args.source
    if not os.path.isfile(path):
        print(f"error: file not found: {path}")
        sys.exit(1)

    with open(path, 'r') as f:
        raw = f.read()

    filename = os.path.basename(path)
    asm = Assembler(filename, raw)
    asm.run()

    for d in sorted(asm.diags, key=lambda d: d.line_no):
        print(f"{filename}:{d.line_no}: {d.level}: {d.msg}")

    errors = [d for d in asm.diags if d.level == 'error']
    warnings = [d for d in asm.diags if d.level == 'warning']
    print(f"\n{len(errors)} error(s), {len(warnings)} warning(s)")

    if errors:
        print("Build aborted: fix all errors first.")
        sys.exit(1)

    base = os.path.splitext(path)[0]
    rom0, rom1 = asm.build_rom_bytes()

    rom0_path = base + "_rom0.hex"
    rom1_path = base + "_rom1.hex"

    with open(rom0_path, 'w') as f:
        f.write(asm.intel_hex(rom0) + '\n')
    with open(rom1_path, 'w') as f:
        f.write(asm.intel_hex(rom1) + '\n')

    print(f"Wrote {rom0_path}")
    print(f"Wrote {rom1_path}")
    print(f"Program length: {asm.program_length} words")

    if args.digital:
        digital_path = os.path.abspath(base + "_digital.hex")
        combined = asm.build_combined_bytes()
        with open(digital_path, 'w') as f:
            f.write(asm.intel_hex(combined) + '\n')
        print(f"Wrote {digital_path}")

        cmd = f"{args.digital_mode}:{digital_path}"
        try:
            response = digital_send(cmd, args.digital_host, args.digital_port)
            print(f"Digital: {response}")
        except DigitalRemoteError as e:
            print(f"Digital load failed: {e}")
            sys.exit(1)

    sys.exit(0)


if __name__ == '__main__':
    main()
