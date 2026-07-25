/* assembler_hex.c -- writes ONLY the ROM hex files (no listing.txt).
 *
 * Usage: assembler_hex program.asm
 * On success (zero errors) writes:
 *     program_rom0.hex   (high byte of every instruction word)
 *     program_rom1.hex   (low byte of every instruction word)
 * Refuses to write anything if there is at least one error.
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

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "error: could not write %s\n", path); exit(1); }
    fputs(content, f);
    fputc('\n', f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s program.asm\n", argv[0]);
        fprintf(stderr, "Assembles and writes program_rom0.hex / program_rom1.hex only.\n");
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

    if (nerrors) {
        printf("Build aborted: fix all errors first.\n");
        asm_free(&a);
        return 1;
    }

    char base[1024];
    snprintf(base, sizeof(base), "%s", source);
    char *dot = strrchr(base, '.');
    char *slash = strrchr(base, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';

    char rom0_path[1100], rom1_path[1100];
    snprintf(rom0_path, sizeof(rom0_path), "%s_rom0.hex", base);
    snprintf(rom1_path, sizeof(rom1_path), "%s_rom1.hex", base);

    unsigned char *rom0, *rom1; int romlen;
    asm_build_rom_bytes(&a, &rom0, &rom1, &romlen);
    char *hex0 = asm_intel_hex(rom0, romlen, 16);
    char *hex1 = asm_intel_hex(rom1, romlen, 16);

    write_file(rom0_path, hex0);
    write_file(rom1_path, hex1);

    printf("Wrote %s\n", rom0_path);
    printf("Wrote %s\n", rom1_path);
    printf("Program length: %d words\n", a.program_length);

    free(hex0); free(hex1); free(rom0); free(rom1);
    asm_free(&a);
    return 0;
}
