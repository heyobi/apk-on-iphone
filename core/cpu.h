/* A small AArch64 interpreter: the no-JIT execution backend.
 *
 * It runs guest code one instruction at a time against guest memory described
 * by a region table. Enough of the A64 base instruction set is implemented to
 * run freestanding Linux programs compiled with -mgeneral-regs-only; anything
 * else stops the CPU with AOI_STOP_UNDEF so the gap is visible. */
#ifndef AOI_CPU_H
#define AOI_CPU_H

#include <stddef.h>
#include <stdint.h>

#define AOI_MAX_REGIONS 16

struct aoi_region {
    uint64_t base, size;
    uint8_t *host;
};

struct aoi_mem {
    int n;
    struct aoi_region r[AOI_MAX_REGIONS];
};

enum aoi_stop { AOI_RUN = 0, AOI_STOP_EXIT, AOI_STOP_UNDEF, AOI_STOP_FAULT, AOI_STOP_SYSCALL };

struct aoi_cpu {
    uint64_t x[31];
    uint64_t sp, pc;
    int n, z, c, v;
    uint64_t tpidr;             /* guest thread pointer, never the host's */
    struct aoi_mem *mem;
    enum aoi_stop stop;
    uint64_t fault_addr;
    uint32_t fault_insn;
    int exit_code;
    uint64_t steps;
};

/* Guest memory access. Returns NULL (and sets a fault) if [addr, addr+len) is unmapped. */
uint8_t *aoi_mem_ptr(struct aoi_mem *mem, uint64_t addr, uint64_t len);
uint8_t *aoi_mem_map(struct aoi_mem *mem, uint64_t base, uint64_t size);

/* Runs until the guest exits or stops; at most max_steps instructions (0 = no limit). */
enum aoi_stop aoi_cpu_run(struct aoi_cpu *cpu, uint64_t max_steps);

/* Provided by the syscall layer (core/linux.c). Returns the value for x0, or
 * sets cpu->stop to end the run. */
uint64_t aoi_linux_syscall(struct aoi_cpu *cpu);

#endif
