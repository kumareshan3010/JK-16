// factorial.asm
// Computes N! iteratively. The accumulator has to live in bank A (MUL
// writes its result into the bank-A operand) and the counter has to be
// readable from bank B (MUL's second operand), so the counter is kept
// in B1 and mirrored into A2 each pass with MOV to decrement and test
// it, then written back into B1 with SWAP for the next multiply.

VAR result

INITSP

LOAD    A0, 0001h       ; A0 = accumulator, starts at 1
LOAD    B1, 0006h        ; B1 = counter, N = 6

fact_loop:
    MUL     A0, B1          ; accumulator *= counter
    MOV     A2, B1          ; A2 = copy of counter (B1 left untouched)
    DEC     A2               ; A2 = counter - 1, sets the zero flag
    JZ      #fact_done       ; counter was 1 - the multiply above was the last one
    SWAP    A2, B1           ; B1 = decremented counter for the next pass
    JMP     #fact_loop

fact_done:
STOREA  A0, #result
HALT
