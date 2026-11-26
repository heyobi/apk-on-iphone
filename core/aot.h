/* Ahead-of-time translated guest libraries (core/aot.c, tools/aotgen.c).
 *
 * tools/aotgen.c reads a guest shared library (an exact build: its GNU build-id) and writes
 * C: per 4 KiB page of its code, one function that runs the guest's blocks from a block
 * start until it leaves the page, branches back to the interpreter's subset, faults or uses
 * up its budget - the accelerator's (core/jit.c) contract, compiled with the app instead of
 * by a JavaScript engine. When a process maps that library, the cpu runs those functions
 * at block starts inside it; anything else (another build, an instruction outside the
 * subset) is interpreted as before. */
#ifndef AOI_AOT_H
#define AOI_AOT_H

#include <stdint.h>

struct aoi_cpu;
struct aoi_proc;

/* Runs from cpu->pc (a guest address in the page), leaves cpu->pc at the next instruction
 * to run (or at a faulting one, cpu->stop set) and returns how many it ran; 0: not a block
 * start it knows (the interpreter goes on). The load bias is cpu->aot_bias. */
typedef int (*aoi_aot_fn)(struct aoi_cpu *c);

struct aoi_aot_lib {
    const char *name;             /* "libsolv.so.1" (information) */
    uint8_t build_id[20];         /* NT_GNU_BUILD_ID of the file it was made from */
    int build_id_len;
    uint64_t text_lo, text_hi;    /* translated virtual addresses (page-aligned) */
    const aoi_aot_fn *pages;      /* one per page from text_lo; NULL: none */
};

/* The libraries this build carries (generated tables; empty without them). */
extern const struct aoi_aot_lib *const aoi_aot_libs[];

/* A process mapped a file executable at guest address a (len bytes from offset off; fd:
 * the host file): if a carried library's build matches, its code runs translated. */
void aoi_aot_mapped(struct aoi_proc *p, uint64_t a, uint64_t len, uint64_t off, int fd);

/* Calls into translated code, and instructions they ran (LXRUN_STATS). */
extern uint64_t aoi_aot_runs, aoi_aot_insns;

#endif
