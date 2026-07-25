/* assembler_full.c -- full CLI, mirrors assembler-4.py:
 *
 *   assembler_full program.asm            -> ASSEMBLE (check only), writes nothing
 *   assembler_full program.asm --build     -> writes program_rom0.hex, program_rom1.hex,
 *                                              program_listing.txt (only if zero errors)
 *   assembler_full program.asm --digital [--digital-mode start|debug]
 *                                           -> also writes program_digital.hex and sends
 *                                              it to Digital (hneemann) over TCP :41114
 *
 * Exit code 0 = no errors, 1 = errors (or file/usage problems).
 */
#define _POSIX_C_SOURCE 200809L
#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

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

static int recv_exact(int fd, unsigned char *buf, int n) {
    int got = 0;
    while (got < n) {
        int r = recv(fd, buf + got, n - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

/* Sends a Java DataOutputStream.writeUTF-style message (2-byte big-endian
 * length prefix + UTF-8 bytes) and reads the response the same way.
 * Returns 0 on success (response starts with "ok"), -1 on failure
 * (message printed to stderr). */
static int digital_send(const char *command, const char *host, int port, char *response_out, size_t response_sz) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fprintf(stderr, "could not create socket\n"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid host: %s\n", host);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "could not reach Digital at %s:%d - is Digital running "
                        "with the circuit open and remote control enabled in "
                        "Settings?\n", host, port);
        close(fd);
        return -1;
    }

    size_t plen = strlen(command);
    unsigned char hdr[2] = { (unsigned char)((plen >> 8) & 0xFF), (unsigned char)(plen & 0xFF) };
    if (send(fd, hdr, 2, 0) != 2 || send(fd, command, plen, 0) != (ssize_t)plen) {
        fprintf(stderr, "connection closed while sending command\n");
        close(fd);
        return -1;
    }
    unsigned char lenbuf[2];
    if (recv_exact(fd, lenbuf, 2) != 0) {
        fprintf(stderr, "connection closed while waiting for Digital's response\n");
        close(fd);
        return -1;
    }
    int resp_len = (lenbuf[0] << 8) | lenbuf[1];
    unsigned char *resp = malloc((size_t)resp_len + 1);
    if (recv_exact(fd, resp, resp_len) != 0) {
        fprintf(stderr, "connection closed while waiting for Digital's response\n");
        free(resp);
        close(fd);
        return -1;
    }
    resp[resp_len] = '\0';
    close(fd);

    snprintf(response_out, response_sz, "%s", resp);
    int ok = (!strcmp((char *)resp, "ok") || !strncmp((char *)resp, "ok:", 3));
    free(resp);
    if (!ok) {
        fprintf(stderr, "Digital reported an error: %s\n", response_out);
        return -1;
    }
    return 0;
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "error: could not write %s\n", path); exit(1); }
    fputs(content, f);
    fputc('\n', f);
    fclose(f);
}

int main(int argc, char **argv) {
    const char *source = NULL;
    int build = 0, digital = 0;
    const char *digital_mode = "debug";
    const char *digital_host = "127.0.0.1";
    int digital_port = 41114;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--build")) build = 1;
        else if (!strcmp(argv[i], "--digital")) digital = 1;
        else if (!strcmp(argv[i], "--digital-mode") && i + 1 < argc) digital_mode = argv[++i];
        else if (!strcmp(argv[i], "--digital-host") && i + 1 < argc) digital_host = argv[++i];
        else if (!strcmp(argv[i], "--digital-port") && i + 1 < argc) digital_port = atoi(argv[++i]);
        else if (!source) source = argv[i];
        else { fprintf(stderr, "unexpected argument: %s\n", argv[i]); return 1; }
    }
    if (!source) {
        fprintf(stderr, "usage: %s program.asm [--build] [--digital] "
                        "[--digital-mode start|debug] [--digital-host HOST] "
                        "[--digital-port PORT]\n", argv[0]);
        return 1;
    }
    if (digital) build = 1;

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

    if (!build) {
        asm_free(&a);
        return nerrors ? 1 : 0;
    }

    if (nerrors) {
        printf("Build aborted: fix all errors first.\n");
        asm_free(&a);
        return 1;
    }

    /* base = path without extension */
    char base[1024];
    snprintf(base, sizeof(base), "%s", source);
    char *dot = strrchr(base, '.');
    char *slash = strrchr(base, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';

    char rom0_path[1100], rom1_path[1100], listing_path[1100];
    snprintf(rom0_path, sizeof(rom0_path), "%s_rom0.hex", base);
    snprintf(rom1_path, sizeof(rom1_path), "%s_rom1.hex", base);
    snprintf(listing_path, sizeof(listing_path), "%s_listing.txt", base);

    unsigned char *rom0, *rom1; int romlen;
    asm_build_rom_bytes(&a, &rom0, &rom1, &romlen);
    char *hex0 = asm_intel_hex(rom0, romlen, 16);
    char *hex1 = asm_intel_hex(rom1, romlen, 16);
    char *listing = asm_build_listing(&a);

    write_file(rom0_path, hex0);
    write_file(rom1_path, hex1);
    write_file(listing_path, listing);

    printf("Wrote %s\n", rom0_path);
    printf("Wrote %s\n", rom1_path);
    printf("Wrote %s\n", listing_path);
    printf("Program length: %d words\n", a.program_length);

    free(hex0); free(hex1); free(listing);

    int rc = 0;
    if (digital) {
        char digital_path[1100];
        snprintf(digital_path, sizeof(digital_path), "%s_digital.hex", base);
        unsigned char *combined; int clen;
        asm_build_combined_bytes(&a, &combined, &clen);
        char *hexc = asm_intel_hex(combined, clen, 16);
        write_file(digital_path, hexc);
        printf("Wrote %s\n", digital_path);
        free(hexc); free(combined);

        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "%s:%s", digital_mode, digital_path);
        char response[256];
        if (digital_send(cmd, digital_host, digital_port, response, sizeof(response)) == 0) {
            printf("Digital: %s\n", response);
        } else {
            rc = 1;
        }
    }

    free(rom0); free(rom1);
    asm_free(&a);
    return rc;
}
