/* Native stand-ins for hot guest functions (core/hle.c). */
#ifndef AOI_HLE_H
#define AOI_HLE_H

#include "cpu.h"

/* A br/blr to target in [hle_lo, hle_hi): runs the stand-ins from there while they
 * apply (c->pc ends at the first guest code to interpret). 1 if any ran, else 0 and
 * the branch is taken as usual. */
int aoi_hle_run(struct aoi_cpu *c, uint64_t target);

/* Set when the guest has a GPU (core/gpu.h): libhwui's HardwareRenderer then gets
 * its surfaces, and the no-GPU stand-ins (no_gpu in core/hle.c) stay out. */
extern int aoi_hle_gpu;

/* Host debugging (AOI_WATCH_LIB=name.so): calls into that library from outside it are
 * logged (offset, caller) instead of libhwui's stand-ins running. */
extern int aoi_hle_watch;

/* libhwui.so is mapped with load bias base: its stand-ins apply to this CPU. */
void aoi_hle_attach(struct aoi_cpu *c, uint64_t base);

#endif
