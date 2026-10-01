/* On-device tests: the real libgmp.so from an APK, run by the interpreter and
 * natively (JIT memory), with timings. Shared by the iOS app and tools/iostest.c. */
#ifndef AOI_GMPTEST_H
#define AOI_GMPTEST_H

#include <stddef.h>

typedef void (*aoi_log_fn)(void *ctx, const char *line);

/* n! with the interpreter. Returns a malloc'ed decimal string or NULL; *secs = time. */
char *aoi_gmp_interp(const void *so, size_t size, unsigned long n, double *secs, aoi_log_fn log, void *ctx);

/* Runs a 2-instruction function from JIT memory with this strategy (1 = ok). */
int aoi_jit_probe(int strategy, aoi_log_fn log, void *ctx);

/* n! natively: libgmp.so linked into JIT memory, imports bound to the host libc.
 * With execute = 0 only loads and links (host builds). */
char *aoi_gmp_native(const void *so, size_t size, int strategy, unsigned long n, int execute,
                     double *secs, aoi_log_fn log, void *ctx);

#endif
