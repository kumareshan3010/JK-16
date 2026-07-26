/*
 * assembler.c - Two-pass assembler for the JK16 16-bit Harvard-architecture
 * CPU. C port of the original Python assembler (assembler-4.py) - same
 * language, same checks, same output formats, same Digital (hneemann)
 * TCP integration.
 *
 * Modes:
 *   ./assembler program.asm             -> ASSEMBLE (check only)
 *       Runs full symbol resolution + encoding, reports every error/warning
 *       in "file:line: level: message" form, writes NOTHING to disk.
 *
 *   ./assembler program.asm --build     -> BUILD
 *       Same checks; if there are zero errors, additionally writes:
 *           program_rom0.hex    (high byte of every instruction word)
 *           program_rom1.hex    (low byte of every instruction word)
 *           program_listing.txt (address / binary / source, for debugging)
 *       Refuses to write any file if there is at least one error.
 *
 *   ./assembler program.asm --digital [--digital-mode start|debug]
 *       Implies --build. Also writes program_digital.hex (combined
 *       16-bit-word format) and sends it to a running Digital simulator
 *       instance over TCP.
 *
 * Exit code is 0 if no errors were found, 1 otherwise.
 *
 * Build: gcc -std=c11 -O2 -o assembler assembler.c
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <regex.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/time.h>

/* ========================================================================
 * Opcode table: mnemonic -> (opcode, format)
 * LOAD is handled specially (shares the mnemonic across opcodes 0x22/0x23).
 * ======================================================================== */
typedef struct {
    const char *mnemonic;
    int opcode;
    const char *fmt;
} OpcodeEntry;

static const OpcodeEntry OPCODES[] = {
    {"ADD", 0x00, "RR"},   {"SUB", 0x01, "RR"},
    {"INC", 0x02, "R"},    {"DEC", 0x03, "R"},
    {"ADC", 0x04, "RR"},   {"SBB", 0x05, "RR"},
    {"NEG", 0x06, "R"},    {"ABS", 0x07, "R"},
    {"MUL", 0x08, "RR"},   {"DIV", 0x09, "RR"},
    {"MOD", 0x0A, "RR"},   {"MIN", 0x0B, "RR"},
    {"MAX", 0x0C, "RR"},   {"CMP", 0x0D, "RR"},
    {"TEST", 0x0E, "RR"},  {"NOP", 0x0F, "NONE"},
    {"AND", 0x10, "RR"},   {"OR", 0x11, "RR"},
    {"XOR", 0x12, "RR"},   {"NOT", 0x13, "R"},
    {"NAND", 0x14, "RR"},  {"NOR", 0x15, "RR"},
    {"XNOR", 0x16, "RR"},  {"MOV", 0x17, "RR"},
    {"SHL", 0x18, "R"},    {"SHR", 0x19, "R"},
    {"ROL", 0x1A, "R"},    {"ROR", 0x1B, "R"},
    {"BITMASK", 0x1C, "R"},
    {"BITSET", 0x1D, "RR"}, {"BITCLR", 0x1E, "RR"}, {"BITTEST", 0x1F, "RR"},
    {"LOADA", 0x20, "RA_ADDR"}, {"STOREA", 0x21, "RA_ADDR"},
    /* 0x22 / 0x23 -> LOAD, handled specially */
    {"STOREB", 0x24, "RB_ADDR"},
    {"LOADR", 0x25, "RR"}, {"STORER", 0x26, "RR"},
    {"CLR", 0x27, "R"},
    {"SWAP", 0x28, "RR"},
    {"PUSH", 0x29, "RB_ONLY"}, {"POP", 0x2A, "RB_ONLY"},
    {"PUSHF", 0x2B, "NONE"}, {"POPF", 0x2C, "NONE"},
    {"MOVM", 0x2D, "MOVM"},
    {"MEMCPY", 0x2E, "MEMCPY"},
    {"MEMSET", 0x2F, "MEMSET"},
    {"JMP", 0x30, "JUMP"}, {"JZ", 0x31, "JUMP"}, {"JNZ", 0x32, "JUMP"},
    {"JC", 0x33, "JUMP"}, {"JNC", 0x34, "JUMP"}, {"JN", 0x35, "JUMP"},
    {"JNN", 0x36, "JUMP"}, {"JV", 0x37, "JUMP"}, {"CALL", 0x38, "JUMP"},
    {"RET", 0x39, "NONE"},
    {"INA", 0x3A, "RA_PORT"}, {"OUTA", 0x3B, "RA_PORT"},
    {"INB", 0x3C, "RB_PORT"}, {"OUTB", 0x3D, "RB_PORT"},
    {"INITSP", 0x3E, "NONE"},
    {"HALT", 0x3F, "NONE"},
};
static const int N_OPCODES = sizeof(OPCODES) / sizeof(OPCODES[0]);

static int words_for_format(const char *fmt) {
    if (!strcmp(fmt, "RR")) return 1;
    if (!strcmp(fmt, "R")) return 1;
    if (!strcmp(fmt, "NONE")) return 1;
    if (!strcmp(fmt, "RB_ONLY")) return 1;
    if (!strcmp(fmt, "RA_ADDR")) return 2;
    if (!strcmp(fmt, "RB_ADDR")) return 2;
    if (!strcmp(fmt, "RA_PORT")) return 2;
    if (!strcmp(fmt, "RB_PORT")) return 2;
    if (!strcmp(fmt, "JUMP")) return 2;
    if (!strcmp(fmt, "LOAD")) return 2;
    if (!strcmp(fmt, "MOVM")) return 3;
    if (!strcmp(fmt, "MEMSET")) return 3;
    if (!strcmp(fmt, "MEMCPY")) return 4;
    return 1;
}

static const char *STACK_OPS[] = {"PUSH", "POP", "PUSHF", "POPF", "CALL", "RET"};
static const int N_STACK_OPS = 6;
static const char *A6_CLOBBER_OPS[] = {
    "MEMSET", "JMP", "JZ", "JNZ", "JC", "JNC", "JN", "JNN", "JV", "CALL", "RET"};
static const int N_A6_CLOBBER_OPS = 11;

#define PORT_LOW  0x0000
#define PORT_HIGH 0x0003
#define STACK_LOW  0xFA00
#define STACK_HIGH 0xFFFF
#define VAR_AUTO_START 0x0004
#define MAX_PROGRAM_WORDS 0x10000

static bool in_set(const char *s, const char **set, int n) {
    for (int i = 0; i < n; i++) if (!strcmp(s, set[i])) return true;
    return false;
}

/* ========================================================================
 * Small dynamic-array / string helpers
 * ======================================================================== */
static char *xstrdup(const char *s) {
    char *r = malloc(strlen(s) + 1);
    strcpy(r, s);
    return r;
}

static void trim(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) { s[len - 1] = '\0'; len--; }
}

static void to_upper(char *s) {
    for (; *s; s++) *s = toupper((unsigned char)*s);
}

/* growable string buffer, used for building listing/hex text */
typedef struct { char *data; size_t len; size_t cap; } SBuf;
static void sbuf_init(SBuf *b) { b->cap = 4096; b->len = 0; b->data = malloc(b->cap); b->data[0] = '\0'; }
static void sbuf_append(SBuf *b, const char *s) {
    size_t sl = strlen(s);
    while (b->len + sl + 1 > b->cap) { b->cap *= 2; b->data = realloc(b->data, b->cap); }
    memcpy(b->data + b->len, s, sl + 1);
    b->len += sl;
}
static void sbuf_appendf(SBuf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sbuf_append(b, tmp);
}

/* ========================================================================
 * Diagnostics
 * ======================================================================== */
typedef enum { LVL_ERROR, LVL_WARNING } Level;
typedef struct { int line_no; Level level; char *msg; } Diag;

typedef struct {
    Diag *items;
    int count, cap;
} DiagList;

static void diaglist_init(DiagList *d) { d->count = 0; d->cap = 32; d->items = malloc(sizeof(Diag) * d->cap); }
static void diaglist_add(DiagList *d, int line_no, Level level, const char *msg) {
    if (d->count >= d->cap) { d->cap *= 2; d->items = realloc(d->items, sizeof(Diag) * d->cap); }
    d->items[d->count].line_no = line_no;
    d->items[d->count].level = level;
    d->items[d->count].msg = xstrdup(msg);
    d->count++;
}
static int diag_cmp(const void *a, const void *b) {
    const Diag *da = a, *db = b;
    return da->line_no - db->line_no;
}

/* ========================================================================
 * Regexes (compiled once at startup)
 * ======================================================================== */
static regex_t RE_REG, RE_IMM, RE_ADDR_LIT, RE_SYMREF, RE_PORT, RE_LABEL_DECL, RE_VAR_DECL;

static void compile_regexes(void) {
    /* ^([AB])([0-7])$  case-insensitive */
    regcomp(&RE_REG, "^([AB])([0-7])$", REG_EXTENDED | REG_ICASE);
    /* ^([0-9A-Fa-f]{1,4})h$  case-sensitive 'h' */
    regcomp(&RE_IMM, "^([0-9A-Fa-f]{1,4})h$", REG_EXTENDED);
    /* ^#([0-9A-Fa-f]{1,4})h$ */
    regcomp(&RE_ADDR_LIT, "^#([0-9A-Fa-f]{1,4})h$", REG_EXTENDED);
    /* ^#([A-Za-z_][A-Za-z0-9_]*)$ */
    regcomp(&RE_SYMREF, "^#([A-Za-z_][A-Za-z0-9_]*)$", REG_EXTENDED);
    /* ^#([0-9A-Fa-f]{1,2})h?$ */
    regcomp(&RE_PORT, "^#([0-9A-Fa-f]{1,2})h?$", REG_EXTENDED);
    /* ^([A-Za-z_][A-Za-z0-9_]*):$ */
    regcomp(&RE_LABEL_DECL, "^([A-Za-z_][A-Za-z0-9_]*):$", REG_EXTENDED);
    /* ^VAR[[:space:]]+([A-Za-z_][A-Za-z0-9_]*)([[:space:]]*=[[:space:]]*#([0-9A-Fa-f]{1,4})h)?$  ignorecase */
    regcomp(&RE_VAR_DECL,
        "^VAR[[:space:]]+([A-Za-z_][A-Za-z0-9_]*)([[:space:]]*=[[:space:]]*#([0-9A-Fa-f]{1,4})h)?$",
        REG_EXTENDED | REG_ICASE);
}

/* Returns true on match, fills up to maxgroups captured substrings into out[]
 * (each caller-allocated, size outsize). Group 0 is the whole match. */
static bool re_match(regex_t *re, const char *s, char out[][128], int maxgroups) {
    regmatch_t m[8];
    if (regexec(re, s, 8, m, 0) != 0) return false;
    for (int i = 0; i < maxgroups; i++) {
        out[i][0] = '\0';
        if (i < 8 && m[i].rm_so != -1) {
            int len = m[i].rm_eo - m[i].rm_so;
            if (len > 127) len = 127;
            memcpy(out[i], s + m[i].rm_so, len);
            out[i][len] = '\0';
        }
    }
    return true;
}

/* ========================================================================
 * Comment stripping (// and /* ... *\/, newline-preserving)
 * ======================================================================== */
typedef struct { char *text; DiagList *pending; } StripResult;

static char *strip_comments(const char *text, DiagList *out_pending_errors) {
    size_t len = strlen(text);
    char *result = malloc(len + 1);
    size_t rlen = 0;
    int line_no = 1;
    size_t pos = 0;
    bool in_comment = false;
    int comment_start_line = 0;

    while (pos < len) {
        char ch = text[pos];
        if (!in_comment) {
            if (text[pos] == '/' && pos + 1 < len && text[pos + 1] == '*') {
                in_comment = true;
                comment_start_line = line_no;
                pos += 2;
                continue;
            }
            if (text[pos] == '/' && pos + 1 < len && text[pos + 1] == '/') {
                while (pos < len && text[pos] != '\n') pos++;
                continue;
            }
            result[rlen++] = ch;
            if (ch == '\n') line_no++;
            pos++;
        } else {
            if (text[pos] == '*' && pos + 1 < len && text[pos + 1] == '/') {
                in_comment = false;
                pos += 2;
                continue;
            }
            if (ch == '\n') { result[rlen++] = '\n'; line_no++; }
            pos++;
        }
    }
    result[rlen] = '\0';

    if (in_comment) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "unterminated block comment '/*' (no matching '*/' found) - "
            "everything after this point was treated as comment and ignored");
        diaglist_add(out_pending_errors, comment_start_line, LVL_ERROR, msg);
    }
    return result;
}

/* ========================================================================
 * Assembler state
 * ======================================================================== */
typedef struct { char name[64]; int addr; int line_no; } Sym;

typedef struct {
    int line_no;
    int addr;
    char mnemonic[16];
    char operands[3][80];
    int noperands;
    int words[4];
    int nwords;
    bool encoded_ok;
} InstrLine;

typedef struct {
    char filename[256];
    char **lines;       /* raw source lines, 0-indexed = line (line_no-1) */
    int nlines;

    DiagList diags;

    Sym *labels; int nlabels, labels_cap;
    Sym *vars;   int nvars, vars_cap;

    InstrLine *instr; int ninstr, instr_cap;

    int program_length;
    bool seen_initsp;
    bool has_mask[7]; /* A0..A6 */
} Assembler;

static void asm_error(Assembler *a, int line_no, const char *fmt, ...) {
    char msg[512];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
    diaglist_add(&a->diags, line_no, LVL_ERROR, msg);
}
static void asm_warn(Assembler *a, int line_no, const char *fmt, ...) {
    char msg[512];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
    diaglist_add(&a->diags, line_no, LVL_WARNING, msg);
}
static bool asm_has_errors(Assembler *a) {
    for (int i = 0; i < a->diags.count; i++) if (a->diags.items[i].level == LVL_ERROR) return true;
    return false;
}

static Sym *find_sym(Sym *arr, int n, const char *name) {
    for (int i = 0; i < n; i++) if (!strcmp(arr[i].name, name)) return &arr[i];
    return NULL;
}
static Sym *find_sym_by_addr(Sym *arr, int n, int addr) {
    for (int i = 0; i < n; i++) if (arr[i].addr == addr) return &arr[i];
    return NULL;
}
static void push_sym(Sym **arr, int *n, int *cap, const char *name, int addr, int line_no) {
    if (*n >= *cap) { *cap = (*cap == 0) ? 16 : *cap * 2; *arr = realloc(*arr, sizeof(Sym) * (*cap)); }
    strncpy((*arr)[*n].name, name, sizeof((*arr)[*n].name) - 1);
    (*arr)[*n].name[sizeof((*arr)[*n].name) - 1] = '\0';
    (*arr)[*n].addr = addr;
    (*arr)[*n].line_no = line_no;
    (*n)++;
}
static void push_instr(Assembler *a, int line_no, int addr, const char *mnemonic,
                        char operands[][80], int noperands) {
    if (a->ninstr >= a->instr_cap) {
        a->instr_cap = (a->instr_cap == 0) ? 64 : a->instr_cap * 2;
        a->instr = realloc(a->instr, sizeof(InstrLine) * a->instr_cap);
    }
    InstrLine *il = &a->instr[a->ninstr];
    il->line_no = line_no;
    il->addr = addr;
    strncpy(il->mnemonic, mnemonic, sizeof(il->mnemonic) - 1);
    il->mnemonic[sizeof(il->mnemonic) - 1] = '\0';
    il->noperands = noperands;
    for (int i = 0; i < noperands && i < 3; i++) {
        strncpy(il->operands[i], operands[i], sizeof(il->operands[i]) - 1);
        il->operands[i][sizeof(il->operands[i]) - 1] = '\0';
    }
    il->nwords = 0;
    il->encoded_ok = false;
    a->ninstr++;
}

static const OpcodeEntry *lookup_opcode(const char *mnemonic) {
    for (int i = 0; i < N_OPCODES; i++)
        if (!strcmp(OPCODES[i].mnemonic, mnemonic)) return &OPCODES[i];
    return NULL;
}

/* ---------------------------------------------------------------- pass1 */
typedef struct { int line_no; char name[64]; int addr; bool has_addr; } PendingVar;

static void pass1(Assembler *a) {
    int addr = 0;
    PendingVar *explicit_vars = malloc(sizeof(PendingVar) * a->nlines);
    int n_explicit = 0;
    PendingVar *implicit_vars = malloc(sizeof(PendingVar) * a->nlines);
    int n_implicit = 0;

    for (int idx = 0; idx < a->nlines; idx++) {
        int i = idx + 1; /* line_no */
        char line[512];
        strncpy(line, a->lines[idx], sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        trim(line);
        if (line[0] == '\0') continue;

        char groups[8][128];
        if (re_match(&RE_LABEL_DECL, line, groups, 2)) {
            const char *name = groups[1];
            if (find_sym(a->labels, a->nlabels, name)) {
                asm_error(a, i, "label '%s' already declared", name);
            } else {
                push_sym(&a->labels, &a->nlabels, &a->labels_cap, name, addr, i);
            }
            continue;
        }

        if (re_match(&RE_VAR_DECL, line, groups, 4)) {
            const char *name = groups[1];
            bool has_addr = groups[3][0] != '\0';
            if (has_addr) {
                explicit_vars[n_explicit].line_no = i;
                strncpy(explicit_vars[n_explicit].name, name, sizeof(explicit_vars[0].name) - 1);
                explicit_vars[n_explicit].addr = (int)strtol(groups[3], NULL, 16);
                explicit_vars[n_explicit].has_addr = true;
                n_explicit++;
            } else {
                implicit_vars[n_implicit].line_no = i;
                strncpy(implicit_vars[n_implicit].name, name, sizeof(implicit_vars[0].name) - 1);
                implicit_vars[n_implicit].has_addr = false;
                n_implicit++;
            }
            continue;
        }

        /* mnemonic = first token, uppercased; rest split by comma */
        char work[512];
        strncpy(work, line, sizeof(work) - 1); work[sizeof(work) - 1] = '\0';
        char *sp = strpbrk(work, " \t");
        char mnemonic[64];
        char rest[448] = "";
        if (sp) {
            size_t mlen = sp - work;
            if (mlen > 63) mlen = 63;
            memcpy(mnemonic, work, mlen); mnemonic[mlen] = '\0';
            strncpy(rest, sp, sizeof(rest) - 1);
            trim(rest);
        } else {
            strncpy(mnemonic, work, sizeof(mnemonic) - 1); mnemonic[sizeof(mnemonic) - 1] = '\0';
        }
        to_upper(mnemonic);

        char operands[3][80];
        int noperands = 0;
        if (rest[0] != '\0') {
            char *tok = strtok(rest, ",");
            while (tok && noperands < 3) {
                char t[80];
                strncpy(t, tok, sizeof(t) - 1); t[sizeof(t) - 1] = '\0';
                trim(t);
                strncpy(operands[noperands], t, sizeof(operands[0]) - 1);
                operands[noperands][sizeof(operands[0]) - 1] = '\0';
                noperands++;
                tok = strtok(NULL, ",");
            }
        }

        const char *fmt;
        if (!strcmp(mnemonic, "LOAD")) {
            fmt = "LOAD";
        } else {
            const OpcodeEntry *e = lookup_opcode(mnemonic);
            if (!e) { asm_error(a, i, "unknown instruction '%s'", mnemonic); continue; }
            fmt = e->fmt;
        }

        if (addr >= MAX_PROGRAM_WORDS) {
            asm_error(a, i, "program exceeds addressable program memory (65536 words)");
            continue;
        }

        push_instr(a, i, addr, mnemonic, operands, noperands);
        addr += words_for_format(fmt);
    }

    a->program_length = addr;

    /* resolve explicit VAR addresses first */
    for (int k = 0; k < n_explicit; k++) {
        PendingVar *pv = &explicit_vars[k];
        if (find_sym(a->vars, a->nvars, pv->name)) {
            asm_error(a, pv->line_no, "variable '%s' already declared", pv->name);
            continue;
        }
        Sym *lbl = find_sym(a->labels, a->nlabels, pv->name);
        if (lbl) {
            asm_error(a, pv->line_no,
                "'%s' is already declared as a label (line %d) - names must "
                "be unique across labels and variables", pv->name, lbl->line_no);
            continue;
        }
        if (pv->addr >= PORT_LOW && pv->addr <= PORT_HIGH) {
            asm_error(a, pv->line_no, "address %04Xh is reserved for ports (0000h-0003h)", pv->addr);
            continue;
        }
        if (pv->addr >= STACK_LOW && pv->addr <= STACK_HIGH) {
            asm_error(a, pv->line_no, "address %04Xh is inside the reserved stack region (FA00h-FFFFh)", pv->addr);
            continue;
        }
        Sym *owner = find_sym_by_addr(a->vars, a->nvars, pv->addr);
        if (owner) {
            asm_error(a, pv->line_no, "address %04Xh already assigned to variable '%s'", pv->addr, owner->name);
            continue;
        }
        push_sym(&a->vars, &a->nvars, &a->vars_cap, pv->name, pv->addr, pv->line_no);
    }

    /* auto-allocate implicit VARs */
    int cursor = VAR_AUTO_START;
    for (int k = 0; k < n_implicit; k++) {
        PendingVar *pv = &implicit_vars[k];
        if (find_sym(a->vars, a->nvars, pv->name)) {
            asm_error(a, pv->line_no, "variable '%s' already declared", pv->name);
            continue;
        }
        Sym *lbl = find_sym(a->labels, a->nlabels, pv->name);
        if (lbl) {
            asm_error(a, pv->line_no,
                "'%s' is already declared as a label (line %d) - names must "
                "be unique across labels and variables", pv->name, lbl->line_no);
            continue;
        }
        bool failed = false;
        for (;;) {
            if (cursor >= PORT_LOW && cursor <= PORT_HIGH) { cursor = PORT_HIGH + 1; continue; }
            if (cursor >= STACK_LOW && cursor <= STACK_HIGH) {
                asm_error(a, pv->line_no,
                    "ran out of data memory for auto-allocated variable '%s' "
                    "(hit reserved stack region)", pv->name);
                failed = true;
                break;
            }
            if (find_sym_by_addr(a->vars, a->nvars, cursor)) { cursor++; continue; }
            break;
        }
        if (failed) continue;
        push_sym(&a->vars, &a->nvars, &a->vars_cap, pv->name, cursor, pv->line_no);
        cursor++;
    }

    free(explicit_vars);
    free(implicit_vars);
}

/* ---------------------------------------------------------------- utils */
static int parse_reg(Assembler *a, const char *tok, char bank, int line_no) {
    char groups[8][128];
    if (!re_match(&RE_REG, tok, groups, 3)) {
        asm_error(a, line_no, "expected %c register, got '%s'", bank, tok);
        return 0;
    }
    char letter = toupper((unsigned char)groups[1][0]);
    int num = atoi(groups[2]);
    if (letter != bank) {
        asm_error(a, line_no, "expected %c register, got '%s'", bank, tok);
        return 0;
    }
    if (num == 7) {
        asm_error(a, line_no, "explicit use of %c7 is not allowed (%s)", bank,
                   bank == 'A' ? "stack pointer" : "hidden register");
        return 0;
    }
    return num;
}

static int parse_immediate(Assembler *a, const char *tok, int line_no) {
    char groups[8][128];
    if (!re_match(&RE_IMM, tok, groups, 2)) {
        asm_error(a, line_no, "expected immediate value 'xxxxh' (no '#'), got '%s'", tok);
        return 0;
    }
    return (int)strtol(groups[1], NULL, 16);
}

static int parse_port(Assembler *a, const char *tok, int line_no) {
    char groups[8][128];
    if (!re_match(&RE_PORT, tok, groups, 2)) {
        asm_error(a, line_no, "expected port '#1'-'#4', got '%s'", tok);
        return 0;
    }
    int val = (int)strtol(groups[1], NULL, 16);
    if (val < 1 || val > 4) {
        asm_error(a, line_no, "port must be 1-4, got '%s'", tok);
        return 0;
    }
    return val;
}

static int parse_addr(Assembler *a, const char *tok, int line_no, bool allow_label, bool allow_var) {
    char groups[8][128];
    /* strict hex-literal pattern first, exactly like the Python version */
    if (re_match(&RE_ADDR_LIT, tok, groups, 2)) {
        int val = (int)strtol(groups[1], NULL, 16);
        if (allow_var) {
            if (val >= PORT_LOW && val <= PORT_HIGH) {
                asm_error(a, line_no, "address %04Xh is reserved for ports (0000h-0003h)", val);
                return 0;
            }
            if (val >= STACK_LOW && val <= STACK_HIGH) {
                asm_error(a, line_no, "address %04Xh is inside the reserved stack region (FA00h-FFFFh)", val);
                return 0;
            }
            Sym *owner = find_sym_by_addr(a->vars, a->nvars, val);
            if (owner) {
                asm_warn(a, line_no, "address %04Xh matches variable '%s' - consider using '#%s'",
                          val, owner->name, owner->name);
            }
        }
        return val;
    }

    if (re_match(&RE_SYMREF, tok, groups, 2)) {
        const char *name = groups[1];
        Sym *lbl = find_sym(a->labels, a->nlabels, name);
        Sym *var = find_sym(a->vars, a->nvars, name);
        if (allow_label && lbl) return lbl->addr;
        if (allow_var && var) return var->addr;
        if (lbl && !allow_label) {
            asm_error(a, line_no, "'%s' is a label, not valid as a data address here", name);
            return 0;
        }
        if (var && !allow_var) {
            asm_error(a, line_no, "'%s' is a variable, not valid as a jump target", name);
            return 0;
        }
        asm_error(a, line_no, "undefined symbol '%s'", name);
        return 0;
    }

    asm_error(a, line_no, "expected '#xxxxh' or '#Name', got '%s'", tok);
    return 0;
}

static bool need(Assembler *a, int noperands, int count, int line_no, const char *mnemonic) {
    if (noperands != count) {
        asm_error(a, line_no, "%s expects %d operand(s), got %d", mnemonic, count, noperands);
        return false;
    }
    return true;
}

static int pack(int opcode, int ra, int rb) {
    return ((opcode & 0x3F) << 10) | ((ra & 0x7) << 7) | ((rb & 0x7) << 4);
}

/* --------------------------------------------------------------- encode */
static void encode(Assembler *a, InstrLine *il) {
    int line_no = il->line_no;
    const char *mnemonic = il->mnemonic;
    char (*ops)[80] = il->operands;
    int noperands = il->noperands;

    if (!strcmp(mnemonic, "LOAD")) {
        if (!need(a, noperands, 2, line_no, mnemonic)) return;
        char groups[8][128];
        if (!re_match(&RE_REG, ops[0], groups, 3)) {
            asm_error(a, line_no, "expected register, got '%s'", ops[0]);
            return;
        }
        char letter = toupper((unsigned char)groups[1][0]);
        int num = atoi(groups[2]);
        if (num == 7) {
            asm_error(a, line_no, "explicit use of %c7 is not allowed", letter);
            return;
        }
        int opcode, ra, rb;
        if (letter == 'A') { opcode = 0x22; ra = num; rb = 7; }
        else { opcode = 0x23; ra = 6; rb = num; }
        int imm = parse_immediate(a, ops[1], line_no);
        il->words[0] = pack(opcode, ra, rb);
        il->words[1] = imm;
        il->nwords = 2;
        il->encoded_ok = true;
        return;
    }

    const OpcodeEntry *e = lookup_opcode(mnemonic);
    int opcode = e->opcode;
    const char *fmt = e->fmt;

    if (!strcmp(fmt, "RR")) {
        if (!need(a, noperands, 2, line_no, mnemonic)) return;
        int ra = parse_reg(a, ops[0], 'A', line_no);
        int rb = parse_reg(a, ops[1], 'B', line_no);
        if (!strcmp(mnemonic, "BITSET") || !strcmp(mnemonic, "BITCLR") || !strcmp(mnemonic, "BITTEST")) {
            if (!a->has_mask[ra]) {
                asm_warn(a, line_no, "no BITMASK generated for A%d before %s (assumed already valid)", ra, mnemonic);
            }
        }
        il->words[0] = pack(opcode, ra, rb); il->nwords = 1; il->encoded_ok = true;
        return;
    }
    if (!strcmp(fmt, "R")) {
        if (!need(a, noperands, 1, line_no, mnemonic)) return;
        int ra = parse_reg(a, ops[0], 'A', line_no);
        if (!strcmp(mnemonic, "BITMASK")) a->has_mask[ra] = true;
        il->words[0] = pack(opcode, ra, 7); il->nwords = 1; il->encoded_ok = true;
        return;
    }
    if (!strcmp(fmt, "NONE")) {
        if (!need(a, noperands, 0, line_no, mnemonic)) return;
        il->words[0] = pack(opcode, 6, 7); il->nwords = 1; il->encoded_ok = true;
        return;
    }
    if (!strcmp(fmt, "RB_ONLY")) {
        if (!need(a, noperands, 1, line_no, mnemonic)) return;
        int rb = parse_reg(a, ops[0], 'B', line_no);
        il->words[0] = pack(opcode, 6, rb); il->nwords = 1; il->encoded_ok = true;
        return;
    }
    if (!strcmp(fmt, "RA_ADDR")) {
        if (!need(a, noperands, 2, line_no, mnemonic)) return;
        int ra = parse_reg(a, ops[0], 'A', line_no);
        int address = parse_addr(a, ops[1], line_no, false, true);
        il->words[0] = pack(opcode, ra, 7); il->words[1] = address; il->nwords = 2; il->encoded_ok = true;
        return;
    }
    if (!strcmp(fmt, "RB_ADDR")) {
        if (!need(a, noperands, 2, line_no, mnemonic)) return;
        int rb = parse_reg(a, ops[0], 'B', line_no);
        int address = parse_addr(a, ops[1], line_no, false, true);
        il->words[0] = pack(opcode, 6, rb); il->words[1] = address; il->nwords = 2; il->encoded_ok = true;
        return;
    }
    if (!strcmp(fmt, "RA_PORT")) {
        if (!need(a, noperands, 2, line_no, mnemonic)) return;
        int ra = parse_reg(a, ops[0], 'A', line_no);
        int port = parse_port(a, ops[1], line_no);
        il->words[0] = pack(opcode, ra, 7); il->words[1] = port; il->nwords = 2; il->encoded_ok = true;
        return;
    }
    if (!strcmp(fmt, "RB_PORT")) {
        if (!need(a, noperands, 2, line_no, mnemonic)) return;
        int rb = parse_reg(a, ops[0], 'B', line_no);
        int port = parse_port(a, ops[1], line_no);
        il->words[0] = pack(opcode, 6, rb); il->words[1] = port; il->nwords = 2; il->encoded_ok = true;
        return;
    }
    if (!strcmp(fmt, "JUMP")) {
        if (!need(a, noperands, 1, line_no, mnemonic)) return;
        int target = parse_addr(a, ops[0], line_no, true, false);
        il->words[0] = pack(opcode, 6, 7); il->words[1] = target; il->nwords = 2; il->encoded_ok = true;
        return;
    }
    if (!strcmp(fmt, "MOVM")) {
        if (!need(a, noperands, 2, line_no, mnemonic)) return;
        int src = parse_addr(a, ops[0], line_no, false, true);
        int dst = parse_addr(a, ops[1], line_no, false, true);
        il->words[0] = pack(opcode, 6, 7); il->words[1] = src; il->words[2] = dst; il->nwords = 3; il->encoded_ok = true;
        return;
    }
    if (!strcmp(fmt, "MEMCPY")) {
        if (!need(a, noperands, 3, line_no, mnemonic)) return;
        int length = parse_immediate(a, ops[0], line_no);
        int src = parse_addr(a, ops[1], line_no, false, true);
        int dst = parse_addr(a, ops[2], line_no, false, true);
        il->words[0] = pack(opcode, 6, 7); il->words[1] = length; il->words[2] = src; il->words[3] = dst;
        il->nwords = 4; il->encoded_ok = true;
        return;
    }
    if (!strcmp(fmt, "MEMSET")) {
        if (!need(a, noperands, 3, line_no, mnemonic)) return;
        int rb = parse_reg(a, ops[0], 'B', line_no);
        int length = parse_immediate(a, ops[1], line_no);
        int dst = parse_addr(a, ops[2], line_no, false, true);
        il->words[0] = pack(opcode, 6, rb); il->words[1] = length; il->words[2] = dst;
        il->nwords = 3; il->encoded_ok = true;
        return;
    }

    asm_error(a, line_no, "internal: unhandled format '%s' for %s", fmt, mnemonic);
}

/* ---------------------------------------------------------------- pass2 */
static void pass2(Assembler *a) {
    for (int i = 0; i < a->ninstr; i++) {
        InstrLine *il = &a->instr[i];
        if (in_set(il->mnemonic, STACK_OPS, N_STACK_OPS)) {
            if (!a->seen_initsp) {
                asm_error(a, il->line_no, "%s used before INITSP has been executed", il->mnemonic);
            }
        }
        if (!strcmp(il->mnemonic, "INITSP")) {
            if (a->seen_initsp) asm_error(a, il->line_no, "INITSP executed more than once");
            a->seen_initsp = true;
        }
        if (in_set(il->mnemonic, A6_CLOBBER_OPS, N_A6_CLOBBER_OPS)) {
            asm_warn(a, il->line_no, "%s will overwrite A6 (scratch register for this operation)", il->mnemonic);
        }
        encode(a, il);
    }

    if (a->ninstr > 0) {
        int last_line_no = a->instr[a->ninstr - 1].line_no;
        bool has_halt = false;
        for (int i = 0; i < a->ninstr; i++) if (!strcmp(a->instr[i].mnemonic, "HALT")) { has_halt = true; break; }
        if (!has_halt) {
            asm_warn(a, last_line_no,
                "program contains no HALT instruction - execution will run past the end of program memory");
        }
    }
}

/* ---------------------------------------------------------- hex output */
static void build_rom_bytes(Assembler *a, unsigned char **rom0_out, unsigned char **rom1_out) {
    unsigned char *rom0 = calloc(a->program_length, 1);
    unsigned char *rom1 = calloc(a->program_length, 1);
    for (int i = 0; i < a->ninstr; i++) {
        InstrLine *il = &a->instr[i];
        if (!il->encoded_ok) continue;
        for (int wi = 0; wi < il->nwords; wi++) {
            int addr = il->addr + wi;
            if (addr < 0 || addr >= a->program_length) continue;
            rom0[addr] = (il->words[wi] >> 8) & 0xFF;
            rom1[addr] = il->words[wi] & 0xFF;
        }
    }
    *rom0_out = rom0; *rom1_out = rom1;
}

static unsigned char *build_combined_bytes(Assembler *a, int *out_len) {
    unsigned char *rom0, *rom1;
    build_rom_bytes(a, &rom0, &rom1);
    unsigned char *combined = malloc(a->program_length * 2);
    for (int i = 0; i < a->program_length; i++) {
        combined[2 * i] = rom0[i];
        combined[2 * i + 1] = rom1[i];
    }
    free(rom0); free(rom1);
    *out_len = a->program_length * 2;
    return combined;
}

static char *intel_hex(const unsigned char *bytes, int n, int bytes_per_line) {
    SBuf b; sbuf_init(&b);
    int i = 0;
    while (i < n) {
        int count = (n - i < bytes_per_line) ? (n - i) : bytes_per_line;
        int checksum = count + ((i >> 8) & 0xFF) + (i & 0xFF) + 0x00;
        char line[128];
        int pos = snprintf(line, sizeof(line), ":%02X%02X%02X00", count, (i >> 8) & 0xFF, i & 0xFF);
        for (int k = 0; k < count; k++) {
            checksum += bytes[i + k];
            pos += snprintf(line + pos, sizeof(line) - pos, "%02X", bytes[i + k]);
        }
        checksum = (-checksum) & 0xFF;
        pos += snprintf(line + pos, sizeof(line) - pos, "%02X", checksum);
        sbuf_append(&b, line);
        sbuf_append(&b, "\n");
        i += count;
    }
    sbuf_append(&b, ":00000001FF");
    return b.data;
}

/* ------------------------------------------------------------- listing */
static int sym_addr_cmp(const void *x, const void *y) {
    const Sym *sx = x, *sy = y;
    return sx->addr - sy->addr;
}

static char *build_listing(Assembler *a) {
    SBuf b; sbuf_init(&b);
    sbuf_appendf(&b, "; Listing for %s\n", a->filename);
    sbuf_appendf(&b, "; Labels: %d   Variables: %d\n", a->nlabels, a->nvars);
    sbuf_append(&b, ";\n");
    if (a->nvars > 0) {
        Sym *sorted = malloc(sizeof(Sym) * a->nvars);
        memcpy(sorted, a->vars, sizeof(Sym) * a->nvars);
        qsort(sorted, a->nvars, sizeof(Sym), sym_addr_cmp);
        sbuf_append(&b, "; Variable map (data memory):\n");
        for (int i = 0; i < a->nvars; i++) {
            sbuf_appendf(&b, ";   %-20s = %04Xh\n", sorted[i].name, sorted[i].addr);
        }
        sbuf_append(&b, ";\n");
        free(sorted);
    }
    sbuf_appendf(&b, "%-6s %-18s SOURCE\n", "ADDR", "WORD(bin)");
    for (int i = 0; i < 60; i++) sbuf_append(&b, "-");
    sbuf_append(&b, "\n");

    for (int i = 0; i < a->ninstr; i++) {
        InstrLine *il = &a->instr[i];
        char src[512] = "";
        if (il->line_no - 1 < a->nlines) {
            strncpy(src, a->lines[il->line_no - 1], sizeof(src) - 1);
            trim(src);
        }
        if (!il->encoded_ok) {
            sbuf_appendf(&b, "%04Xh  %-18s %s\n", il->addr, "<e>", src);
            continue;
        }
        for (int wi = 0; wi < il->nwords; wi++) {
            int addr = il->addr + wi;
            char binstr[17];
            for (int bit = 0; bit < 16; bit++)
                binstr[bit] = ((il->words[wi] >> (15 - bit)) & 1) ? '1' : '0';
            binstr[16] = '\0';
            if (wi == 0) sbuf_appendf(&b, "%04Xh  %s   %s\n", addr, binstr, src);
            else sbuf_appendf(&b, "%04Xh  %s     (operand word)\n", addr, binstr);
        }
    }
    return b.data;
}

/* ========================================================================
 * Digital (hneemann) TCP remote interface.
 *
 * Protocol (reverse-engineered from hneemann/Assembler's RemoteInterface.java):
 *   - Connect to 127.0.0.1:41114 (must be enabled in Digital's Settings -
 *     the remote server is OFF by default in current versions).
 *   - Every message (both directions) uses Java's DataOutputStream.writeUTF
 *     wire format: a 2-byte big-endian length prefix (byte length of the
 *     UTF-8 payload, not character count) followed by the UTF-8 bytes.
 *   - Commands: "start:<hexfilepath>", "debug:<hexfilepath>", "run",
 *     "step", "stop". start/debug tell Digital to load that hex file into
 *     the circuit's program memory; start also begins free-running clocking,
 *     debug does not (leaves you to step/run manually from Digital's GUI).
 *   - Response: "ok", "ok:<hex address>" (run/step), or an error string.
 * ======================================================================== */
static bool recv_exact(int sock, unsigned char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(sock, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

/* Returns 0 on success (response copied into out_response), -1 on error
 * (error message copied into out_response). */
static int digital_send(const char *command, const char *host, int port,
                         int timeout_sec, char *out_response, size_t out_size) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        snprintf(out_response, out_size, "could not create socket: %s", strerror(errno));
        return -1;
    }

    struct timeval tv; tv.tv_sec = timeout_sec; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        snprintf(out_response, out_size, "invalid host address '%s'", host);
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        snprintf(out_response, out_size,
            "could not reach Digital at %s:%d - is Digital running with the "
            "circuit open and remote control enabled in Settings? (%s)",
            host, port, strerror(errno));
        close(sock);
        return -1;
    }

    size_t payload_len = strlen(command);
    unsigned char *packet = malloc(2 + payload_len);
    packet[0] = (unsigned char)((payload_len >> 8) & 0xFF);
    packet[1] = (unsigned char)(payload_len & 0xFF);
    memcpy(packet + 2, command, payload_len);
    ssize_t sent = send(sock, packet, 2 + payload_len, 0);
    free(packet);
    if (sent < 0) {
        snprintf(out_response, out_size, "send failed: %s", strerror(errno));
        close(sock);
        return -1;
    }

    unsigned char lenbuf[2];
    if (!recv_exact(sock, lenbuf, 2)) {
        snprintf(out_response, out_size, "connection closed while waiting for Digital's response");
        close(sock);
        return -1;
    }
    int resp_len = (lenbuf[0] << 8) | lenbuf[1];
    unsigned char *respbuf = malloc(resp_len + 1);
    if (!recv_exact(sock, respbuf, (size_t)resp_len)) {
        snprintf(out_response, out_size, "connection closed while waiting for Digital's response");
        free(respbuf);
        close(sock);
        return -1;
    }
    respbuf[resp_len] = '\0';
    close(sock);

    bool ok = (!strcmp((char *)respbuf, "ok") || !strncmp((char *)respbuf, "ok:", 3));
    if (!ok) {
        snprintf(out_response, out_size, "Digital reported an error: %s", respbuf);
        free(respbuf);
        return -1;
    }
    strncpy(out_response, (char *)respbuf, out_size - 1);
    out_response[out_size - 1] = '\0';
    free(respbuf);
    return 0;
}

/* ========================================================================
 * main
 * ======================================================================== */
static char **split_lines(const char *text, int *out_n) {
    int n = 1;
    for (const char *p = text; *p; p++) if (*p == '\n') n++;
    char **lines = malloc(sizeof(char *) * n);
    int idx = 0;
    const char *start = text;
    for (const char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            size_t len = p - start;
            char *line = malloc(len + 1);
            memcpy(line, start, len);
            line[len] = '\0';
            lines[idx++] = line;
            start = p + 1;
            if (*p == '\0') break;
        }
    }
    *out_n = idx;
    return lines;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Custom ISA two-pass assembler (C port)\n\n"
        "usage: %s <source.asm> [--build] [--digital]\n"
        "                        [--digital-mode start|debug]\n"
        "                        [--digital-host HOST] [--digital-port PORT]\n\n"
        "  (no flags)   ASSEMBLE (check only) - reports errors/warnings, writes nothing\n"
        "  --build      writes <name>_rom0.hex, <name>_rom1.hex, <name>_listing.txt\n"
        "  --digital    implies --build; also writes <name>_digital.hex and sends it\n"
        "               to a running Digital simulator instance over TCP\n",
        prog);
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    const char *source = NULL;
    bool build = false, digital = false;
    const char *digital_mode = "debug";
    const char *digital_host = "127.0.0.1";
    int digital_port = 41114;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--build")) build = true;
        else if (!strcmp(argv[i], "--digital")) digital = true;
        else if (!strcmp(argv[i], "--digital-mode") && i + 1 < argc) digital_mode = argv[++i];
        else if (!strcmp(argv[i], "--digital-host") && i + 1 < argc) digital_host = argv[++i];
        else if (!strcmp(argv[i], "--digital-port") && i + 1 < argc) digital_port = atoi(argv[++i]);
        else if (argv[i][0] != '-') source = argv[i];
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); print_usage(argv[0]); return 1; }
    }
    if (!source) { print_usage(argv[0]); return 1; }
    if (digital) build = true;

    FILE *f = fopen(source, "rb");
    if (!f) { printf("error: file not found: %s\n", source); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *raw = malloc(fsize + 1);
    fread(raw, 1, fsize, f);
    raw[fsize] = '\0';
    fclose(f);

    compile_regexes();

    Assembler a;
    memset(&a, 0, sizeof(a));
    const char *base_name = strrchr(source, '/');
    base_name = base_name ? base_name + 1 : source;
    strncpy(a.filename, base_name, sizeof(a.filename) - 1);
    diaglist_init(&a.diags);

    char *clean_text = strip_comments(raw, &a.diags);
    a.lines = split_lines(clean_text, &a.nlines);

    pass1(&a);
    pass2(&a);

    qsort(a.diags.items, a.diags.count, sizeof(Diag), diag_cmp);
    int nerrors = 0, nwarnings = 0;
    for (int i = 0; i < a.diags.count; i++) {
        Diag *d = &a.diags.items[i];
        printf("%s:%d: %s: %s\n", a.filename, d->line_no, d->level == LVL_ERROR ? "error" : "warning", d->msg);
        if (d->level == LVL_ERROR) nerrors++; else nwarnings++;
    }
    printf("\n%d error(s), %d warning(s)\n", nerrors, nwarnings);

    if (!build) {
        return nerrors ? 1 : 0;
    }

    if (nerrors) {
        printf("Build aborted: fix all errors first.\n");
        return 1;
    }

    /* strip extension for output base name */
    char base[512];
    strncpy(base, source, sizeof(base) - 1); base[sizeof(base) - 1] = '\0';
    char *dot = strrchr(base, '.');
    char *slash = strrchr(base, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';

    unsigned char *rom0, *rom1;
    build_rom_bytes(&a, &rom0, &rom1);

    char rom0_path[560], rom1_path[560], listing_path[560];
    snprintf(rom0_path, sizeof(rom0_path), "%s_rom0.hex", base);
    snprintf(rom1_path, sizeof(rom1_path), "%s_rom1.hex", base);
    snprintf(listing_path, sizeof(listing_path), "%s_listing.txt", base);

    char *hex0 = intel_hex(rom0, a.program_length, 16);
    char *hex1 = intel_hex(rom1, a.program_length, 16);
    char *listing = build_listing(&a);
    /* build_listing() ends each row (including the last) with '\n'; strip
     * that one trailing newline so the single '\n' added at file-write
     * time below matches the Python version's output exactly. */
    { size_t ll = strlen(listing); if (ll > 0 && listing[ll - 1] == '\n') listing[ll - 1] = '\0'; }

    FILE *fo;
    fo = fopen(rom0_path, "w"); fprintf(fo, "%s\n", hex0); fclose(fo);
    fo = fopen(rom1_path, "w"); fprintf(fo, "%s\n", hex1); fclose(fo);
    fo = fopen(listing_path, "w"); fprintf(fo, "%s\n", listing); fclose(fo);

    printf("Wrote %s\n", rom0_path);
    printf("Wrote %s\n", rom1_path);
    printf("Wrote %s\n", listing_path);
    printf("Program length: %d words\n", a.program_length);

    if (digital) {
        char digital_path[560];
        snprintf(digital_path, sizeof(digital_path), "%s_digital.hex", base);
        int clen;
        unsigned char *combined = build_combined_bytes(&a, &clen);
        char *hexc = intel_hex(combined, clen, 16);
        fo = fopen(digital_path, "w"); fprintf(fo, "%s\n", hexc); fclose(fo);
        printf("Wrote %s\n", digital_path);

        /* Digital's protocol wants an absolute path; if a relative path was
         * used, resolve it. */
        char abspath[1024];
        if (digital_path[0] != '/') {
            char cwd[768];
            if (getcwd(cwd, sizeof(cwd))) snprintf(abspath, sizeof(abspath), "%s/%s", cwd, digital_path);
            else strncpy(abspath, digital_path, sizeof(abspath) - 1);
        } else {
            strncpy(abspath, digital_path, sizeof(abspath) - 1);
        }

        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "%s:%s", digital_mode, abspath);
        char response[512];
        if (digital_send(cmd, digital_host, digital_port, 5, response, sizeof(response)) == 0) {
            printf("Digital: %s\n", response);
        } else {
            printf("Digital load failed: %s\n", response);
            return 1;
        }
    }

    return 0;
}
