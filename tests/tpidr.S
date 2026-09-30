    .text
    .globl _start
_start:
    movz x1, #0x1234
    movk x1, #0xabcd, lsl #16
    msr  tpidr_el0, x1
    mrs  x2, tpidr_el0
    cmp  x1, x2
    cset x0, ne              // 0 if equal
    mov  x8, #93             // exit
    svc  #0
