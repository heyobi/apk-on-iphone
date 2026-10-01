#define _POSIX_C_SOURCE 200809L
#include "gmptest.h"
#include "../core/bionic.h"
#include "../core/cpu.h"
#include "../core/dl.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void logf_(aoi_log_fn log, void *ctx, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log(ctx, buf);
}
#define LOG(...) logf_(log, ctx, __VA_ARGS__)

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* ---------------- interpreter ---------------- */

static const char *why(struct aoi_cpu *c, enum aoi_stop st, char *buf, size_t n)
{
    switch (st) {
    case AOI_STOP_UNDEF:  snprintf(buf, n, "undefined instruction 0x%08x at 0x%" PRIx64, c->fault_insn, c->pc); break;
    case AOI_STOP_FAULT:  snprintf(buf, n, "fault at 0x%" PRIx64 " (pc 0x%" PRIx64 ")", c->fault_addr, c->pc); break;
    case AOI_STOP_IMPORT: snprintf(buf, n, "missing import %s", c->stop_name); break;
    default:              snprintf(buf, n, "stopped (%d)", (int)st); break;
    }
    return buf;
}

char *aoi_gmp_interp(const void *so, size_t size, unsigned long n, double *secs, aoi_log_fn log, void *ctx)
{
    static struct aoi_dl dl;
    struct aoi_cpu cpu;
    const char *err;
    char why_buf[128], *out;
    uint64_t f_init, f_fac, f_get, z, s, a[3], len = 0;
    enum aoi_stop st;
    uint8_t *p;
    double t0;

    if ((err = aoi_bionic_init(&dl)) || (err = aoi_dl_load(&dl, "libgmp.so", so, size))) { LOG("interp: %s", err); return NULL; }
    f_init = aoi_dl_sym(&dl, "__gmpz_init");
    f_fac = aoi_dl_sym(&dl, "__gmpz_fac_ui");
    f_get = aoi_dl_sym(&dl, "__gmpz_get_str");
    if (!f_init || !f_fac || !f_get) { LOG("interp: GMP symbols not found"); return NULL; }
    aoi_dl_cpu(&dl, &cpu);
    z = aoi_dl_malloc(&dl, 16);
    t0 = now();
    a[0] = z;                         if ((st = aoi_call(&cpu, f_init, a, 1, 0)) != AOI_STOP_RETURN) goto fail;
    a[0] = z; a[1] = n;               if ((st = aoi_call(&cpu, f_fac, a, 2, 0)) != AOI_STOP_RETURN) goto fail;
    a[0] = 0; a[1] = 10; a[2] = z;    if ((st = aoi_call(&cpu, f_get, a, 3, 0)) != AOI_STOP_RETURN) goto fail;
    *secs = now() - t0;
    s = cpu.x[0];
    while ((p = aoi_mem_ptr(&dl.mem, s + len, 1)) && *p) len++;
    if (!(out = malloc(len + 1)) || !(p = aoi_mem_ptr(&dl.mem, s, len + 1))) { free(out); return NULL; }
    memcpy(out, p, len + 1);
    LOG("interp: %" PRIu64 " guest instructions, %.1f M/s", cpu.steps, *secs > 0 ? (double)cpu.steps / *secs / 1e6 : 0.0);
    return out;
fail:
    LOG("interp: %s", why(&cpu, st, why_buf, sizeof why_buf));
    return NULL;
}

