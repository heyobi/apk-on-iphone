#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE                /* ru_maxrss on macOS */
/* aoiproc: run an unmodified Android program, with Android's own linker64,
 * in the interpreter. usage: aoiproc [-t] [-e NAME=VALUE]... ROOT PROGRAM [args...]
 * ROOT is a guest root made by tools/android-root.sh; PROGRAM is a guest path
 * such as /system/bin/toybox. -t logs every syscall to stderr; -e adds to the
 * guest environment; -p prints where the guest spent its instructions
 * (one sample per 100k-instruction time slice, grouped by library).
 * Debug environment: AOI_UFFD=1 offers userfaultfd (ART: CMC GC + boot image),
 * AOI_STOP_AT=N stops after N instructions, AOI_DUMP=addr,len,file saves guest
 * memory at the end; build/aoiproc-debug adds AOI_WATCH and AOI_PCRING (core/cpu.c). */
#include "../core/binder.h"
#include "../core/proc.h"
#ifdef AOI_ORACLE
#include "../core/oracle.h"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/resource.h>

/* Resident memory at its peak: what a phone would have to hold (chunks also count
 * file mappings that were never touched). */
/* AOI_TAPS="x,y;x,y;...": after the first frame, taps in screen pixels 4 s apart
 * (aoi_proc_touch from another thread, as the iOS view sends them); "back" presses
 * the back key; "x,y,ms" holds the finger down that long (a long press; it moves a
 * pixel every 0.1 s, as a finger on glass does). */
/* The host clipboard (proc.h, clip): a buffer, AOI_CLIP at the start; each change is logged. */
static char clipbuf[4096];
static void clip(void *ctx, int op, const char *path)
{
    FILE *f;
    (void)ctx;
    if (op == 's' && (f = fopen(path, "rb"))) {
        size_t n = fread(clipbuf, 1, sizeof clipbuf - 1, f);
        clipbuf[n] = 0;
        fclose(f);
        fprintf(stderr, "[aoiproc] clipboard set: \"%s\"\n", clipbuf);
    } else if (op == 'g') {
        if (!clipbuf[0]) { remove(path); return; }
        if ((f = fopen(path, "wb"))) { fputs(clipbuf, f); fclose(f); }
        fprintf(stderr, "[aoiproc] clipboard read: \"%s\"\n", clipbuf);
    } else if (op == 'h' && (f = fopen(path, "wb"))) {
        fputs(clipbuf[0] ? "1" : "0", f);
        fclose(f);
    }
}

static void *taps(void *arg)
{
    const char *s = getenv("AOI_TAPS");
    float x, y;
    int n, hold;
    for (;;) {
        if (!strncmp(s, "back", 4)) {                     /* "back": the back key */
            sleep(getenv("AOI_TAP_GAP") ? (unsigned)atoi(getenv("AOI_TAP_GAP")) : 4);
            fprintf(stderr, "[aoiproc] back\n");
            aoi_proc_touch(arg, 3, 0, 0);
            s += 4;
            if (*s == ';') s++;
            continue;
        }
        if (sscanf(s, "%f,%f%n", &x, &y, &n) != 2) break;
        hold = 120;
        { int h, m; if (sscanf(s + n, ",%d%n", &h, &m) == 1) { hold = h; n += m; } }
        sleep(getenv("AOI_TAP_GAP") ? (unsigned)atoi(getenv("AOI_TAP_GAP")) : 4);
        { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
          fprintf(stderr, "[aoiproc] tap %.0f,%.0f at %.3f s, %llu instructions\n", x, y,
                  (double)ts.tv_sec + (double)ts.tv_nsec / 1e9, (unsigned long long)((struct aoi_proc *)arg)->cpu.steps); }
        aoi_proc_touch(arg, 0, x, y);
        for (; hold > 0; hold -= 100) {                   /* a tap: ~0.1 s; a held finger moves a little, as on glass */
            struct timespec ts = { 0, (long)(hold < 100 ? hold : 100) * 1000000 };
            nanosleep(&ts, NULL);
            if (hold > 100) aoi_proc_touch(arg, 2, x + (hold / 100 % 2), y);
        }
        aoi_proc_touch(arg, 1, x, y);
        s += n;
        if (*s == ';') s++;
    }
    if (getenv("AOI_TAPS_THEN_STOP")) {                  /* let the last tap draw, then stop (profile report) */
        sleep((unsigned)atoi(getenv("AOI_TAPS_THEN_STOP")));
        ((struct aoi_proc *)arg)->stop_request = 1;
    }
    return NULL;
}

static void first_frame(void *ctx, const uint8_t *px, uint32_t w, uint32_t h)
{
    static int started;
    pthread_t t;
    struct timespec ts;
    (void)px; (void)w; (void)h;
    clock_gettime(CLOCK_MONOTONIC, &ts);              /* for benchmarks: instructions and time per frame */
    fprintf(stderr, "[frame] %d after %llu instructions, at %.3f s\n", started + 1,
            (unsigned long long)((struct aoi_proc *)ctx)->cpu.steps, (double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
    if (started++) return;
    if (getenv("AOI_PROFILE_FROM_FRAME")) ((struct aoi_proc *)ctx)->nsamples = 0;   /* profile the taps only */
    pthread_create(&t, NULL, taps, ctx);
    pthread_detach(t);
}

static long peak_rss_mib(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return (long)(ru.ru_maxrss >> 20);
#else
    return ru.ru_maxrss >> 10;
#endif
}

static struct aoi_proc proc;
static char stop_at[32];

#ifdef AOI_DEBUG
extern uint32_t aoi_watch_val;
extern void (*aoi_watch_fn)(struct aoi_cpu *c, uint64_t a, uint64_t v, int len);
static struct aoi_proc *watch_proc;
static void watch_print(struct aoi_cpu *c, uint64_t a, uint64_t v, int len)
{
    char w[256], w2[256];
    fprintf(stderr, "[watch] %llu: store%d %#llx <- %#llx at %s lr %s\n", (unsigned long long)c->steps, len,
            (unsigned long long)a, (unsigned long long)v, aoi_proc_where(watch_proc, c->pc, w, sizeof w),
            aoi_proc_where(watch_proc, c->x[30], w2, sizeof w2));
}

#endif

int main(int argc, char **argv)
{
    static const char *base_env[] = {
        "PATH=/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin",
        "ANDROID_ROOT=/system", "ANDROID_DATA=/data", "ANDROID_ART_ROOT=/apex/com.android.art",
        "ANDROID_I18N_ROOT=/apex/com.android.i18n", "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
        "HOME=/", "TMPDIR=/data/local/tmp", NULL };
    const char *envp[64];
    int ne = 0;
    const char *err;
    enum aoi_stop st;
    int a = 1, trace = 0, profile = 0;
    struct timespec t0, t1;
    double secs;

    while (base_env[ne]) { envp[ne] = base_env[ne]; ne++; }
    for (;;) {
        if (argc > a && !strcmp(argv[a], "-t")) { trace = 1; a++; }
        else if (argc > a && !strcmp(argv[a], "-p")) { profile = 1; a++; }
        else if (argc > a + 1 && !strcmp(argv[a], "-e") && ne < 63) { envp[ne++] = argv[a + 1]; a += 2; }
        else break;
    }
    envp[ne] = NULL;
    if (argc - a < 2) { fprintf(stderr, "usage: %s [-t] ROOT PROGRAM [args...]\n", argv[0]); return 2; }
    if (getenv("AOI_SNAPSHOT_LOAD")) {               /* resume a saved process (core/snap.c) instead */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if ((err = aoi_snap_load(&proc, getenv("AOI_SNAPSHOT_LOAD"), argv[a], getenv("AOI_DATA")))) {
            fprintf(stderr, "[aoiproc] snapshot %s: %s\n", getenv("AOI_SNAPSHOT_LOAD"), err);
            return 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        { extern uint64_t aoi_vm_mapped_bytes, aoi_vm_copied_bytes;
          fprintf(stderr, "[aoiproc] snapshot: file pages %llu MB mapped, %llu MB copied\n",
                  (unsigned long long)(aoi_vm_mapped_bytes >> 20), (unsigned long long)(aoi_vm_copied_bytes >> 20)); }
        fprintf(stderr, "[aoiproc] snapshot loaded in %.2f s, at %llu instructions\n",
                (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9,
                (unsigned long long)proc.cpu.steps);
        if (getenv("AOI_STOP_AFTER")) {             /* AOI_STOP_AT, counted from the snapshot */
            snprintf(stop_at, sizeof stop_at, "%llu",
                     (unsigned long long)(proc.cpu.steps + strtoull(getenv("AOI_STOP_AFTER"), NULL, 0)));
            setenv("AOI_STOP_AT", stop_at, 1);
        }
    } else if ((err = aoi_proc_exec(&proc, argv[a], argv[a + 1], argc - a - 1, (const char *const *)argv + a + 1, (const char *const *)envp))) {
        fprintf(stderr, "[aoiproc] exec %s: %s\n", argv[a + 1], err);
        return 1;
    }
    if (getenv("AOI_DATA") && !getenv("AOI_SNAPSHOT_LOAD"))     /* guest /data is this host directory */
        snprintf(proc.data, sizeof proc.data, "%s", getenv("AOI_DATA"));
    if (getenv("AOI_SNAPSHOT_SAVE")) snprintf(proc.snap_path, sizeof proc.snap_path, "%s", getenv("AOI_SNAPSHOT_SAVE"));
    if (trace) proc.trace = stderr;
#ifdef AOI_DEBUG
    { extern uint64_t *aoi_pcring; if (getenv("AOI_PCRING")) aoi_pcring = calloc(1024, 8); }
    if (getenv("AOI_WATCH")) { aoi_watch_val = (uint32_t)strtoul(getenv("AOI_WATCH"), NULL, 0); watch_proc = &proc; aoi_watch_fn = watch_print; }
#endif
    proc.uffd = getenv("AOI_UFFD") && *getenv("AOI_UFFD") == '1';
#ifdef AOI_ORACLE
    if ((err = aoi_oracle_attach(&proc.cpu))) { fprintf(stderr, "oracle: %s\n", err); return 1; }
#endif
    if (profile && (proc.samples = calloc(1 << 20, sizeof *proc.samples))) proc.maxsamples = 1 << 20;
    proc.log = stderr;                               /* guest liblog -> "P/tag: message" */
    if (getenv("AOI_TAPS")) { proc.frame = first_frame; proc.frame_ctx = &proc; }
    if (getenv("AOI_CLIP")) { snprintf(clipbuf, sizeof clipbuf, "%s", getenv("AOI_CLIP")); proc.clip = clip; }
    if (getenv("AOI_SNAPSHOT_LOAD")) aoi_sf_redraw(&proc);      /* the frame it was showing: taps start */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    st = aoi_proc_run(&proc, getenv("AOI_STOP_AT") ? strtoull(getenv("AOI_STOP_AT"), NULL, 0) : 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fflush(stdout);
#ifdef AOI_ORACLE
    aoi_oracle_report(&proc.cpu, st);
#endif
    if (getenv("AOI_DUMP")) {                        /* AOI_DUMP=addr,len,file: guest memory at the end */
        unsigned long long da = 0, dl = 0;
        char path[512];
        FILE *df;
        if (sscanf(getenv("AOI_DUMP"), "%llx,%llx,%511s", &da, &dl, path) == 3 && (df = fopen(path, "wb"))) {
            unsigned long long o;
            for (o = 0; o < dl; o += 4096) {
                static uint8_t pg[4096];
                if (!aoi_vm_read(&proc.vm, da + o, pg, 4096, 0)) memset(pg, 0xee, 4096);
                fwrite(pg, 1, 4096, df);
            }
            fclose(df);
        }
    }
    {
        char w1[256], w2[256];
        if (st != AOI_STOP_EXIT)
            fprintf(stderr, "[aoiproc] pc in %s, lr in %s\n", aoi_proc_where(&proc, proc.cpu.pc, w1, sizeof w1),
                    aoi_proc_where(&proc, proc.cpu.x[30], w2, sizeof w2));
    }
    secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (proc.samples) {                              /* samples per library, most first */
        static char names[256][256];
        static size_t counts[256];
        size_t i, n = 0, j;
        for (i = 0; i < proc.nsamples; i++) {
            char w[256], *plus;
            aoi_proc_where(&proc, proc.samples[i], w, sizeof w);
            if (getenv("AOI_PROFILE_RAW")) fprintf(stderr, "[sample] %s\n", w);
            if ((plus = strrchr(w, '+'))) *plus = 0;
            for (j = 0; j < n && strcmp(names[j], w); j++) {}
            if (j == n && n < 256) { snprintf(names[n], sizeof names[n], "%s", w); n++; }
            if (j < 256) counts[j]++;
        }
        for (;;) {
            size_t best = 0, bi = 0;
            for (j = 0; j < n; j++) if (counts[j] > best) { best = counts[j]; bi = j; }
            if (!best || best * 200 < proc.nsamples) break;
            fprintf(stderr, "[profile] %5.1f%%  %s\n", 100.0 * (double)best / (double)proc.nsamples, names[bi]);
            counts[bi] = 0;
        }
    }
    switch (st) {
    case AOI_STOP_EXIT:
        fprintf(stderr, "[aoiproc] exit %d, %" PRIu64 " instructions, %.2f s, %llu MiB of host chunks, peak RSS %ld MiB\n",
                proc.cpu.exit_code, proc.cpu.steps, secs, (unsigned long long)(proc.vm.nchunks * (AOI_VM_CHUNK >> 20)),
                peak_rss_mib());
        return proc.cpu.exit_code;
    case AOI_STOP_UNDEF:
        fprintf(stderr, "[aoiproc] undefined instruction %#010x at pc=%#" PRIx64 " after %" PRIu64 " instructions\n",
                proc.cpu.fault_insn, proc.cpu.pc, proc.cpu.steps);
        return 3;
    case AOI_STOP_FAULT:
        fprintf(stderr, "[aoiproc] fault at %#" PRIx64 " (pc=%#" PRIx64 ", lr=%#" PRIx64 ") after %" PRIu64 " instructions\n",
                proc.cpu.fault_addr, proc.cpu.pc, proc.cpu.x[30], proc.cpu.steps);
        return 5;
    default:
        {
            char w[256];
            fprintf(stderr, "[aoiproc] stopped (%d) at pc=%#" PRIx64 " in %s, x7=%#" PRIx64 ", %llu MiB of host chunks, peak RSS %ld MiB\n",
                    (int)st, proc.cpu.pc, aoi_proc_where(&proc, proc.cpu.pc, w, sizeof w), proc.cpu.x[7],
                    (unsigned long long)(proc.vm.nchunks * (AOI_VM_CHUNK >> 20)), peak_rss_mib());
        }
        return 6;
    }
}
