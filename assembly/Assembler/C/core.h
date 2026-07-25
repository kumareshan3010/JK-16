/* core.h -- shared core for the custom 16-bit Harvard CPU assembler.
 *
 * This is a line-for-line port of the logic in assembler-4.py (two-pass
 * assembler: pass1 collects labels/vars, pass2 encodes instructions).
 * All four command-line tools (assembler_full, assembler_check,
 * assembler_hex, assembler_txt) link against core.c and just differ in
 * what they do with the results in main().
 */
#ifndef ASM_CORE_H
#define ASM_CORE_H

#include <stdio.h>

#define MAX_NAME       64
#define MAX_MNEMONIC   16
#define MAX_OPERAND    32
#define MAX_OPERANDS   4
#define MAX_WORDS      4
#define MAX_MSG        256

#define PORT_LOW        0x0000
#define PORT_HIGH       0x0003
#define STACK_LOW       0xFA00
#define STACK_HIGH      0xFFFF
#define VAR_AUTO_START  0x0004
#define MAX_PROGRAM_WORDS 0x10000

typedef struct {
    int line_no;
    int is_error;          /* 1 = error, 0 = warning */
    char msg[MAX_MSG];
} Diag;

typedef struct {
    char name[MAX_NAME];
    int addr;
    int decl_line;
} Label;

typedef struct {
    char name[MAX_NAME];
    int addr;
} Var;

typedef struct {
    int line_no;
    int addr;
    char mnemonic[MAX_MNEMONIC];
    char operands[MAX_OPERANDS][MAX_OPERAND];
    int nops;
} Instr;

typedef struct {
    int valid;                 /* 0 if encoding failed for this instr */
    int words[MAX_WORDS];
    int nwords;
} EncodedWords;

typedef struct {
    char mnemonic[MAX_MNEMONIC];
    int opcode;
    char fmt[16];
} OpcodeEntry;

extern const OpcodeEntry OPCODES[];
extern const int OPCODES_COUNT;

typedef struct {
    char filename[512];

    char **lines;           /* source split by '\n', comments stripped */
    int nlines;

    Diag *diags;
    int ndiags, diags_cap;

    Label *labels;
    int nlabels, labels_cap;

    Var *vars;
    int nvars, vars_cap;

    /* var_addr_owner: addr -> index into vars[], parallel lookup table */
    int *var_owner_addr;    /* addresses that are owned */
    int *var_owner_idx;     /* matching vars[] index */
    int nvar_owners, var_owners_cap;

    Instr *instr_lines;
    int ninstr, instr_cap;

    EncodedWords *words_out;   /* parallel to instr_lines */

    int program_length;
    int seen_initsp;

    /* has_mask["A0".."A6"] -> bool, indexed 0..6 */
    int has_mask[7];
} Assembler;

/* lifecycle */
void asm_init(Assembler *a, const char *filename, const char *raw_text);
void asm_free(Assembler *a);
void asm_run(Assembler *a);              /* pass1 + pass2 */
int  asm_has_errors(const Assembler *a);

/* diagnostics, sorted by line number for printing */
void asm_sort_diags(Assembler *a);

/* output builders (only call when asm_has_errors() == 0) */
char *asm_build_listing(const Assembler *a);      /* caller frees */
void  asm_build_rom_bytes(const Assembler *a, unsigned char **rom0,
                           unsigned char **rom1, int *len);   /* caller frees */
void  asm_build_combined_bytes(const Assembler *a, unsigned char **combined,
                                int *len);                    /* caller frees */
char *asm_intel_hex(const unsigned char *bytes, int n, int bytes_per_line); /* caller frees */

/* comment stripping: returns malloc'd clean text; appends any
 * "unterminated block comment" diagnostic via the callback-free approach
 * (the line number and message are returned through out params). */
char *strip_comments(const char *text, int *err_line, char *err_msg, size_t err_msg_sz);

#endif
