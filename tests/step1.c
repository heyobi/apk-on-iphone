/* One-instruction entry point for tests/difftest.py: load a register state,
 * execute insn at pc against one memory region, store the state back. */
#include "../core/cpu.h"

#include <string.h>

/* st: x0..x30, sp, pc, nzcv (bits 31..28), tpidr, then v0..v31 as lo/hi pairs,
 * fpsr — 100 words. Returns the stop
 * reason (AOI_RUN means the instruction completed). */
int aoi_step1(uint32_t insn, uint64_t *st, uint8_t *mem, uint64_t membase, uint64_t memsize,
              uint64_t *fault_addr)
{
    struct aoi_mem m = {0};
    struct aoi_cpu c = {0};
    static uint8_t code[4096];   /* the page around pc, zeroed, like Unicorn's */
    uint64_t page = st[32] & ~(uint64_t)0xfff;
    int i;
    memset(code, 0, sizeof code);
    for (i = 0; i < 4; i++) code[(st[32] & 0xfff) + i] = (uint8_t)(insn >> (8 * i));
    m.n = 2;
    m.r[0].base = membase; m.r[0].size = memsize; m.r[0].host = mem;
    m.r[1].base = page;   m.r[1].size = 4096;    m.r[1].host = code;
    for (i = 0; i < 31; i++) c.x[i] = st[i];
    c.sp = st[31]; c.pc = st[32];
    c.n = st[33] >> 31 & 1; c.z = st[33] >> 30 & 1; c.c = st[33] >> 29 & 1; c.v = st[33] >> 28 & 1;
    c.tpidr = st[34];
    for (i = 0; i < 32; i++) { c.vreg[i][0] = st[35 + 2 * i]; c.vreg[i][1] = st[36 + 2 * i]; }
    c.fpsr = (uint32_t)st[99];
    c.mem = &m;
    aoi_cpu_run(&c, 1);
    for (i = 0; i < 31; i++) st[i] = c.x[i];
    st[31] = c.sp; st[32] = c.pc;
    st[33] = (uint64_t)c.n << 31 | (uint64_t)c.z << 30 | (uint64_t)c.c << 29 | (uint64_t)c.v << 28;
    st[34] = c.tpidr;
    for (i = 0; i < 32; i++) { st[35 + 2 * i] = c.vreg[i][0]; st[36 + 2 * i] = c.vreg[i][1]; }
    st[99] = c.fpsr;
    *fault_addr = c.fault_addr;
    return c.stop;
}
