/* Native backend loader: lays out an Android arm64 .so at a host address so the
 * CPU can run it directly (on iOS, in JIT memory). Platform-independent: it only
 * writes into memory the caller provides; making the text executable is the
 * caller's job (ios/jitmem.c). */
#ifndef AOI_NATIVE_H
#define AOI_NATIVE_H

#include <stddef.h>
#include <stdint.h>

/* An import resolver: host address for `name`, or NULL if it is missing. */
typedef void *(*aoi_native_resolve)(const char *name, void *ctx);

struct aoi_native_layout {
    uint64_t span;          /* bytes to reserve (16 KB-page rounded) */
    uint64_t text_end;      /* [0, text_end) holds only executable segments */
    int text_ok;            /* 1 if no writable segment shares those pages */
};

/* Reads the PT_LOAD layout. Returns NULL or an error. */
const char *aoi_native_layout(const void *so, size_t size, struct aoi_native_layout *out);

/* Copies the segments into `buf` (span bytes, writable, zeroed) and applies the
 * RELA relocations as if the image were at `load` (often load == buf; on iOS 26
 * TXM devices the code is written through a separate alias). Imports come from
 * `resolve`. On a missing import, returns an error and points *missing at it. */
const char *aoi_native_link(const void *so, size_t size, uint8_t *buf, uint8_t *load,
                            aoi_native_resolve resolve, void *ctx, const char **missing);

/* Address of an exported symbol in a linked image, NULL if absent. */
void *aoi_native_sym(const void *so, size_t size, uint8_t *base, const char *name);

#endif
