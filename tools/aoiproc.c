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
 * (aoi_proc_touch from another thread, as the iOS view sends them). */
static void *taps(void *arg)
{
    const char *s = getenv("AOI_TAPS");
    float x, y;
    int n;
    while (sscanf(s, "%f,%f%n", &x, &y, &n) == 2) {
        sleep(getenv("AOI_TAP_GAP") ? (unsigned)atoi(getenv("AOI_TAP_GAP")) : 4);
        fprintf(stderr, "[aoiproc] tap %.0f,%.0f\n", x, y);
        aoi_proc_touch(arg, 0, x, y);
        { struct timespec ts = { 0, 120000000 }; nanosleep(&ts, NULL); }   /* a finger stays ~0.1 s */
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
    (void)px; (void)w; (void)h;
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
    if ((err = aoi_proc_exec(&proc, argv[a], argv[a + 1], argc - a - 1, (const char *const *)argv + a + 1, (const char *const *)envp))) {
        fprintf(stderr, "[aoiproc] exec %s: %s\n", argv[a + 1], err);
        return 1;
    }
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
            fprintf(stderr, "[aoiproc] stopped (%d) at pc=%#" PRIx64 " in %s, x7=%#" PRIx64 "\n", (int)st, proc.cpu.pc,
                    aoi_proc_where(&proc, proc.cpu.pc, w, sizeof w), proc.cpu.x[7]);
        }
        return 6;
    }
}
