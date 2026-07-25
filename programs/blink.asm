// blink.asm
// Drives bit 0 of GPIO port 1 high and low alternately, with a software
// delay loop in between so the toggling is visible on a scope or LED.
// Demonstrates OUTA, XOR-based bit toggling, and a nested countdown
// delay using two bank-A counters.
//
// Runs forever by design, so there is no HALT - the assembler's
// "no HALT instruction" warning for this file is expected.

INITSP

LOAD    A0, 0001h      ; A0 = output pattern, bit 0 set
LOAD    B0, 0001h      ; B0 = toggle mask for bit 0

blink_loop:
    OUTA    A0, #1          ; drive GPIO port 1 with the current pattern
    XOR     A0, B0          ; flip bit 0 for the next pass

    LOAD    A1, 00FFh       ; outer delay counter
delay_outer:
    LOAD    A2, 00FFh       ; inner delay counter
delay_inner:
    DEC     A2
    JNZ     #delay_inner
    DEC     A1
    JNZ     #delay_outer

    JMP     #blink_loop
