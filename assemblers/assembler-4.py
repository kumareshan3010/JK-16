#!/usr/bin/env python3
"""
Two-pass assembler for the custom 16-bit Harvard-architecture CPU.

Modes (for Geany's "Assemble" / "Build" menu entries):
    python3 assembler.py program.asm             -> ASSEMBLE (check only)
        Runs full symbol resolution + encoding, reports every error/warning
        in "file:line: level: message" form (Geany can jump to the line),
        writes NOTHING to disk.

    python3 assembler.py program.asm --build      -> BUILD
        Same checks; if there are zero errors, additionally writes:
            program_rom0.hex   (high byte of every instruction word)
            program_rom1.hex   (low byte of every instruction word)
            program_listing.txt (address / binary / source, for debugging)
        Refuses to write any file if there is at least one error.

Exit code is 0 if no errors were found, 1 otherwise (so Geany shows
success/failure correctly).
"""

import sys
import re
import os
import argparse

# --------------------------------------------------------------------------
# Opcode table:  mnemonic -> (opcode, format)
# LOAD is handled specially (shares the mnemonic across opcodes 0x22/0x23).
# --------------------------------------------------------------------------
OPCODES = {
    'ADD': (0x00, 'RR'),   'SUB': (0x01, 'RR'),
    'INC': (0x02, 'R'),    'DEC': (0x03, 'R'),
    'ADC': (0x04, 'RR'),   'SBB': (0x05, 'RR'),
    'NEG': (0x06, 'R'),    'ABS': (0x07, 'R'),
    'MUL': (0x08, 'RR'),   'DIV': (0x09, 'RR'),
    'MOD': (0x0A, 'RR'),   'MIN': (0x0B, 'RR'),
    'MAX': (0x0C, 'RR'),   'CMP': (0x0D, 'RR'),
    'TEST': (0x0E, 'RR'),  'NOP': (0x0F, 'NONE'),
    'AND': (0x10, 'RR'),   'OR': (0x11, 'RR'),
    'XOR': (0x12, 'RR'),   'NOT': (0x13, 'R'),
    'NAND': (0x14, 'RR'),  'NOR': (0x15, 'RR'),
    'XNOR': (0x16, 'RR'),  'MOV': (0x17, 'RR'),
    'SHL': (0x18, 'R'),    'SHR': (0x19, 'R'),
    'ROL': (0x1A, 'R'),    'ROR': (0x1B, 'R'),
    'BITMASK': (0x1C, 'R'),
    'BITSET': (0x1D, 'RR'), 'BITCLR': (0x1E, 'RR'), 'BITTEST': (0x1F, 'RR'),
    'LOADA': (0x20, 'RA_ADDR'), 'STOREA': (0x21, 'RA_ADDR'),
    # 0x22 / 0x23 -> LOAD, handled specially
    'STOREB': (0x24, 'RB_ADDR'),
    'LOADR': (0x25, 'RR'), 'STORER': (0x26, 'RR'),
    'CLR': (0x27, 'R'),
    'SWAP': (0x28, 'RR'),
    'PUSH': (0x29, 'RB_ONLY'), 'POP': (0x2A, 'RB_ONLY'),
    'PUSHF': (0x2B, 'NONE'), 'POPF': (0x2C, 'NONE'),
    'MOVM': (0x2D, 'MOVM'),
    'MEMCPY': (0x2E, 'MEMCPY'),
    'MEMSET': (0x2F, 'MEMSET'),
    'JMP': (0x30, 'JUMP'), 'JZ': (0x31, 'JUMP'), 'JNZ': (0x32, 'JUMP'),
    'JC': (0x33, 'JUMP'), 'JNC': (0x34, 'JUMP'), 'JN': (0x35, 'JUMP'),
    'JNN': (0x36, 'JUMP'), 'JV': (0x37, 'JUMP'), 'CALL': (0x38, 'JUMP'),
    'RET': (0x39, 'NONE'),
    'INA': (0x3A, 'RA_PORT'), 'OUTA': (0x3B, 'RA_PORT'),
    'INB': (0x3C, 'RB_PORT'), 'OUTB': (0x3D, 'RB_PORT'),
    'INITSP': (0x3E, 'NONE'),
    'HALT': (0x3F, 'NONE'),
}

WORDS_PER_FORMAT = {
    'RR': 1, 'R': 1, 'NONE': 1, 'RB_ONLY': 1,
    'RA_ADDR': 2, 'RB_ADDR': 2, 'RA_PORT': 2, 'RB_PORT': 2,
    'JUMP': 2, 'LOAD': 2, 'MOVM': 3, 'MEMSET': 3, 'MEMCPY': 4,
}

STACK_OPS = {'PUSH', 'POP', 'PUSHF', 'POPF', 'CALL', 'RET'}
A6_CLOBBER_OPS = {'MEMSET', 'JMP', 'JZ', 'JNZ', 'JC', 'JNC', 'JN', 'JNN',
                   'JV', 'CALL', 'RET'}

PORT_LOW, PORT_HIGH = 0x0000, 0x0003          # reserved for ports
STACK_LOW, STACK_HIGH = 0xFA00, 0xFFFF        # reserved for stack
VAR_AUTO_START = 0x0004
MAX_PROGRAM_WORDS = 0x10000                   # 16-bit word address space

# --------------------------------------------------------------------------
# Regexes
# --------------------------------------------------------------------------
REG_RE = re.compile(r'^([AB])([0-7])$', re.IGNORECASE)
IMM_RE = re.compile(r'^([0-9A-Fa-f]{1,4})h$')                  # xxxxh
ADDR_LIT_RE = re.compile(r'^#([0-9A-Fa-f]{1,4})h$')            # #xxxxh
SYMREF_RE = re.compile(r'^#([A-Za-z_][A-Za-z0-9_]*)$')         # #Name
PORT_RE = re.compile(r'^#([0-9A-Fa-f]{1,2})h?$')               # #port
LABEL_DECL_RE = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*):$')
VAR_DECL_RE = re.compile(
    r'^VAR\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*=\s*#([0-9A-Fa-f]{1,4})h)?$',
    re.IGNORECASE)


class Diag:
    def __init__(self, line_no, level, msg):
        self.line_no = line_no
        self.level = level      # 'error' or 'warning'
        self.msg = msg


def strip_comments(text):
    """Remove // line comments and /* ... */ block comments (newlines
    preserved so line numbers stay accurate). Returns (clean_text, errors)
    where errors is a list of (line_no, message) for any unterminated
    block comment. If a /* is never closed, everything from that point to
    end of file is treated as comment (never silently assembled as code) 
    and a single error is reported at the line the comment opened."""
    errors = []
    result = []
    line_no = 1
    pos = 0
    length = len(text)
    in_comment = False
    comment_start_line = None

    while pos < length:
        ch = text[pos]
        if not in_comment:
            if text.startswith('/*', pos):
                in_comment = True
                comment_start_line = line_no
                pos += 2
                continue
            if text.startswith('//', pos):
                nl = text.find('\n', pos)
                pos = length if nl == -1 else nl
                continue
            result.append(ch)
            if ch == '\n':
                line_no += 1
            pos += 1
        else:
            if text.startswith('*/', pos):
                in_comment = False
                pos += 2
                continue
            if ch == '\n':
                result.append('\n')
                line_no += 1
            pos += 1

    if in_comment:
        errors.append((comment_start_line,
                        "unterminated block comment '/*' (no matching '*/' "
                        "found) - everything after this point was treated "
                        "as comment and ignored"))
    return ''.join(result), errors


class Assembler:
    def __init__(self, filename, raw_text):
        self.filename = filename
        clean_text, comment_errors = strip_comments(raw_text)
        self.lines = clean_text.split('\n')
        self.diags = []
        for line_no, msg in comment_errors:
            self.error(line_no, msg)
        self.labels = {}          # name -> word address
        self.label_lines = {}     # name -> declaration line number
        self.vars = {}            # name -> data address
        self.var_addr_owner = {}  # address -> name
        self.instr_lines = []     # (line_no, address, mnemonic, operands)
        self.words_out = {}       # address -> list of 16-bit ints
        self.seen_initsp = False
        self.has_mask = {}        # 'A0'..'A6' -> bool

    def error(self, line_no, msg):
        self.diags.append(Diag(line_no, 'error', msg))

    def warn(self, line_no, msg):
        self.diags.append(Diag(line_no, 'warning', msg))

    def has_errors(self):
        return any(d.level == 'error' for d in self.diags)

    # ---------------------------------------------------------------- pass1
    def pass1(self):
        addr = 0
        explicit_vars = []
        implicit_vars = []

        for i, raw in enumerate(self.lines, start=1):
            line = raw.strip()
            if not line:
                continue

            m = LABEL_DECL_RE.match(line)
            if m:
                name = m.group(1)
                if name in self.labels:
                    self.error(i, f"label '{name}' already declared")
                else:
                    self.labels[name] = addr
                    self.label_lines[name] = i
                continue

            m = VAR_DECL_RE.match(line)
            if m:
                name, hexaddr = m.group(1), m.group(2)
                if hexaddr is not None:
                    explicit_vars.append((i, name, int(hexaddr, 16)))
                else:
                    implicit_vars.append((i, name))
                continue

            mnemonic = line.split()[0].upper()
            rest = line[len(line.split()[0]):].strip()
            operands = [t.strip() for t in rest.split(',')] if rest else []

            if mnemonic == 'LOAD':
                fmt = 'LOAD'
            elif mnemonic in OPCODES:
                fmt = OPCODES[mnemonic][1]
            else:
                self.error(i, f"unknown instruction '{mnemonic}'")
                continue

            if addr >= MAX_PROGRAM_WORDS:
                self.error(i, "program exceeds addressable program memory (65536 words)")
                continue

            self.instr_lines.append((i, addr, mnemonic, operands))
            addr += WORDS_PER_FORMAT[fmt]

        self.program_length = addr

        # --- resolve explicit VAR addresses first ---
        for line_no, name, vaddr in explicit_vars:
            if name in self.vars:
                self.error(line_no, f"variable '{name}' already declared")
                continue
            if name in self.labels:
                self.error(line_no,
                            f"'{name}' is already declared as a label "
                            f"(line {self.label_lines[name]}) - names must "
                            f"be unique across labels and variables")
                continue
            if PORT_LOW <= vaddr <= PORT_HIGH:
                self.error(line_no,
                            f"address {vaddr:04X}h is reserved for ports "
                            f"(0000h-0003h)")
                continue
            if STACK_LOW <= vaddr <= STACK_HIGH:
                self.error(line_no,
                            f"address {vaddr:04X}h is inside the reserved "
                            f"stack region (FA00h-FFFFh)")
                continue
            if vaddr in self.var_addr_owner:
                self.error(line_no,
                            f"address {vaddr:04X}h already assigned to "
                            f"variable '{self.var_addr_owner[vaddr]}'")
                continue
            self.vars[name] = vaddr
            self.var_addr_owner[vaddr] = name

        # --- auto-allocate implicit VARs ---
        cursor = VAR_AUTO_START
        for line_no, name in implicit_vars:
            if name in self.vars:
                self.error(line_no, f"variable '{name}' already declared")
                continue
            if name in self.labels:
                self.error(line_no,
                            f"'{name}' is already declared as a label "
                            f"(line {self.label_lines[name]}) - names must "
                            f"be unique across labels and variables")
                continue
            while True:
                if PORT_LOW <= cursor <= PORT_HIGH:
                    cursor = PORT_HIGH + 1
                    continue
                if STACK_LOW <= cursor <= STACK_HIGH:
                    self.error(line_no,
                                "ran out of data memory for auto-allocated "
                                f"variable '{name}' (hit reserved stack region)")
                    cursor = None
                    break
                if cursor in self.var_addr_owner:
                    cursor += 1
                    continue
                break
            if cursor is None:
                continue
            self.vars[name] = cursor
            self.var_addr_owner[cursor] = name
            cursor += 1

    # ---------------------------------------------------------------- utils
    def parse_reg(self, tok, bank, line_no):
        m = REG_RE.match(tok)
        if not m:
            self.error(line_no, f"expected {bank} register, got '{tok}'")
            return 0
        letter, num = m.group(1).upper(), int(m.group(2))
        if letter != bank:
            self.error(line_no, f"expected {bank} register, got '{tok}'")
            return 0
        if num == 7:
            self.error(line_no,
                        f"explicit use of {bank}7 is not allowed "
                        f"({'stack pointer' if bank == 'A' else 'hidden register'})")
            return 0
        return num

    def parse_immediate(self, tok, line_no):
        m = IMM_RE.match(tok)
        if not m:
            self.error(line_no,
                        f"expected immediate value 'xxxxh' (no '#'), got '{tok}'")
            return 0
        return int(m.group(1), 16)

    def parse_port(self, tok, line_no):
        m = PORT_RE.match(tok)
        if not m:
            self.error(line_no, f"expected port '#1'-'#4', got '{tok}'")
            return 0
        val = int(m.group(1), 16)
        if not (1 <= val <= 4):
            self.error(line_no, f"port must be 1-4, got '{tok}'")
            return 0
        return val

    def parse_addr(self, tok, line_no, allow_label, allow_var):
        # Check the strict hex-literal pattern FIRST. Hex digits A-F are
        # also valid identifier letters, so a token like '#FB00h' matches
        # both the literal pattern and the symbol-name pattern - if we
        # checked symbols first, '#FB00h' would be misread as a symbol
        # named "FB00h" instead of the address FB00h. Literal wins.
        m = ADDR_LIT_RE.match(tok)
        if m:
            val = int(m.group(1), 16)
            if allow_var:
                # This operand addresses data memory - the reserved zones
                # (ports, stack) apply here, same as they do to VAR
                # declarations.
                if PORT_LOW <= val <= PORT_HIGH:
                    self.error(line_no,
                                f"address {val:04X}h is reserved for ports "
                                f"(0000h-0003h)")
                    return 0
                if STACK_LOW <= val <= STACK_HIGH:
                    self.error(line_no,
                                f"address {val:04X}h is inside the reserved "
                                f"stack region (FA00h-FFFFh)")
                    return 0
                if val in self.var_addr_owner:
                    self.warn(line_no,
                                f"address {val:04X}h matches variable "
                                f"'{self.var_addr_owner[val]}' - consider using "
                                f"'#{self.var_addr_owner[val]}'")
            return val

        m = SYMREF_RE.match(tok)
        if m:
            name = m.group(1)
            if allow_label and name in self.labels:
                return self.labels[name]
            if allow_var and name in self.vars:
                return self.vars[name]
            if name in self.labels and not allow_label:
                self.error(line_no,
                            f"'{name}' is a label, not valid as a data address here")
                return 0
            if name in self.vars and not allow_var:
                self.error(line_no,
                            f"'{name}' is a variable, not valid as a jump target")
                return 0
            self.error(line_no, f"undefined symbol '{name}'")
            return 0

        self.error(line_no, f"expected '#xxxxh' or '#Name', got '{tok}'")
        return 0

    def need(self, operands, count, line_no, mnemonic):
        if len(operands) != count:
            self.error(line_no,
                        f"{mnemonic} expects {count} operand(s), got {len(operands)}")
            return False
        return True

    # ---------------------------------------------------------------- pass2
    def pass2(self):
        for line_no, addr, mnemonic, ops in self.instr_lines:

            if mnemonic in STACK_OPS:
                if not self.seen_initsp:
                    self.error(line_no,
                                f"{mnemonic} used before INITSP has been executed")
            if mnemonic == 'INITSP':
                if self.seen_initsp:
                    self.error(line_no, "INITSP executed more than once")
                self.seen_initsp = True

            if mnemonic in A6_CLOBBER_OPS:
                self.warn(line_no,
                            f"{mnemonic} will overwrite A6 (scratch register "
                            "for this operation)")

            words = self.encode(line_no, addr, mnemonic, ops)
            if words is not None:
                self.words_out[addr] = words

        if self.instr_lines:
            last_line_no = self.instr_lines[-1][0]
            has_halt = any(m == 'HALT' for _, _, m, _ in self.instr_lines)
            if not has_halt:
                self.warn(last_line_no,
                            "program contains no HALT instruction - "
                            "execution will run past the end of program memory")

    def encode(self, line_no, addr, mnemonic, ops):
        if mnemonic == 'LOAD':
            if not self.need(ops, 2, line_no, mnemonic):
                return None
            m = REG_RE.match(ops[0])
            if not m:
                self.error(line_no, f"expected register, got '{ops[0]}'")
                return None
            letter, num = m.group(1).upper(), int(m.group(2))
            if num == 7:
                self.error(line_no, f"explicit use of {letter}7 is not allowed")
                return None
            if letter == 'A':
                opcode, ra, rb = 0x22, num, 7
            else:
                opcode, ra, rb = 0x23, 6, num
            imm = self.parse_immediate(ops[1], line_no)
            return [self.pack(opcode, ra, rb), imm]

        opcode, fmt = OPCODES[mnemonic]

        if fmt == 'RR':
            if not self.need(ops, 2, line_no, mnemonic):
                return None
            ra = self.parse_reg(ops[0], 'A', line_no)
            rb = self.parse_reg(ops[1], 'B', line_no)
            if mnemonic in ('BITSET', 'BITCLR', 'BITTEST'):
                key = f'A{ra}'
                if not self.has_mask.get(key):
                    self.warn(line_no,
                                f"no BITMASK generated for A{ra} before "
                                f"{mnemonic} (assumed already valid)")
            return [self.pack(opcode, ra, rb)]

        if fmt == 'R':
            if not self.need(ops, 1, line_no, mnemonic):
                return None
            ra = self.parse_reg(ops[0], 'A', line_no)
            if mnemonic == 'BITMASK':
                self.has_mask[f'A{ra}'] = True
            return [self.pack(opcode, ra, 7)]

        if fmt == 'NONE':
            if not self.need(ops, 0, line_no, mnemonic):
                return None
            return [self.pack(opcode, 6, 7)]

        if fmt == 'RB_ONLY':
            if not self.need(ops, 1, line_no, mnemonic):
                return None
            rb = self.parse_reg(ops[0], 'B', line_no)
            return [self.pack(opcode, 6, rb)]

        if fmt == 'RA_ADDR':
            if not self.need(ops, 2, line_no, mnemonic):
                return None
            ra = self.parse_reg(ops[0], 'A', line_no)
            address = self.parse_addr(ops[1], line_no, allow_label=False, allow_var=True)
            return [self.pack(opcode, ra, 7), address]

        if fmt == 'RB_ADDR':
            if not self.need(ops, 2, line_no, mnemonic):
                return None
            rb = self.parse_reg(ops[0], 'B', line_no)
            address = self.parse_addr(ops[1], line_no, allow_label=False, allow_var=True)
            return [self.pack(opcode, 6, rb), address]

        if fmt == 'RA_PORT':
            if not self.need(ops, 2, line_no, mnemonic):
                return None
            ra = self.parse_reg(ops[0], 'A', line_no)
            port = self.parse_port(ops[1], line_no)
            return [self.pack(opcode, ra, 7), port]

        if fmt == 'RB_PORT':
            if not self.need(ops, 2, line_no, mnemonic):
                return None
            rb = self.parse_reg(ops[0], 'B', line_no)
            port = self.parse_port(ops[1], line_no)
            return [self.pack(opcode, 6, rb), port]

        if fmt == 'JUMP':
            if not self.need(ops, 1, line_no, mnemonic):
                return None
            target = self.parse_addr(ops[0], line_no, allow_label=True, allow_var=False)
            return [self.pack(opcode, 6, 7), target]

        if fmt == 'MOVM':
            if not self.need(ops, 2, line_no, mnemonic):
                return None
            src = self.parse_addr(ops[0], line_no, allow_label=False, allow_var=True)
            dst = self.parse_addr(ops[1], line_no, allow_label=False, allow_var=True)
            return [self.pack(opcode, 6, 7), src, dst]

        if fmt == 'MEMCPY':
            if not self.need(ops, 3, line_no, mnemonic):
                return None
            length = self.parse_immediate(ops[0], line_no)
            src = self.parse_addr(ops[1], line_no, allow_label=False, allow_var=True)
            dst = self.parse_addr(ops[2], line_no, allow_label=False, allow_var=True)
            return [self.pack(opcode, 6, 7), length, src, dst]

        if fmt == 'MEMSET':
            if not self.need(ops, 3, line_no, mnemonic):
                return None
            rb = self.parse_reg(ops[0], 'B', line_no)
            length = self.parse_immediate(ops[1], line_no)
            dst = self.parse_addr(ops[2], line_no, allow_label=False, allow_var=True)
            return [self.pack(opcode, 6, rb), length, dst]

        self.error(line_no, f"internal: unhandled format '{fmt}' for {mnemonic}")
        return None

    @staticmethod
    def pack(opcode, ra, rb):
        return ((opcode & 0x3F) << 10) | ((ra & 0x7) << 7) | ((rb & 0x7) << 4)

    # ------------------------------------------------------------- listing
    def build_listing(self):
        out = []
        out.append(f"; Listing for {self.filename}")
        out.append(f"; Labels: {len(self.labels)}   Variables: {len(self.vars)}")
        out.append(";")
        if self.vars:
            out.append("; Variable map (data memory):")
            for name, a in sorted(self.vars.items(), key=lambda kv: kv[1]):
                out.append(f";   {name:<20} = {a:04X}h")
            out.append(";")
        out.append(f"{'ADDR':<6} {'WORD(bin)':<18} SOURCE")
        out.append("-" * 60)

        source_by_line = {i + 1: l for i, l in enumerate(self.lines)}
        for line_no, addr, mnemonic, ops in self.instr_lines:
            words = self.words_out.get(addr)
            src = source_by_line.get(line_no, '').strip()
            if words is None:
                out.append(f"{addr:04X}h  {'<error>':<18} {src}")
                continue
            for wi, w in enumerate(words):
                a = addr + wi
                tag = src if wi == 0 else '  (operand word)'
                out.append(f"{a:04X}h  {w:016b}   {tag}")
        return '\n'.join(out)

    # ---------------------------------------------------------- hex output
    def build_rom_bytes(self):
        """Returns (rom0_bytes, rom1_bytes) - rom0=high byte, rom1=low byte,
        one entry per program-memory word address."""
        rom0 = [0] * self.program_length
        rom1 = [0] * self.program_length
        for addr, words in self.words_out.items():
            for wi, w in enumerate(words):
                a = addr + wi
                rom0[a] = (w >> 8) & 0xFF
                rom1[a] = w & 0xFF
        return rom0, rom1

    def build_combined_bytes(self):
        """Returns one byte array with each 16-bit word split MSB-first
        (high byte, then low byte) - for a single 16-bit-wide ROM, as used
        by Digital (hneemann) when the ROM's 'Big Endian' option is
        enabled. Byte-address of word N is 2*N, matching Digital's own
        hex-file addressing for multi-byte-wide memories."""
        rom0, rom1 = self.build_rom_bytes()
        combined = []
        for hi, lo in zip(rom0, rom1):
            combined.append(hi)
            combined.append(lo)
        return combined

    @staticmethod
    def intel_hex(byte_array, bytes_per_line=16):
        lines = []
        i = 0
        n = len(byte_array)
        while i < n:
            chunk = byte_array[i:i + bytes_per_line]
            count = len(chunk)
            rec = [count, (i >> 8) & 0xFF, i & 0xFF, 0x00] + chunk
            checksum = (-sum(rec)) & 0xFF
            hexstr = ''.join(f'{b:02X}' for b in rec)
            lines.append(f':{hexstr}{checksum:02X}')
            i += count
        lines.append(':00000001FF')
        return '\n'.join(lines)

    def run(self):
        self.pass1()
        self.pass2()


# --------------------------------------------------------------------------
# Digital (hneemann) TCP remote interface.
#
# Protocol (reverse-engineered from hneemann/Assembler's RemoteInterface.java):
#   - Connect to 127.0.0.1:41114 (must be enabled in Digital's Settings -
#     the remote server is OFF by default in current versions).
#   - Every message (both directions) uses Java's DataOutputStream.writeUTF
#     wire format: a 2-byte big-endian length prefix (byte length of the
#     UTF-8 payload, not character count) followed by the UTF-8 bytes.
#   - Commands: "start:<hexfilepath>", "debug:<hexfilepath>", "run",
#     "step", "stop". start/debug tell Digital to load that hex file into
#     the circuit's program memory; start also begins free-running clocking,
#     debug does not (leaves you to step/run manually from Digital's GUI).
#   - Response: "ok", "ok:<hex address>" (run/step), or an error string.
# --------------------------------------------------------------------------
class DigitalRemoteError(Exception):
    pass


def digital_send(command, host='127.0.0.1', port=41114, timeout=5.0):
    import socket

    def recv_exact(sock, n):
        buf = b''
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise DigitalRemoteError(
                    "connection closed while waiting for Digital's response")
            buf += chunk
        return buf

    try:
        with socket.create_connection((host, port), timeout=timeout) as s:
            payload = command.encode('utf-8')
            s.sendall(len(payload).to_bytes(2, 'big') + payload)
            resp_len = int.from_bytes(recv_exact(s, 2), 'big')
            response = recv_exact(s, resp_len).decode('utf-8')
    except OSError as e:
        raise DigitalRemoteError(
            f"could not reach Digital at {host}:{port} - is Digital running "
            f"with the circuit open and remote control enabled in "
            f"Settings? ({e})")

    if not (response == 'ok' or response.startswith('ok:')):
        raise DigitalRemoteError(f"Digital reported an error: {response}")
    return response


def main():
    parser = argparse.ArgumentParser(description="Custom ISA two-pass assembler")
    parser.add_argument('source', help='input .asm file')
    parser.add_argument('--build', action='store_true',
                         help='write rom0.hex/rom1.hex/listing.txt (default: check only)')
    parser.add_argument('--digital', action='store_true',
                         help='also write a combined 16-bit-word hex file and '
                              'send it to Digital (hneemann) over TCP (implies --build)')
    parser.add_argument('--digital-mode', choices=['start', 'debug'], default='debug',
                         help="'debug' loads without starting the clock (default); "
                              "'start' loads and begins free-running clocking immediately")
    parser.add_argument('--digital-host', default='127.0.0.1')
    parser.add_argument('--digital-port', type=int, default=41114)
    args = parser.parse_args()

    if args.digital:
        args.build = True

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

    if not args.build:
        # ASSEMBLE (check-only) mode: never writes files.
        sys.exit(1 if errors else 0)

    # BUILD mode
    if errors:
        print("Build aborted: fix all errors first.")
        sys.exit(1)

    base = os.path.splitext(path)[0]
    rom0, rom1 = asm.build_rom_bytes()

    rom0_path = base + "_rom0.hex"
    rom1_path = base + "_rom1.hex"
    listing_path = base + "_listing.txt"

    with open(rom0_path, 'w') as f:
        f.write(asm.intel_hex(rom0) + '\n')
    with open(rom1_path, 'w') as f:
        f.write(asm.intel_hex(rom1) + '\n')
    with open(listing_path, 'w') as f:
        f.write(asm.build_listing() + '\n')

    print(f"Wrote {rom0_path}")
    print(f"Wrote {rom1_path}")
    print(f"Wrote {listing_path}")
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
