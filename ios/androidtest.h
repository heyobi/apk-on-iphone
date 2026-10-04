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
 * when it was taken for the same build, APK, compiled code and display. display is
 * "width height dpi" (aoi.DisplayManager); home() is called when the app leaves for the
 * launcher (back on its root activity). */
typedef void (*aoi_frame_fn)(void *ctx, const unsigned char *rgbx, unsigned width, unsigned height);
int aoi_android_app(const char *root, const char *datadir, const char *logpath, const char *display,
                    aoi_frame_fn frame, void (*home)(void *), void *frame_ctx, aoi_log_fn log, void *ctx);

/* Android started before its app is chosen (the iOS app does it when it opens, and
 * whenever no app runs): the runtime and the app-independent services come up with
 * warmdir as /data (a /data like an app's, without an APK), then the process waits.
 * The first time it is saved (warmdir.snap), so later warm starts resume in a second.
 * aoi_android_go makes it the app's; this returns when that process ends: the app's
 * exit code, -2 if it ended without an app (aoi_android_stop), or -3 if it ended after
 * aoi_android_go handed it one but before it took it (the app did not start). */
int aoi_android_warm(const char *root, const char *warmdir, const char *display, aoi_log_fn log, void *ctx);

/* Ends the warm process while it has no app (before it began too: aoi_android_warm
 * then returns -2 at once). An app it became is not touched (aoi_android_stop). */
void aoi_android_warm_stop(void);

/* Hands the warm process an app, with aoi_android_app's arguments: 0 taken (it opens
 * where Android already is; its snapshot, if any, is not used), -1 none waits (or for
 * another display): start it with aoi_android_app. */
int aoi_android_go(const char *root, const char *datadir, const char *logpath, const char *display,
                   aoi_frame_fn frame, void (*home)(void *), void *frame_ctx, aoi_log_fn log, void *ctx);

/* The app started without a screen, to be saved (its snapshot) and ended: the iOS app
 * does it after a compile, so the first tap resumes it. 1 saved, -1 not (the app does
 * not let itself be saved: a GL game, a WebView), -3 not this time (it did not get to
 * its save in 4 min, or could not start: worth another try); if aoi_android_show made
 * it the shown app, its exit code when it ends. */
int aoi_android_app_hidden(const char *root, const char *datadir, const char *logpath, const char *display,
                           aoi_log_fn log, void *ctx);

/* Whether the app has a snapshot made for its APK, compiled code, display and this
 * build; when its last one is gone or does not fit, the clean one saved after its
 * compile (<datadir>.snap0) is put back first. */
int aoi_android_snapshot_fits(const char *datadir);

/* The app aoi_android_app_hidden runs goes to this screen and stays (0), or -1. */
int aoi_android_show(aoi_frame_fn frame, void (*home)(void *), void *frame_ctx);

/* Ends the running app's process (after aoi_android_snapshot, it resumes from there);
 * an app still starting stops as soon as it begins. */
void aoi_android_stop(void);

/* Saves the running app (aoi_android_snapshot) and ends that same process: never the
 * next app, should this one end by itself meanwhile. */
void aoi_android_save_stop(double timeout);

/* Whether the last app ended by itself with an error (not stopped, not exit 0): 1 and
 * how ("exit 1 after 12 s", "stopped (3) in libfoo.so+0x1234 after 5 s"), else 0. */
int aoi_android_last_end(char *why, size_t n);

/* The running app's screen is sent again (the iOS screen was hidden and drops frames meanwhile). */
void aoi_android_redraw(void);

/* The sound output starts again after iOS interrupted it (a call, another app's audio session). */
void aoi_android_audio_kick(void);

/* A touch for the running app (from any thread): action 0 down, 1 up, 2 move, 4
 * cancel, at (x, y) in its screen pixels (the frames' size). Ignored when no app runs. */
void aoi_android_touch(int action, float x, float y);

/* Android's back for the running app (its resumed activity's onBackPressed). */
void aoi_android_back(void);

/* The host's clipboard for apps (core/proc.h, clip): op 's' set it from the UTF-8 text
 * in the file at path, 'g' write its text there (remove the file: none), 'h' write
 * "1"/"0" there (has text), 'u' open the URL in the file. Called on the app's thread. */
void aoi_android_set_clipboard(void (*fn)(int op, const char *path));

/* The host's keyboard for apps (core/proc.h, ime): fn(1) when a text field asks for
 * it, fn(0) when the app hides it. Called on the app's thread. */
void aoi_android_set_keyboard(void (*fn)(int show));

/* Typing on the host's keyboard, for the running app's text field: action 6 types
 * the Unicode character `value`, 7 is backspace, 8 return, 9 the keyboard was closed. */
void aoi_android_key(int action, int value);

/* The app is saved as it is now (a snapshot next to datadir, "<datadir>.snap"), so the
 * next launch resumes it in seconds instead of starting it again; the first one is
 * taken by itself once the app has started. Waits up to timeout s: 0 when written. */
int aoi_android_snapshot(double timeout);

/* The app's code compiled by dex2oat (androidtest.c), once per APK, in a thread of its
 * own: the iOS app starts it at install or from the app's card; a launch never does.
 * 0 started, 1 nothing to do (compiled already), -1 another one runs, -2 too little
 * memory next to the running app, -3 too little free space on the device. faster: an app compiled `verify` (more than 16 MB of
 * dex) gets its hot code compiled, from its profile. Its output goes to logpath. */
int aoi_android_compile(const char *root, const char *datadir, const char *logpath, int faster,
                        aoi_log_fn log, void *ctx);

/* How long it would take on the phone, in seconds (a rough figure, from the dex size). */
double aoi_android_compile_estimate(const char *datadir, int faster);

/* Whether one runs. */
int aoi_android_compiling(void);

/* Ends the one that runs (the app keeps what it had; its card offers it again). */
void aoi_android_compile_cancel(void);

/* The one that runs: for which app (datadir), its filter, progress 0..0.99, the seconds
 * it has run and those it likely still takes (-1: not known yet, -2: nearly done), held (the phone is hot
 * or in Low Power Mode). active 0: none runs. */
struct aoi_compile_info {
    int active, held;
    char datadir[1024], filter[16];
    double progress, elapsed, eta;
};
void aoi_android_compile_info(struct aoi_compile_info *ci);

/* What the last finished run left for this APK: "speed", "verify", "speed-profile",
 * "failed", or "" (never compiled, or cut short). */
void aoi_android_compile_state(const char *datadir, char *out, size_t n);

/* Removes the compiled code (the app runs uncompiled; it may be compiled again). */
void aoi_android_compile_remove(const char *datadir);

/* Called (on dex2oat's thread) when a run ends, with its state as above. */
void aoi_android_set_compile_done(void (*fn)(const char *datadir, const char *state));

/* Hold (1) or let go on (0) the background dex2oat: the iOS app calls it when the phone
 * gets hot (thermal state serious or critical) or enters Low Power Mode, and back. */
void aoi_android_compile_hold(int hold);

#endif
