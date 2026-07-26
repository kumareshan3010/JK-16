#!/usr/bin/env python3
"""
asm_check.py - syntax/semantic checker for the JK16 ISA.

Runs full two-pass assembly (symbol resolution + encoding) and reports
every error/warning in "file:line: level: message" form, but writes
NOTHING to disk under any circumstances. Use this when you only want to
know whether a program is valid - for machine code output, use
asm_machinecode.py; for a debug listing, use asm_listing.py.

Usage:
    python3 asm_check.py program.asm

Exit code: 0 if no errors were found, 1 otherwise.
"""

import sys
import os

from assembler_core import Assembler


def main():
    if len(sys.argv) != 2:
        print("usage: asm_check.py <program.asm>")
        sys.exit(1)

    path = sys.argv[1]
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

    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
