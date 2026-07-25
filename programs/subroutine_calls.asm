// subroutine_calls.asm
// Calls a small subroutine twice to double a value, and uses PUSH/POP
// around the calls to illustrate the calling convention: any register
// the caller needs preserved across a CALL is saved beforehand and
// restored afterward, since a subroutine is free to use registers as
// scratch (and CALL/RET themselves use A6 internally).

VAR result

INITSP

LOAD    A0, 0003h        ; A0 = 3, the value to double twice
LOAD    A3, 00FFh        ; A3 = a value that must survive the calls below

PUSH    B0                 ; save B0 before the calls
CALL    #double             ; A0 = A0 * 2 = 6
CALL    #double             ; A0 = A0 * 2 = 12
POP     B0                  ; restore B0

STOREA  A0, #result
HALT

// --------------------------------------------------------------
// double: doubles A0 in place and returns.
// Clobbers: A6 (used internally by CALL/RET).
// --------------------------------------------------------------
double:
    SHL     A0
    RET
