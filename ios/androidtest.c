/* On-device test of step 1: an unmodified Android program, loaded by Android's
 * own linker64, run by core/proc.c in the interpreter. The guest's stdout and
 * stderr go to a temporary file whose text is logged. Shared by the iOS app and
 * tools/iostest.c. */
#define _POSIX_C_SOURCE 200809L
#include "androidtest.h"
#include "../core/binder.h"
#include "../core/proc.h"

#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <sys/resource.h>
#endif
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void say(aoi_log_fn log, void *ctx, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log(ctx, buf);
}

/* The process's memory now and at its peak, in MB (iOS: phys_footprint, which
 * jetsam judges; elsewhere: peak RSS for both). */
static void memory_mb(double *now, double *peak)
{
#ifdef __APPLE__
    task_vm_info_data_t vi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    memset(&vi, 0, sizeof vi);
    *now = *peak = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS) {
        *now = vi.phys_footprint / 1e6;
        *peak = vi.ledger_phys_footprint_peak / 1e6;
    }
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    *now = *peak = ru.ru_maxrss / 1e3;
#endif
}

int aoi_android_run(const char *root, const char *tmpdir, int argc, const char *const *argv,
                    aoi_log_fn log, void *ctx)
{
    return aoi_android_run_env(root, tmpdir, argc, argv, NULL, log, ctx);
}

/* The six core boot jars: enough for ART to start (see docs/STATUS.md). */
#define CORE_BCP "/apex/com.android.art/javalib/core-oj.jar:/apex/com.android.art/javalib/core-libart.jar:" \
    "/apex/com.android.art/javalib/okhttp.jar:/apex/com.android.art/javalib/bouncycastle.jar:" \
    "/apex/com.android.art/javalib/apache-xml.jar:/apex/com.android.i18n/javalib/core-icu4j.jar"

static int art_run(const char *root, const char *tmpdir, const char *dex, aoi_log_fn log, void *ctx)
{
    static const char *const env[] = { "BOOTCLASSPATH=" CORE_BCP, "DEX2OATBOOTCLASSPATH=" CORE_BCP, NULL };
    const char *const argv[] = { "/apex/com.android.art/bin/dalvikvm64", "-Xverify:none",
                                 "-Ximage:/system/framework/boot.art", "-cp", dex, "Hello", NULL };
    return aoi_android_run_env(root, tmpdir, 6, argv, env, log, ctx);
}

int aoi_android_art_hello(const char *root, const char *tmpdir, aoi_log_fn log, void *ctx)
{
    return art_run(root, tmpdir, "/data/local/tmp/hello.dex", log, ctx);
}

int aoi_android_art_gc(const char *root, const char *tmpdir, aoi_log_fn log, void *ctx)
{
    return art_run(root, tmpdir, "/data/local/tmp/gc.dex", log, ctx);
}

int aoi_android_run_env(const char *root, const char *tmpdir, int argc, const char *const *argv,
                        const char *const *env, aoi_log_fn log, void *ctx)
{
    static const char *const base[] = {
        "PATH=/system/bin", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", "HOME=/",
        "TMPDIR=/data/local/tmp", "ANDROID_ART_ROOT=/apex/com.android.art",
        "ANDROID_I18N_ROOT=/apex/com.android.i18n", "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata", NULL };
    const char *envp[32];
    int ne = 0;
    struct aoi_proc *p = calloc(1, sizeof *p);
    char tmp[1024], line[1024];
    const char *err;
    enum aoi_stop st;
    struct timespec t0, t1;
    double secs;
    int fd, rc = -1;
    FILE *f;

    if (!p) { say(log, ctx, "android: out of memory"); return -1; }
    while (base[ne]) { envp[ne] = base[ne]; ne++; }
    while (env && *env && ne < 31) envp[ne++] = *env++;
    envp[ne] = NULL;
    snprintf(tmp, sizeof tmp, "%s/aoi-out-XXXXXX", tmpdir);
    if ((fd = mkstemp(tmp)) < 0) { say(log, ctx, "android: cannot create %s", tmp); free(p); return -1; }
    if ((err = aoi_proc_exec(p, root, argv[0], argc, argv, (const char *const *)envp))) {
        say(log, ctx, "android: exec %s: %s", argv[0], err);
        aoi_proc_free(p); free(p); close(fd); unlink(tmp);
        return -1;
    }
    p->fd[1].host = fd;                 /* guest stdout/stderr -> the temporary file */
    p->fd[2].host = fd;
    p->log = fdopen(dup(fd), "w");      /* and liblog's lines (logd emulation) */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    st = aoi_proc_run(p, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    if ((f = fdopen(fd, "r"))) {
        rewind(f);
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            if (!strstr(line, "ld.config.txt") && !strstr(line, "cutils-trace"))   /* known, harmless */
                say(log, ctx, "  | %s", line);
        }
        fclose(f);
    } else close(fd);
    unlink(tmp);

    if (st == AOI_STOP_EXIT) {
        rc = p->cpu.exit_code;
        say(log, ctx, "android: exit %d, %llu instructions, %.3f s (%.1f M/s), %llu MiB of host chunks", rc,
            (unsigned long long)p->cpu.steps, secs, secs > 0 ? (double)p->cpu.steps / secs / 1e6 : 0.0,
            (unsigned long long)(p->vm.nchunks * (AOI_VM_CHUNK >> 20)));
    } else {
        char w[256];
        say(log, ctx, "android: stopped (%d) at pc=%#llx in %s, fault %#llx, insn %#x after %llu instructions",
            (int)st, (unsigned long long)p->cpu.pc, aoi_proc_where(p, p->cpu.pc, w, sizeof w),
            (unsigned long long)p->cpu.fault_addr, p->cpu.fault_insn, (unsigned long long)p->cpu.steps);
    }
    {
        double now, peak;
        memory_mb(&now, &peak);
        say(log, ctx, "android: memory %.0f MB now, %.0f MB peak", now, peak);
    }
    if (p->log) fclose(p->log);
    aoi_proc_free(p);
    free(p);
    return rc;
}

/* "-e NAME=VALUE" pairs of the guest's /data/system/environ/classpath. */
static int classpath_env(const char *datadir, char vals[3][4096], const char **env)
{
    char path[1024], line[4096], name[64];
    int n = 0;
    FILE *f;
    snprintf(path, sizeof path, "%s/system/environ/classpath", datadir);
    if (!(f = fopen(path, "r"))) return 0;
    while (n < 3 && fgets(line, sizeof line, f)) {
        char val[4000];
        if (sscanf(line, "export %63s %3999s", name, val) != 2) continue;
        if (strcmp(name, "BOOTCLASSPATH") && strcmp(name, "DEX2OATBOOTCLASSPATH") && strcmp(name, "SYSTEMSERVERCLASSPATH"))
            continue;
        snprintf(vals[n], sizeof vals[n], "%s=%s", name, val);
        env[n] = vals[n];
        n++;
    }
    fclose(f);
    env[n] = NULL;
    return n;
}

static struct aoi_proc *volatile running;   /* the app's process, while aoi_android_app runs */

/* While an app runs: its memory every 10 s (phys_footprint and what it is made of,
 * the guest's host chunks, file pages mapped/copied), to find where it goes. */
struct watch { aoi_log_fn log; void *ctx; struct aoi_proc *p; };
static void *memory_watch(void *arg)
{
    struct watch w = *(struct watch *)arg;
    extern uint64_t aoi_vm_mapped_bytes, aoi_vm_copied_bytes;
    free(arg);
    for (;;) {
        int i;
        for (i = 0; i < 100 && running == w.p; i++) { struct timespec ts = { 0, 100000000 }; nanosleep(&ts, NULL); }
        if (running != w.p) return NULL;
#ifdef __APPLE__
        {
            task_vm_info_data_t vi;
            mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
            memset(&vi, 0, sizeof vi);
            if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS)
                say(w.log, w.ctx, "memory: footprint %.0f MB (internal %.0f, compressed %.0f, external %.0f, resident %.0f), "
                    "guest chunks %llu MB, file pages %llu MB mapped / %llu MB copied",
                    vi.phys_footprint / 1e6, vi.internal / 1e6, vi.compressed / 1e6, vi.external / 1e6,
                    vi.resident_size / 1e6, (unsigned long long)(w.p->vm.nchunks * (AOI_VM_CHUNK >> 20)),
                    (unsigned long long)(aoi_vm_mapped_bytes >> 20), (unsigned long long)(aoi_vm_copied_bytes >> 20));
        }
#else
        (void)aoi_vm_mapped_bytes; (void)aoi_vm_copied_bytes;
#endif
    }
}
static char snap_path[1024];                /* where its snapshot goes */

int aoi_android_snapshot(double timeout)
{
    struct aoi_proc *p = running;
    struct timespec ts = { 0, 20000000 };
    double waited = 0;
    if (!p || !snap_path[0]) return -1;
    snprintf(p->snap_path, sizeof p->snap_path, "%s", snap_path);
    p->snap_request = 1;
    while (p->snap_request && running == p && waited < timeout) { nanosleep(&ts, NULL); waited += 0.02; }
    while (p->snap_path[0] && running == p && waited < timeout) { nanosleep(&ts, NULL); waited += 0.02; }   /* written */
    return running == p && !p->snap_path[0] ? 0 : -1;
}

void aoi_android_touch(int action, float x, float y)
{
    struct aoi_proc *p = running;
    if (p) aoi_proc_touch(p, action, x, y);
}

void aoi_android_stop(void)
{
    struct aoi_proc *p = running;
    if (p) p->stop_request = 1;                         /* aoi_proc_run returns; aoi_android_app ends */
}

void aoi_android_back(void)
{
    struct aoi_proc *p = running;
    if (p) aoi_proc_touch(p, 3, 0, 0);                  /* aoi.Input: the activity's onBackPressed */
}

/* One guest process with the app environment: /data in datadir, output to fd. Its
 * exit code, or -1 (logged). frame: SurfaceFlinger's frames (the app), or NULL. */
/* The launch a snapshot was taken for: the APK and its compiled code. A snapshot is
 * loaded only with the same key next to it (the build is checked inside). */
static void snap_key(const char *datadir, char *key, size_t n)
{
    char a[1024], o[1024];
    struct stat sa, so;
    snprintf(a, sizeof a, "%s/app/apk/base.apk", datadir);
    snprintf(o, sizeof o, "%s/app/apk/oat/arm64/base.odex", datadir);
    if (stat(a, &sa)) memset(&sa, 0, sizeof sa);
    if (stat(o, &so)) memset(&so, 0, sizeof so);
    {
        char d[1024], disp[64] = "";                       /* the display the snapshot was laid out for */
        FILE *f;
        snprintf(d, sizeof d, "%s/local/tmp/aoi.display", datadir);
        if ((f = fopen(d, "r"))) { if (!fgets(disp, sizeof disp, f)) disp[0] = 0; fclose(f); }
        disp[strcspn(disp, "\n")] = 0;
        snprintf(key, n, "apk %lld %lld odex %lld %lld display %s\n", (long long)sa.st_size, (long long)sa.st_mtime,
                 (long long)so.st_size, (long long)so.st_mtime, disp);
    }
}

static int snap_key_ok(const char *snap, const char *key)
{
    char path[1100], got[256] = "";
    FILE *f;
    snprintf(path, sizeof path, "%s.key", snap);
    if (!(f = fopen(path, "r"))) return 0;
    if (!fgets(got, sizeof got, f)) got[0] = 0;
    fclose(f);
    return !strcmp(got, key);
}

static void snap_key_write(const char *snap, const char *key)
{
    char path[1100];
    FILE *f;
    snprintf(path, sizeof path, "%s.key", snap);
    if ((f = fopen(path, "w"))) { fputs(key, f); fclose(f); }
}

static int run_guest(const char *root, const char *datadir, int fd, const char *const *argv, int argc,
                     aoi_frame_fn frame, void (*home)(void *), void *frame_ctx, const char *what, const char *snap,
                     aoi_log_fn log, void *ctx)
{
    static const char *const base[] = {
        "PATH=/system/bin", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", "HOME=/",
        "TMPDIR=/data/local/tmp", "ANDROID_ART_ROOT=/apex/com.android.art",
        "ANDROID_I18N_ROOT=/apex/com.android.i18n", "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
        "CLASSPATH=/data/local/tmp/aoi.dex", NULL };
    static char vals[3][4096];
    const char *cp[4], *envp[16];
    struct aoi_proc *p = calloc(1, sizeof *p);
    const char *err;
    enum aoi_stop st;
    struct timespec t0, t1;
    double secs, now, peak;
    int ne = 0, i, rc = -1, resumed = 0;

    if (!p) { say(log, ctx, "%s: out of memory", what); return -1; }
    if (!classpath_env(datadir, vals, cp)) { say(log, ctx, "%s: no %s/system/environ/classpath", what, datadir); free(p); return -1; }
    while (base[ne]) { envp[ne] = base[ne]; ne++; }
    for (i = 0; cp[i]; i++) envp[ne++] = cp[i];
    envp[ne] = NULL;
    if (snap) {                         /* resume the app where a snapshot left it, if it fits */
        char key[256];
        snap_key(datadir, key, sizeof key);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (!access(snap, R_OK) && snap_key_ok(snap, key)) {
            if (!(err = aoi_snap_load(p, snap, root, datadir))) {
                clock_gettime(CLOCK_MONOTONIC, &t1);
                say(log, ctx, "%s: resumed from its snapshot in %.1f s", what,
                    (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
                resumed = 1;
            } else {
                say(log, ctx, "%s: snapshot not usable (%s), starting afresh", what, err);
                aoi_proc_free(p);
                memset(p, 0, sizeof *p);
            }
        }
        if (!resumed) {                 /* a new one is taken once the app is idle (aoi.Main) */
            unlink(snap);
            snap_key_write(snap, key);
        }
    }
    if (!resumed && (err = aoi_proc_exec(p, root, argv[0], argc, argv, envp))) {
        say(log, ctx, "%s: exec: %s", what, err);
        aoi_proc_free(p); free(p);
        return -1;
    }
    if (snap && !resumed) snprintf(p->snap_path, sizeof p->snap_path, "%s", snap);
    if (snap) snprintf(snap_path, sizeof snap_path, "%s", snap);
    snprintf(p->data, sizeof p->data, "%s", datadir);
    p->uffd = 1;                        /* ART's CMC GC and the boot image */
    p->frame = frame;
    p->home = home;
    p->frame_ctx = frame_ctx;
    p->fd[1].host = fd;
    p->fd[2].host = fd;
    p->log = fdopen(dup(fd), "w");
    if (p->log) setvbuf(p->log, NULL, _IOLBF, 0);
    if (resumed) aoi_sf_redraw(p);      /* the screen it was showing */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (frame) {
        struct watch *w = malloc(sizeof *w);
        pthread_t th;
        running = p;
        if (w) {
            w->log = log; w->ctx = ctx; w->p = p;
            if (!pthread_create(&th, NULL, memory_watch, w)) pthread_detach(th); else free(w);
        }
    }
    st = aoi_proc_run(p, 0);
    running = NULL;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (st == AOI_STOP_EXIT) {
        rc = p->cpu.exit_code;
        say(log, ctx, "%s: exit %d after %.1f s, %llu instructions", what, rc, secs, (unsigned long long)p->cpu.steps);
    } else {
        char w[256];
        say(log, ctx, "%s: stopped (%d) at pc=%#llx in %s, insn %#x after %.1f s, %llu instructions", what, (int)st,
            (unsigned long long)p->cpu.pc, aoi_proc_where(p, p->cpu.pc, w, sizeof w), p->cpu.fault_insn, secs,
            (unsigned long long)p->cpu.steps);
    }
    memory_mb(&now, &peak);
    say(log, ctx, "%s: memory %.0f MB now, %.0f MB peak", what, now, peak);
    if (p->log) fclose(p->log);
    aoi_proc_free(p);
    free(p);
    return rc;
}

/* The APK's code compiled ahead of time (dex2oat, `speed`), once per APK: oat/arm64/
 * next to it, where ART looks. In the interpreter the app's dex code would otherwise
 * be interpreted twice (by ART, inside ours): 63 % of a frame. */
static void compile_apk(const char *root, const char *datadir, int fd, aoi_log_fn log, void *ctx)
{
    static const char *const argv[] = { "/apex/com.android.art/bin/dex2oat64", "--dex-file=/data/app/apk/base.apk",
        "--oat-file=/data/app/apk/oat/arm64/base.odex", "--instruction-set=arm64", "--compiler-filter=speed",
        "--class-loader-context=PCL[]", NULL };
    char odex[1024], dir[1024], apk[1024];
    struct stat so, sa;
    snprintf(apk, sizeof apk, "%s/app/apk/base.apk", datadir);
    snprintf(odex, sizeof odex, "%s/app/apk/oat/arm64/base.odex", datadir);
    if (!stat(odex, &so) && !stat(apk, &sa) && so.st_size > 0 && so.st_mtime >= sa.st_mtime) return;   /* done */
    snprintf(dir, sizeof dir, "%s/app/apk/oat", datadir); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/app/apk/oat/arm64", datadir); mkdir(dir, 0755);
    say(log, ctx, "dex2oat: the app's code is compiled once (a few minutes) ...");
    if (run_guest(root, datadir, fd, argv, 6, NULL, NULL, NULL, "dex2oat", NULL, log, ctx) != 0) unlink(odex);   /* run interpreted */
}

int aoi_android_app(const char *root, const char *datadir, const char *logpath, const char *display,
                    aoi_frame_fn frame, void (*home)(void *), void *frame_ctx, aoi_log_fn log, void *ctx)
{
    static const char *const argv[] = { "/system/bin/app_process64", "/system/bin", "aoi.Main",
                                        "/data/app/apk/base.apk", NULL };
    int fd, rc;
    char snap[1100], path[1100];
    FILE *f;
    snprintf(path, sizeof path, "%s.1", logpath);         /* the last run's log stays (a crash's stack) */
    rename(logpath, path);
    if ((fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) { say(log, ctx, "app: cannot write %s", logpath); return -1; }
    snprintf(path, sizeof path, "%s/local/tmp/aoi.display", datadir);   /* aoi.DisplayManager reads it */
    if (display && (f = fopen(path, "w"))) { fprintf(f, "%s\n", display); fclose(f); }
    snprintf(snap, sizeof snap, "%s.snap", datadir);
    compile_apk(root, datadir, fd, log, ctx);
    rc = run_guest(root, datadir, fd, argv, 4, frame, home, frame_ctx, "app", snap, log, ctx);
    close(fd);
    return rc;
}
