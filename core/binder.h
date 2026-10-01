/* In-process binder driver for core/proc.c (see binder.c), and the native services
 * that live behind its handles (core/sf.c). */
#ifndef AOI_BINDER_H
#define AOI_BINDER_H

#include <stdint.h>

#include "parcel.h"

struct aoi_proc;

/* An ioctl on a binder fd: 0 or -errno. *block = 1: the calling thread must wait
 * and run the same ioctl again (nothing to read yet). */
uint64_t aoi_binder_ioctl(struct aoi_proc *p, uint64_t cmd, uint64_t arg, int *block);

/* The guest mmapped its binder fd at [addr, addr+len): replies are written there. */
void aoi_binder_mapped(struct aoi_proc *p, uint64_t addr, uint64_t len);

void aoi_binder_free(struct aoi_proc *p);

/* A native object: called for each transaction `code` with the request after its
 * interface token; it fills the reply (reply->status starts as UNKNOWN_TRANSACTION:
 * set it to 0 for a normal reply). */
typedef void (*aoi_native_fn)(struct aoi_proc *p, void *self, uint32_t code, struct aoi_reader *req,
                              struct aoi_parcel *reply);

/* Creates one; with a name it is also registered with servicemanager. Its handle,
 * or 0 if the table is full. */
uint32_t aoi_binder_native(struct aoi_proc *p, const char *name, const char *iface, aoi_native_fn fn, void *self);

/* SurfaceFlinger (core/sf.c): registered when binder starts; tick() sends due vsync events. */
void aoi_sf_init(struct aoi_proc *p);
void aoi_sf_tick(struct aoi_proc *p);
void aoi_sf_free(struct aoi_proc *p);

/* gralloc (core/gralloc.c): the allocator service, and the buffers it handed out. */
struct aoi_gbuf {
    int used, refs;
    uint32_t id, width, height, stride, size;   /* stride in pixels; size: pixel bytes */
    int32_t format, bpp;
    uint64_t usage, addr, len;                  /* guest memory: pixels, then the metadata page */
};
void aoi_gralloc_init(struct aoi_proc *p);
void aoi_gralloc_free(struct aoi_proc *p);
struct aoi_gbuf *aoi_gralloc_find(struct aoi_proc *p, uint32_t id);
void aoi_gralloc_retain(struct aoi_proc *p, uint32_t id);
void aoi_gralloc_release(struct aoi_proc *p, uint32_t id);
uint64_t aoi_gralloc_syscall(struct aoi_proc *p, uint64_t op, uint64_t id);    /* AOI_SYS_GRALLOC */

#endif
