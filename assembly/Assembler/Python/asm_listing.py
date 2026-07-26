#!/usr/bin/env python3
"""
asm_listing.py - human-readable listing generator for the JK16 ISA.

Assembles a program and, if there are zero errors, writes ONLY the
debug listing:
    <name>_listing.txt   address / binary word / source line, plus a
                          variable map

No machine-code hex files are produced - use asm_machinecode.py for
that. Refuses to write the listing if there is at least one error.

Usage:
    python3 asm_listing.py program.asm

Exit code: 0 on a successful build, 1 if there were errors.
"""

import sys
import os

from assembler_core import Assembler


def main():
    if len(sys.argv) != 2:
        print("usage: asm_listing.py <program.asm>")
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

    if errors:
        print("Build aborted: fix all errors first.")
        sys.exit(1)

    base = os.path.splitext(path)[0]
    listing_path = base + "_listing.txt"

    with open(listing_path, 'w') as f:
        f.write(asm.build_listing() + '\n')

    print(f"Wrote {listing_path}")
    sys.exit(0)


if __name__ == '__main__':
    main()
