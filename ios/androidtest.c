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
#include <sys/statvfs.h>
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

/* ---------- sound out ----------
 * AudioFlinger (core/af.c) hands its 48 kHz stereo mix to audio_push on the guest's
 * thread; a ring of one second holds it and an AudioQueue plays it (its own thread
 * takes from the ring, silence when it runs dry). The queue starts with the first
 * sound and stops after 3 s without any, so a quiet app costs nothing. */
#define RING (48000 * 2)                              /* samples: 1 s of stereo */
static int16_t ring[RING];
static volatile uint64_t ring_w, ring_r;              /* samples written / read, ever */
static volatile int64_t audio_last;                   /* when the guest last sent sound (ns) */

static void audio_push(void *ctx, const int16_t *lr, unsigned frames)
{
    uint64_t w = ring_w, r = ring_r, n = 2ull * frames, i;
    int loud = 0;
    (void)ctx;
    for (i = 0; i < n && !loud; i++) loud = lr[i] != 0;
    if (!loud && w == r) return;                      /* silence with nothing queued: no need to wake the queue */
    if (w - r + n > RING) return;                     /* the queue is behind (it has stopped?): drop */
    for (i = 0; i < n; i++) ring[(w + i) % RING] = lr[i];
    __atomic_store_n(&ring_w, w + n, __ATOMIC_RELEASE);
    audio_last = aoi_mono_ns();
#ifdef __APPLE__
    void audio_start(void);
    audio_start();
#endif
}

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
static AudioQueueRef queue;
static volatile int playing;
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;

static void audio_fill(void *ctx, AudioQueueRef q, AudioQueueBufferRef b)
{
    int16_t *out = b->mAudioData;
    uint64_t r = ring_r, w = __atomic_load_n(&ring_w, __ATOMIC_ACQUIRE), want = b->mAudioDataBytesCapacity / 2, i;
    (void)ctx;
    for (i = 0; i < want; i++) out[i] = r + i < w ? ring[(r + i) % RING] : 0;
    __atomic_store_n(&ring_r, r + (w - r < want ? w - r : want), __ATOMIC_RELEASE);
    b->mAudioDataByteSize = (UInt32)(want * 2);
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
    if (w == r && aoi_mono_ns() - audio_last > 3000000000LL) {   /* quiet for 3 s: pause (buffers stay queued) */
        pthread_mutex_lock(&audio_lock);
        if (playing) { AudioQueuePause(q); playing = 0; }
        pthread_mutex_unlock(&audio_lock);
    }
}

void audio_start(void)
{
    AudioStreamBasicDescription f;
    int k;
    if (playing) return;
    pthread_mutex_lock(&audio_lock);
    if (!queue) {
        memset(&f, 0, sizeof f);
        f.mSampleRate = 48000; f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        f.mBytesPerPacket = f.mBytesPerFrame = 4; f.mFramesPerPacket = 1; f.mChannelsPerFrame = 2; f.mBitsPerChannel = 16;
        if (AudioQueueNewOutput(&f, audio_fill, NULL, NULL, NULL, 0, &queue) != noErr) queue = NULL;
        for (k = 0; queue && k < 3; k++) {            /* 3 x 1024 frames: ~64 ms queued */
            AudioQueueBufferRef b;
            if (AudioQueueAllocateBuffer(queue, 4096, &b) == noErr) {
                memset(b->mAudioData, 0, 4096);
                b->mAudioDataByteSize = 4096;
                AudioQueueEnqueueBuffer(queue, b, 0, NULL);
            }
        }
    }
    if (queue && AudioQueueStart(queue, NULL) == noErr) playing = 1;
    pthread_mutex_unlock(&audio_lock);
}

void aoi_android_audio_kick(void)
{
    playing = 0;                                      /* (iOS paused it: a call, another app's session) */
    if (queue && ring_w != ring_r) audio_start();
}
#else
void aoi_android_audio_kick(void) {}
#endif

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

/* Other threads (the UI's touches, keys, snapshots, stops; the memory watcher) reach
 * the app's process through proc_get/proc_put: run_guest frees it only once none holds
 * it. A stop asked while an app is still starting (running not yet set: its libraries,
 * its snapshot loading) is kept for it (stop_pending), so it is not lost. */
static pthread_mutex_t run_lock = PTHREAD_MUTEX_INITIALIZER;
static int run_users, launching, stop_pending;
static unsigned run_gen;                    /* one more for each process that becomes running */
static char end_why[300];                   /* how the last app ended by itself ("" if it was stopped or exited 0) */

int aoi_android_last_end(char *why, size_t n)
{
    snprintf(why, n, "%s", end_why);
    return end_why[0] != 0;
}

static struct aoi_proc *proc_get(void)
{
    struct aoi_proc *p;
    pthread_mutex_lock(&run_lock);
    if ((p = running)) run_users++;
    pthread_mutex_unlock(&run_lock);
    return p;
}

static void proc_put(struct aoi_proc *p)
{
    if (!p) return;
    pthread_mutex_lock(&run_lock);
    run_users--;
    pthread_mutex_unlock(&run_lock);
}

static void launch_begin(void)
{
    pthread_mutex_lock(&run_lock);
    end_why[0] = 0;
    launching = 1;
    stop_pending = 0;
    pthread_mutex_unlock(&run_lock);
}

static void launch_end(void)
{
    pthread_mutex_lock(&run_lock);
    launching = 0;
    stop_pending = 0;
    pthread_mutex_unlock(&run_lock);
}

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
struct watch { aoi_log_fn log; void *ctx; struct aoi_proc *p; unsigned gen; };
static void compile_cut(void);

static void *memory_watch(void *arg)
{
    struct watch w = *(struct watch *)arg;
    extern uint64_t aoi_vm_mapped_bytes, aoi_vm_copied_bytes;
    free(arg);
    for (;;) {
        int i;
        struct aoi_proc *held;
        for (i = 0; i < 100 && running == w.p; i++) { struct timespec ts = { 0, 100000000 }; nanosleep(&ts, NULL); }
        held = proc_get();                                 /* not freed while it is read */
        if (held != w.p || run_gen != w.gen) { proc_put(held); return NULL; }
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
        proc_put(held);
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
    struct aoi_proc *p = proc_get();
    struct timespec ts = { 0, 20000000 };
    double waited = 0;
    int rc;
    if (!p || !snap_path[0] || p->stop_request) { proc_put(p); return -1; }
    snprintf(p->snap_path, sizeof p->snap_path, "%s", snap_path);
    if (p->gpu_live) aoi_proc_touch(p, 5, 0, 0);       /* aoi.Snapshot: the GPU's state goes first */
    else p->snap_request = 1;
    while (p->snap_request && running == p && waited < timeout) { nanosleep(&ts, NULL); waited += 0.02; }
    while (p->snap_path[0] && running == p && waited < timeout) { nanosleep(&ts, NULL); waited += 0.02; }   /* written */
    rc = running == p && !p->snap_path[0] ? 0 : -1;
    proc_put(p);
    return rc;
}

void aoi_android_touch(int action, float x, float y)
{
    struct aoi_proc *p = proc_get();
    if (p) aoi_proc_touch(p, action, x, y);
    proc_put(p);
}

void aoi_android_key(int action, int value)
{
    struct aoi_proc *p = proc_get();
    if (p) aoi_proc_key(p, action, value);
    proc_put(p);
}

void aoi_android_stop(void)
{
    pthread_mutex_lock(&run_lock);
    if (running) running->stop_request = 1;            /* aoi_proc_run returns; aoi_android_app ends */
    else if (launching) stop_pending = 1;              /* still starting: it stops as it begins */
    pthread_mutex_unlock(&run_lock);
}

void aoi_android_save_stop(double timeout)
{
    unsigned gen;
    pthread_mutex_lock(&run_lock);
    if (!running) {
        if (launching) stop_pending = 1;
        pthread_mutex_unlock(&run_lock);
        return;
    }
    gen = run_gen;
    pthread_mutex_unlock(&run_lock);
    aoi_android_snapshot(timeout);
    pthread_mutex_lock(&run_lock);
    if (running && run_gen == gen) running->stop_request = 1;   /* that one, not the next app */
    pthread_mutex_unlock(&run_lock);
}

void aoi_android_redraw(void)
{
    struct aoi_proc *p = proc_get();
    if (p) p->redraw_request = 1;                      /* the screen it shows, again (core/proc.c) */
    proc_put(p);
}

void aoi_android_back(void)
{
    struct aoi_proc *p = proc_get();
    if (p) aoi_proc_touch(p, 3, 0, 0);                  /* aoi.Input: the activity's onBackPressed */
    proc_put(p);
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
static int snap_restore(const char *datadir);

static int warm_take(struct aoi_proc *p);
static pthread_mutex_t warm_lock = PTHREAD_MUTEX_INITIALIZER;
static struct aoi_proc *warm_p;             /* the warm process, while it has no app */
static int warm_stopping;                   /* aoi_android_warm_stop: before or while it runs */
static struct {                             /* the app aoi_android_go hands it */
    int pending, done, missed;              /* missed: handed, but it ended before taking it */
    char root[1024], datadir[980], logpath[1024], display[64];
    aoi_frame_fn frame;
    void (*home)(void *);
    void *frame_ctx;
    aoi_log_fn log;
    void *ctx;
} warm_go;

static int run_guest(const char *root, const char *datadir, int fd, const char *const *argv, int argc,
                     aoi_frame_fn frame, void (*home)(void *), void *frame_ctx, const char *what, const char *snap,
                     aoi_log_fn log, void *ctx, int warm)
{
    static const char *const base[] = {
        "PATH=/system/bin", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", "HOME=/",
        "TMPDIR=/data/local/tmp", "ANDROID_ART_ROOT=/apex/com.android.art",
        "ANDROID_I18N_ROOT=/apex/com.android.i18n", "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
        "CLASSPATH=/data/local/tmp/aoi.dex",
        "ANDROID_NO_USE_FWMARK_CLIENT=1", NULL };            /* no netd fwmarkd: sockets untagged (core/proc.c) */
    char vals[3][4096];                 /* (dex2oat's thread runs this too, at the same time) */
    const char *cp[4], *envp[24];
    struct aoi_proc *p = calloc(1, sizeof *p);
    static char snap_app[1100];
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
    if (warm) envp[ne++] = "AOI_WARM=1";    /* aoi.Main waits for its app (aoi_android_warm) */
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
                char snap0[1200];
                struct stat a, b;
                snprintf(snap0, sizeof snap0, "%s0", snap);   /* the clean one, if it is that file: not again */
                if (!stat(snap, &a) && !stat(snap0, &b) && a.st_dev == b.st_dev && a.st_ino == b.st_ino) unlink(snap0);
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
    if (frame) p->audio = audio_push;   /* an app: its sound to the speaker */
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
    if (warm) {
        p->warm = warm_take;
        pthread_mutex_lock(&warm_lock);
        warm_p = p;
        if (warm_stopping) p->stop_request = 1;
        pthread_mutex_unlock(&warm_lock);
    }
    if (resumed) aoi_sf_redraw(p);      /* the screen it was showing */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (frame) {
        struct watch *w = malloc(sizeof *w);
        pthread_t th;
        pthread_mutex_lock(&run_lock);
        running = p;
        run_gen++;
        if (stop_pending && !warm) p->stop_request = 1;    /* stopped while it was starting */
        stop_pending = 0;
        launching = 0;
        pthread_mutex_unlock(&run_lock);
        if (w) {
            w->log = log; w->ctx = ctx; w->p = p; w->gen = run_gen;
            if (!pthread_create(&th, NULL, memory_watch, w)) pthread_detach(th); else free(w);
        }
    }
    if (!strcmp(what, "dex2oat")) compile_attach(p);
    st = aoi_proc_run(p, 0);
    if (!strcmp(what, "dex2oat")) compile_attach(NULL);
    if (frame) {
        struct timespec wait = { 0, 5000000 };
        pthread_mutex_lock(&run_lock);
        running = NULL;
        while (run_users) {                 /* another thread still reads it: freed after */
            pthread_mutex_unlock(&run_lock);
            nanosleep(&wait, NULL);
            pthread_mutex_lock(&run_lock);
        }
        pthread_mutex_unlock(&run_lock);
    }
    if (warm) {
        pthread_mutex_lock(&warm_lock);
        warm_p = NULL;
        if (warm_go.pending && !warm_go.done) warm_go.missed = 1;
        warm_go.pending = 0;
        pthread_mutex_unlock(&warm_lock);
        if (warm_go.done) {                 /* it became an app: what follows is the app's */
            what = "app";
            snprintf(snap_app, sizeof snap_app, "%s.snap", warm_go.datadir);
            snap = snap_app;
            resumed = 0;
        }
    }
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
    if (frame && strcmp(what, "android")) {         /* an app: did it end by itself, and how */
        end_why[0] = 0;
        if (!p->stop_request && st == AOI_STOP_EXIT && rc != 0)
            snprintf(end_why, sizeof end_why, "exit %d after %.0f s", rc, secs);
        else if (!p->stop_request && st != AOI_STOP_EXIT) {
            char w[256];
            snprintf(end_why, sizeof end_why, "stopped (%d) in %s after %.0f s", (int)st, aoi_proc_where(p, p->cpu.pc, w, sizeof w), secs);
        }
    }
    if (snap && rc != 0 && !p->stop_request) {      /* it died by itself: was its snapshot the way there? */
        struct stat ss;
        if ((resumed && secs < 30) || (!stat(snap, &ss) && time(NULL) - ss.st_mtime < 60)) {
            char snap0[1200];
            struct stat s0;
            snprintf(snap0, sizeof snap0, "%s0", snap);  /* the clean one too, if it was the way there */
            if (!stat(snap, &ss) && !stat(snap0, &s0) && ss.st_dev == s0.st_dev && ss.st_ino == s0.st_ino) unlink(snap0);
            unlink(snap);                           /* (resumed, it would die again: WhatsApp 0.60 did, each launch) */
            say(log, ctx, "%s: it ended soon after its snapshot: the snapshot is dropped (the next launch: the clean one, if it was not that one)", what);
        }
    }
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
 *   - up to 16 MB of dex: everything (`speed`): Qalculate 100 s on the phone.
 *   - more (Molly 59 MB: 41 min, 750 MB with `speed`): `verify` (WhatsApp, 86 MB: 249 s);
 *     asked for again (faster), once the app has a profile: `speed-profile`, its hot code.
 * It runs once per APK, when the iOS app asks (at install, or its "Derle"), in a thread of
 * its own; an app may run meanwhile, uncompiled. A launch never starts one. It writes
 * oat.new/arm64, which then takes oat/arm64's place, so a launch never sees a
 * half-written odex; the next launch (the snapshot's key has the odex in it) starts with
 * the compiled code. oat/.state holds the filter of the last run that finished for this
 * APK (speed, verify, speed-profile, failed); one cut short (cancelled, memory, iOS ended
 * the app) leaves none. Its progress: instructions run against what the filter takes per
 * MB of dex (measured, and corrected by each finished run on this device). */
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
static int compile_cancelled;               /* aoi_android_compile_cancel */
static aoi_log_fn compile_log;
static void *compile_ctx;
static char compile_dir[1024], compile_filter[16];
static double compile_expect;               /* instructions it should take */
static double compile_t0, compile_rate, compile_last_t, compile_last_steps;
static uint64_t compile_steps;              /* of the run that ended, while none runs */
static void (*compile_done_fn)(const char *datadir, const char *state);

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Instructions per MB of dex: measured (host and phone, 0.53-0.60): speed 3.2 G
 * (NewPipe, Cromite, Qalculate), verify 0.2 G (Element, Molly, WhatsApp), speed-profile
 * 0.37 G (Molly). Each finished run on this device corrects them (a file next to the
 * apps: dirname(datadir)/.dex2oat-rates). */
static const char *const filters[3] = { "speed", "verify", "speed-profile" };
static const double per_mb[3] = { 3.2e9, 2.1e8, 3.7e8 };

static int filter_index(const char *f)
{
    int i;
    for (i = 0; i < 3; i++) if (!strcmp(f, filters[i])) return i;
    return 0;
}

static void rates_path(const char *datadir, char *out, size_t n)
{
    const char *slash = strrchr(datadir, '/');
    int len = slash ? (int)(slash - datadir) : 0;
    snprintf(out, n, "%.*s/.dex2oat-rates", len, datadir);
}

static void rates_read(const char *datadir, double r[3])
{
    char path[1100];
    FILE *f;
    r[0] = r[1] = r[2] = 1;
    rates_path(datadir, path, sizeof path);
    if ((f = fopen(path, "r"))) {
        if (fscanf(f, "%lf %lf %lf", &r[0], &r[1], &r[2]) != 3) r[0] = r[1] = r[2] = 1;
        fclose(f);
    }
}

static void rates_learn(const char *datadir, const char *filter, double actual, double expected)
{
    char path[1100];
    double r[3], k;
    int i = filter_index(filter);
    FILE *f;
    if (expected <= 0 || actual <= 0) return;
    rates_read(datadir, r);
    k = actual / (expected / r[i]);                    /* this run's own correction */
    if (k < 0.1 || k > 10) return;
    r[i] = r[i] * 0.5 + k * 0.5;
    rates_path(datadir, path, sizeof path);
    if ((f = fopen(path, "w"))) { fprintf(f, "%f %f %f\n", r[0], r[1], r[2]); fclose(f); }
}

static size_t apk_dex_bytes(const char *datadir)
{
    char apk[1100];
    struct stat sa;
    size_t dex = 0;
    int d;
    snprintf(apk, sizeof apk, "%s/app/apk/base.apk", datadir);
    if (stat(apk, &sa) || (d = open(apk, O_RDONLY)) < 0) return 0;
    {
        void *z = mmap(NULL, (size_t)sa.st_size, PROT_READ, MAP_PRIVATE, d, 0);
        close(d);
        if (z != MAP_FAILED) { dex = aoi_apk_dex_bytes(z, (size_t)sa.st_size); munmap(z, (size_t)sa.st_size); }
    }
    return dex;
}

static void compile_attach(struct aoi_proc *p)
{
    pthread_mutex_lock(&compile_lock);
    if (!p && compile_proc) compile_steps = compile_proc->cpu.steps;
    compile_proc = p;
    if (p) {
        p->pause_request = compile_hold;
        if (compile_cancelled) p->stop_request = 1;
    }
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
 * back), the app keeps running; the app's card then offers to compile again. */
static void compile_cut(void)
{
    pthread_mutex_lock(&compile_lock);
    if (compile_proc && !compile_proc->stop_request) {
        compile_proc->stop_request = 1;
        compile_stopped = 1;
    }
    pthread_mutex_unlock(&compile_lock);
}

void aoi_android_compile_cancel(void)
{
    pthread_mutex_lock(&compile_lock);
    if (compiling) {
        compile_cancelled = 1;
        if (compile_proc) compile_proc->stop_request = 1;
    }
    pthread_mutex_unlock(&compile_lock);
}

void aoi_android_set_compile_done(void (*fn)(const char *datadir, const char *state)) { compile_done_fn = fn; }

void aoi_android_compile_info(struct aoi_compile_info *ci)
{
    double t = now_s(), steps;
    memset(ci, 0, sizeof *ci);
    pthread_mutex_lock(&compile_lock);
    if (compiling) {
        ci->active = 1;
        ci->held = compile_hold;
        snprintf(ci->datadir, sizeof ci->datadir, "%s", compile_dir);
        snprintf(ci->filter, sizeof ci->filter, "%s", compile_filter);
        steps = compile_proc ? (double)compile_proc->cpu.steps : 0;
        if (t - compile_last_t >= 2 && !compile_hold && steps >= compile_last_steps) {   /* the recent rate */
            double r = (steps - compile_last_steps) / (t - compile_last_t);
            if (compile_last_t > 0 && r > 0) compile_rate = compile_rate > 0 ? compile_rate * 0.7 + r * 0.3 : r;
            compile_last_t = t; compile_last_steps = steps;
        }
        ci->elapsed = t - compile_t0;
        ci->progress = compile_expect > 0 ? steps / compile_expect : 0;
        if (ci->progress > 0.99) ci->progress = 0.99;      /* the rest: it says when it is done */
        ci->eta = steps >= compile_expect * 0.97 ? -2                  /* longer than it should: nearly done */
                : compile_rate > 0 ? (compile_expect - steps) / compile_rate : -1;
    }
    pthread_mutex_unlock(&compile_lock);
}

void aoi_android_compile_state(const char *datadir, char *out, size_t n)
{
    char apk[1100], state[1100];
    struct stat sd, sa;
    FILE *f;
    out[0] = 0;
    snprintf(apk, sizeof apk, "%s/app/apk/base.apk", datadir);
    snprintf(state, sizeof state, "%s/app/apk/oat/.state", datadir);
    if (stat(apk, &sa) || stat(state, &sd) || sd.st_mtime < sa.st_mtime || !(f = fopen(state, "r"))) return;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
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

void aoi_android_compile_remove(const char *datadir)
{
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/app/apk/oat/arm64", datadir);
    remove_dir(dir);
    snprintf(dir, sizeof dir, "%s/app/apk/oat/.state", datadir);
    unlink(dir);
    {                                           /* its snapshots were of the compiled code */
        static const char *const ext[] = { ".snap", ".snap.key", ".snap0", ".snap0.key", ".nosnap" };
        unsigned i;
        for (i = 0; i < sizeof ext / sizeof *ext; i++) {
            snprintf(dir, sizeof dir, "%s%s", datadir, ext[i]);
            unlink(dir);
        }
    }
}

static void *compile_thread(void *arg)
{
    struct compile_job *j = arg;
    const char *argv[10] = { "/apex/com.android.art/bin/dex2oat64", "--dex-file=/data/app/apk/base.apk",
        "--oat-file=/data/app/apk/oat.new/arm64/base.odex", "--instruction-set=arm64", NULL,
        "--class-loader-context=PCL[]", "--no-watch-dog", NULL, NULL };   /* its 9.5 min limit: big apps take longer */
    char dir[1100], newdir[1100], olddir[1100], state[1100], profarg[450], fa[64];
    const char *filter = j->filter;
    int argc = 7, rc, cancelled;
    double expect;
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
    rc = run_guest(j->root, j->datadir, j->fd, argv, argc, NULL, NULL, NULL, "dex2oat", NULL, j->log, j->ctx, 0);
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
    cancelled = compile_cancelled;
    if ((compile_stopped || cancelled) && rc) filter = NULL;   /* no state: the card offers it again */
    compile_stopped = 0;
    expect = compile_expect;
    pthread_mutex_unlock(&compile_lock);
    if (filter && (f = fopen(state, "w"))) { fprintf(f, "%s\n", filter); fclose(f); }
    if (rc == 0) rates_learn(j->datadir, j->filter, (double)compile_steps, expect);
    if (!filter) say(j->log, j->ctx, cancelled ? "dex2oat: cancelled" : "dex2oat: stopped, memory is short; compile it again later");
    else if (!strcmp(filter, "failed")) say(j->log, j->ctx, "dex2oat: failed; the app runs uncompiled");
    else say(j->log, j->ctx, "dex2oat: done (%s): the next launch uses it", filter);
    close(j->fd);
    pthread_mutex_lock(&compile_lock);
    compiling = 0;
    compile_cancelled = 0;
    pthread_mutex_unlock(&compile_lock);
    if (compile_done_fn) compile_done_fn(j->datadir, filter ? filter : "");
    free(j);
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

double aoi_android_compile_estimate(const char *datadir, int faster)
{
    char last[32];
    double r[3];
    size_t dex = apk_dex_bytes(datadir);
    int i;
    aoi_android_compile_state(datadir, last, sizeof last);
    i = faster && !strcmp(last, "verify") ? 2 : dex > 16u << 20 ? 1 : 0;
    rates_read(datadir, r);
    return (double)dex / 1e6 * per_mb[i] * r[i] / 75e6;       /* the phone: ~75 M instructions/s on dex2oat */
}

int aoi_android_compile(const char *root, const char *datadir, const char *logpath, int faster, aoi_log_fn log, void *ctx)
{
    char last[32], prof[400] = "";
    const char *filter = NULL;
    struct compile_job *j;
    pthread_t th;
    size_t dex = apk_dex_bytes(datadir);
    double r[3];
    int fd;
    aoi_android_compile_state(datadir, last, sizeof last);
    if (!last[0] || !strcmp(last, "failed")) filter = dex > 16u << 20 ? "verify" : "speed";
    else if (faster && !strcmp(last, "verify") && app_profile(datadir, prof, sizeof prof)) filter = "speed-profile";
    if (!filter || !dex) return 1;                                  /* compiled already */
    {                                   /* its output: the odex and vdex, a few times the dex */
        struct statvfs vs;
        if (!statvfs(datadir, &vs) && (uint64_t)vs.f_bavail * vs.f_frsize < (uint64_t)dex * 4 + ((uint64_t)300 << 20)) {
            say(log, ctx, "dex2oat: %llu MB free on the device, %zu MB of dex: not enough room",
                (unsigned long long)((uint64_t)vs.f_bavail * vs.f_frsize >> 20), dex >> 20);
            return -3;
        }
    }
#ifdef __APPLE__
    int busy;
    pthread_mutex_lock(&run_lock);
    busy = running && !running->warm;
    pthread_mutex_unlock(&run_lock);
    if (busy) {                         /* dex2oat's peak (WhatsApp verify 409 MB, Molly speed-profile 313 MB) next to the app's */
        size_t avail = os_proc_available_memory(), need = (size_t)(strcmp(filter, "speed") ? 1100 : 900) << 20;
        if (avail < need) {
            say(log, ctx, "dex2oat: %zu MB free next to the running app: not now", avail >> 20);
            return -2;
        }
    }
#endif
    pthread_mutex_lock(&compile_lock);
    if (compiling) { pthread_mutex_unlock(&compile_lock); return -1; }
    compiling = 1;
    compile_cancelled = 0;
    rates_read(datadir, r);
    snprintf(compile_dir, sizeof compile_dir, "%s", datadir);
    snprintf(compile_filter, sizeof compile_filter, "%s", filter);
    compile_expect = (double)dex / 1e6 * per_mb[filter_index(filter)] * r[filter_index(filter)];
    compile_t0 = now_s();
    compile_rate = 0; compile_last_t = 0; compile_last_steps = 0; compile_steps = 0;
    pthread_mutex_unlock(&compile_lock);
    if ((fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644)) < 0) fd = open("/dev/null", O_WRONLY);
    if (!(j = calloc(1, sizeof *j))) {
        close(fd);
        pthread_mutex_lock(&compile_lock); compiling = 0; pthread_mutex_unlock(&compile_lock);
        return -1;
    }
    snprintf(j->root, sizeof j->root, "%s", root);
    snprintf(j->datadir, sizeof j->datadir, "%s", datadir);
    snprintf(j->filter, sizeof j->filter, "%s", filter);
    if (!strcmp(filter, "speed-profile")) snprintf(j->prof, sizeof j->prof, "%s", prof);
    j->fd = fd;
    j->log = log; j->ctx = ctx;
    compile_log = log; compile_ctx = ctx;
    say(log, ctx, "dex2oat: %s, %zu MB of code (%s)", strrchr(datadir, '/') ? strrchr(datadir, '/') + 1 : datadir,
        dex >> 20, filter);
    if (pthread_create(&th, NULL, compile_thread, j)) {
        close(j->fd); free(j);
        pthread_mutex_lock(&compile_lock); compiling = 0; pthread_mutex_unlock(&compile_lock);
        return -1;
    }
    pthread_detach(th);
    return 0;
}

/* What a launch sets up in the app's /data first: its display (aoi.DisplayManager
 * reads it), Chromium's flags, the APK's native libraries. The log: the last run's
 * stays as .1 (a crash's stack). The new log's fd, or -1. */
static int app_setup(const char *datadir, const char *logpath, const char *display, aoi_log_fn log, void *ctx)
{
    char path[1100];
    int fd;
    FILE *f;
    snprintf(path, sizeof path, "%s.1", logpath);
    rename(logpath, path);
    if ((fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) { say(log, ctx, "app: cannot write %s", logpath); return -1; }
    snprintf(path, sizeof path, "%s/local/tmp/aoi.display", datadir);
    if (display && (f = fopen(path, "w"))) { fprintf(f, "%s\n", display); fclose(f); }
    snprintf(path, sizeof path, "%s/local/chrome-command-line", datadir);   /* Chromium's flags (a rooted device's) */
    if (access(path, F_OK) && (f = fopen(path, "w"))) {          /* its renderer in the app's process: no */
        fprintf(f, "_ --single-process\n");                        /* child processes here */
        fclose(f);
    }
    install_libs(datadir, log, ctx);
    return fd;
}

static const char *const app_argv[] = { "/system/bin/app_process64", "/system/bin", "aoi.Main",
                                        "/data/app/apk/base.apk", NULL };

int aoi_android_app(const char *root, const char *datadir, const char *logpath, const char *display,
                    aoi_frame_fn frame, void (*home)(void *), void *frame_ctx, aoi_log_fn log, void *ctx)
{
    int fd, rc;
    char snap[1100];
    launch_begin();
    if ((fd = app_setup(datadir, logpath, display, log, ctx)) < 0) { launch_end(); return -1; }
    snap_restore(datadir);
    snprintf(snap, sizeof snap, "%s.snap", datadir);
    rc = run_guest(root, datadir, fd, app_argv, 4, frame, home, frame_ctx, "app", snap, log, ctx, 0);
    launch_end();
    close(fd);
    return rc;
}

/* ---------- an app started out of sight, to be saved ----------
 * After its code is compiled an app's snapshot no longer fits (its key has the odex):
 * the first launch would start it from nothing. The iOS app starts it here instead,
 * without a screen, right after the compile: once aoi.Main has saved it (at idle) the
 * process ends, and the user's first tap resumes it in a second. A tap while it is
 * still starting shows it (aoi_android_show) and it goes on as the app. */

static volatile int hidden_on, hidden_shown, hidden_late;
static pthread_mutex_t hidden_lock = PTHREAD_MUTEX_INITIALIZER;

static void drop_frame(void *ctx, const unsigned char *rgbx, unsigned w, unsigned h) { (void)ctx; (void)rgbx; (void)w; (void)h; }

static void *hidden_watch(void *arg)
{
    struct timespec ts = { 0, 250000000 };
    double waited = 0, limit = *(double *)arg;
    int seen = 0;
    while (waited < limit && !hidden_shown) {
        struct aoi_proc *p = proc_get();
        int end = 0;
        if (p && p->snap_path[0]) seen = 1;
        if (p && seen && !p->snap_path[0]) end = 1;         /* saved (or the app does not let itself be) */
        if (p && !seen && waited > 20) end = 1;             /* resumed: nothing to save */
        proc_put(p);
        if (end) break;
        nanosleep(&ts, NULL);
        waited += 0.25;
    }
    hidden_late = waited >= limit;
    for (;;) {                                              /* end it (it may not run yet: wait for it) */
        struct aoi_proc *p;
        int done = 0;
        pthread_mutex_lock(&hidden_lock);
        pthread_mutex_lock(&run_lock);
        p = running;
        if (hidden_shown || !hidden_on) done = 1;
        else if (p) { p->stop_request = 1; done = 1; }
        pthread_mutex_unlock(&run_lock);
        pthread_mutex_unlock(&hidden_lock);
        if (done) break;
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* A snapshot the next launch would resume: this launch's key, this build's. */
static int snap_fits(const char *snap, const char *key)
{
    return !access(snap, R_OK) && snap_key_ok(snap, key) && aoi_snap_this_build(snap);
}

/* The clean snapshot (<datadir>.snap0): the app just started, saved out of sight after
 * its compile. The one the app runs on (.snap) is saved again as it is used; when that
 * one is gone (a restart, a crash after it) or no longer fits, the clean one takes its
 * place, so a compiled app never starts from nothing again. Both names are links to a
 * file a save replaces whole (rename): one never changes under the other. */
static int snap_restore(const char *datadir)
{
    char key[256], snap[1100], snap0[1100];
    snprintf(snap, sizeof snap, "%s.snap", datadir);
    snprintf(snap0, sizeof snap0, "%s.snap0", datadir);
    snap_key(datadir, key, sizeof key);
    if (snap_fits(snap, key)) return 1;
    if (!snap_fits(snap0, key)) return 0;
    unlink(snap);
    if (link(snap0, snap)) return 0;
    snap_key_write(snap, key);
    return 1;
}

static void snap_keep_clean(const char *datadir)
{
    char key[256], snap[1100], snap0[1100];
    snprintf(snap, sizeof snap, "%s.snap", datadir);
    snprintf(snap0, sizeof snap0, "%s.snap0", datadir);
    snap_key(datadir, key, sizeof key);
    if (!snap_fits(snap, key)) return;
    unlink(snap0);
    if (!link(snap, snap0)) snap_key_write(snap0, key);
}

int aoi_android_snapshot_fits(const char *datadir)
{
    return snap_restore(datadir);
}

int aoi_android_app_hidden(const char *root, const char *datadir, const char *logpath, const char *display,
                           aoi_log_fn log, void *ctx)
{
    static double limit = 240;               /* a big app's first start on the phone: under 4 min */
    char snap[1100];
    struct stat st;
    time_t t0 = time(NULL);
    pthread_t th;
    int fd, rc;
    launch_begin();
    if ((fd = app_setup(datadir, logpath, display, log, ctx)) < 0) { launch_end(); return -3; }
    snprintf(snap, sizeof snap, "%s.snap", datadir);
    hidden_shown = 0;
    hidden_late = 0;
    hidden_on = 1;
    if (pthread_create(&th, NULL, hidden_watch, &limit)) { close(fd); hidden_on = 0; launch_end(); return -3; }
    say(log, ctx, "app: started out of sight, to be saved for the next launch");
    rc = run_guest(root, datadir, fd, app_argv, 4, drop_frame, NULL, NULL, "app", snap, log, ctx, 0);
    launch_end();
    hidden_on = 0;
    pthread_join(th, NULL);
    close(fd);
    if (hidden_shown) return rc;
    if (stat(snap, &st) || st.st_mtime < t0 || !st.st_size) return hidden_late ? -3 : -1;
    snap_keep_clean(datadir);
    return 1;
}

int aoi_android_show(aoi_frame_fn frame, void (*home)(void *), void *frame_ctx)
{
    struct aoi_proc *p;
    pthread_mutex_lock(&hidden_lock);
    pthread_mutex_lock(&run_lock);
    p = running;
    if (!hidden_on || hidden_shown || !p || p->stop_request) {
        pthread_mutex_unlock(&run_lock);
        pthread_mutex_unlock(&hidden_lock);
        return -1;
    }
    p->frame_ctx = frame_ctx;
    p->home = home;
    p->frame = frame;
    hidden_shown = 1;
    p->redraw_request = 1;
    pthread_mutex_unlock(&run_lock);
    pthread_mutex_unlock(&hidden_lock);
    return 0;
}

/* ---------- the warm process: Android up before its app is chosen ----------
 * aoi.Main (AOI_WARM) starts the runtime and the services that do not depend on the
 * app, is saved once (warmdir.snap: the next warm start resumes it in a second), and
 * waits in open("/dev/aoi_warm"). aoi_android_go hands it an app; on the guest's
 * thread (warm_take) the process becomes the app's: its /data (p->data: what it opened
 * so far is the same in every /data, the build's), log, frames and snapshot. */

static int warm_take(struct aoi_proc *p)
{
    char key[256], snap[1000];
    int fd;
    pthread_mutex_lock(&warm_lock);
    if (!warm_go.pending) { pthread_mutex_unlock(&warm_lock); return 0; }
    warm_go.pending = 0;
    warm_go.done = 1;
    warm_p = NULL;
    pthread_mutex_unlock(&warm_lock);
    if ((fd = app_setup(warm_go.datadir, warm_go.logpath, warm_go.display, warm_go.log, warm_go.ctx)) >= 0) {
        if (p->log) { fflush(p->log); dup2(fd, fileno(p->log)); }
        dup2(fd, p->fd[1].host);            /* stdout and stderr (the same host fd): the app's log now */
        close(fd);
    }
    snprintf(p->data, sizeof p->data, "%s", warm_go.datadir);
    p->frame = warm_go.frame;
    p->home = warm_go.home;
    p->frame_ctx = warm_go.frame_ctx;
    snprintf(snap, sizeof snap, "%s.snap", warm_go.datadir);
    unlink(snap);                           /* (the iOS app found none it could resume) */
    snap_key(warm_go.datadir, key, sizeof key);
    snap_key_write(snap, key);
    snprintf(p->snap_path, sizeof p->snap_path, "%s", snap);
    snprintf(snap_path, sizeof snap_path, "%s", snap);
    p->warm = NULL;
    say(warm_go.log, warm_go.ctx, "app: on the Android that was already up");
    return 1;
}

int aoi_android_warm(const char *root, const char *warmdir, const char *display, aoi_log_fn log, void *ctx)
{
    char logpath[1100], snap[1100];
    int fd, rc;
    snprintf(logpath, sizeof logpath, "%s.log", warmdir);
    if ((fd = app_setup(warmdir, logpath, display, log, ctx)) < 0) return -2;
    snprintf(snap, sizeof snap, "%s.snap", warmdir);
    pthread_mutex_lock(&warm_lock);
    if (warm_stopping) {                    /* stopped before it began */
        warm_stopping = 0;
        pthread_mutex_unlock(&warm_lock);
        close(fd);
        return -2;
    }
    memset(&warm_go, 0, sizeof warm_go);
    snprintf(warm_go.display, sizeof warm_go.display, "%s", display ? display : "");
    pthread_mutex_unlock(&warm_lock);
    rc = run_guest(root, warmdir, fd, app_argv, 4, drop_frame, NULL, NULL, "android", snap, log, ctx, 1);
    close(fd);
    pthread_mutex_lock(&warm_lock);
    warm_stopping = 0;
    pthread_mutex_unlock(&warm_lock);
    return warm_go.done ? rc : warm_go.missed ? -3 : -2;
}

void aoi_android_warm_stop(void)
{
    pthread_mutex_lock(&warm_lock);
    if (warm_p) warm_p->stop_request = 1;
    else if (!warm_go.done) warm_stopping = 1;
    pthread_mutex_unlock(&warm_lock);
}

int aoi_android_go(const char *root, const char *datadir, const char *logpath, const char *display,
                   aoi_frame_fn frame, void (*home)(void *), void *frame_ctx, aoi_log_fn log, void *ctx)
{
    int ok;
    pthread_mutex_lock(&warm_lock);
    ok = warm_p && !warm_go.pending && !warm_go.done && !strcmp(display ? display : "", warm_go.display);
    if (ok) {
        end_why[0] = 0;
        snprintf(warm_go.root, sizeof warm_go.root, "%s", root);
        snprintf(warm_go.datadir, sizeof warm_go.datadir, "%s", datadir);
        snprintf(warm_go.logpath, sizeof warm_go.logpath, "%s", logpath);
        warm_go.frame = frame; warm_go.home = home; warm_go.frame_ctx = frame_ctx;
        warm_go.log = log; warm_go.ctx = ctx;
        warm_go.pending = 1;
    }
    pthread_mutex_unlock(&warm_lock);
    return ok ? 0 : -1;
}
