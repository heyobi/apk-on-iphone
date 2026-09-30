#include "bionic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DL(cpu) ((struct aoi_dl *)(cpu)->host_ctx)

static uint8_t *G(struct aoi_cpu *c, uint64_t a, uint64_t n)
{
    uint8_t *p = aoi_mem_ptr(c->mem, a, n ? n : 1);
    if (!p) { c->stop = AOI_STOP_FAULT; c->fault_addr = a; }
    return p;
}

static uint64_t guest_strlen(struct aoi_cpu *c, uint64_t a)
{
    uint64_t n = 0;
    uint8_t *p;
    while ((p = G(c, a + n, 1)) && *p) n++;
    return n;
}

static uint64_t h_malloc(struct aoi_cpu *c) { return aoi_dl_malloc(DL(c), c->x[0]); }
static uint64_t h_calloc(struct aoi_cpu *c)
{
    uint64_t n = c->x[0] * c->x[1], p = aoi_dl_malloc(DL(c), n);
    uint8_t *h = p ? G(c, p, n) : NULL;
    if (h) memset(h, 0, n);
    return p;
}
static uint64_t h_free(struct aoi_cpu *c) { (void)c; return 0; }   /* bump heap: never reused */
static uint64_t h_realloc(struct aoi_cpu *c)
{
    uint64_t old = c->x[0], size = c->x[1], np, oldsize = 0;
    uint8_t *src, *dst;
    if (!old) return aoi_dl_malloc(DL(c), size);
    if ((src = G(c, old - 16, 8))) memcpy(&oldsize, src, 8);
    if (!(np = aoi_dl_malloc(DL(c), size))) return 0;
    if (oldsize > size) oldsize = size;
    if (oldsize && (src = G(c, old, oldsize)) && (dst = G(c, np, oldsize))) memcpy(dst, src, oldsize);
    return np;
}
static uint64_t h_memcpy(struct aoi_cpu *c)
{
    uint8_t *d, *s;
    if (c->x[2] && (d = G(c, c->x[0], c->x[2])) && (s = G(c, c->x[1], c->x[2]))) memmove(d, s, c->x[2]);
    return c->x[0];
}
static uint64_t h_memset(struct aoi_cpu *c)
{
    uint8_t *d;
    if (c->x[2] && (d = G(c, c->x[0], c->x[2]))) memset(d, (int)c->x[1], c->x[2]);
    return c->x[0];
}
static uint64_t h_strlen(struct aoi_cpu *c) { return guest_strlen(c, c->x[0]); }
static uint64_t h_strchr(struct aoi_cpu *c)
{
    uint64_t a = c->x[0];
    uint8_t *p, ch = (uint8_t)c->x[1];
    for (;; a++) {
        if (!(p = G(c, a, 1))) return 0;
        if (*p == ch) return a;
        if (!*p) return 0;
    }
}
static uint64_t h_abort(struct aoi_cpu *c) { c->stop = AOI_STOP_EXIT; c->exit_code = 134; return 0; }
static uint64_t h_zero(struct aoi_cpu *c) { (void)c; return 0; }   /* __cxa_atexit & co: accept */

static uint64_t lconv_addr;
static uint64_t h_localeconv(struct aoi_cpu *c) { (void)c; return lconv_addr; }

static struct aoi_host_sym table[] = {
    { "malloc", h_malloc, 0 },   { "calloc", h_calloc, 0 },  { "realloc", h_realloc, 0 },
    { "free", h_free, 0 },       { "memcpy", h_memcpy, 0 },  { "memmove", h_memcpy, 0 },
    { "memset", h_memset, 0 },   { "strlen", h_strlen, 0 },  { "strchr", h_strchr, 0 },
    { "abort", h_abort, 0 },     { "__cxa_atexit", h_zero, 0 }, { "__cxa_finalize", h_zero, 0 },
    { "__register_atfork", h_zero, 0 }, { "localeconv", h_localeconv, 0 },
    { "stdin", NULL, 0 }, { "stdout", NULL, 0 }, { "stderr", NULL, 0 },
};

const char *aoi_bionic_init(struct aoi_dl *dl)
{
    const char *err;
    size_t i, n = sizeof(table) / sizeof(table[0]);
    uint64_t files, dot;
    uint8_t *p;

    if ((err = aoi_dl_init(dl, table, (int)n))) return err;
    /* stdin/stdout/stderr: each a guest pointer variable to a dummy FILE */
    files = aoi_dl_malloc(dl, 3 * 8 + 3 * 256);
    for (i = 0; i < 3; i++) {
        uint64_t fileobj = files + 24 + i * 256;
        p = aoi_mem_ptr(&dl->mem, files + i * 8, 8);
        memcpy(p, &fileobj, 8);
        table[n - 3 + i].data_addr = files + i * 8;
    }
    /* struct lconv: decimal_point, thousands_sep, grouping, ... all "." / "" */
    lconv_addr = aoi_dl_malloc(dl, 96 + 16);
    dot = lconv_addr + 96;
    p = aoi_mem_ptr(&dl->mem, dot, 2); p[0] = '.'; p[1] = 0;
    for (i = 0; i < 12; i++) {
        uint64_t v = i == 0 ? dot : dot + 1;
        memcpy(aoi_mem_ptr(&dl->mem, lconv_addr + i * 8, 8), &v, 8);
    }
    return NULL;
}
