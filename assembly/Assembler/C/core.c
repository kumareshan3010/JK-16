/* core.c -- see core.h. Line-for-line port of assembler-4.py's Assembler
 * class. Behaviour (error messages, address rules, encoding) is intended
 * to match the Python original exactly. */
#define _POSIX_C_SOURCE 200809L
#include "core.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void pass2_process(Assembler *a, int idx); /* forward decl, defined below */

/* -------------------------------------------------------------------- */
/* Opcode table                                                          */
/* -------------------------------------------------------------------- */
const OpcodeEntry OPCODES[] = {
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
const int OPCODES_COUNT = (int)(sizeof(OPCODES) / sizeof(OPCODES[0]));

static const char *STACK_OPS[] = {"PUSH", "POP", "PUSHF", "POPF", "CALL", "RET"};
static const int STACK_OPS_COUNT = 6;
static const char *A6_CLOBBER_OPS[] = {"MEMSET", "JMP", "JZ", "JNZ", "JC", "JNC",
                                        "JN", "JNN", "JV", "CALL", "RET"};
static const int A6_CLOBBER_OPS_COUNT = 11;

static int words_per_format(const char *fmt) {
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
    return -1;
}

static const OpcodeEntry *find_opcode(const char *mnemonic) {
    for (int i = 0; i < OPCODES_COUNT; i++)
        if (!strcmp(OPCODES[i].mnemonic, mnemonic)) return &OPCODES[i];
    return NULL;
}

static int str_in_list(const char *s, const char **list, int n) {
    for (int i = 0; i < n; i++) if (!strcmp(s, list[i])) return 1;
    return 0;
}

/* -------------------------------------------------------------------- */
/* small string helpers                                                  */
/* -------------------------------------------------------------------- */
static void strip_inplace(char *s) {
    /* trim leading/trailing whitespace in place */
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    size_t len = strlen(start);
    while (len > 0 && isspace((unsigned char)start[len - 1])) len--;
    memmove(s, start, len);
    s[len] = '\0';
}

static void to_upper_str(char *s) {
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static int is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static int is_hexdigit_c(char c) { return isxdigit((unsigned char)c); }

/* ^([0-9A-Fa-f]{1,4})h$  -- lowercase 'h' required, matched against a
 * whole (already-trimmed) token. On match returns 1 and *val = hex value. */
static int match_imm(const char *tok, int *val) {
    size_t n = strlen(tok);
    if (n < 2 || n > 5) return 0;
    if (tok[n - 1] != 'h') return 0;
    for (size_t i = 0; i < n - 1; i++) if (!is_hexdigit_c(tok[i])) return 0;
    char buf[8];
    memcpy(buf, tok, n - 1);
    buf[n - 1] = '\0';
    *val = (int)strtol(buf, NULL, 16);
    return 1;
}

/* ^#([0-9A-Fa-f]{1,4})h$ */
static int match_addr_lit(const char *tok, int *val) {
    if (tok[0] != '#') return 0;
    return match_imm(tok + 1, val);
}

/* ^#([A-Za-z_][A-Za-z0-9_]*)$ */
static int match_symref(const char *tok, char *name_out, size_t name_sz) {
    if (tok[0] != '#') return 0;
    const char *p = tok + 1;
    if (!*p || !is_ident_start(*p)) return 0;
    size_t i = 0;
    for (; p[i]; i++) if (!is_ident_char(p[i])) return 0;
    if (i >= name_sz) return 0;
    memcpy(name_out, p, i + 1);
    return 1;
}

/* ^#([0-9A-Fa-f]{1,2})h?$ */
static int match_port(const char *tok, int *val) {
    if (tok[0] != '#') return 0;
    const char *p = tok + 1;
    size_t n = strlen(p);
    int has_h = (n > 0 && p[n - 1] == 'h');
    size_t digits = has_h ? n - 1 : n;
    if (digits < 1 || digits > 2) return 0;
    for (size_t i = 0; i < digits; i++) if (!is_hexdigit_c(p[i])) return 0;
    char buf[4];
    memcpy(buf, p, digits);
    buf[digits] = '\0';
    *val = (int)strtol(buf, NULL, 16);
    return 1;
}

/* ^([A-Za-z_][A-Za-z0-9_]*):$ */
static int match_label_decl(const char *line, char *name_out, size_t name_sz) {
    size_t n = strlen(line);
    if (n < 2 || line[n - 1] != ':') return 0;
    if (!is_ident_start(line[0])) return 0;
    for (size_t i = 1; i < n - 1; i++) if (!is_ident_char(line[i])) return 0;
    size_t namelen = n - 1;
    if (namelen >= name_sz) return 0;
    memcpy(name_out, line, namelen);
    name_out[namelen] = '\0';
    return 1;
}

/* ^([AB])([0-7])$  case-insensitive */
static int match_reg(const char *tok, char *letter_out, int *num_out) {
    if (strlen(tok) != 2) return 0;
    char c0 = (char)toupper((unsigned char)tok[0]);
    if (c0 != 'A' && c0 != 'B') return 0;
    if (tok[1] < '0' || tok[1] > '7') return 0;
    *letter_out = c0;
    *num_out = tok[1] - '0';
    return 1;
}

/* VAR\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*=\s*#([0-9A-Fa-f]{1,4})h)? -- case
 * insensitive for the "VAR" keyword. Returns 1 if it matches the whole
 * (trimmed) line. has_val is set if the "= #xxxxh" part is present. */
static int match_var_decl(const char *line, char *name_out, size_t name_sz,
                           int *has_val, int *val_out) {
    size_t n = strlen(line);
    if (n < 4) return 0;
    if (toupper((unsigned char)line[0]) != 'V' ||
        toupper((unsigned char)line[1]) != 'A' ||
        toupper((unsigned char)line[2]) != 'R') return 0;
    size_t i = 3;
    if (!isspace((unsigned char)line[i])) return 0;
    while (i < n && isspace((unsigned char)line[i])) i++;
    if (i >= n || !is_ident_start(line[i])) return 0;
    size_t name_start = i;
    while (i < n && is_ident_char(line[i])) i++;
    size_t namelen = i - name_start;
    if (namelen >= name_sz) return 0;
    memcpy(name_out, line + name_start, namelen);
    name_out[namelen] = '\0';

    /* optional trailing whitespace then end of line (no value) */
    size_t j = i;
    while (j < n && isspace((unsigned char)line[j])) j++;
    if (j == n) { *has_val = 0; return 1; }

    if (line[j] != '=') return 0;
    j++;
    while (j < n && isspace((unsigned char)line[j])) j++;
    if (j >= n || line[j] != '#') return 0;
    j++;
    size_t hex_start = j;
    while (j < n && is_hexdigit_c(line[j])) j++;
    size_t hexlen = j - hex_start;
    if (hexlen < 1 || hexlen > 4) return 0;
    if (j >= n || line[j] != 'h') return 0;
    j++;
    if (j != n) return 0; /* trailing garbage */
    char buf[8];
    memcpy(buf, line + hex_start, hexlen);
    buf[hexlen] = '\0';
    *val_out = (int)strtol(buf, NULL, 16);
    *has_val = 1;
    return 1;
}

/* -------------------------------------------------------------------- */
/* comment stripping                                                     */
/* -------------------------------------------------------------------- */
char *strip_comments(const char *text, int *err_line, char *err_msg, size_t err_msg_sz) {
    size_t length = strlen(text);
    char *result = malloc(length + 1);
    size_t out_len = 0;
    int line_no = 1;
    size_t pos = 0;
    int in_comment = 0;
    int comment_start_line = 0;
    *err_line = 0;
    if (err_msg && err_msg_sz) err_msg[0] = '\0';

    while (pos < length) {
        if (!in_comment) {
            if (pos + 1 < length && text[pos] == '/' && text[pos + 1] == '*') {
                in_comment = 1;
                comment_start_line = line_no;
                pos += 2;
                continue;
            }
            if (pos + 1 < length && text[pos] == '/' && text[pos + 1] == '/') {
                const char *nl = strchr(text + pos, '\n');
                pos = nl ? (size_t)(nl - text) : length;
                continue;
            }
            result[out_len++] = text[pos];
            if (text[pos] == '\n') line_no++;
            pos++;
        } else {
            if (pos + 1 < length && text[pos] == '*' && text[pos + 1] == '/') {
                in_comment = 0;
                pos += 2;
                continue;
            }
            if (text[pos] == '\n') {
                result[out_len++] = '\n';
                line_no++;
            }
            pos++;
        }
    }
    result[out_len] = '\0';

    if (in_comment) {
        *err_line = comment_start_line;
        if (err_msg && err_msg_sz)
            snprintf(err_msg, err_msg_sz,
                     "unterminated block comment '/*' (no matching '*/' "
                     "found) - everything after this point was treated "
                     "as comment and ignored");
    }
    return result;
}

/* -------------------------------------------------------------------- */
/* dynamic array helpers                                                 */
/* -------------------------------------------------------------------- */
static void diag_push(Assembler *a, int line_no, int is_error, const char *msg) {
    if (a->ndiags >= a->diags_cap) {
        a->diags_cap = a->diags_cap ? a->diags_cap * 2 : 64;
        a->diags = realloc(a->diags, sizeof(Diag) * a->diags_cap);
    }
    Diag *d = &a->diags[a->ndiags++];
    d->line_no = line_no;
    d->is_error = is_error;
    snprintf(d->msg, MAX_MSG, "%s", msg);
}

#define ERRORF(a, line_no, ...) do { \
    char _buf[MAX_MSG]; snprintf(_buf, sizeof(_buf), __VA_ARGS__); \
    diag_push((a), (line_no), 1, _buf); \
} while (0)
#define WARNF(a, line_no, ...) do { \
    char _buf[MAX_MSG]; snprintf(_buf, sizeof(_buf), __VA_ARGS__); \
    diag_push((a), (line_no), 0, _buf); \
} while (0)

static Label *label_push(Assembler *a) {
    if (a->nlabels >= a->labels_cap) {
        a->labels_cap = a->labels_cap ? a->labels_cap * 2 : 64;
        a->labels = realloc(a->labels, sizeof(Label) * a->labels_cap);
    }
    return &a->labels[a->nlabels++];
}
static Label *find_label(Assembler *a, const char *name) {
    for (int i = 0; i < a->nlabels; i++)
        if (!strcmp(a->labels[i].name, name)) return &a->labels[i];
    return NULL;
}

static Var *var_push(Assembler *a) {
    if (a->nvars >= a->vars_cap) {
        a->vars_cap = a->vars_cap ? a->vars_cap * 2 : 64;
        a->vars = realloc(a->vars, sizeof(Var) * a->vars_cap);
    }
    return &a->vars[a->nvars++];
}
static Var *find_var(Assembler *a, const char *name) {
    for (int i = 0; i < a->nvars; i++)
        if (!strcmp(a->vars[i].name, name)) return &a->vars[i];
    return NULL;
}

static void var_owner_add(Assembler *a, int addr, int var_idx) {
    if (a->nvar_owners >= a->var_owners_cap) {
        a->var_owners_cap = a->var_owners_cap ? a->var_owners_cap * 2 : 64;
        a->var_owner_addr = realloc(a->var_owner_addr, sizeof(int) * a->var_owners_cap);
        a->var_owner_idx = realloc(a->var_owner_idx, sizeof(int) * a->var_owners_cap);
    }
    a->var_owner_addr[a->nvar_owners] = addr;
    a->var_owner_idx[a->nvar_owners] = var_idx;
    a->nvar_owners++;
}
/* returns vars[] index owning addr, or -1 */
static int var_owner_find(Assembler *a, int addr) {
    for (int i = 0; i < a->nvar_owners; i++)
        if (a->var_owner_addr[i] == addr) return a->var_owner_idx[i];
    return -1;
}

static Instr *instr_push(Assembler *a) {
    if (a->ninstr >= a->instr_cap) {
        a->instr_cap = a->instr_cap ? a->instr_cap * 2 : 64;
        a->instr_lines = realloc(a->instr_lines, sizeof(Instr) * a->instr_cap);
        a->words_out = realloc(a->words_out, sizeof(EncodedWords) * a->instr_cap);
    }
    return &a->instr_lines[a->ninstr++];
}

/* -------------------------------------------------------------------- */
/* init / free                                                           */
/* -------------------------------------------------------------------- */
void asm_init(Assembler *a, const char *filename, const char *raw_text) {
    memset(a, 0, sizeof(*a));
    snprintf(a->filename, sizeof(a->filename), "%s", filename);

    int err_line = 0;
    char err_msg[MAX_MSG];
    char *clean = strip_comments(raw_text, &err_line, err_msg, sizeof(err_msg));

    /* split clean text on '\n' */
    int cap = 256, n = 0;
    char **lines = malloc(sizeof(char *) * cap);
    const char *p = clean;
    while (1) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (n >= cap) { cap *= 2; lines = realloc(lines, sizeof(char *) * cap); }
        char *line = malloc(len + 1);
        memcpy(line, p, len);
        line[len] = '\0';
        lines[n++] = line;
        if (!nl) break;
        p = nl + 1;
    }
    free(clean);
    a->lines = lines;
    a->nlines = n;

    if (err_line) diag_push(a, err_line, 1, err_msg);
}

void asm_free(Assembler *a) {
    for (int i = 0; i < a->nlines; i++) free(a->lines[i]);
    free(a->lines);
    free(a->diags);
    free(a->labels);
    free(a->vars);
    free(a->var_owner_addr);
    free(a->var_owner_idx);
    free(a->instr_lines);
    free(a->words_out);
}

int asm_has_errors(const Assembler *a) {
    for (int i = 0; i < a->ndiags; i++) if (a->diags[i].is_error) return 1;
    return 0;
}

static int diag_cmp(const void *pa, const void *pb) {
    const Diag *da = pa, *db = pb;
    return da->line_no - db->line_no;
}
void asm_sort_diags(Assembler *a) {
    qsort(a->diags, a->ndiags, sizeof(Diag), diag_cmp);
}

/* -------------------------------------------------------------------- */
/* pass1                                                                 */
/* -------------------------------------------------------------------- */
typedef struct { int line_no; char name[MAX_NAME]; int val; } PendingVar;

void asm_run(Assembler *a) {
    int addr = 0;

    PendingVar *explicit_vars = NULL, *implicit_vars = NULL;
    int n_explicit = 0, cap_explicit = 0;
    int n_implicit = 0, cap_implicit = 0;

    for (int i = 0; i < a->nlines; i++) {
        int line_no = i + 1;
        char line[2048];
        snprintf(line, sizeof(line), "%s", a->lines[i]);
        strip_inplace(line);
        if (!line[0]) continue;

        char name[MAX_NAME];
        if (match_label_decl(line, name, sizeof(name))) {
            if (find_label(a, name)) {
                ERRORF(a, line_no, "label '%s' already declared", name);
            } else {
                Label *l = label_push(a);
                snprintf(l->name, sizeof(l->name), "%s", name);
                l->addr = addr;
                l->decl_line = line_no;
            }
            continue;
        }

        int has_val = 0, val = 0;
        if (match_var_decl(line, name, sizeof(name), &has_val, &val)) {
            if (has_val) {
                if (n_explicit >= cap_explicit) {
                    cap_explicit = cap_explicit ? cap_explicit * 2 : 32;
                    explicit_vars = realloc(explicit_vars, sizeof(PendingVar) * cap_explicit);
                }
                explicit_vars[n_explicit].line_no = line_no;
                snprintf(explicit_vars[n_explicit].name, MAX_NAME, "%s", name);
                explicit_vars[n_explicit].val = val;
                n_explicit++;
            } else {
                if (n_implicit >= cap_implicit) {
                    cap_implicit = cap_implicit ? cap_implicit * 2 : 32;
                    implicit_vars = realloc(implicit_vars, sizeof(PendingVar) * cap_implicit);
                }
                implicit_vars[n_implicit].line_no = line_no;
                snprintf(implicit_vars[n_implicit].name, MAX_NAME, "%s", name);
                n_implicit++;
            }
            continue;
        }

        /* tokenize: first whitespace-delimited token is mnemonic, rest
         * (after stripping) is comma-split operand list */
        char linecopy[2048];
        snprintf(linecopy, sizeof(linecopy), "%s", line);
        char *save = NULL;
        char *tok0 = strtok_r(linecopy, " \t", &save);
        char mnemonic[MAX_MNEMONIC];
        snprintf(mnemonic, sizeof(mnemonic), "%s", tok0 ? tok0 : "");
        to_upper_str(mnemonic);

        size_t tok0len = tok0 ? strlen(tok0) : 0;
        char rest[2048];
        snprintf(rest, sizeof(rest), "%s", line + tok0len);
        strip_inplace(rest);

        char operands[MAX_OPERANDS][MAX_OPERAND];
        int nops = 0;
        if (rest[0]) {
            char *s = rest;
            char *comma;
            while (1) {
                comma = strchr(s, ',');
                char field[MAX_OPERAND];
                size_t flen = comma ? (size_t)(comma - s) : strlen(s);
                if (flen >= MAX_OPERAND) flen = MAX_OPERAND - 1;
                memcpy(field, s, flen);
                field[flen] = '\0';
                strip_inplace(field);
                if (nops < MAX_OPERANDS) snprintf(operands[nops++], MAX_OPERAND, "%s", field);
                if (!comma) break;
                s = comma + 1;
            }
        }

        const char *fmt;
        if (!strcmp(mnemonic, "LOAD")) {
            fmt = "LOAD";
        } else {
            const OpcodeEntry *oe = find_opcode(mnemonic);
            if (!oe) {
                ERRORF(a, line_no, "unknown instruction '%s'", mnemonic);
                continue;
            }
            fmt = oe->fmt;
        }

        if (addr >= MAX_PROGRAM_WORDS) {
            ERRORF(a, line_no, "program exceeds addressable program memory (65536 words)");
            continue;
        }

        Instr *ins = instr_push(a);
        ins->line_no = line_no;
        ins->addr = addr;
        snprintf(ins->mnemonic, sizeof(ins->mnemonic), "%s", mnemonic);
        ins->nops = nops;
        for (int k = 0; k < nops; k++) snprintf(ins->operands[k], MAX_OPERAND, "%s", operands[k]);

        addr += words_per_format(fmt);
    }

    a->program_length = addr;

    /* explicit VARs first */
    for (int i = 0; i < n_explicit; i++) {
        int line_no = explicit_vars[i].line_no;
        const char *name = explicit_vars[i].name;
        int vaddr = explicit_vars[i].val;

        if (find_var(a, name)) { ERRORF(a, line_no, "variable '%s' already declared", name); continue; }
        Label *lb = find_label(a, name);
        if (lb) {
            ERRORF(a, line_no,
                   "'%s' is already declared as a label (line %d) - names must "
                   "be unique across labels and variables", name, lb->decl_line);
            continue;
        }
        if (vaddr >= PORT_LOW && vaddr <= PORT_HIGH) {
            ERRORF(a, line_no, "address %04Xh is reserved for ports (0000h-0003h)", vaddr);
            continue;
        }
        if (vaddr >= STACK_LOW && vaddr <= STACK_HIGH) {
            ERRORF(a, line_no, "address %04Xh is inside the reserved stack region (FA00h-FFFFh)", vaddr);
            continue;
        }
        int owner = var_owner_find(a, vaddr);
        if (owner >= 0) {
            ERRORF(a, line_no, "address %04Xh already assigned to variable '%s'", vaddr, a->vars[owner].name);
            continue;
        }
        Var *v = var_push(a);
        snprintf(v->name, sizeof(v->name), "%s", name);
        v->addr = vaddr;
        var_owner_add(a, vaddr, a->nvars - 1);
    }

    /* auto-allocate implicit VARs */
    int cursor = VAR_AUTO_START;
    for (int i = 0; i < n_implicit; i++) {
        int line_no = implicit_vars[i].line_no;
        const char *name = implicit_vars[i].name;

        if (find_var(a, name)) { ERRORF(a, line_no, "variable '%s' already declared", name); continue; }
        Label *lb = find_label(a, name);
        if (lb) {
            ERRORF(a, line_no,
                   "'%s' is already declared as a label (line %d) - names must "
                   "be unique across labels and variables", name, lb->decl_line);
            continue;
        }
        int ran_out = 0;
        while (1) {
            if (cursor >= PORT_LOW && cursor <= PORT_HIGH) { cursor = PORT_HIGH + 1; continue; }
            if (cursor >= STACK_LOW && cursor <= STACK_HIGH) {
                ERRORF(a, line_no,
                       "ran out of data memory for auto-allocated variable '%s' "
                       "(hit reserved stack region)", name);
                ran_out = 1;
                break;
            }
            if (var_owner_find(a, cursor) >= 0) { cursor++; continue; }
            break;
        }
        if (ran_out) continue;
        Var *v = var_push(a);
        snprintf(v->name, sizeof(v->name), "%s", name);
        v->addr = cursor;
        var_owner_add(a, cursor, a->nvars - 1);
        cursor++;
    }
    free(explicit_vars);
    free(implicit_vars);

    /* -------------------------------------------------------------- pass2 */
    for (int i = 0; i < a->ninstr; i++) pass2_process(a, i);

    if (a->ninstr > 0) {
        int last_line_no = a->instr_lines[a->ninstr - 1].line_no;
        int has_halt = 0;
        for (int i = 0; i < a->ninstr; i++)
            if (!strcmp(a->instr_lines[i].mnemonic, "HALT")) { has_halt = 1; break; }
        if (!has_halt)
            WARNF(a, last_line_no,
                  "program contains no HALT instruction - execution will run "
                  "past the end of program memory");
    }
}

/* -------------------------------------------------------------------- */
/* pass2 helpers: parse_reg / parse_immediate / parse_port / parse_addr  */
/* -------------------------------------------------------------------- */
static int parse_reg(Assembler *a, const char *tok, char bank, int line_no) {
    char letter; int num;
    if (!match_reg(tok, &letter, &num)) {
        ERRORF(a, line_no, "expected %c register, got '%s'", bank, tok);
        return 0;
    }
    if (letter != bank) {
        ERRORF(a, line_no, "expected %c register, got '%s'", bank, tok);
        return 0;
    }
    if (num == 7) {
        ERRORF(a, line_no, "explicit use of %c7 is not allowed (%s)", bank,
               bank == 'A' ? "stack pointer" : "hidden register");
        return 0;
    }
    return num;
}

static int parse_immediate(Assembler *a, const char *tok, int line_no) {
    int val;
    if (!match_imm(tok, &val)) {
        ERRORF(a, line_no, "expected immediate value 'xxxxh' (no '#'), got '%s'", tok);
        return 0;
    }
    return val;
}

static int parse_port(Assembler *a, const char *tok, int line_no) {
    int val;
    if (!match_port(tok, &val)) {
        ERRORF(a, line_no, "expected port '#1'-'#4', got '%s'", tok);
        return 0;
    }
    if (val < 1 || val > 4) {
        ERRORF(a, line_no, "port must be 1-4, got '%s'", tok);
        return 0;
    }
    return val;
}

static int parse_addr(Assembler *a, const char *tok, int line_no, int allow_label, int allow_var) {
    int val;
    if (match_addr_lit(tok, &val)) {
        if (allow_var) {
            if (val >= PORT_LOW && val <= PORT_HIGH) {
                ERRORF(a, line_no, "address %04Xh is reserved for ports (0000h-0003h)", val);
                return 0;
            }
            if (val >= STACK_LOW && val <= STACK_HIGH) {
                ERRORF(a, line_no, "address %04Xh is inside the reserved stack region (FA00h-FFFFh)", val);
                return 0;
            }
            int owner = var_owner_find(a, val);
            if (owner >= 0) {
                WARNF(a, line_no, "address %04Xh matches variable '%s' - consider using '#%s'",
                      val, a->vars[owner].name, a->vars[owner].name);
            }
        }
        return val;
    }

    char name[MAX_NAME];
    if (match_symref(tok, name, sizeof(name))) {
        Label *lb = find_label(a, name);
        Var *v = find_var(a, name);
        if (allow_label && lb) return lb->addr;
        if (allow_var && v) return v->addr;
        if (lb && !allow_label) {
            ERRORF(a, line_no, "'%s' is a label, not valid as a data address here", name);
            return 0;
        }
        if (v && !allow_var) {
            ERRORF(a, line_no, "'%s' is a variable, not valid as a jump target", name);
            return 0;
        }
        ERRORF(a, line_no, "undefined symbol '%s'", name);
        return 0;
    }

    ERRORF(a, line_no, "expected '#xxxxh' or '#Name', got '%s'", tok);
    return 0;
}

static int need(Assembler *a, Instr *ins, int count, int line_no, const char *mnemonic) {
    if (ins->nops != count) {
        ERRORF(a, line_no, "%s expects %d operand(s), got %d", mnemonic, count, ins->nops);
        return 0;
    }
    return 1;
}

static int pack(int opcode, int ra, int rb) {
    return ((opcode & 0x3F) << 10) | ((ra & 0x7) << 7) | ((rb & 0x7) << 4);
}

/* encode() - fills ew; ew->valid stays 0 on any failure */
static void encode_instr(Assembler *a, Instr *ins, EncodedWords *ew) {
    int line_no = ins->line_no;
    ew->valid = 0;
    ew->nwords = 0;

    if (!strcmp(ins->mnemonic, "LOAD")) {
        if (!need(a, ins, 2, line_no, ins->mnemonic)) return;
        char letter; int num;
        if (!match_reg(ins->operands[0], &letter, &num)) {
            ERRORF(a, line_no, "expected register, got '%s'", ins->operands[0]);
            return;
        }
        if (num == 7) {
            ERRORF(a, line_no, "explicit use of %c7 is not allowed", letter);
            return;
        }
        int opcode, ra, rb;
        if (letter == 'A') { opcode = 0x22; ra = num; rb = 7; }
        else { opcode = 0x23; ra = 6; rb = num; }
        int imm = parse_immediate(a, ins->operands[1], line_no);
        ew->words[0] = pack(opcode, ra, rb);
        ew->words[1] = imm;
        ew->nwords = 2;
        ew->valid = 1;
        return;
    }

    const OpcodeEntry *oe = find_opcode(ins->mnemonic);
    if (!oe) { ERRORF(a, line_no, "internal: unknown mnemonic '%s'", ins->mnemonic); return; }
    int opcode = oe->opcode;
    const char *fmt = oe->fmt;

    if (!strcmp(fmt, "RR")) {
        if (!need(a, ins, 2, line_no, ins->mnemonic)) return;
        int ra = parse_reg(a, ins->operands[0], 'A', line_no);
        int rb = parse_reg(a, ins->operands[1], 'B', line_no);
        if (!strcmp(ins->mnemonic, "BITSET") || !strcmp(ins->mnemonic, "BITCLR") ||
            !strcmp(ins->mnemonic, "BITTEST")) {
            if (ra >= 0 && ra <= 6 && !a->has_mask[ra]) {
                WARNF(a, line_no, "no BITMASK generated for A%d before %s (assumed already valid)",
                      ra, ins->mnemonic);
            }
        }
        ew->words[0] = pack(opcode, ra, rb); ew->nwords = 1; ew->valid = 1; return;
    }
    if (!strcmp(fmt, "R")) {
        if (!need(a, ins, 1, line_no, ins->mnemonic)) return;
        int ra = parse_reg(a, ins->operands[0], 'A', line_no);
        if (!strcmp(ins->mnemonic, "BITMASK") && ra >= 0 && ra <= 6) a->has_mask[ra] = 1;
        ew->words[0] = pack(opcode, ra, 7); ew->nwords = 1; ew->valid = 1; return;
    }
    if (!strcmp(fmt, "NONE")) {
        if (!need(a, ins, 0, line_no, ins->mnemonic)) return;
        ew->words[0] = pack(opcode, 6, 7); ew->nwords = 1; ew->valid = 1; return;
    }
    if (!strcmp(fmt, "RB_ONLY")) {
        if (!need(a, ins, 1, line_no, ins->mnemonic)) return;
        int rb = parse_reg(a, ins->operands[0], 'B', line_no);
        ew->words[0] = pack(opcode, 6, rb); ew->nwords = 1; ew->valid = 1; return;
    }
    if (!strcmp(fmt, "RA_ADDR")) {
        if (!need(a, ins, 2, line_no, ins->mnemonic)) return;
        int ra = parse_reg(a, ins->operands[0], 'A', line_no);
        int address = parse_addr(a, ins->operands[1], line_no, 0, 1);
        ew->words[0] = pack(opcode, ra, 7); ew->words[1] = address; ew->nwords = 2; ew->valid = 1; return;
    }
    if (!strcmp(fmt, "RB_ADDR")) {
        if (!need(a, ins, 2, line_no, ins->mnemonic)) return;
        int rb = parse_reg(a, ins->operands[0], 'B', line_no);
        int address = parse_addr(a, ins->operands[1], line_no, 0, 1);
        ew->words[0] = pack(opcode, 6, rb); ew->words[1] = address; ew->nwords = 2; ew->valid = 1; return;
    }
    if (!strcmp(fmt, "RA_PORT")) {
        if (!need(a, ins, 2, line_no, ins->mnemonic)) return;
        int ra = parse_reg(a, ins->operands[0], 'A', line_no);
        int port = parse_port(a, ins->operands[1], line_no);
        ew->words[0] = pack(opcode, ra, 7); ew->words[1] = port; ew->nwords = 2; ew->valid = 1; return;
    }
    if (!strcmp(fmt, "RB_PORT")) {
        if (!need(a, ins, 2, line_no, ins->mnemonic)) return;
        int rb = parse_reg(a, ins->operands[0], 'B', line_no);
        int port = parse_port(a, ins->operands[1], line_no);
        ew->words[0] = pack(opcode, 6, rb); ew->words[1] = port; ew->nwords = 2; ew->valid = 1; return;
    }
    if (!strcmp(fmt, "JUMP")) {
        if (!need(a, ins, 1, line_no, ins->mnemonic)) return;
        int target = parse_addr(a, ins->operands[0], line_no, 1, 0);
        ew->words[0] = pack(opcode, 6, 7); ew->words[1] = target; ew->nwords = 2; ew->valid = 1; return;
    }
    if (!strcmp(fmt, "MOVM")) {
        if (!need(a, ins, 2, line_no, ins->mnemonic)) return;
        int src = parse_addr(a, ins->operands[0], line_no, 0, 1);
        int dst = parse_addr(a, ins->operands[1], line_no, 0, 1);
        ew->words[0] = pack(opcode, 6, 7); ew->words[1] = src; ew->words[2] = dst; ew->nwords = 3; ew->valid = 1; return;
    }
    if (!strcmp(fmt, "MEMCPY")) {
        if (!need(a, ins, 3, line_no, ins->mnemonic)) return;
        int length = parse_immediate(a, ins->operands[0], line_no);
        int src = parse_addr(a, ins->operands[1], line_no, 0, 1);
        int dst = parse_addr(a, ins->operands[2], line_no, 0, 1);
        ew->words[0] = pack(opcode, 6, 7); ew->words[1] = length; ew->words[2] = src; ew->words[3] = dst;
        ew->nwords = 4; ew->valid = 1; return;
    }
    if (!strcmp(fmt, "MEMSET")) {
        if (!need(a, ins, 3, line_no, ins->mnemonic)) return;
        int rb = parse_reg(a, ins->operands[0], 'B', line_no);
        int length = parse_immediate(a, ins->operands[1], line_no);
        int dst = parse_addr(a, ins->operands[2], line_no, 0, 1);
        ew->words[0] = pack(opcode, 6, rb); ew->words[1] = length; ew->words[2] = dst;
        ew->nwords = 3; ew->valid = 1; return;
    }

    ERRORF(a, line_no, "internal: unhandled format '%s' for %s", fmt, ins->mnemonic);
}

static void pass2_process(Assembler *a, int idx) {
    Instr *ins = &a->instr_lines[idx];
    int line_no = ins->line_no;

    if (str_in_list(ins->mnemonic, STACK_OPS, STACK_OPS_COUNT)) {
        if (!a->seen_initsp)
            ERRORF(a, line_no, "%s used before INITSP has been executed", ins->mnemonic);
    }
    if (!strcmp(ins->mnemonic, "INITSP")) {
        if (a->seen_initsp) ERRORF(a, line_no, "INITSP executed more than once");
        a->seen_initsp = 1;
    }
    if (str_in_list(ins->mnemonic, A6_CLOBBER_OPS, A6_CLOBBER_OPS_COUNT)) {
        WARNF(a, line_no, "%s will overwrite A6 (scratch register for this operation)", ins->mnemonic);
    }

    EncodedWords *ew = &a->words_out[idx];
    encode_instr(a, ins, ew);
}

/* -------------------------------------------------------------------- */
/* output: listing                                                       */
/* -------------------------------------------------------------------- */
static void sbuf_append(char **buf, size_t *len, size_t *cap, const char *s) {
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap = (*cap) ? (*cap) * 2 : 4096;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, n + 1);
    *len += n;
}

typedef struct { const char *name; int addr; } VarSortEnt;
static int varsort_cmp(const void *pa, const void *pb) {
    const VarSortEnt *a1 = pa, *b1 = pb;
    return a1->addr - b1->addr;
}

char *asm_build_listing(const Assembler *a) {
    char *buf = NULL; size_t len = 0, cap = 0;
    char line[1024];

    snprintf(line, sizeof(line), "; Listing for %s\n", a->filename); sbuf_append(&buf, &len, &cap, line);
    snprintf(line, sizeof(line), "; Labels: %d   Variables: %d\n", a->nlabels, a->nvars); sbuf_append(&buf, &len, &cap, line);
    sbuf_append(&buf, &len, &cap, ";\n");

    if (a->nvars > 0) {
        sbuf_append(&buf, &len, &cap, "; Variable map (data memory):\n");
        VarSortEnt *sorted = malloc(sizeof(VarSortEnt) * a->nvars);
        for (int i = 0; i < a->nvars; i++) { sorted[i].name = a->vars[i].name; sorted[i].addr = a->vars[i].addr; }
        qsort(sorted, a->nvars, sizeof(VarSortEnt), varsort_cmp);
        for (int i = 0; i < a->nvars; i++) {
            snprintf(line, sizeof(line), ";   %-20s = %04Xh\n", sorted[i].name, sorted[i].addr);
            sbuf_append(&buf, &len, &cap, line);
        }
        free(sorted);
        sbuf_append(&buf, &len, &cap, ";\n");
    }

    snprintf(line, sizeof(line), "%-6s %-18s SOURCE\n", "ADDR", "WORD(bin)"); sbuf_append(&buf, &len, &cap, line);
    for (int i = 0; i < 60; i++) sbuf_append(&buf, &len, &cap, "-");
    sbuf_append(&buf, &len, &cap, "\n");

    for (int i = 0; i < a->ninstr; i++) {
        Instr *ins = &a->instr_lines[i];
        char src[1024];
        snprintf(src, sizeof(src), "%s", (ins->line_no - 1 < a->nlines) ? a->lines[ins->line_no - 1] : "");
        strip_inplace(src);

        const EncodedWords *ew = &a->words_out[i];
        if (!ew->valid) {
            snprintf(line, sizeof(line), "%04Xh  %-18s %s\n", ins->addr, "<error>", src);
            sbuf_append(&buf, &len, &cap, line);
            continue;
        }
        for (int wi = 0; wi < ew->nwords; wi++) {
            int addr = ins->addr + wi;
            char bin[17];
            for (int b = 0; b < 16; b++) bin[b] = ((ew->words[wi] >> (15 - b)) & 1) ? '1' : '0';
            bin[16] = '\0';
            if (wi == 0)
                snprintf(line, sizeof(line), "%04Xh  %s   %s\n", addr, bin, src);
            else
                snprintf(line, sizeof(line), "%04Xh  %s     (operand word)\n", addr, bin);
            sbuf_append(&buf, &len, &cap, line);
        }
    }
    if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
    return buf;
}

/* -------------------------------------------------------------------- */
/* output: hex                                                           */
/* -------------------------------------------------------------------- */
void asm_build_rom_bytes(const Assembler *a, unsigned char **rom0, unsigned char **rom1, int *outlen) {
    int n = a->program_length;
    unsigned char *r0 = calloc(n > 0 ? n : 1, 1);
    unsigned char *r1 = calloc(n > 0 ? n : 1, 1);
    for (int i = 0; i < a->ninstr; i++) {
        const Instr *ins = &a->instr_lines[i];
        const EncodedWords *ew = &a->words_out[i];
        if (!ew->valid) continue;
        for (int wi = 0; wi < ew->nwords; wi++) {
            int addr = ins->addr + wi;
            if (addr < 0 || addr >= n) continue;
            r0[addr] = (unsigned char)((ew->words[wi] >> 8) & 0xFF);
            r1[addr] = (unsigned char)(ew->words[wi] & 0xFF);
        }
    }
    *rom0 = r0; *rom1 = r1; *outlen = n;
}

void asm_build_combined_bytes(const Assembler *a, unsigned char **combined, int *outlen) {
    unsigned char *r0, *r1; int n;
    asm_build_rom_bytes(a, &r0, &r1, &n);
    size_t sz = (size_t)n * 2 > 0 ? (size_t)n * 2 : 1;
    unsigned char *c = malloc(sz);
    for (int i = 0; i < n; i++) { c[2 * i] = r0[i]; c[2 * i + 1] = r1[i]; }
    free(r0); free(r1);
    *combined = c; *outlen = n * 2;
}

char *asm_intel_hex(const unsigned char *bytes, int n, int bytes_per_line) {
    char *buf = NULL; size_t len = 0, cap = 0;
    int i = 0;
    char line[128];
    while (i < n) {
        int count = n - i < bytes_per_line ? n - i : bytes_per_line;
        int checksum = count + ((i >> 8) & 0xFF) + (i & 0xFF) + 0x00;
        char hexstr[8 + 2 * 16 + 1];
        int p = 0;
        p += snprintf(hexstr + p, sizeof(hexstr) - p, "%02X%02X%02X%02X",
                      count, (i >> 8) & 0xFF, i & 0xFF, 0x00);
        for (int k = 0; k < count; k++) {
            checksum += bytes[i + k];
            p += snprintf(hexstr + p, sizeof(hexstr) - p, "%02X", bytes[i + k]);
        }
        checksum = (-checksum) & 0xFF;
        snprintf(line, sizeof(line), ":%s%02X\n", hexstr, checksum);
        sbuf_append(&buf, &len, &cap, line);
        i += count;
    }
    sbuf_append(&buf, &len, &cap, ":00000001FF");
    return buf;
}
