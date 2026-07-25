// fibonacci.asm
// Computes fib(N) iteratively using the identity
//   fib_prev, fib_curr = fib_curr, fib_prev + fib_curr
// implemented here as ADD followed by SWAP. ADD always writes its
// result into the bank-A operand, so right after the add, the bank-B
// operand still holds the pre-add value - swapping the two registers
// rotates them into position for the next iteration in a single
// extra instruction, with no separate temporary needed.

VAR result

INITSP

LOAD    A0, 0000h      ; A0 = fib_prev, fib(0) = 0
LOAD    B0, 0001h      ; B0 = fib_curr, fib(1) = 1
LOAD    A2, 0009h      ; A2 = N, computing fib(9)

fib_loop:
    ADD     A0, B0          ; A0 = fib_prev + fib_curr
    SWAP    A0, B0          ; fib_prev <- old fib_curr, fib_curr <- new sum
    DEC     A2
    JNZ     #fib_loop

STOREB  B0, #result         ; fib(N) ends up in B0
HALT
