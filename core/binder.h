/* In-process binder driver for core/proc.c (see binder.c). */
#ifndef AOI_BINDER_H
#define AOI_BINDER_H

#include <stdint.h>

struct aoi_proc;

/* An ioctl on a binder fd: 0 or -errno. *block = 1: the calling thread must wait
 * and run the same ioctl again (nothing to read yet). */
uint64_t aoi_binder_ioctl(struct aoi_proc *p, uint64_t cmd, uint64_t arg, int *block);

/* The guest mmapped its binder fd at [addr, addr+len): replies are written there. */
void aoi_binder_mapped(struct aoi_proc *p, uint64_t addr, uint64_t len);

void aoi_binder_free(struct aoi_proc *p);

#endif
