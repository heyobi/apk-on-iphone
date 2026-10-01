/* Runs an unmodified Android program from a guest root (see androidtest.c). */
#ifndef AOI_ANDROIDTEST_H
#define AOI_ANDROIDTEST_H

#include "gmptest.h"

/* argv[0] is the guest path of the program. Its output is logged line by line;
 * returns its exit code, or -1 if it did not exit normally. tmpdir: a writable
 * host directory for the captured output. */
int aoi_android_run(const char *root, const char *tmpdir, int argc, const char *const *argv,
                    aoi_log_fn log, void *ctx);

#endif
