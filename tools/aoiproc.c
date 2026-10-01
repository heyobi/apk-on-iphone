#define _POSIX_C_SOURCE 200809L
/* aoiproc: run an unmodified Android program, with Android's own linker64,
 * in the interpreter. usage: aoiproc [-t] [-e NAME=VALUE]... ROOT PROGRAM [args...]
 * ROOT is a guest root made by tools/android-root.sh; PROGRAM is a guest path
 * such as /system/bin/toybox. -t logs every syscall to stderr; -e adds to the
 * guest environment; -p prints where the guest spent its instructions
 * (one sample per 100k-instruction time slice, grouped by library).
 * Debug environment: AOI_UFFD=1 offers userfaultfd (ART: CMC GC + boot image),
 * AOI_STOP_AT=N stops after N instructions, AOI_DUMP=addr,len,file saves guest
 * memory at the end. */
#include "../core/proc.h"
#ifdef AOI_ORACLE
#include "../core/oracle.h"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct aoi_proc proc;

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
    proc.uffd = getenv("AOI_UFFD") && *getenv("AOI_UFFD") == '1';
#ifdef AOI_ORACLE
    if ((err = aoi_oracle_attach(&proc.cpu))) { fprintf(stderr, "oracle: %s\n", err); return 1; }
#endif
    if (profile && (proc.samples = calloc(1 << 20, sizeof *proc.samples))) proc.maxsamples = 1 << 20;
    proc.log = stderr;                               /* guest liblog -> "P/tag: message" */
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
        fprintf(stderr, "[aoiproc] exit %d, %" PRIu64 " instructions, %.2f s, %llu MiB of host chunks\n",
                proc.cpu.exit_code, proc.cpu.steps, secs, (unsigned long long)(proc.vm.nchunks * (AOI_VM_CHUNK >> 20)));
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
