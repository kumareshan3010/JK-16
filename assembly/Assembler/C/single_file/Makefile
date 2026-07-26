CC = gcc
CFLAGS = -std=c11 -Wall -O2

assembler: assembler.c
	$(CC) $(CFLAGS) -o assembler assembler.c

clean:
	rm -f assembler *.hex *.txt

.PHONY: clean
