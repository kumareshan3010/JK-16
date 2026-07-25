CC ?= cc
CFLAGS ?= -O2 -Wall -std=c11

BINARIES = assembler_full assembler_check assembler_hex assembler_txt

all: $(BINARIES)

assembler_full: core.c assembler_full.c core.h
	$(CC) $(CFLAGS) core.c assembler_full.c -o assembler_full

assembler_check: core.c assembler_check.c core.h
	$(CC) $(CFLAGS) core.c assembler_check.c -o assembler_check

assembler_hex: core.c assembler_hex.c core.h
	$(CC) $(CFLAGS) core.c assembler_hex.c -o assembler_hex

assembler_txt: core.c assembler_txt.c core.h
	$(CC) $(CFLAGS) core.c assembler_txt.c -o assembler_txt

clean:
	rm -f $(BINARIES) *.o

.PHONY: all clean
