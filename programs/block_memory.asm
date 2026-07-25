// block_memory.asm
// Clears a 10-word buffer with MEMSET, writes a marker value into its
// first word, then duplicates the whole buffer elsewhere with MEMCPY.
// Demonstrates the block-memory instructions, which take their length
// as an immediate and loop internally rather than needing an
// assembly-level loop.

VAR bufferA = #8100h
VAR bufferB = #8200h

INITSP

LOAD    B0, 0000h
MEMSET  B0, 000Ah, #bufferA        ; zero-fill 10 words at bufferA

LOAD    A0, 00AAh
STOREA  A0, #bufferA                ; mark the first word so the copy is visible

MEMCPY  000Ah, #bufferA, #bufferB   ; duplicate all 10 words into bufferB

HALT
