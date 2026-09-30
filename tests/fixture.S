// A static AArch64 "program" containing one of each site apkscan must find,
// plus ordinary instructions that must not be reported.
    .text
    .globl _start
_start:
    str  x30, [x18], #8          // shadow call stack push
    mrs  x0, tpidr_el0           // bionic reads its thread pointer
    mrs  x7, tpidr_el0
    msr  tpidr_el0, x1           // and sets it for a new thread
    mov  x8, #64                 // __NR_write
    svc  #0
    svc  #0
    svc  #1                      // not the Linux form: must not be counted
    mrs  x0, tpidrro_el0         // Darwin's TLS register: must not be counted
    add  x18, x18, #8            // other x18 use: not a shadow-stack pattern
    ldr  x30, [x18, #-8]!        // shadow call stack pop
    ret
