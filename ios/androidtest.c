/* On-device test of step 1: an unmodified Android program, loaded by Android's
 * own linker64, run by core/proc.c in the interpreter. The guest's stdout and
 * stderr go to a temporary file whose text is logged. Shared by the iOS app and
 * tools/iostest.c. */
#define _POSIX_C_SOURCE 200809L
#include "androidtest.h"
#include "../core/proc.h"

#include <fcntl.h>
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

int aoi_android_run(const char *root, const char *tmpdir, int argc, const char *const *argv,
                    aoi_log_fn log, void *ctx)
{
    static const char *const envp[] = {
        "PATH=/system/bin", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", "HOME=/",
        "TMPDIR=/data/local/tmp", NULL };
    struct aoi_proc *p = calloc(1, sizeof *p);
    char tmp[1024], line[1024];
    const char *err;
    enum aoi_stop st;
    struct timespec t0, t1;
    double secs;
    int fd, rc = -1;
    FILE *f;

    if (!p) { say(log, ctx, "android: out of memory"); return -1; }
    snprintf(tmp, sizeof tmp, "%s/aoi-out-XXXXXX", tmpdir);
    if ((fd = mkstemp(tmp)) < 0) { say(log, ctx, "android: cannot create %s", tmp); free(p); return -1; }
    if ((err = aoi_proc_exec(p, root, argv[0], argc, argv, envp))) {
        say(log, ctx, "android: exec %s: %s", argv[0], err);
        aoi_proc_free(p); free(p); close(fd); unlink(tmp);
        return -1;
    }
    p->fd[1].host = fd;                 /* guest stdout/stderr -> the temporary file */
    p->fd[2].host = fd;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    st = aoi_proc_run(p, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    if ((f = fdopen(fd, "r"))) {
        rewind(f);
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            if (!strstr(line, "ld.config.txt")) say(log, ctx, "  | %s", line);   /* known: no linkerconfig yet */
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
    aoi_proc_free(p);
    free(p);
    return rc;
}
