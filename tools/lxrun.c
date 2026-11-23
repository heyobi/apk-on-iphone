#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
/* lxrun: run a program from an ordinary aarch64 Linux root file system (Alpine's
 * minirootfs, a Debian rootfs) in the interpreter, as root, with a Linux
 * environment instead of Android's. The "Linux userland" mode of the core: the same
 * cpu, vm, proc and ELF loader as aoiproc, none of the Android services.
 *
 * usage: lxrun [-t] [-e NAME=VALUE]... [-C DIR] ROOT PROGRAM [args...]
 * -t logs every syscall to stderr (AOI_CHILD_TRACE=file adds child processes);
 * -C sets the starting directory (a guest path, default /root).
 * AOI_SNAPSHOT_SAVE=file saves the process when it opens /dev/aoi_snapshot;
 * AOI_SNAPSHOT_LOAD=file resumes one instead of starting PROGRAM. */
#include "../core/proc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

static struct aoi_proc proc;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
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

int main(int argc, char **argv)
{
    static const char *base_env[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root", "USER=root", "LOGNAME=root", "SHELL=/bin/sh", "LANG=C.UTF-8", NULL };
    const char *envp[80], *cwd = "/root", *err;
    char term[64];
    int ne = 0, a = 1, trace = 0, quiet = getenv("LXRUN_QUIET") != NULL;
    enum aoi_stop st;
    double t0, t1;
    while (base_env[ne]) { envp[ne] = base_env[ne]; ne++; }
    snprintf(term, sizeof term, "TERM=%s", getenv("TERM") ? getenv("TERM") : "xterm-256color");
    envp[ne++] = term;
    for (;;) {
        if (argc > a && !strcmp(argv[a], "-t")) { trace = 1; a++; }
        else if (argc > a + 1 && !strcmp(argv[a], "-e") && ne < 79) { envp[ne++] = argv[a + 1]; a += 2; }
        else if (argc > a + 1 && !strcmp(argv[a], "-C")) { cwd = argv[a + 1]; a += 2; }
        else break;
    }
    envp[ne] = NULL;
    if (argc - a < 2 && !(argc - a == 1 && getenv("AOI_SNAPSHOT_LOAD"))) {
        fprintf(stderr, "usage: %s [-t] [-e NAME=VALUE]... [-C DIR] ROOT PROGRAM [args...]\n", argv[0]);
        return 2;
    }
    t0 = now_s();
    if (getenv("AOI_SNAPSHOT_LOAD")) {
        if ((err = aoi_snap_load(&proc, getenv("AOI_SNAPSHOT_LOAD"), argv[a], NULL))) {
            fprintf(stderr, "[lxrun] snapshot %s: %s\n", getenv("AOI_SNAPSHOT_LOAD"), err);
            return 1;
        }
        if (!quiet) fprintf(stderr, "[lxrun] snapshot loaded in %.3f s\n", now_s() - t0);
    } else {
        if ((err = aoi_proc_exec(&proc, argv[a], argv[a + 1], argc - a - 1, (const char *const *)argv + a + 1,
                                 (const char *const *)envp))) {
            fprintf(stderr, "[lxrun] exec %s: %s\n", argv[a + 1], err);
            return 1;
        }
        aoi_proc_linux(&proc, cwd);
    }
    if (getenv("AOI_SNAPSHOT_SAVE")) snprintf(proc.snap_path, sizeof proc.snap_path, "%s", getenv("AOI_SNAPSHOT_SAVE"));
    if (trace) proc.trace = stderr;
    proc.log = quiet ? NULL : stderr;
    t0 = now_s();
    st = aoi_proc_run(&proc, 0);
    t1 = now_s();
    fflush(stdout);
    if (st == AOI_STOP_EXIT) {
        if (!quiet || getenv("LXRUN_STATS"))
            fprintf(stderr, "[lxrun] exit %d, %" PRIu64 " instructions, %.2f s, peak RSS %ld MiB\n",
                    proc.cpu.exit_code, proc.cpu.steps, t1 - t0, peak_rss_mib());
        return proc.cpu.exit_code & 0xff;
    }
    {
        char w1[256], w2[256];
        fprintf(stderr, "[lxrun] stopped (%d) at pc %s, lr %s, after %" PRIu64 " instructions", (int)st,
                aoi_proc_where(&proc, proc.cpu.pc, w1, sizeof w1), aoi_proc_where(&proc, proc.cpu.x[30], w2, sizeof w2),
                proc.cpu.steps);
        if (st == AOI_STOP_UNDEF) fprintf(stderr, ": undefined instruction %#010x", proc.cpu.fault_insn);
        if (st == AOI_STOP_FAULT) fprintf(stderr, ": fault at %#" PRIx64, proc.cpu.fault_addr);
        fprintf(stderr, "\n");
    }
    return 128 + 11;
}
