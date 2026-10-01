/* Runs an unmodified Android program from a guest root (see androidtest.c). */
#ifndef AOI_ANDROIDTEST_H
#define AOI_ANDROIDTEST_H

#include "gmptest.h"

/* argv[0] is the guest path of the program. Its output is logged line by line;
 * returns its exit code, or -1 if it did not exit normally. tmpdir: a writable
 * host directory for the captured output. */
int aoi_android_run(const char *root, const char *tmpdir, int argc, const char *const *argv,
                    aoi_log_fn log, void *ctx);

/* The same with extra NAME=VALUE environment entries (NULL-terminated). */
int aoi_android_run_env(const char *root, const char *tmpdir, int argc, const char *const *argv,
                        const char *const *env, aoi_log_fn log, void *ctx);

/* ART: dalvikvm64 runs the bundled /data/local/tmp/hello.dex (prints a greeting). */
int aoi_android_art_hello(const char *root, const char *tmpdir, aoi_log_fn log, void *ctx);

/* ART: /data/local/tmp/gc.dex allocates 20 MB of garbage, then Runtime.gc(). */
int aoi_android_art_gc(const char *root, const char *tmpdir, aoi_log_fn log, void *ctx);

#endif
