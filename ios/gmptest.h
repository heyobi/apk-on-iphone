/* On-device test: the real libgmp.so from an APK, run by the interpreter, with
 * timings. Shared by the iOS app and tools/iostest.c. */
#ifndef AOI_GMPTEST_H
#define AOI_GMPTEST_H

#include <stddef.h>

typedef void (*aoi_log_fn)(void *ctx, const char *line);

/* n! with the interpreter. Returns a malloc'ed decimal string or NULL; *secs = time. */
char *aoi_gmp_interp(const void *so, size_t size, unsigned long n, double *secs, aoi_log_fn log, void *ctx);

#endif
