/* On-device test of step 1: an unmodified Android program, loaded by Android's
 * own linker64, run by core/proc.c in the interpreter. The guest's stdout and
 * stderr go to a temporary file whose text is logged. Shared by the iOS app and
 * tools/iostest.c. */
#define _POSIX_C_SOURCE 200809L
#include "androidtest.h"
#include "../core/proc.h"

#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <sys/resource.h>
#endif
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
