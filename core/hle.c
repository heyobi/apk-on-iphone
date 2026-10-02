/* Native stand-ins for hot guest functions ("high-level emulation").
 *
 * Software rendering spends most of an app's frame in Skia's raster pipeline
 * (libhwui.so): small "stage" functions, each run for 4 pixels at a time, chained by
 * tail calls. In the interpreter a stage costs some 200 host instructions per guest
 * instruction; here the same stage is a few dozen host instructions.
 *
 * The guest system image is fixed, so each stage is known by its offset in libhwui.so
 * and checked by a hash of its code before it is replaced (a different libhwui just
 * runs interpreted). A stage's convention (Skia's highp ABI on arm64): x0 points at
 * the program, {fn, ctx} pairs; x1, x2 = dx, dy; v0-v3 = r, g, b, a and v4-v7 = dr,
 * dg, db, da, 4 float lanes each. A stage reads its ctx from [x0 + 8], advances x0 by
 * 16 and branches to the next fn ("ldr x4, [x0, #16]!; br x4"); the program ends in
 * just_return ("ret").
 *
 * Exactness: each stand-in does what the stage's own instructions do, in the same
 * order (fused multiply-adds where the code has fmla, plain multiply then add where it
 * has fmul and fadd; this file must not contract them), with ARM's min/max on zeros.
 * Inputs or results the host could treat differently (NaNs, a non-default FPCR,
 * memory that is not plain) make it decline: the stage is then interpreted.
 * AOI_HLE_CHECK=n runs every n-th stand-in call against the interpreter and reports
 * differences (host testing; 1: all of them); AOI_HLE=0 turns them off. */
#include "hle.h"
#include "cpu_impl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#define ST_FN static int

/* ---------- registers and memory ---------- */

static void getf(const struct aoi_cpu *c, int r, float f[4]) { memcpy(f, c->vreg[r], 16); }
static void setf(struct aoi_cpu *c, int r, const float f[4]) { memcpy(c->vreg[r], f, 16); }
static void setu(struct aoi_cpu *c, int r, const uint32_t u[4]) { memcpy(c->vreg[r], u, 16); }
static void dupf(struct aoi_cpu *c, int r, float x) { float f[4] = { x, x, x, x }; setf(c, r, f); }

static int nan4(const float f[4]) { return f[0] != f[0] || f[1] != f[1] || f[2] != f[2] || f[3] != f[3]; }

/* A plain read of guest memory (no fault, no straddled chunk): 0 = decline. */
static int hrd(struct aoi_cpu *c, uint64_t a, void *out, int len)
{
    const uint8_t *p = gptr(c, a, len, AOI_PROT_R);
    if (!p) return 0;
    memcpy(out, p, (size_t)len);
    return 1;
}

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* ARM fmax/fmin for non-NaN operands: equal values pick +0 over -0 (max) or -0 (min). */
static float amax(float x, float y) { return x == y ? bitsf(fbits(x) & fbits(y)) : x > y ? x : y; }
static float amin(float x, float y) { return x == y ? bitsf(fbits(x) | fbits(y)) : x < y ? x : y; }

/* The pixel address of a SkRasterPipeline_MemoryCtx {pixels, stride}: row dy, column dx. */
static int pixel_addr(struct aoi_cpu *c, uint64_t ctx, uint64_t *a)
{
    uint64_t pixels;
    int32_t stride;
    if (!hrd(c, ctx, &pixels, 8) || !hrd(c, ctx + 8, &stride, 4)) return 0;
    *a = pixels + (((uint64_t)(int64_t)stride * c->x[2]) << 2) + (c->x[1] << 2);
    return 1;
}

/* ---------- the stages (libhwui.so of the bundled image) ---------- */

/* seed_shader: r = dx + {0.5, 1.5, 2.5, 3.5}, g = dy + 0.5, b = 1, a = 0. */
ST_FN seed_shader(struct aoi_cpu *c, uint64_t ctx)
{
    float k[4], r[4], g, x;
    int i;
    (void)ctx;
    if (!hrd(c, c->hle_base + 0x16b30, k, 16)) return 0;
    x = (float)(int32_t)(uint32_t)c->x[1];
    g = (float)(int32_t)(uint32_t)c->x[2] + 0.5f;
    for (i = 0; i < 4; i++) r[i] = x + k[i];
    setf(c, 0, r); dupf(c, 1, g); dupf(c, 2, 1.0f); dupf(c, 3, 0.0f);
    return 1;
}

/* matrix_2x3: r' = r*m0 + (g*m1 + m2), g' = r*m3 + (g*m4 + m5), fused. */
ST_FN matrix_2x3(struct aoi_cpu *c, uint64_t ctx)
{
    float m[6], r[4], g[4], nr[4], ng[4];
    int i;
    if (!hrd(c, ctx, m, 24)) return 0;
    getf(c, 0, r); getf(c, 1, g);
    for (i = 0; i < 4; i++) {
        nr[i] = fmaf(r[i], m[0], fmaf(g[i], m[1], m[2]));
        ng[i] = fmaf(r[i], m[3], fmaf(g[i], m[4], m[5]));
    }
    if (nan4(nr) || nan4(ng)) return 0;
    setf(c, 0, nr); setf(c, 1, ng);
    return 1;
}

/* evenly_spaced_2_stop_gradient: {f[4], b[4]}; channel i = t*f[i] + b[i], t = r. */
ST_FN gradient_2stop(struct aoi_cpu *c, uint64_t ctx)
{
    float k[8], t[4], o[4][4];
    int i, j;
    if (!hrd(c, ctx, k, 32)) return 0;
    getf(c, 0, t);
    for (j = 0; j < 4; j++) {
        for (i = 0; i < 4; i++) o[j][i] = fmaf(t[i], k[j], k[4 + j]);
        if (nan4(o[j])) return 0;
    }
    for (j = 0; j < 4; j++) setf(c, j, o[j]);
    return 1;
}

/* clamp_x_1: r = min(max(r, 0), 1). */
ST_FN clamp_x_1(struct aoi_cpu *c, uint64_t ctx)
{
    float r[4];
    int i;
    (void)ctx;
    getf(c, 0, r);
    if (nan4(r)) return 0;
    for (i = 0; i < 4; i++) r[i] = amin(amax(r[i], 0.0f), 1.0f);
    setf(c, 0, r);
    return 1;
}

/* clamp_01: r, g, b, a = min(max(x, 0), 1). */
ST_FN clamp_01(struct aoi_cpu *c, uint64_t ctx)
{
    float v[4][4];
    int i, j;
    (void)ctx;
    for (j = 0; j < 4; j++) { getf(c, j, v[j]); if (nan4(v[j])) return 0; }
    for (j = 0; j < 4; j++) {
        for (i = 0; i < 4; i++) v[j][i] = amin(amax(v[j][i], 0.0f), 1.0f);
        setf(c, j, v[j]);
    }
    return 1;
}

/* dither: an 8x8 ordered-dither value from (dx + lane, dy), scaled by *ctx (rate),
 * added to r, g, b, then clamped to [0, a]. */
ST_FN dither(struct aoi_cpu *c, uint64_t ctx)
{
    uint32_t iota[4];
    float rate, d[4], v[3][4], a[4];
    int i, j;
    if (!hrd(c, c->hle_base + 0x16400, iota, 16) || !hrd(c, ctx, &rate, 4)) return 0;
    getf(c, 3, a);
    for (i = 0; i < 4; i++) {
        uint32_t X = (uint32_t)c->x[1] + iota[i], M = X ^ (uint32_t)c->x[2];
        uint32_t D = ((X << 4) & 16) | ((X >> 2) & 1) | ((X << 1) & 4) | ((M << 5) & 32) | ((M << 2) & 8) | ((M >> 1) & 2);
        float f = (float)(int32_t)D * bitsf(0x3c800000u);           /* 1/64 */
        f = f + bitsf(0xbefc0000u);                                 /* - 63/128 */
        d[i] = f * rate;
    }
    if (nan4(d) || nan4(a)) return 0;
    for (j = 0; j < 3; j++) {
        getf(c, j, v[j]);
        for (i = 0; i < 4; i++) v[j][i] = d[i] + v[j][i];
        if (nan4(v[j])) return 0;
    }
    for (j = 0; j < 3; j++) {
        for (i = 0; i < 4; i++) v[j][i] = amax(amin(v[j][i], a[i]), 0.0f);
        setf(c, j, v[j]);
    }
    return 1;
}

/* load_8888_dst: dr, dg, db, da = bytes of 4 RGBA pixels / 255. */
ST_FN load_8888_dst(struct aoi_cpu *c, uint64_t ctx)
{
    uint64_t a;
    uint32_t px[4];
    float o[4][4], k = bitsf(0x3b808081u);                          /* 1/255 */
    int i, j;
    if (!pixel_addr(c, ctx, &a) || !hrd(c, a, px, 16)) return 0;
    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++) o[j][i] = (float)(int32_t)(px[i] >> (8 * j) & 0xff) * k;
    for (j = 0; j < 4; j++) setf(c, 4 + j, o[j]);
    return 1;
}

/* store_8888: r, g, b, a clamped to [0, 1], times 255, rounded to nearest even,
 * packed as RGBA bytes into 4 pixels. */
ST_FN store_8888(struct aoi_cpu *c, uint64_t ctx)
{
    uint64_t a;
    uint32_t px[4] = { 0, 0, 0, 0 };
    uint8_t *p;
    float v[4];
    int i, j;
    if (!pixel_addr(c, ctx, &a) || !(p = gptr(c, a, 16, AOI_PROT_W))) return 0;
    for (j = 0; j < 4; j++) {
        getf(c, j, v);
        if (nan4(v)) return 0;
        for (i = 0; i < 4; i++) {
            float x = amin(amax(v[i], 0.0f), 1.0f) * 255.0f;
            px[i] |= (uint32_t)rintf(x) << (8 * j);
        }
    }
    memcpy(p, px, 16);
    return 1;
}

/* dstin: r, g, b, a = a * dr, a * dg, a * db, a * da. */
ST_FN dstin(struct aoi_cpu *c, uint64_t ctx)
{
    float a[4], d[4], o[4][4];
    int i, j;
    (void)ctx;
    getf(c, 3, a);
    for (j = 0; j < 4; j++) {
        getf(c, 4 + j, d);
        for (i = 0; i < 4; i++) o[j][i] = a[i] * d[i];
        if (nan4(o[j])) return 0;
    }
    for (j = 0; j < 4; j++) setf(c, j, o[j]);
    return 1;
}

/* ---------- called functions (bl/blr; they return to x30) ---------- */

/* SkOpts rect_memset32(uint32_t *dst, uint32_t v, int count, size_t rowBytes, int height):
 * a rectangle of one colour. Rewriting a row twice is harmless, so a row that is not
 * plain memory just declines (the guest code then fills it all again). */
ST_FN rect_memset32(struct aoi_cpu *c, uint64_t ctx)
{
    uint64_t dst = c->x[0], rb = c->x[3];
    uint32_t v = (uint32_t)c->x[1], vv[4] = { v, v, v, v };
    int32_t n = (int32_t)c->x[2], h = (int32_t)c->x[4], y;
    (void)ctx;
    if (h < 1) return 1;
    for (y = 0; y < h; y++, dst += rb) {
        uint64_t a = dst, left = n > 0 ? (uint64_t)n * 4 : 0;
        while (left) {
            uint64_t piece = 4096 - (a & 4095);
            uint8_t *p;
            if (piece > left) piece = left;
            if (!(p = gptr(c, a, (int)piece, AOI_PROT_W))) return 0;
            for (; piece >= 4; piece -= 4, p += 4, a += 4, left -= 4) *(aoi_u32u *)p = v;
        }
    }
    c->x[0] = dst; c->x[4] = 0;
    setu(c, 0, vv);
    c->steps += (uint64_t)h * ((uint64_t)(n > 0 ? n : 0) + 6);
    return 1;
}

/* x/255 rounded, as the NEON code does it: (p + ((p + 128) >> 8) + 128) >> 8. */
static uint32_t div255(uint32_t p) { return (p + ((p + 128) >> 8) + 128) >> 8; }

/* SkOpts blit_row_color32(uint32_t *dst, int count, SkPMColor color): an opaque color
 * fills; otherwise each byte becomes ((d * scale + 128) >> 8) + c (wrapping), with
 * scale = 255 - a + (a < 128). */
ST_FN blit_row_color32(struct aoi_cpu *c, uint64_t ctx)
{
    uint64_t dst = c->x[0];
    int32_t n = (int32_t)c->x[1], i;
    uint32_t col = (uint32_t)c->x[2], a = col >> 24, scale = 255 - a + (a < 128);
    (void)ctx;
    for (i = 0; i < n; i++)                                 /* all plain memory first: blending twice */
        if (!gptr(c, dst + 4 * (uint64_t)i, 4, AOI_PROT_W)) return 0;   /* is not harmless */
    for (i = 0; i < n; i++, dst += 4) {
        uint8_t *p = gptr(c, dst, 4, AOI_PROT_W);
        uint32_t d, o = 0;
        int k;
        if (a == 0xff) { *(aoi_u32u *)p = col; continue; }
        if (!a) break;
        d = *(aoi_u32u *)p;
        for (k = 0; k < 32; k += 8)
            o |= (((((d >> k & 0xff) * scale + 128) >> 8) + (col >> k & 0xff)) & 0xff) << k;
        *(aoi_u32u *)p = o;
    }
    c->x[0] = dst;
    c->steps += (uint64_t)(n > 0 ? n : 0) * 3 + 8;
    return 1;
}

/* SkOpts blit_row_s32a_opaque(uint32_t *dst, const uint32_t *src, int count): premultiplied
 * source over destination, each byte s + d * (255 - sa) / 255 (rounded, saturated). A
 * negative count still blends one pixel, as the code's tail does. */
ST_FN blit_row_s32a(struct aoi_cpu *c, uint64_t ctx)
{
    uint64_t dst = c->x[0], src = c->x[1];
    int32_t n = (int32_t)c->x[2], i;
    (void)ctx;
    if (n < 0) n = 1;
    for (i = 0; i < n; i++)                                 /* all plain memory first */
        if (!gptr(c, dst + 4 * (uint64_t)i, 4, AOI_PROT_W) || !gptr(c, src + 4 * (uint64_t)i, 4, AOI_PROT_R)) return 0;
    for (i = 0; i < n; i++, dst += 4, src += 4) {
        uint8_t *dp = gptr(c, dst, 4, AOI_PROT_W), *sp = gptr(c, src, 4, AOI_PROT_R);
        uint32_t s, d, inv, o = 0;
        int k;
        s = *(aoi_u32u *)sp; d = *(aoi_u32u *)dp; inv = 255 - (s >> 24);
        for (k = 0; k < 32; k += 8) {
            uint32_t v = (s >> k & 0xff) + div255((d >> k & 0xff) * inv);
            o |= (v > 255 ? 255 : v) << k;
        }
        *(aoi_u32u *)dp = o;
    }
    c->x[0] = dst; c->x[1] = src;
    c->steps += (uint64_t)n * 3 + 8;
    return 1;
}

/* ---------- the table ---------- */

struct stage {
    uint32_t off, insns, hash;          /* offset in libhwui.so, length, FNV-1a of its code */
    int (*fn)(struct aoi_cpu *c, uint64_t ctx);   /* NULL: just_return (ret) */
    const char *name;
    int call;                           /* 1: a called function (returns to x30); 2: the pipeline loop */
};

static const struct stage stages[] = {
    { 0x489bac, 25, 0xf271759bu, rect_memset32, "rect_memset32", 1 },
    { 0x49e20c, 64, 0xa2464ffeu, blit_row_color32, "blit_row_color32", 1 },
    { 0x5afb24, 64, 0x48558843u, blit_row_s32a, "blit_row_s32a_opaque", 1 },
    { 0x5d9dec, 12, 0x810c2fd5u, clamp_01, "clamp_01", 0 },
    { 0x5d9f54, 15, 0x079ded96u, seed_shader, "seed_shader", 0 },
    { 0x5da6c4, 27, 0x22b04677u, load_8888_dst, "load_8888_dst", 0 },
    { 0x5da730, 35, 0xe99fc076u, store_8888, "store_8888", 0 },
    { 0x5db148,  6, 0xed2a674du, dstin, "dstin", 0 },
    { 0x5db778, 17, 0x6c359a68u, matrix_2x3, "matrix_2x3", 0 },
    { 0x5db910,  6, 0x2a82e9dcu, clamp_x_1, "clamp_x_1", 0 },
    { 0x5dbc5c, 18, 0xbb3d44eeu, gradient_2stop, "evenly_spaced_2_stop_gradient", 0 },
    { 0x5dc160, 51, 0x1fd55019u, dither, "dither", 0 },
    { 0x5e4570,  1, 0xccfdfb85u, NULL, "just_return", 0 },
    { 0x5e4648, 18, 0x0f9cff5cu, NULL, "start_pipeline loop", 2 },
};
#define NSTAGES (int)(sizeof stages / sizeof stages[0])
#define LO 0x489bac
#define HI (0x5e4648 + 4)

/* Per library base: which stages matched their hash (checked on first use). */
static uint64_t checked_base;
static signed char ok[NSTAGES];         /* 1 matches, 0 not (yet) checked, -1 differs */
static int check_mode = -1;             /* AOI_HLE_CHECK=n: every n-th run is checked (0: none) */
static uint64_t check_count;
#define CHECK_NOW() (check_mode && check_count++ % (uint64_t)check_mode == 0)
static uint64_t n_checked, n_bad;

static int stage_ok(struct aoi_cpu *c, int i)
{
    if (checked_base != c->hle_base) { memset(ok, 0, sizeof ok); checked_base = c->hle_base; }
    if (!ok[i]) {
        uint32_t h = 0x811c9dc5u, k;
        uint8_t b[4];
        for (k = 0; k < stages[i].insns * 4; k++) {
            if (!(k & 3) && !hrd(c, c->hle_base + stages[i].off + k, b, 4)) { ok[i] = -1; return 0; }
            h = (h ^ b[k & 3]) * 0x01000193u;
        }
        ok[i] = h == stages[i].hash ? 1 : -1;
        if (ok[i] < 0) fprintf(stderr, "[hle] %s differs in this libhwui: interpreted\n", stages[i].name);
    }
    return ok[i] > 0;
}

static int find(struct aoi_cpu *c, uint64_t pc)
{
    uint64_t off = pc - c->hle_base;
    int lo = 0, hi = NSTAGES - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (stages[mid].off == off) return stage_ok(c, mid) ? mid : -1;
        if (stages[mid].off < off) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* AOI_HLE_CHECK: the stand-in on a copy of the CPU, the interpreter on the real one,
 * then the vector registers (and a store's pixels) compared. */
static void check(struct aoi_cpu *c, int i, uint64_t ctx)
{
    static struct aoi_cpu h;
    uint8_t before[16], hpx[16], ipx[16];
    uint64_t a = 0, next = 0, lo = c->hle_lo, hi = c->hle_hi, steps = c->steps;
    int r, k, store = stages[i].fn == store_8888, bad = 0;
    if (store && (!pixel_addr(c, ctx, &a) || !hrd(c, a, before, 16))) return;
    h = *c;
    r = stages[i].fn(&h, ctx);
    if (store) { hrd(c, a, hpx, 16); memcpy(gptr(c, a, 16, AOI_PROT_W), before, 16); }
    hrd(c, c->x[0] + 16, &next, 8);
    c->hle_lo = c->hle_hi = 0;                                      /* the guest code itself */
    while (c->pc != next && c->stop == AOI_RUN && c->steps < steps + 200) aoi_cpu_run(c, c->steps + 1);
    c->hle_lo = lo; c->hle_hi = hi;
    if (!r) return;                                                 /* declined: nothing to compare */
    n_checked++;
    for (k = 0; k < 8; k++)
        if (memcmp(h.vreg[k], c->vreg[k], 16)) bad = 1;
    if (store) { hrd(c, a, ipx, 16); if (memcmp(hpx, ipx, 16)) bad = 1; }
    if (bad && n_bad++ < 20) {
        fprintf(stderr, "[hle] %s differs from the interpreter:", stages[i].name);
        for (k = 0; k < 8; k++)
            if (memcmp(h.vreg[k], c->vreg[k], 16))
                fprintf(stderr, " v%d %016llx%016llx/%016llx%016llx", k, (unsigned long long)h.vreg[k][1],
                        (unsigned long long)h.vreg[k][0], (unsigned long long)c->vreg[k][1], (unsigned long long)c->vreg[k][0]);
        if (store) fprintf(stderr, " pixels %08x/%08x", *(uint32_t *)hpx, *(uint32_t *)ipx);
        fprintf(stderr, "\n");
    }
    if ((n_checked & 0xfffff) == 0) fprintf(stderr, "[hle] %llu checked, %llu differ\n",
                                            (unsigned long long)n_checked, (unsigned long long)n_bad);
}

/* AOI_HLE_CHECK for a called function that writes one row of pixels at x0: the
 * stand-in's pixels against the guest code's. */
static void check_call(struct aoi_cpu *c, int i)
{
    static struct aoi_cpu h;
    static uint8_t before[65536], hpx[65536], ipx[65536];
    uint64_t a = c->x[0], ret = c->x[30], steps = c->steps, lo = c->hle_lo, hi = c->hle_hi, len;
    int32_t n = stages[i].fn == rect_memset32 ? 0 : (int32_t)(stages[i].fn == blit_row_s32a ? c->x[2] : c->x[1]);
    int r;
    if (n < 1) n = 1;
    len = 4 * (uint64_t)n;
    if (stages[i].fn == rect_memset32 || len > sizeof before || !hrd(c, a, before, (int)len)) {
        c->hle_lo = c->hle_hi = 0;                                  /* not compared: the guest code runs */
        while (c->pc != ret && c->stop == AOI_RUN && c->steps < steps + 100000000) aoi_cpu_run(c, c->steps + 1);
        c->hle_lo = lo; c->hle_hi = hi;
        return;
    }
    h = *c;
    r = stages[i].fn(&h, 0);
    if (r) {
        uint8_t *w = gptr(c, a, (int)len, AOI_PROT_W);
        hrd(c, a, hpx, (int)len);
        if (w) memcpy(w, before, len); else r = 0;
    }
    c->hle_lo = c->hle_hi = 0;
    while (c->pc != ret && c->stop == AOI_RUN && c->steps < steps + 100000000) aoi_cpu_run(c, c->steps + 1);
    c->hle_lo = lo; c->hle_hi = hi;
    if (!r) return;
    n_checked++;
    hrd(c, a, ipx, (int)len);
    if (memcmp(hpx, ipx, len) && n_bad++ < 20) {
        uint64_t k;
        for (k = 0; k < len && hpx[k] == ipx[k]; k++) {}
        fprintf(stderr, "[hle] %s differs from the interpreter at byte %llu: %02x/%02x\n", stages[i].name,
                (unsigned long long)k, hpx[k], ipx[k]);
    }
}

static void report(void)
{
    fprintf(stderr, "[hle] %llu stage runs checked against the interpreter, %llu differ\n",
            (unsigned long long)n_checked, (unsigned long long)n_bad);
}

static int pipeline_loop(struct aoi_cpu *c);

/* Runs the stand-ins from c->pc on while they apply. 2: the program returned (a
 * just_return: pc = x30); 1: some ran, c->pc is guest code to interpret; 0: none. */
static int run(struct aoi_cpu *c)
{
    int i, ran = 0;
    while ((i = find(c, c->pc)) >= 0) {
        uint64_t ctx, next;
        if (stages[i].call == 2) return pipeline_loop(c) | ran;     /* the 4-pixel loop */
        if (!stages[i].fn) {                                        /* just_return */
            c->pc = c->x[30];
            c->steps++;
            return 2;
        }
        if (stages[i].call) {                                       /* a function: runs, returns */
            if (CHECK_NOW()) { check_call(c, i); ran = 1; if (c->stop != AOI_RUN) break; continue; }
            if (!stages[i].fn(c, 0)) break;
            c->pc = c->x[30];
            ran = 1;
            continue;
        }
        if (!hrd(c, c->x[0] + 8, &ctx, 8) || !hrd(c, c->x[0] + 16, &next, 8)) break;
        if (CHECK_NOW()) { check(c, i, ctx); ran = 1; if (c->stop != AOI_RUN) break; continue; }
        if (!stages[i].fn(c, ctx)) break;                           /* declined: interpreted from here */
        c->x[0] += 16;
        c->x[4] = next;
        c->pc = next;
        c->steps += stages[i].insns;
        ran = 1;
    }
    return ran;
}

/* The highp pipeline's inner loop (start_pipeline, at +0x5e4648): for x = x28 in steps
 * of 4 while x + 4 <= x25 (xlimit), the program (x24; first stage x19) runs on row
 * x23 with zeroed v0-v7. Each iteration does what its instructions do (registers,
 * flags); a stage without a stand-in leaves the rest of the iteration to the guest. */
static int pipeline_loop(struct aoi_cpu *c)
{
    const uint64_t ret = c->hle_base + 0x5e467c, out = c->hle_base + 0x5e4690;
    for (;;) {
        uint64_t x8, res;
        int k;
        for (k = 0; k < 8; k++) c->vreg[k][0] = c->vreg[k][1] = 0;
        c->x[0] = c->x[24]; c->x[1] = c->x[28]; c->x[2] = c->x[23]; c->x[3] = 0;
        c->x[30] = ret;
        c->pc = c->x[19];
        c->steps += 13;
        if (run(c) != 2 || c->pc != ret) return 1;                  /* the guest goes on from c->pc */
        x8 = c->x[28] + 8;                                          /* add x8, x28, #8; add x27, x28, #4 */
        c->x[8] = x8; c->x[27] = c->x[28] + 4;
        res = x8 - c->x[25];                                        /* cmp x8, x25 */
        c->n = (int)(res >> 63); c->z = res == 0; c->c = x8 >= c->x[25];
        c->v = (int)(((x8 ^ c->x[25]) & (x8 ^ res)) >> 63);
        c->x[28] = c->x[27];                                        /* mov x28, x27 */
        c->steps += 5;
        if (!(!c->c || c->z)) { c->pc = out; return 1; }            /* b.ls */
    }
}

int aoi_hle_run(struct aoi_cpu *c, uint64_t target)
{
    if (check_mode < 0 && (check_mode = getenv("AOI_HLE_CHECK") ? atoi(getenv("AOI_HLE_CHECK")) : 0) > 0) atexit(report);
    if (check_mode < 0) check_mode = 0;
    if (c->fpcr & 0x03c00000u) return 0;                            /* not round-to-nearest IEEE: interpret */
    c->pc = target;
    return run(c) != 0;
}

void aoi_hle_attach(struct aoi_cpu *c, uint64_t base)
{
    if ((getenv("AOI_HLE") && *getenv("AOI_HLE") == '0') || !base) { c->hle_base = c->hle_lo = c->hle_hi = 0; return; }
    c->hle_base = base;
    c->hle_lo = base + LO;
    c->hle_hi = base + HI;
}
