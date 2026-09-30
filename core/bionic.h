/* The bionic shim: host implementations of the libc functions Android
 * libraries import. Grown one function at a time as real libraries need them;
 * anything absent stops the CPU with the function's name (AOI_STOP_IMPORT). */
#ifndef AOI_BIONIC_H
#define AOI_BIONIC_H

#include "dl.h"

/* Initializes dl with the shim's host table (data symbols such as stdout get
 * guest storage here). */
const char *aoi_bionic_init(struct aoi_dl *dl);

#endif
