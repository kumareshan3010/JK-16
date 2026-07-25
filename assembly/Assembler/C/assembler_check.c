/* assembler_check.c -- syntax/semantic checker ONLY.
 *
 * Runs the full two-pass assembler (symbol resolution + encoding) so every
 * error and warning that the real assembler would catch is reported, but
 * it NEVER writes any file, regardless of flags. Useful as a fast
 * "does this even assemble" check, e.g. wired into an editor or a CI step.
 *
 * Usage: assembler_check program.asm
 * Output: "file:line: level: message" lines (errors and warnings), then
 *         "N error(s), M warning(s)".
 * Exit code: 0 if zero errors, 1 otherwise.
 */
#define _POSIX_C_SOURCE 200809L
#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s program.asm\n", argv[0]);
        fprintf(stderr, "Checks the assembly source for syntax/semantic errors. "
                        "Never writes any output file.\n");
        return 1;
    }
    const char *source = argv[1];
    char *raw = read_file(source);
    if (!raw) { fprintf(stderr, "error: file not found: %s\n", source); return 1; }

    char pathcopy[1024];
    snprintf(pathcopy, sizeof(pathcopy), "%s", source);
    char *filename = basename(pathcopy);

    Assembler a;
    asm_init(&a, filename, raw);
    free(raw);
    asm_run(&a);
    asm_sort_diags(&a);

    int nerrors = 0, nwarnings = 0;
    for (int i = 0; i < a.ndiags; i++) {
        Diag *d = &a.diags[i];
        printf("%s:%d: %s: %s\n", filename, d->line_no, d->is_error ? "error" : "warning", d->msg);
        if (d->is_error) nerrors++; else nwarnings++;
    }
    printf("\n%d error(s), %d warning(s)\n", nerrors, nwarnings);
    if (nerrors == 0) printf("OK: %s assembles cleanly.\n", filename);

    asm_free(&a);
    return nerrors ? 1 : 0;
}
