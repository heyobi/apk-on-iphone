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

struct aoi_vm;

/* Guest memory: either a flat address space (vm, core/vm.h) or, for the small
 * test tools, a table of regions. */
struct aoi_mem {
    struct aoi_vm *vm;
    int n;
    struct aoi_region r[AOI_MAX_REGIONS];
};

/* AOI_STOP_YIELD: a syscall finished (x0 set, pc past the svc) but asked the
 * scheduler to switch threads, e.g. a futex wait (core/proc.c). */
enum aoi_stop { AOI_RUN = 0, AOI_STOP_EXIT, AOI_STOP_UNDEF, AOI_STOP_FAULT, AOI_STOP_SYSCALL,
                AOI_STOP_RETURN, AOI_STOP_IMPORT, AOI_STOP_YIELD, AOI_STOP_NEWPC };
/* AOI_STOP_NEWPC: the syscall set every register and pc itself (rt_sigreturn);
 * the CPU continues at pc without touching x0. */

struct aoi_cpu {
    uint64_t x[31];
    uint64_t sp, pc;
    int n, z, c, v;
    uint64_t vreg[32][2];       /* SIMD&FP V0-V31, little-endian lo/hi halves */
    uint32_t fpcr, fpsr;
    uint64_t excl_addr;         /* exclusive monitor for ldxr/stxr */
    int excl_valid;
    uint64_t tpidr;             /* guest thread pointer, never the host's */
    struct aoi_mem *mem;
    enum aoi_stop stop;
    uint64_t fault_addr;
    uint32_t fault_insn;
    int exit_code;
    uint64_t steps;

    /* Host calls. A branch into [thunk_base, thunk_base + 4*n) does not execute
     * guest code: slot 0 means "the host's call returned" (AOI_STOP_RETURN); any
     * other slot runs host_call(cpu, slot), puts the result in x0 and returns to
     * x30. This is how imports such as malloc are served by the host. */
    uint64_t thunk_base, thunk_slots;
    uint64_t (*host_call)(struct aoi_cpu *cpu, unsigned slot);
    void *host_ctx;
    const char *stop_name;      /* import name for AOI_STOP_IMPORT */

    /* svc #0 handler; NULL means core/linux.c's minimal one. A Linux process
     * (core/proc.c) installs its full syscall layer here. */
    uint64_t (*syscall)(struct aoi_cpu *cpu);

    /* Optional per-instruction hook (the reference-CPU oracle, core/oracle.c):
     * called with after=0 before and after=1 after each guest instruction.
     * While it is set, wr() logs the stores of the current instruction. */
    void (*trace)(struct aoi_cpu *cpu, int after);
    void *trace_ctx;
    uint64_t wlog_addr[4];
    int wlog_len[4], nwlog;
};

/* Guest memory access. Returns NULL (and sets a fault) if [addr, addr+len) is unmapped. */
uint8_t *aoi_mem_ptr(struct aoi_mem *mem, uint64_t addr, uint64_t len);
uint8_t *aoi_mem_map(struct aoi_mem *mem, uint64_t base, uint64_t size);

/* Runs until the guest exits or stops; at most max_steps instructions (0 = no limit). */
enum aoi_stop aoi_cpu_run(struct aoi_cpu *cpu, uint64_t max_steps);

/* Calls guest function fn(args...) on the current stack and runs to its return.
 * Needs thunk_base set. Returns the stop reason (AOI_STOP_RETURN on success). */
enum aoi_stop aoi_call(struct aoi_cpu *cpu, uint64_t fn, const uint64_t *args, int nargs,
                       uint64_t max_steps);

/* Provided by the syscall layer (core/linux.c). Returns the value for x0, or
 * sets cpu->stop to end the run. */
uint64_t aoi_linux_syscall(struct aoi_cpu *cpu);

#endif
