/* On-device test of step 1: an unmodified Android program, loaded by Android's
 * own linker64, run by core/proc.c in the interpreter. The guest's stdout and
 * stderr go to a temporary file whose text is logged. Shared by the iOS app and
 * tools/iostest.c. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE                 /* mincore */
#define _DARWIN_C_SOURCE
#include "androidtest.h"
#include "../core/apk.h"
#include "../core/binder.h"
#include "../core/proc.h"
#ifdef AOI_GPU
#include "../core/hle.h"
#include "../gpu/host.h"
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <os/proc.h>
#include <pthread/qos.h>
#else
#include <sys/resource.h>
#endif
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void say(aoi_log_fn log, void *ctx, const char *fmt, ...)
{
    char buf[4096];
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
        "ANDROID_I18N_ROOT=/apex/com.android.i18n", "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
        "ANDROID_NO_USE_FWMARK_CLIENT=1", NULL };            /* no netd fwmarkd: sockets untagged */
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
#ifdef AOI_GPU
    aoi_gpu_end();
#endif
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

static void (*clipboard_fn)(int op, const char *path);
void aoi_android_set_clipboard(void (*fn)(int op, const char *path)) { clipboard_fn = fn; }
static void clipboard(void *ctx, int op, const char *path) { (void)ctx; if (clipboard_fn) clipboard_fn(op, path); }
static void (*keyboard_fn)(int show);
void aoi_android_set_keyboard(void (*fn)(int show)) { keyboard_fn = fn; }
static void keyboard(void *ctx, int show) { (void)ctx; if (keyboard_fn) keyboard_fn(show); }

/* Where the guest's host memory is: the host pages of its chunks that exist (resident,
 * or compressed: MINCORE_PAGED_OUT on Apple), in total and for the 64 MB guest windows
 * holding the most, each named by the largest mapping in it (file pages count too:
 * they are "external" in the footprint; chunks with no file mapped are anonymous). */
#define WIN_SHIFT 26
static void guest_breakdown(struct aoi_proc *p, char *out, size_t outn)
{
    static unsigned char vec[AOI_VM_CHUNK / 4096];
    static uint32_t win[1 << 12];                    /* host pages per window */
    long hp = sysconf(_SC_PAGESIZE);
    uint64_t nci = p->vm.size >> AOI_VM_CHUNK_SHIFT, ci, total = 0, anon = 0, mapped = 0, i;
    uint64_t nwin = (p->vm.size >> WIN_SHIFT) < (1 << 12) ? (p->vm.size >> WIN_SHIFT) : (1 << 12);
    uint64_t bits[8] = { 0 };
    size_t o;
    int k;
    if (hp <= 0 || hp > (long)AOI_VM_CHUNK) { snprintf(out, outn, "?"); return; }
    memset(win, 0, sizeof win);
    for (ci = 0; ci < nci; ci++) {
        uint8_t *c = p->vm.chunk[ci];
        uint64_t n = 0, j;
        if (!c || mincore((void *)c, AOI_VM_CHUNK, (void *)vec)) continue;
        for (j = 0; j < AOI_VM_CHUNK / (uint64_t)hp; j++) {
            int b;
            if (vec[j] & AOI_MINCORE_EXISTS) n++;
            for (b = 0; b < 8; b++) if (vec[j] >> b & 1) bits[b]++;
        }
        total += n;
        if (!p->vm.filemap[ci]) anon += n;
        if ((ci << AOI_VM_CHUNK_SHIFT >> WIN_SHIFT) < nwin) win[ci << AOI_VM_CHUNK_SHIFT >> WIN_SHIFT] += (uint32_t)n;
    }
    for (i = 0; i < p->vm.size / AOI_VM_PAGE; i++) if (p->vm.prot[i]) mapped++;
    o = (size_t)snprintf(out, outn, "guest in host memory %llu MB (%llu MB in chunks without files) of %llu MB mapped;",
                         (unsigned long long)(total * (uint64_t)hp >> 20), (unsigned long long)(anon * (uint64_t)hp >> 20),
                         (unsigned long long)(mapped * AOI_VM_PAGE >> 20));
    for (k = 0; k < (getenv("AOI_MEM_WINDOWS") ? atoi(getenv("AOI_MEM_WINDOWS")) : 6) && o < outn; k++) {   /* the biggest windows */
        uint64_t best = 0, w, bl = 0;
        const char *name = "anonymous";
        for (w = 1; w < nwin; w++) if (win[w] > win[best]) best = w;
        if (!win[best]) break;
        for (i = 0; i < (uint64_t)p->nmaps; i++) {
            const struct aoi_proc_map *m = &p->maps[i];
            uint64_t ws = best << WIN_SHIFT, we = ws + (1ULL << WIN_SHIFT);
            if (m->start < we && m->start + m->len > ws && m->len > bl) { bl = m->len; name = m->path; }
        }
        o += (size_t)snprintf(out + o, outn - o, " %#llx: %llu MB (%s);", (unsigned long long)(best << WIN_SHIFT),
                              (unsigned long long)((uint64_t)win[best] * (uint64_t)hp >> 20), name);
        win[best] = 0;
    }
    if (o < outn)                                   /* mincore's bits over all chunk pages, in MB */
        snprintf(out + o, outn - o, " mincore bits 1/2/4/8/10/20/40/80: %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu MB",
                 (unsigned long long)(bits[0] * (uint64_t)hp >> 20), (unsigned long long)(bits[1] * (uint64_t)hp >> 20),
                 (unsigned long long)(bits[2] * (uint64_t)hp >> 20), (unsigned long long)(bits[3] * (uint64_t)hp >> 20),
                 (unsigned long long)(bits[4] * (uint64_t)hp >> 20), (unsigned long long)(bits[5] * (uint64_t)hp >> 20),
                 (unsigned long long)(bits[6] * (uint64_t)hp >> 20), (unsigned long long)(bits[7] * (uint64_t)hp >> 20));
}

/* While an app runs: its memory every 10 s (phys_footprint and what it is made of,
 * the guest's host chunks, file pages mapped/copied), to find where it goes. */
struct watch { aoi_log_fn log; void *ctx; struct aoi_proc *p; };
static void compile_cut(void);

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
        if (os_proc_available_memory() < (size_t)200 << 20) compile_cut(); /* iOS would end the app itself */
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
        (void)aoi_vm_mapped_bytes; (void)aoi_vm_copied_bytes; (void)compile_cut;
#endif
        {
            char b[4000];
#ifdef __APPLE__
            malloc_statistics_t ms;
            memset(&ms, 0, sizeof ms);
            malloc_zone_statistics(NULL, &ms);
            say(w.log, w.ctx, "memory: malloc %.0f MB in use", ms.size_in_use / 1e6);
#endif
            guest_breakdown(w.p, b, sizeof b);
            say(w.log, w.ctx, "memory: %s", b);
        }
    }
}
static char snap_path[1024];                /* where its snapshot goes */

/* The guest's OpenGL ES on this device's GPU (gpu/host.c: ANGLE on Metal): apps then
 * draw with HWUI's GPU pipeline (AOI_HWUI, aoi.Main). */
static int gpu_on(void)
{
#ifdef AOI_GPU
    static int on = -1;
    if (on < 0) on = aoi_gpu_available() && !getenv("AOI_NO_GPU");
    return on;
#else
    return 0;
#endif
}

int aoi_android_snapshot(double timeout)
{
    struct aoi_proc *p = running;
    struct timespec ts = { 0, 20000000 };
    double waited = 0;
    if (!p || !snap_path[0]) return -1;
    snprintf(p->snap_path, sizeof p->snap_path, "%s", snap_path);
    if (p->gpu_live) aoi_proc_touch(p, 5, 0, 0);       /* aoi.Snapshot: the GPU's state goes first */
    else p->snap_request = 1;
    while (p->snap_request && running == p && waited < timeout) { nanosleep(&ts, NULL); waited += 0.02; }
    while (p->snap_path[0] && running == p && waited < timeout) { nanosleep(&ts, NULL); waited += 0.02; }   /* written */
    return running == p && !p->snap_path[0] ? 0 : -1;
}

void aoi_android_touch(int action, float x, float y)
{
    struct aoi_proc *p = running;
    if (p) aoi_proc_touch(p, action, x, y);
}

void aoi_android_key(int action, int value)
{
    struct aoi_proc *p = running;
    if (p) aoi_proc_key(p, action, value);
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
#define APP_UID 10100                      /* the app's uid (aoi.Main's ApplicationInfo.uid): not root, as WebView wants */

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
        snprintf(key, n, "apk %lld %lld odex %lld %lld display %s gpu %d uid %d\n", (long long)sa.st_size, (long long)sa.st_mtime,
                 (long long)so.st_size, (long long)so.st_mtime, disp, gpu_on(), APP_UID);
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

static void compile_attach(struct aoi_proc *p);

static int run_guest(const char *root, const char *datadir, int fd, const char *const *argv, int argc,
                     aoi_frame_fn frame, void (*home)(void *), void *frame_ctx, const char *what, const char *snap,
                     aoi_log_fn log, void *ctx)
{
    static const char *const base[] = {
        "PATH=/system/bin", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", "HOME=/",
        "TMPDIR=/data/local/tmp", "ANDROID_ART_ROOT=/apex/com.android.art",
        "ANDROID_I18N_ROOT=/apex/com.android.i18n", "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
        "CLASSPATH=/data/local/tmp/aoi.dex",
        "ANDROID_NO_USE_FWMARK_CLIENT=1", NULL };            /* no netd fwmarkd: sockets untagged (core/proc.c) */
    static char vals[3][4096];
    const char *cp[4], *envp[24];
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
    if (frame && gpu_on()) envp[ne++] = "AOI_HWUI=1";
    envp[ne] = NULL;
#ifdef AOI_GPU
    if (frame) aoi_hle_gpu = gpu_on();  /* libhwui's own GPU code, not the stand-ins (core/hle.c) */
#endif
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
    p->uid = frame ? APP_UID : 0;       /* dex2oat and the tests: root, as installd's */
    p->uffd = 1;                        /* ART's CMC GC and the boot image */
    p->frame = frame;
    p->home = home;
    p->clip = clipboard;
    p->ime = keyboard;
    p->frame_ctx = frame_ctx;
#ifdef AOI_GPU
    if (gpu_on()) p->gpu = aoi_gpu_call;
#endif
    p->fd[1].host = fd;
    p->fd[2].host = fd;
    p->log = fdopen(dup(fd), "w");
    if (p->log) setvbuf(p->log, NULL, _IOLBF, 0);
    if (frame && getenv("AOI_APP_TRACE") && (p->trace = fopen(getenv("AOI_APP_TRACE"), "w")))   /* every syscall (host tests) */
        setvbuf(p->trace, NULL, _IOLBF, 0);
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
    if (!strcmp(what, "dex2oat")) compile_attach(p);
    st = aoi_proc_run(p, 0);
    if (!strcmp(what, "dex2oat")) compile_attach(NULL);
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
        aoi_proc_log_threads(p);                                   /* where each thread was: the log */
    }
    memory_mb(&now, &peak);
    say(log, ctx, "%s: memory %.0f MB now, %.0f MB peak", what, now, peak);
    if (p->log) fclose(p->log);
#ifdef AOI_GPU
    if (frame) aoi_gpu_end();           /* the next app must not see this one's contexts or strings (not dex2oat's end) */
#endif
    aoi_proc_free(p);
    free(p);
    return rc;
}

/* The APK's compressed native libraries into app/apk/lib/arm64/ (on nativeloader's
 * library path), once per APK, as the package manager does at install: the linker
 * loads only stored ones from inside the APK. */
static void install_libs(const char *datadir, aoi_log_fn log, void *ctx)
{
    char apk[1024], dir[1024], stamp[1100];
    struct stat ss, sa;
    const char *err = NULL;
    void *z;
    int fd, n;
    snprintf(apk, sizeof apk, "%s/app/apk/base.apk", datadir);
    snprintf(dir, sizeof dir, "%s/app/apk/lib", datadir); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/app/apk/lib/arm64", datadir); mkdir(dir, 0755);
    snprintf(stamp, sizeof stamp, "%s/.installed", dir);
    if (stat(apk, &sa) || (!stat(stamp, &ss) && ss.st_mtime >= sa.st_mtime)) return;   /* done */
    if ((fd = open(apk, O_RDONLY)) < 0) return;
    z = mmap(NULL, (size_t)sa.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (z == MAP_FAILED) return;
    n = aoi_apk_extract_libs(z, (size_t)sa.st_size, dir, &err);
    munmap(z, (size_t)sa.st_size);
    if (n < 0) { say(log, ctx, "app: native libraries: %s", err); return; }
    if (n) say(log, ctx, "app: %d native libraries extracted", n);
    if ((fd = open(stamp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) close(fd);
}

/* The app's profile (ProfileInstaller's from the APK's baseline profile, ART's own
 * from the methods its JIT found hot): data/misc/profiles/cur/0/<package>/primary.prof,
 * aoi.Main makes the directory. Its guest path in `guest`, or 0. */
static int app_profile(const char *datadir, char *guest, size_t n)
{
    char dir[1024], path[1300];
    struct dirent *e;
    struct stat st;
    DIR *d;
    int found = 0;
    snprintf(dir, sizeof dir, "%s/misc/profiles/cur/0", datadir);
    if (!(d = opendir(dir))) return 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s/primary.prof", dir, e->d_name);
        if (!stat(path, &st) && st.st_size > 16) {
            snprintf(guest, n, "/data/misc/profiles/cur/0/%s/primary.prof", e->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

/* The APK's code compiled ahead of time by dex2oat, into oat/arm64/ next to it, where
 * ART looks. In the interpreter the app's dex code would otherwise be interpreted twice
 * (by ART, inside ours): 63 % of a frame.
 *   - up to 16 MB of dex: everything (`speed`), once: Qalculate 100 s on the phone.
 *   - more (Molly 59 MB: 41 min, 750 MB with `speed`): first `verify` (WhatsApp, 86 MB:
 *     249 s), then, once the app has a profile, `speed-profile`: its hot code (5 min).
 * Nobody waits minutes for an app to open: dex2oat runs in a thread of its own while the
 * app starts at once without it (ART runs the dex from the APK). It writes oat.new/arm64,
 * which then takes oat/arm64's place, so a launch never sees a half-written odex; the
 * next launch (the snapshot's key has the odex in it) starts with the compiled code.
 * oat/.state holds the filter of the last run that finished for this APK (speed, verify,
 * speed-profile, failed); a run cut short (iOS ended the app) leaves none: run again. */
struct compile_job {
    char root[1024], datadir[1024], filter[16], prof[400];
    int fd;
    aoi_log_fn log;
    void *ctx;
};

static pthread_mutex_t compile_lock = PTHREAD_MUTEX_INITIALIZER;
static int compiling;                       /* one dex2oat at a time */
static struct aoi_proc *compile_proc;       /* its process while it runs */
static int compile_hold;                    /* aoi_android_compile_hold: the phone is hot, Low Power Mode */
static int compile_stopped;                 /* compile_cut ended it: memory was short */
static aoi_log_fn compile_log;
static void *compile_ctx;

static void compile_attach(struct aoi_proc *p)
{
    pthread_mutex_lock(&compile_lock);
    compile_proc = p;
    if (p) p->pause_request = compile_hold;
    pthread_mutex_unlock(&compile_lock);
}

void aoi_android_compile_hold(int hold)
{
    int changed;
    pthread_mutex_lock(&compile_lock);
    changed = compile_hold != !!hold;
    compile_hold = !!hold;
    if (compile_proc) compile_proc->pause_request = compile_hold;
    else changed = 0;
    pthread_mutex_unlock(&compile_lock);
    if (changed) say(compile_log, compile_ctx, hold ? "dex2oat: paused (the phone is hot or in Low Power Mode)"
                                                    : "dex2oat: going on");
}

/* Memory is short with the app and dex2oat both running: dex2oat ends (its memory goes
 * back), the app keeps running, and a later launch compiles again. */
static void compile_cut(void)
{
    pthread_mutex_lock(&compile_lock);
    if (compile_proc && !compile_proc->stop_request) {
        compile_proc->stop_request = 1;
        compile_stopped = 1;
    }
    pthread_mutex_unlock(&compile_lock);
}

static void remove_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[1300];
    if (d) {
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            unlink(path);
        }
        closedir(d);
    }
    rmdir(dir);
}

static void *compile_thread(void *arg)
{
    struct compile_job *j = arg;
    const char *argv[10] = { "/apex/com.android.art/bin/dex2oat64", "--dex-file=/data/app/apk/base.apk",
        "--oat-file=/data/app/apk/oat.new/arm64/base.odex", "--instruction-set=arm64", NULL,
        "--class-loader-context=PCL[]", "--no-watch-dog", NULL, NULL };   /* its 9.5 min limit: big apps take longer */
    char dir[1100], newdir[1100], olddir[1100], state[1100], profarg[450], fa[64];
    const char *filter = j->filter;
    int argc = 7, rc;
    FILE *f;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);   /* behind the app's threads, on the cooler cores */
#endif
    snprintf(fa, sizeof fa, "--compiler-filter=%s", j->filter);
    argv[4] = fa;
    if (j->prof[0]) { snprintf(profarg, sizeof profarg, "--profile-file=%s", j->prof); argv[argc++] = profarg; }
    snprintf(dir, sizeof dir, "%s/app/apk/oat", j->datadir); mkdir(dir, 0755);
    snprintf(newdir, sizeof newdir, "%s/app/apk/oat.new", j->datadir);
    remove_dir(newdir);
    mkdir(newdir, 0755);
    snprintf(newdir, sizeof newdir, "%s/app/apk/oat.new/arm64", j->datadir); mkdir(newdir, 0755);
    rc = run_guest(j->root, j->datadir, j->fd, argv, argc, NULL, NULL, NULL, "dex2oat", NULL, j->log, j->ctx);
    snprintf(dir, sizeof dir, "%s/app/apk/oat/arm64", j->datadir);
    snprintf(olddir, sizeof olddir, "%s/app/apk/oat/arm64.old", j->datadir);
    if (rc == 0) {                                  /* in place of the old one (a running app keeps its maps) */
        remove_dir(olddir);
        rename(dir, olddir);
        if (rename(newdir, dir)) filter = "failed";
        remove_dir(olddir);
    } else {
        filter = "failed";                          /* the app keeps running from what it had */
    }
    snprintf(newdir, sizeof newdir, "%s/app/apk/oat.new", j->datadir);
    remove_dir(newdir);
    snprintf(state, sizeof state, "%s/app/apk/oat/.state", j->datadir);
    pthread_mutex_lock(&compile_lock);
    if (compile_stopped && rc) filter = NULL;           /* no state: compiled again at a later launch */
    compile_stopped = 0;
    pthread_mutex_unlock(&compile_lock);
    if (filter && (f = fopen(state, "w"))) { fprintf(f, "%s\n", filter); fclose(f); }
    if (!filter) say(j->log, j->ctx, "dex2oat: stopped, memory is short; a later launch compiles the app");
    else if (!strcmp(filter, "failed")) say(j->log, j->ctx, "dex2oat: failed; the app runs uncompiled");
    else say(j->log, j->ctx, "dex2oat: done (%s): the next launch uses it", filter);
    close(j->fd);
    free(j);
    pthread_mutex_lock(&compile_lock);
    compiling = 0;
    pthread_mutex_unlock(&compile_lock);
    return NULL;
}

int aoi_android_compiling(void)
{
    int c;
    pthread_mutex_lock(&compile_lock);
    c = compiling;
    pthread_mutex_unlock(&compile_lock);
    return c;
}

/* What dex2oat run this launch starts in the background, if any. */
static void compile_apk(const char *root, const char *datadir, int fd, aoi_log_fn log, void *ctx)
{
    char apk[1024], state[1024], last[32] = "", prof[400] = "";
    const char *filter = NULL;
    struct stat sd, sa;
    struct compile_job *j;
    pthread_t th;
    size_t dex = 0;
    int d;
    FILE *f;
    snprintf(apk, sizeof apk, "%s/app/apk/base.apk", datadir);
    snprintf(state, sizeof state, "%s/app/apk/oat/.state", datadir);
    if (stat(apk, &sa)) return;
    if (!stat(state, &sd) && sd.st_mtime >= sa.st_mtime && (f = fopen(state, "r"))) {
        if (!fgets(last, sizeof last, f)) last[0] = 0;
        fclose(f);
        last[strcspn(last, "\n")] = 0;
        if (!strcmp(last, "verify") && app_profile(datadir, prof, sizeof prof)) filter = "speed-profile";   /* step two */
    } else {
        if ((d = open(apk, O_RDONLY)) >= 0) {
            void *z = mmap(NULL, (size_t)sa.st_size, PROT_READ, MAP_PRIVATE, d, 0);
            close(d);
            if (z != MAP_FAILED) { dex = aoi_apk_dex_bytes(z, (size_t)sa.st_size); munmap(z, (size_t)sa.st_size); }
        }
        filter = dex > 16u << 20 ? "verify" : "speed";
    }
    if (!filter) return;
#ifdef __APPLE__
    {   /* dex2oat's peak (WhatsApp verify 409 MB, Molly speed-profile 313 MB) next to the app's */
        size_t avail = os_proc_available_memory(), need = (size_t)(strcmp(filter, "speed") ? 1100 : 900) << 20;
        if (avail < need) {
            say(log, ctx, "dex2oat: %zu MB free now: the app starts uncompiled, a later launch compiles it", avail >> 20);
            return;
        }
    }
#endif
    pthread_mutex_lock(&compile_lock);
    if (compiling) { pthread_mutex_unlock(&compile_lock); return; }   /* (another app's, or this one's from before) */
    compiling = 1;
    pthread_mutex_unlock(&compile_lock);
    if (!(j = calloc(1, sizeof *j))) { compiling = 0; return; }
    snprintf(j->root, sizeof j->root, "%s", root);
    snprintf(j->datadir, sizeof j->datadir, "%s", datadir);
    snprintf(j->filter, sizeof j->filter, "%s", filter);
    if (!strcmp(filter, "speed-profile")) snprintf(j->prof, sizeof j->prof, "%s", prof);
    j->fd = dup(fd);
    j->log = log; j->ctx = ctx;
    compile_log = log; compile_ctx = ctx;
    if (!strcmp(filter, "speed-profile"))
        say(log, ctx, "dex2oat: the app's hot code is compiled in the background, with its profile");
    else if (!strcmp(filter, "verify"))
        say(log, ctx, "dex2oat: a big app (%zu MB of code): verified in the background, its hot code later", dex >> 20);
    else
        say(log, ctx, "dex2oat: the app's code is compiled in the background; it starts uncompiled now");
    if (pthread_create(&th, NULL, compile_thread, j)) {
        close(j->fd); free(j);
        pthread_mutex_lock(&compile_lock); compiling = 0; pthread_mutex_unlock(&compile_lock);
        return;
    }
    pthread_detach(th);
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
    snprintf(path, sizeof path, "%s/local/chrome-command-line", datadir);   /* Chromium's flags (a rooted device's) */
    if (access(path, F_OK) && (f = fopen(path, "w"))) {          /* its renderer in the app's process: no */
        fprintf(f, "_ --single-process\n");                         /* child processes here */
        fclose(f);
    }
    install_libs(datadir, log, ctx);
    compile_apk(root, datadir, fd, log, ctx);
    rc = run_guest(root, datadir, fd, argv, 4, frame, home, frame_ctx, "app", snap, log, ctx);
    close(fd);
    return rc;
}
