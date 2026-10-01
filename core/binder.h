/* In-process binder driver for core/proc.c (see binder.c), and the native services
 * that live behind its handles (core/sf.c). */
#ifndef AOI_BINDER_H
#define AOI_BINDER_H

#include <stdint.h>
#include <stdio.h>

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

/* A one-way call into a local object of the guest (its flat_binder_object's binder
 * and cookie), e.g. SurfaceFlinger releasing a buffer: 0, or -1 if it cannot be sent. */
int aoi_binder_send(struct aoi_proc *p, uint64_t ptr, uint64_t cookie, uint32_t code, const struct aoi_parcel *data);

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

/* Snapshots (core/snap.c): save = 1 writes the state to f, 0 reads it back (after
 * guest memory and fds). 0, or -1. The native objects of sf and gralloc are named
 * by (kind, index) across a snapshot: *_native_id finds one, *_native_ref resolves it. */
int aoi_binder_snap(struct aoi_proc *p, FILE *f, int save);
int aoi_sf_snap(struct aoi_proc *p, FILE *f, int save);
int aoi_gralloc_snap(struct aoi_proc *p, FILE *f, int save);
int aoi_sf_native_id(struct aoi_proc *p, void *self, aoi_native_fn fn, int32_t *kind, int32_t *idx);
int aoi_sf_native_ref(struct aoi_proc *p, int32_t kind, int32_t idx, aoi_native_fn *fn, void **self, const char **iface);
int aoi_gralloc_native_id(struct aoi_proc *p, void *self, aoi_native_fn fn, int32_t *kind, int32_t *idx);
int aoi_gralloc_native_ref(struct aoi_proc *p, int32_t kind, int32_t idx, aoi_native_fn *fn, void **self, const char **iface);
void aoi_sf_redraw(struct aoi_proc *p);     /* the frame on screen to p->frame again */

#endif
