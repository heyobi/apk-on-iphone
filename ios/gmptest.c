#define _GNU_SOURCE
#include "gmptest.h"
#include "jitmem.h"
#include "../core/bionic.h"
#include "../core/cpu.h"
#include "../core/dl.h"
#include "../core/native.h"

#include <dlfcn.h>
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
    LOG("interp: %" PRIu64 " guest instructions", cpu.steps);
    return out;
fail:
    LOG("interp: %s", why(&cpu, st, why_buf, sizeof why_buf));
    return NULL;
}

/* ---------------- native ---------------- */

typedef int (*probe_fn)(void);

int aoi_jit_probe(int strategy, aoi_log_fn log, void *ctx)
{
    static const uint32_t code[2] = { 0x52800540u, 0xd65f03c0u };   /* mov w0, #42; ret */
    const size_t page = 0x4000;
    const char *err = NULL;
    uint8_t *m = aoi_jit_reserve(strategy, 2 * page, &err);
    int r;
    if (!m) { LOG("  %s: %s", aoi_jit_name(strategy), err); return 0; }
    memcpy(m, code, sizeof code);
    if ((err = aoi_jit_seal(strategy, m, page, 2 * page))) {
        LOG("  %s: %s", aoi_jit_name(strategy), err);
        aoi_jit_release(strategy, m, 2 * page);
        return 0;
    }
    r = ((probe_fn)(uintptr_t)m)();
    aoi_jit_release(strategy, m, 2 * page);
    LOG("  %s: returned %d %s", aoi_jit_name(strategy), r, r == 42 ? "(works)" : "(WRONG)");
    return r == 42;
}

/* bionic -> host libc. Same names and calling convention for these; the few
 * that differ are listed. Variadic functions differ on Darwin (stack-passed
 * varargs) and va_list differs too, so those trap instead of corrupting. */
static void unsupported_import(void) { fprintf(stderr, "apk-on-iphone: unsupported variadic import called\n"); abort(); }
static int no_atfork(void) { return 0; }

static void *resolve(const char *name, void *ctx)
{
    static const char *const variadic[] = { "printf", "fprintf", "snprintf", "sprintf", "sscanf", "fscanf",
                                            "vfprintf", "vsnprintf", "vsprintf", "vprintf", NULL };
    int i;
    (void)ctx;
    for (i = 0; variadic[i]; i++) if (!strcmp(name, variadic[i])) return (void *)(uintptr_t)unsupported_import;
    if (!strcmp(name, "__register_atfork")) return (void *)(uintptr_t)no_atfork;
#ifdef __APPLE__
    { extern FILE *__stdinp, *__stdoutp, *__stderrp;
      if (!strcmp(name, "stdin")) return &__stdinp;
      if (!strcmp(name, "stdout")) return &__stdoutp;
      if (!strcmp(name, "stderr")) return &__stderrp; }
#else
    if (!strcmp(name, "stdin")) return &stdin;
    if (!strcmp(name, "stdout")) return &stdout;
    if (!strcmp(name, "stderr")) return &stderr;
#endif
    return dlsym(RTLD_DEFAULT, name);
}

struct mpz { int alloc, size; void *d; };
typedef void (*mpz_init_fn)(struct mpz *);
typedef void (*mpz_fac_fn)(struct mpz *, unsigned long);
typedef char *(*mpz_get_str_fn)(char *, int, const struct mpz *);

char *aoi_gmp_native(const void *so, size_t size, int strategy, unsigned long n, int execute,
                     double *secs, aoi_log_fn log, void *ctx)
{
    struct aoi_native_layout lay;
    const char *err, *missing = NULL;
    uint8_t *base;
    void *fi, *ff, *fg;
    char *s, *out;
    struct mpz z;
    double t0;

    if ((err = aoi_native_layout(so, size, &lay))) { LOG("native: %s", err); return NULL; }
    if (!lay.text_ok) { LOG("native: code and data share a page; cannot protect them separately"); return NULL; }
    if (!(base = aoi_jit_reserve(strategy, lay.span, &err))) { LOG("native: %s", err); return NULL; }
    if ((err = aoi_native_link(so, size, base, resolve, NULL, &missing))) {
        LOG("native: %s%s%s", err, missing ? ": " : "", missing ? missing : "");
        aoi_jit_release(strategy, base, lay.span);
        return NULL;
    }
    fi = aoi_native_sym(so, size, base, "__gmpz_init");
    ff = aoi_native_sym(so, size, base, "__gmpz_fac_ui");
    fg = aoi_native_sym(so, size, base, "__gmpz_get_str");
    LOG("native: libgmp.so linked at %p (%" PRIu64 " KB, code %" PRIu64 " KB)", (void *)base, lay.span >> 10, lay.text_end >> 10);
    if (!fi || !ff || !fg) { LOG("native: GMP symbols not found"); aoi_jit_release(strategy, base, lay.span); return NULL; }
    if (!execute) { aoi_jit_release(strategy, base, lay.span); return NULL; }
    if ((err = aoi_jit_seal(strategy, base, lay.text_end, lay.span))) {
        LOG("native: %s", err);
        aoi_jit_release(strategy, base, lay.span);
        return NULL;
    }
    t0 = now();
    ((mpz_init_fn)(uintptr_t)fi)(&z);
    ((mpz_fac_fn)(uintptr_t)ff)(&z, n);
    s = ((mpz_get_str_fn)(uintptr_t)fg)(NULL, 10, &z);
    *secs = now() - t0;
    out = s ? strdup(s) : NULL;
    free(s);          /* GMP allocated it with the host malloc we bound */
    /* the image stays mapped: z's limbs were allocated through it; this is a test */
    return out;
}
