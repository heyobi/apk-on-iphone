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

/* A whole app: app_process64 runs aoi.Main (our framework services, java/src) on the
 * APK at guest path /data/app/apk/base.apk. The root (bundle) is read-only; datadir is
 * the writable host directory that is the guest's /data (a copy of the root's data/
 * plus the APK). Every frame SurfaceFlinger shows goes to frame(). The app's
 * logcat and output go to logpath. Returns when the app's process ends. A snapshot
 * of the started app (aoi_android_snapshot) is loaded instead of starting it again
 * when it was taken for the same build, APK and compiled code. */
typedef void (*aoi_frame_fn)(void *ctx, const unsigned char *rgbx, unsigned width, unsigned height);
int aoi_android_app(const char *root, const char *datadir, const char *logpath, aoi_frame_fn frame,
                    void *frame_ctx, aoi_log_fn log, void *ctx);

/* A touch for the running app (from any thread): action 0 down, 1 up, 2 move, 4
 * cancel, at (x, y) in its screen pixels (the frames' size). Ignored when no app runs. */
void aoi_android_touch(int action, float x, float y);

/* Android's back for the running app (its resumed activity's onBackPressed). */
void aoi_android_back(void);

/* The app is saved as it is now (a snapshot next to datadir, "<datadir>.snap"), so the
 * next launch resumes it in seconds instead of starting it again; the first one is
 * taken by itself once the app has started. Waits up to timeout s: 0 when written. */
int aoi_android_snapshot(double timeout);

#endif
