#define _POSIX_C_SOURCE 200809L
/* aoiproc: run an unmodified Android program, with Android's own linker64,
 * in the interpreter. usage: aoiproc [-t] ROOT PROGRAM [args...]
 * ROOT is a guest root made by tools/android-root.sh; PROGRAM is a guest path
 * such as /system/bin/toybox. -t logs every syscall to stderr. */
#include "../core/proc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct aoi_proc proc;

int main(int argc, char **argv)
{
    static const char *const envp[] = {
        "PATH=/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin",
        "ANDROID_ROOT=/system", "ANDROID_DATA=/data", "ANDROID_ART_ROOT=/apex/com.android.art",
        "ANDROID_I18N_ROOT=/apex/com.android.i18n", "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
        "HOME=/", "TMPDIR=/data/local/tmp", NULL };
    const char *err;
    enum aoi_stop st;
    int a = 1, trace = 0;
    struct timespec t0, t1;
    double secs;

    if (argc > 1 && !strcmp(argv[1], "-t")) { trace = 1; a++; }
    if (argc - a < 2) { fprintf(stderr, "usage: %s [-t] ROOT PROGRAM [args...]\n", argv[0]); return 2; }
    if ((err = aoi_proc_exec(&proc, argv[a], argv[a + 1], argc - a - 1, (const char *const *)argv + a + 1, envp))) {
        fprintf(stderr, "[aoiproc] exec %s: %s\n", argv[a + 1], err);
        return 1;
    }
    if (trace) proc.trace = stderr;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    st = aoi_proc_run(&proc, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fflush(stdout);
    {
        char w1[256], w2[256];
        if (st != AOI_STOP_EXIT)
            fprintf(stderr, "[aoiproc] pc in %s, lr in %s\n", aoi_proc_where(&proc, proc.cpu.pc, w1, sizeof w1),
                    aoi_proc_where(&proc, proc.cpu.x[30], w2, sizeof w2));
    }
    secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    switch (st) {
    case AOI_STOP_EXIT:
        fprintf(stderr, "[aoiproc] exit %d, %" PRIu64 " instructions, %.2f s\n", proc.cpu.exit_code, proc.cpu.steps, secs);
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
        fprintf(stderr, "[aoiproc] stopped (%d) at pc=%#" PRIx64 "\n", (int)st, proc.cpu.pc);
        return 6;
    }
}
