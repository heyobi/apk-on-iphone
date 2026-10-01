/* SIMD&FP data processing for the interpreter: the Advanced SIMD (NEON) integer
 * classes, scalar floating point, and single-lane loads/stores. Written from
 * the ARM ARM pseudocode, class by class, and checked against Unicorn by
 * tools/isacheck on every encoding that occurs in real Android libraries.
 *
 * Lanes are addressed as bytes of the 128-bit V register (little-endian host,
 * which both iOS and the x86/arm64 dev machines are). */
#include "cpu_impl.h"

#include <math.h>

#define UNDEF() do { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; return 1; } while (0)

typedef uint64_t vec[2];

static uint64_t lane(const uint64_t *v, int i, int esz)
{
    uint64_t r = 0;
    memcpy(&r, (const uint8_t *)v + i * esz, (size_t)esz);
    return r;
}
static void setlane(uint64_t *v, int i, int esz, uint64_t x) { memcpy((uint8_t *)v + i * esz, &x, (size_t)esz); }
static uint64_t emask(int esz) { return esz == 8 ? ~0ULL : ((uint64_t)1 << (8 * esz)) - 1; }
static int64_t sx(uint64_t v, int esz) { return (int64_t)sextn(v & emask(esz), 8 * esz); }

/* write a result: 64-bit (Q=0) results clear the upper half */
static void setv(struct aoi_cpu *c, int d, const vec r, int q)
{
    c->vreg[d][0] = r[0];
    c->vreg[d][1] = q ? r[1] : 0;
}

/* ---------------- floating point helpers ---------------- */

static double bd(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static float bf(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint64_t db(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static uint32_t fb(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

/* NaN rules of the ARM ARM (FPCR.DN = 0): a signalling NaN operand wins, then a
 * quiet one, first operand first; both come back quieted. An operation that makes
 * a NaN from non-NaN inputs returns the default NaN (positive, quiet). */
static int isnan_b(uint64_t b, int dbl) { return dbl ? (b & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL : (b & 0x7fffffffu) > 0x7f800000u; }
static int issnan_b(uint64_t b, int dbl) { return isnan_b(b, dbl) && !(b & (dbl ? 0x0008000000000000ULL : 0x00400000u)); }
static uint64_t quiet(uint64_t b, int dbl) { return b | (dbl ? 0x0008000000000000ULL : 0x00400000u); }
static uint64_t defnan(int dbl) { return dbl ? 0x7ff8000000000000ULL : 0x7fc00000u; }

static int pick_nan(uint64_t a, uint64_t b, int nb, int dbl, uint64_t *out)
{
    if (issnan_b(a, dbl)) { *out = quiet(a, dbl); return 1; }
    if (nb > 1 && issnan_b(b, dbl)) { *out = quiet(b, dbl); return 1; }
    if (isnan_b(a, dbl)) { *out = quiet(a, dbl); return 1; }
    if (nb > 1 && isnan_b(b, dbl)) { *out = quiet(b, dbl); return 1; }
    return 0;
}
static uint64_t fixnan(uint64_t r, int dbl) { return isnan_b(r, dbl) ? defnan(dbl) : r; }

static uint64_t fp2(int op, uint64_t a, uint64_t b, int dbl)
{
    uint64_t r;
    if (op == 8) return fp2(0, a, b, dbl) ^ (dbl ? 1ULL << 63 : 1ULL << 31);   /* fnmul = FPNeg(FPMul) */
    if (pick_nan(a, b, 2, dbl, &r) && op != 6 && op != 7) return r;
    if (op == 6 || op == 7) {                       /* fmaxnm/fminnm: a quiet NaN loses */
        if (isnan_b(a, dbl) && !issnan_b(a, dbl) && !isnan_b(b, dbl)) a = b;
        else if (isnan_b(b, dbl) && !issnan_b(b, dbl) && !isnan_b(a, dbl)) b = a;
        if (pick_nan(a, b, 2, dbl, &r)) return r;
        op -= 2;
    }
    if (dbl) {
        double x = bd(a), y = bd(b), z;
        switch (op) {
        case 0: z = x * y; break;  case 1: z = x / y; break;
        case 2: z = x + y; break;  case 3: z = x - y; break;
        case 4: case 5:            /* fmax/fmin: +0 > -0 */
            if (x == y) return op == 4 ? (a & b) : (a | b);
            return (op == 4) == (x > y) ? a : b;
        default: return defnan(1);
        }
        return fixnan(db(z), 1);
    } else {
        float x = bf((uint32_t)a), y = bf((uint32_t)b), z;
        switch (op) {
        case 0: z = x * y; break;  case 1: z = x / y; break;
        case 2: z = x + y; break;  case 3: z = x - y; break;
        case 4: case 5:
            if (x == y) return op == 4 ? (a & b) : (a | b);
            return (op == 4) == (x > y) ? a : b;
        default: return defnan(0);
        }
        return fixnan(fb(z), 0);
    }
}

/* NZCV for fcmp: = 0110, < 1000, > 0010, unordered 0011 */
static void fcmp_flags(struct aoi_cpu *c, uint64_t a, uint64_t b, int dbl)
{
    double x = dbl ? bd(a) : bf((uint32_t)a), y = dbl ? bd(b) : bf((uint32_t)b);
    if (isnan_b(a, dbl) || isnan_b(b, dbl)) { c->n = 0; c->z = 0; c->c = 1; c->v = 1; }
    else if (x == y) { c->n = 0; c->z = 1; c->c = 1; c->v = 0; }
    else if (x < y) { c->n = 1; c->z = 0; c->c = 0; c->v = 0; }
    else { c->n = 0; c->z = 0; c->c = 1; c->v = 0; }
}

/* round to integral: mode 0 nearest-even, 1 +inf, 2 -inf, 3 zero, 4 nearest-away */
static double round_mode(double x, int mode)
{
    switch (mode) {
    case 0: { double r = floor(x), f = x - r;
              if (f > 0.5 || (f == 0.5 && fmod(r, 2.0) != 0)) r += 1.0;
              return r == 0 ? copysign(0.0, x) : r; }
    case 1: return ceil(x);
    case 2: return floor(x);
    case 3: return trunc(x);
    default: return round(x);
    }
}

/* FP -> integer with ARM saturation (NaN -> 0) */
static uint64_t fcvt_int(uint64_t bits, int dbl, int mode, int is_unsigned, int is64, int fbits)
{
    double x = dbl ? bd(bits) : (double)bf((uint32_t)bits);
    if (isnan_b(bits, dbl)) return 0;
    x = round_mode(ldexp(x, fbits), mode);
    if (is_unsigned) {
        double lim = is64 ? 18446744073709551616.0 : 4294967296.0;
        if (x <= 0) return 0;
        if (x >= lim) return is64 ? ~0ULL : 0xffffffffu;
        return (uint64_t)x;
    } else {
        double lim = is64 ? 9223372036854775808.0 : 2147483648.0;
        if (x >= lim) return is64 ? 0x7fffffffffffffffULL : 0x7fffffffu;
        if (x < -lim) return is64 ? 0x8000000000000000ULL : 0x80000000u;
        return is64 ? (uint64_t)(int64_t)x : (uint32_t)(int32_t)x;
    }
}

/* integer -> FP (round to nearest even, as the host does by default) */
static uint64_t int_fcvt(uint64_t v, int is64, int is_unsigned, int dbl, int fbits)
{
    if (!is64) v = is_unsigned ? (uint32_t)v : (uint64_t)(int64_t)(int32_t)v;
    if (dbl) {
        double d = is_unsigned || !is64 ? (is_unsigned ? (double)v : (double)(int64_t)v) : (double)(int64_t)v;
        return db(ldexp(d, -fbits));     /* ldexp is exact unless it underflows */
    } else {
        float f = is_unsigned ? (float)v : (float)(int64_t)v;
        return fb(ldexpf(f, -fbits));
    }
}

static uint64_t vfp_expand(unsigned imm8, int dbl)
{
    uint64_t sign = imm8 >> 7, b6 = imm8 >> 6 & 1, hi = imm8 >> 4 & 3, frac = imm8 & 0xf;
    if (dbl) return sign << 63 | ((!b6) << 10 | (b6 ? 0xffULL : 0) << 2 | hi) << 52 | frac << 48;
    return sign << 31 | ((!b6) << 7 | (b6 ? 0x1fULL : 0) << 2 | hi) << 23 | frac << 19;
}

/* fcvt between single and double, keeping a NaN's payload (quieted) */
static uint64_t cvt_sd(uint64_t a, int to_dbl)
{
    if (isnan_b(a, !to_dbl))
        return to_dbl ? (a & 0x80000000u) << 32 | 0x7ff8000000000000ULL | (a & 0x3fffffu) << 29
                      : (uint64_t)((a >> 32) & 0x80000000u) | 0x7fc00000u | ((a >> 29) & 0x3fffffu);
    return to_dbl ? db((double)bf((uint32_t)a)) : fb((float)bd(a));
}

/* acc + a*b fused, with the FMA NaN order (addend first); neg_* flip signs first */
static uint64_t fma_bits(uint64_t acc, uint64_t a, uint64_t b, int dbl, int neg_prod, int neg_acc)
{
    uint64_t sgn = dbl ? 1ULL << 63 : 1ULL << 31;
    /* Arm negates the operands first (FPNeg), so a propagated NaN keeps the new sign */
    if (neg_prod) a ^= sgn;
    if (neg_acc) acc ^= sgn;
    if (issnan_b(acc, dbl)) return quiet(acc, dbl);
    if (issnan_b(a, dbl)) return quiet(a, dbl);
    if (issnan_b(b, dbl)) return quiet(b, dbl);
    if (isnan_b(acc, dbl)) {
        /* inf * 0 is Invalid even with a quiet NaN addend: default NaN */
        double x = dbl ? bd(a) : bf((uint32_t)a), y = dbl ? bd(b) : bf((uint32_t)b);
        if ((isinf(x) && y == 0) || (x == 0 && isinf(y))) return dbl ? 0x7ff8000000000000ULL : 0x7fc00000u;
        return acc;
    }
    if (isnan_b(a, dbl)) return quiet(a, dbl);
    if (isnan_b(b, dbl)) return quiet(b, dbl);
    return dbl ? fixnan(db(fma(bd(a), bd(b), bd(acc))), 1)
               : fixnan(fb(fmaf(bf((uint32_t)a), bf((uint32_t)b), bf((uint32_t)acc))), 0);
}

/* FP two-register-misc ops shared by the vector and scalar forms; key = U:a:opcode */
static int fp_misc(int key, uint64_t x, int dbl, uint64_t *out)
{
    int fesz = dbl ? 8 : 4;
    double v = dbl ? bd(x) : bf((uint32_t)x);
    int nan = isnan_b(x, dbl);
    switch (key) {
    case 0x2c: *out = !nan && v > 0 ? emask(fesz) : 0; return 1;    /* fcmgt #0 */
    case 0x6c: *out = !nan && v >= 0 ? emask(fesz) : 0; return 1;   /* fcmge #0 */
    case 0x2d: *out = !nan && v == 0 ? emask(fesz) : 0; return 1;   /* fcmeq #0 */
    case 0x6d: *out = !nan && v <= 0 ? emask(fesz) : 0; return 1;   /* fcmle #0 */
    case 0x2e: *out = !nan && v < 0 ? emask(fesz) : 0; return 1;    /* fcmlt #0 */
    case 0x2f: *out = x & (emask(fesz) >> 1); return 1;              /* fabs */
    case 0x6f: *out = x ^ (1ULL << (8 * fesz - 1)); return 1;       /* fneg */
    case 0x1d: *out = int_fcvt(x, dbl, 0, dbl, 0); return 1;         /* scvtf */
    case 0x5d: *out = int_fcvt(x, dbl, 1, dbl, 0); return 1;         /* ucvtf */
    case 0x3b: *out = fcvt_int(x, dbl, 3, 0, dbl, 0); return 1;      /* fcvtzs */
    case 0x7b: *out = fcvt_int(x, dbl, 3, 1, dbl, 0); return 1;      /* fcvtzu */
    case 0x1a: *out = fcvt_int(x, dbl, 0, 0, dbl, 0); return 1;      /* fcvtns */
    case 0x1b: *out = fcvt_int(x, dbl, 2, 0, dbl, 0); return 1;      /* fcvtms */
    case 0x3a: *out = fcvt_int(x, dbl, 1, 0, dbl, 0); return 1;      /* fcvtps */
    case 0x1c: *out = fcvt_int(x, dbl, 4, 0, dbl, 0); return 1;      /* fcvtas */
    default: return 0;
    }
}

/* ---------------- Advanced SIMD integer classes ---------------- */

static int three_same(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, size = insn >> 22 & 3, op = insn >> 11 & 0x1f;
    int m = insn >> 16 & 31, n = insn >> 5 & 31, d = insn & 31, esz = 1 << size, ne = (q ? 16 : 8) / esz, i;
    const uint64_t *a = c->vreg[n], *b = c->vreg[m];
    vec r = {0, 0};

    if (op == 0x03) {                                /* logical, on whole halves */
        for (i = 0; i < 2; i++) {
            uint64_t x = a[i], y = b[i], z = c->vreg[d][i];
            switch (u << 2 | size) {
            case 0: r[i] = x & y; break;   case 1: r[i] = x & ~y; break;
            case 2: r[i] = x | y; break;   case 3: r[i] = x | ~y; break;
            case 4: r[i] = x ^ y; break;
            case 5: r[i] = (z & x) | (~z & y); break;                 /* bsl */
            case 6: r[i] = (z & ~y) | (x & y); break;                 /* bit */
            default: r[i] = (z & y) | (x & ~y); break;                /* bif */
            }
        }
        setv(c, d, r, q);
        return 1;
    }
    if (op >= 0x18) {                                 /* vector FP three-same */
        int dbl = size & 1, fesz = dbl ? 8 : 4, fne = (q ? 16 : 8) / fesz, hi = size >> 1, k;
        if (dbl && !q) return 0;
        for (i = 0; i < fne; i++) {
            uint64_t x = lane(a, i, fesz), y = lane(b, i, fesz), z = 0;
            k = u << 6 | hi << 5 | op;
            switch (k) {
            case 0x1a: z = fp2(2, x, y, dbl); break;                 /* fadd */
            case 0x3a: z = fp2(3, x, y, dbl); break;                 /* fsub */
            case 0x5b: z = fp2(0, x, y, dbl); break;                 /* fmul */
            case 0x5f: z = fp2(1, x, y, dbl); break;                 /* fdiv */
            case 0x19: case 0x39: {                                   /* fmla / fmls (fused) */
                uint64_t acc = lane(c->vreg[d], i, fesz);
                z = fma_bits(acc, k == 0x39 ? x ^ (1ULL << (8 * fesz - 1)) : x, y, dbl, 0, 0);
                break; }
            case 0x1e: z = fp2(4, x, y, dbl); break;                 /* fmax */
            case 0x3e: z = fp2(5, x, y, dbl); break;                 /* fmin */
            case 0x1c: case 0x5c: case 0x7c: {                        /* fcmeq / fcmge / fcmgt */
                double xv = dbl ? bd(x) : bf((uint32_t)x), yv = dbl ? bd(y) : bf((uint32_t)y);
                int t = isnan_b(x, dbl) || isnan_b(y, dbl) ? 0 : k == 0x1c ? xv == yv : k == 0x5c ? xv >= yv : xv > yv;
                z = t ? emask(fesz) : 0; break; }
            default: return 0;
            }
            setlane(r, i, fesz, z);
        }
        setv(c, d, r, q);
        return 1;
    }
    if (size == 3 && !q) return 0;
    for (i = 0; i < ne; i++) {
        uint64_t x = lane(a, i, esz), y = lane(b, i, esz), z;
        int64_t sxv = sx(x, esz), syv = sx(y, esz);
        switch (op) {
        case 0x10: z = u ? x - y : x + y; break;                                   /* add/sub */
        case 0x06: z = (u ? x > y : sxv > syv) ? ~0ULL : 0; break;                 /* cmhi/cmgt */
        case 0x07: z = (u ? x >= y : sxv >= syv) ? ~0ULL : 0; break;               /* cmhs/cmge */
        case 0x11: z = (u ? x == y : (x & y) != 0) ? ~0ULL : 0; break;             /* cmeq/cmtst */
        case 0x0c: z = u ? (x > y ? x : y) : (uint64_t)(sxv > syv ? sxv : syv); break;   /* max */
        case 0x0d: z = u ? (x < y ? x : y) : (uint64_t)(sxv < syv ? sxv : syv); break;   /* min */
        case 0x13: if (u || size == 3) return 0; z = x * y; break;                  /* mul */
        case 0x12: if (size == 3) return 0;                                          /* mla / mls */
                   z = u ? lane(c->vreg[d], i, esz) - x * y : lane(c->vreg[d], i, esz) + x * y; break;
        case 0x00: case 0x02:                                                        /* [su]hadd / [su]rhadd */
            if (size == 3) return 0;
            z = u ? (x + y + (op == 2)) >> 1 : (uint64_t)((sxv + syv + (op == 2)) >> 1); break;
        case 0x08: {                                                                 /* sshl/ushl */
            int sh = (int8_t)(y & 0xff), bits = 8 * esz;
            if (sh >= 0) z = sh >= bits ? 0 : x << sh;
            else if (u) z = -sh >= bits ? 0 : x >> -sh;
            else z = (uint64_t)(-sh >= bits ? (sxv < 0 ? -1 : 0) : sxv >> -sh);
            break; }
        case 0x17: {                                                                 /* addp */
            const uint64_t *src = i < ne / 2 ? a : b;
            int j = (i % (ne / 2)) * 2;
            if (u) return 0;
            z = lane(src, j, esz) + lane(src, j + 1, esz);
            break; }
        default: return 0;
        }
        setlane(r, i, esz, z & emask(esz));
    }
    setv(c, d, r, q);
    return 1;
}

static int two_misc(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, size = insn >> 22 & 3, op = insn >> 12 & 0x1f;
    int n = insn >> 5 & 31, d = insn & 31, esz = 1 << size, ne = (q ? 16 : 8) / esz, i, j;
    const uint64_t *a = c->vreg[n];
    vec r = {0, 0};

    if ((op == 0x16 || op == 0x17) && !u && size == 1) {   /* fcvtn{2} d->s, fcvtl{2} s->d */
        if (op == 0x16) {
            r[0] = q ? c->vreg[d][0] : 0; r[1] = 0;
            for (i = 0; i < 2; i++) setlane(r, i + (q ? 2 : 0), 4, cvt_sd(lane(a, i, 8), 0));
            c->vreg[d][0] = r[0]; c->vreg[d][1] = q ? r[1] : 0;
            return 1;
        }
        for (i = 0; i < 2; i++) setlane(r, i, 8, cvt_sd(lane(a, i + (q ? 2 : 0), 4), 1));
        setv(c, d, r, 1);
        return 1;
    }
    if (op >= 0x0c && op != 0x12) {                   /* FP forms: size = a:sz */
        int dbl = size & 1, fesz = dbl ? 8 : 4, key = u << 6 | (size >> 1) << 5 | op;
        uint64_t z;
        if (dbl && !q) return 0;
        for (i = 0; i < (q ? 16 : 8) / fesz; i++) {
            if (!fp_misc(key, lane(a, i, fesz), dbl, &z)) return 0;
            setlane(r, i, fesz, z);
        }
        setv(c, d, r, q);
        return 1;
    }
    switch (op) {
    case 0x00: case 0x01: {                           /* rev64 / rev32 / rev16 */
        int cont = op == 1 ? 2 : (u ? 4 : 8);         /* container bytes */
        if (op == 1 && u) return 0;
        if (esz >= cont) return 0;
        for (i = 0; i < ne; i++) {
            int per = cont / esz, base = i / per * per;
            setlane(r, i, esz, lane(a, base + per - 1 - (i - base), esz));
        }
        break; }
    case 0x05:
        if (size == 0) {
            for (i = 0; i < (q ? 16 : 8); i++)
                setlane(r, i, 1, u ? ~lane(a, i, 1) & 0xff : (uint64_t)__builtin_popcountll(lane(a, i, 1)));  /* not / cnt */
        } else if (size == 1 && u) {
            for (i = 0; i < (q ? 16 : 8); i++) {                                                   /* rbit */
                uint64_t x = lane(a, i, 1), y = 0; int k;
                for (k = 0; k < 8; k++) if (x >> k & 1) y |= 1u << (7 - k);
                setlane(r, i, 1, y);
            }
        } else return 0;
        break;
    case 0x02: case 0x06: {                           /* [su]addlp / [su]adalp */
        if (size == 3) return 0;
        for (i = 0; i < ne / 2; i++) {
            uint64_t x = lane(a, 2 * i, esz), y = lane(a, 2 * i + 1, esz);
            uint64_t s = u ? x + y : (uint64_t)(sx(x, esz) + sx(y, esz));
            if (op == 6) s += lane(c->vreg[d], i, 2 * esz);
            setlane(r, i, 2 * esz, s & emask(2 * esz));
        }
        break; }
    case 0x08: case 0x09: case 0x0a: case 0x0b:
        if (size == 3 && !q) return 0;
        for (i = 0; i < ne; i++) {
            int64_t x = sx(lane(a, i, esz), esz);
            uint64_t z;
            switch (op << 1 | u) {
            case 0x10: z = x > 0 ? ~0ULL : 0; break;      /* cmgt #0 */
            case 0x11: z = x >= 0 ? ~0ULL : 0; break;     /* cmge #0 */
            case 0x12: z = x == 0 ? ~0ULL : 0; break;     /* cmeq #0 */
            case 0x13: z = x <= 0 ? ~0ULL : 0; break;     /* cmle #0 */
            case 0x14: z = x < 0 ? ~0ULL : 0; break;      /* cmlt #0 */
            case 0x16: z = (uint64_t)(x < 0 ? -x : x); break;   /* abs */
            case 0x17: z = (uint64_t)-x; break;                 /* neg */
            default: return 0;
            }
            setlane(r, i, esz, z & emask(esz));
        }
        break;
    case 0x12:                                        /* xtn / xtn2 */
        if (u || size == 3) return 0;
        r[0] = c->vreg[d][0]; r[1] = c->vreg[d][1];
        if (!q) r[1] = 0;
        for (i = 0; i < 8 / esz; i++) setlane(r, i + (q ? 8 / esz : 0), esz, lane(a, i, 2 * esz));
        c->vreg[d][0] = r[0]; c->vreg[d][1] = r[1];
        if (!q) c->vreg[d][1] = 0;
        return 1;
    default:
        (void)j;
        return 0;
    }
    setv(c, d, r, q);
    return 1;
}

static int across(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, size = insn >> 22 & 3, op = insn >> 12 & 0x1f;
    int n = insn >> 5 & 31, d = insn & 31, esz = 1 << size, ne = (q ? 16 : 8) / esz, i;
    const uint64_t *a = c->vreg[n];
    uint64_t acc;
    int resz = esz;
    if (size == 3 || (size == 2 && !q)) return 0;
    acc = op == 0x1a && u ? emask(esz) : 0;
    for (i = 0; i < ne; i++) {
        uint64_t x = lane(a, i, esz);
        switch (op << 1 | u) {
        case 0x36: acc += x; break;                                          /* addv */
        case 0x07: acc += x; resz = 2 * esz; break;                          /* uaddlv */
        case 0x06: acc += (uint64_t)sx(x, esz); resz = 2 * esz; break;       /* saddlv */
        case 0x15: if (!i || x > acc) acc = x; break;                        /* umaxv */
        case 0x14: if (!i || sx(x, esz) > sx(acc, esz)) acc = x; break;      /* smaxv */
        case 0x35: if (!i || x < acc) acc = x; break;                        /* uminv */
        case 0x34: if (!i || sx(x, esz) < sx(acc, esz)) acc = x; break;      /* sminv */
        default: return 0;
        }
    }
    c->vreg[d][0] = acc & emask(resz);
    c->vreg[d][1] = 0;
    return 1;
}

static int shift_imm(struct aoi_cpu *c, uint32_t insn, int scalar)
{
    int q = scalar ? 1 : insn >> 30 & 1, u = insn >> 29 & 1, immh = insn >> 19 & 0xf, op = insn >> 11 & 0x1f;
    int n = insn >> 5 & 31, d = insn & 31, immhb = insn >> 16 & 0x7f, i;
    int esz = immh & 8 ? 8 : immh & 4 ? 4 : immh & 2 ? 2 : 1, bits = 8 * esz, ne;
    int rsh = 2 * bits - immhb, lsh = immhb - bits;
    const uint64_t *a = c->vreg[n];
    vec r = {0, 0};

    if (scalar && esz != 8) return 0;
    if (op == 0x14) {                                  /* [su]shll{2}: widen */
        if (esz == 8) return 0;
        for (i = 0; i < 8 / esz; i++) {
            uint64_t x = lane(a, i + (q ? 8 / esz : 0), esz);
            uint64_t w = u ? x : (uint64_t)sx(x, esz);
            setlane(r, i, 2 * esz, (w << lsh) & emask(2 * esz));
        }
        setv(c, d, r, 1);
        return 1;
    }
    if (op == 0x10 && !u) {                            /* shrn{2}: narrow (esz is the result size) */
        int dsz = esz;
        if (esz == 8) return 0;
        r[0] = c->vreg[d][0]; r[1] = q ? c->vreg[d][1] : 0;
        for (i = 0; i < 8 / dsz; i++)
            setlane(r, i + (q ? 8 / dsz : 0), dsz, (lane(a, i, 2 * dsz) >> rsh) & emask(dsz));
        c->vreg[d][0] = r[0]; c->vreg[d][1] = r[1];
        return 1;
    }
    if (esz == 8 && !q) return 0;
    ne = scalar ? 1 : (q ? 16 : 8) / esz;
    for (i = 0; i < ne; i++) {
        uint64_t x = lane(a, i, esz), z, dv = lane(c->vreg[d], i, esz);
        switch (op) {
        case 0x00: case 0x02:                          /* [su]shr / [su]sra */
            z = u ? (rsh >= 64 ? 0 : x >> rsh) : (uint64_t)(rsh >= 64 ? (sx(x, esz) < 0 ? -1 : 0) : sx(x, esz) >> rsh);
            if (op == 2) z += dv;
            break;
        case 0x0a:                                     /* shl / sli */
            z = x << lsh;
            if (u) z |= dv & ((1ULL << lsh) - 1);
            break;
        case 0x08:                                     /* sri */
            if (!u) return 0;
            z = (rsh >= 64 ? 0 : x >> rsh) | (rsh >= 64 ? dv : dv & ~(emask(esz) >> rsh));
            break;
        default: return 0;
        }
        setlane(r, i, esz, z & emask(esz));
    }
    setv(c, d, r, scalar ? 0 : q);
    return 1;
}

static int three_diff(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, size = insn >> 22 & 3, op = insn >> 12 & 0xf;
    int m = insn >> 16 & 31, n = insn >> 5 & 31, d = insn & 31, esz = 1 << size, i, half = q ? 8 / esz : 0;
    vec r = {0, 0};
    if (size == 3) return 0;
    if (op == 0x4 || op == 0x6) {                      /* [r]addhn{2} / [r]subhn{2}: high half, narrowed */
        int bits = 8 * esz;
        r[0] = q ? c->vreg[d][0] : 0;
        for (i = 0; i < 8 / esz; i++) {
            uint64_t x = lane(c->vreg[n], i, 2 * esz), y = lane(c->vreg[m], i, 2 * esz);
            uint64_t z = (op == 4 ? x + y : x - y) + (u ? 1ULL << (bits - 1) : 0);
            setlane(r, i + half, esz, (z >> bits) & emask(esz));
        }
        c->vreg[d][0] = r[0]; c->vreg[d][1] = q ? r[1] : 0;
        return 1;
    }
    for (i = 0; i < 8 / esz; i++) {
        uint64_t x, y, z, acc = lane(c->vreg[d], i, 2 * esz);
        x = op == 1 || op == 3 ? lane(c->vreg[n], i, 2 * esz) : lane(c->vreg[n], i + half, esz);
        y = lane(c->vreg[m], i + half, esz);
        if (!u) { y = (uint64_t)sx(y, esz); if (!(op == 1 || op == 3)) x = (uint64_t)sx(x, esz); }
        switch (op) {
        case 0x0: case 0x1: z = x + y; break;          /* [su]addl / [su]addw */
        case 0x2: case 0x3: z = x - y; break;          /* [su]subl / [su]subw */
        case 0xc: z = x * y; break;                    /* [su]mull */
        case 0x8: z = acc + x * y; break;              /* [su]mlal */
        case 0xa: z = acc - x * y; break;              /* [su]mlsl */
        default: return 0;
        }
        setlane(r, i, 2 * esz, z & emask(2 * esz));
    }
    setv(c, d, r, 1);
    return 1;
}

static int copy(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, op = insn >> 29 & 1, imm5 = insn >> 16 & 31, imm4 = insn >> 11 & 0xf;
    int n = insn >> 5 & 31, d = insn & 31, size, esz, idx, i;
    vec r = {0, 0};
    if (!(imm5 & 0xf)) return 0;
    size = __builtin_ctz((unsigned)imm5);
    esz = 1 << size;
    idx = imm5 >> (size + 1);
    if (op) {                                          /* ins (element) */
        if (!q) return 0;
        setlane(c->vreg[d], idx, esz, lane(c->vreg[n], imm4 >> size, esz));
        return 1;
    }
    switch (imm4) {
    case 0x0:                                          /* dup (element) */
        if (esz == 8 && !q) return 0;
        for (i = 0; i < (q ? 16 : 8) / esz; i++) setlane(r, i, esz, lane(c->vreg[n], idx, esz));
        setv(c, d, r, q);
        return 1;
    case 0x1:                                          /* dup (general) */
        if (esz == 8 && !q) return 0;
        for (i = 0; i < (q ? 16 : 8) / esz; i++) setlane(r, i, esz, X(c, n) & emask(esz));
        setv(c, d, r, q);
        return 1;
    case 0x3:                                          /* ins (general) */
        if (!q) return 0;
        setlane(c->vreg[d], idx, esz, X(c, n) & emask(esz));
        return 1;
    case 0x5: {                                        /* smov */
        uint64_t v = (uint64_t)sx(lane(c->vreg[n], idx, esz), esz);
        if (esz >= (q ? 8 : 4)) return 0;
        setX(c, d, q ? v : v & 0xffffffffu);
        return 1; }
    case 0x7:                                          /* umov */
        if ((esz == 8) != q) return 0;
        setX(c, d, lane(c->vreg[n], idx, esz));
        return 1;
    default:
        return 0;
    }
}

static int permute(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, size = insn >> 22 & 3, op = insn >> 12 & 7;
    int m = insn >> 16 & 31, n = insn >> 5 & 31, d = insn & 31, esz = 1 << size, ne = (q ? 16 : 8) / esz, i;
    const uint64_t *a = c->vreg[n], *b = c->vreg[m];
    vec r = {0, 0};
    if (esz == 8 && !q) return 0;
    for (i = 0; i < ne; i++) {
        uint64_t z;
        int k;
        switch (op) {
        case 1: case 5: k = 2 * i + (op == 5); z = k < ne ? lane(a, k, esz) : lane(b, k - ne, esz); break;  /* uzp1/2 */
        case 2: case 6: k = (i & ~1) + (op == 6); z = i & 1 ? lane(b, k, esz) : lane(a, k, esz); break;     /* trn1/2 */
        case 3: case 7: k = i / 2 + (op == 7 ? ne / 2 : 0); z = i & 1 ? lane(b, k, esz) : lane(a, k, esz); break; /* zip1/2 */
        default: return 0;
        }
        setlane(r, i, esz, z);
    }
    setv(c, d, r, q);
    return 1;
}

static int ext(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, m = insn >> 16 & 31, pos = insn >> 11 & 0xf, n = insn >> 5 & 31, d = insn & 31;
    int len = q ? 16 : 8, i;
    uint8_t cat[32];
    vec r = {0, 0};
    if (!q && pos > 7) return 0;
    memcpy(cat, c->vreg[n], (size_t)len);
    memcpy(cat + len, c->vreg[m], (size_t)len);
    memcpy(r, cat + pos, (size_t)len);
    (void)i;
    setv(c, d, r, q);
    return 1;
}

static int tbl(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, m = insn >> 16 & 31, len = (insn >> 13 & 3) + 1, tbx = insn >> 12 & 1;
    int n = insn >> 5 & 31, d = insn & 31, i;
    uint8_t table[64];
    vec r = {0, 0};
    for (i = 0; i < len; i++) memcpy(table + 16 * i, c->vreg[(n + i) & 31], 16);
    for (i = 0; i < (q ? 16 : 8); i++) {
        unsigned idx = (unsigned)lane(c->vreg[m], i, 1);
        setlane(r, i, 1, idx < (unsigned)(16 * len) ? table[idx] : tbx ? lane(c->vreg[d], i, 1) : 0);
    }
    setv(c, d, r, q);
    return 1;
}

static int scalar_misc(struct aoi_cpu *c, uint32_t insn)
{
    int u = insn >> 29 & 1, size = insn >> 22 & 3, op = insn >> 12 & 0x1f, n = insn >> 5 & 31, d = insn & 31;
    int dbl = size & 1, key = u << 6 | (size >> 1) << 5 | op;
    uint64_t z;
    if (op < 0x0c || !fp_misc(key, lane(c->vreg[n], 0, dbl ? 8 : 4), dbl, &z)) return 0;
    c->vreg[d][0] = z; c->vreg[d][1] = 0;
    return 1;
}

static int scalar_pairwise(struct aoi_cpu *c, uint32_t insn)
{
    int u = insn >> 29 & 1, size = insn >> 22 & 3, op = insn >> 12 & 0x1f, n = insn >> 5 & 31, d = insn & 31;
    if (u || size != 3 || op != 0x1b) return 0;       /* addp d, v.2d */
    c->vreg[d][0] = c->vreg[n][0] + c->vreg[n][1];
    c->vreg[d][1] = 0;
    return 1;
}

/* fmul / fmla / fmls by element, vector and scalar */
static int fp_elem(struct aoi_cpu *c, uint32_t insn, int scalar)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, sz = insn >> 22 & 1, L = insn >> 21 & 1, M = insn >> 20 & 1;
    int rm = insn >> 16 & 15, op = insn >> 12 & 0xf, H = insn >> 11 & 1, n = insn >> 5 & 31, d = insn & 31;
    int dbl = sz, fesz = dbl ? 8 : 4, idx, ne, i;
    uint64_t e;
    vec r = {0, 0};
    if (!(insn >> 23 & 1) || u || (dbl && L) || (!scalar && dbl && !q)) return 0;
    idx = dbl ? H : H << 1 | L;
    e = lane(c->vreg[M << 4 | rm], idx, fesz);
    ne = scalar ? 1 : (q ? 16 : 8) / fesz;
    for (i = 0; i < ne; i++) {
        uint64_t x = lane(c->vreg[n], i, fesz), acc = lane(c->vreg[d], i, fesz), z;
        switch (op) {
        case 0x9: z = fp2(0, x, e, dbl); break;                           /* fmul */
        case 0x1: z = fma_bits(acc, x, e, dbl, 0, 0); break;              /* fmla */
        case 0x5: z = fma_bits(acc, x, e, dbl, 1, 0); break;              /* fmls */
        default: return 0;
        }
        setlane(r, i, fesz, z);
    }
    setv(c, d, r, scalar ? 0 : q);
    return 1;
}

/* mov (scalar, element) = dup b/h/s/d, v.t[i] */
static int scalar_dup(struct aoi_cpu *c, uint32_t insn)
{
    int imm5 = insn >> 16 & 31, n = insn >> 5 & 31, d = insn & 31, size, esz;
    if (!(imm5 & 0xf)) return 0;
    size = __builtin_ctz((unsigned)imm5); esz = 1 << size;
    c->vreg[d][0] = lane(c->vreg[n], imm5 >> (size + 1), esz);
    c->vreg[d][1] = 0;
    return 1;
}

/* ld1/st1 (single structure, one lane) and ld1r */
static int ldst_single(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, post = insn >> 23 & 1, load = insn >> 22 & 1, R = insn >> 21 & 1;
    int rm = insn >> 16 & 31, opcode = insn >> 13 & 7, S = insn >> 12 & 1, size = insn >> 10 & 3;
    int rn = insn >> 5 & 31, t = insn & 31, esz, idx, i;
    uint64_t base = rn == 31 ? c->sp : c->x[rn];
    if (R || (opcode & 1)) return 0;                   /* ld2-ld4 single: not yet */
    if (!post && rm) return 0;
    switch (opcode >> 1) {
    case 0: esz = 1; idx = q << 3 | S << 2 | size; break;
    case 1: if (size & 1) return 0; esz = 2; idx = q << 2 | S << 1 | size >> 1; break;
    case 2: if (size == 0) { esz = 4; idx = q << 1 | S; }
            else if (size == 1 && !S) { esz = 8; idx = q; }
            else return 0;
            break;
    default: {                                         /* ld1r */
        vec r = {0, 0};
        uint64_t v;
        if (!load || S) return 0;
        esz = 1 << size;
        v = rd(c, base, esz);
        if (c->stop != AOI_RUN) return 1;
        for (i = 0; i < (q ? 16 : 8) / esz; i++) setlane(r, i, esz, v);
        setv(c, t, r, q);
        goto wb; }
    }
    if (load) {
        uint64_t v = rd(c, base, esz);
        if (c->stop != AOI_RUN) return 1;
        setlane(c->vreg[t], idx, esz, v);
    } else wr(c, base, lane(c->vreg[t], idx, esz), esz);
wb:
    if (post && c->stop == AOI_RUN) {
        uint64_t nb = base + (rm == 31 ? (uint64_t)esz : c->x[rm]);
        if (rn == 31) c->sp = nb; else c->x[rn] = nb;
    }
    return 1;
}

/* ---------------- scalar floating point ---------------- */

static int fp_scalar(struct aoi_cpu *c, uint32_t insn)
{
    int ptype = insn >> 22 & 3, n = insn >> 5 & 31, d = insn & 31, m = insn >> 16 & 31;
    int dbl = ptype == 1, fsz = dbl ? 8 : 4;
    uint64_t a = lane(c->vreg[n], 0, fsz), b = lane(c->vreg[m], 0, fsz), r;
    #define SETF(v) do { c->vreg[d][0] = (v) & emask(fsz); c->vreg[d][1] = 0; } while (0)

    if ((insn & 0x7f20fc00u) == 0x1e200000u) {         /* FP <-> integer, fmov general */
        int sf = insn >> 31, rmode = insn >> 19 & 3, op = insn >> 16 & 7;
        if (ptype == 2 && op >= 6 && rmode == 1 && sf) {  /* fmov x, v.d[1] / v.d[1], x */
            if (op == 6) setX(c, d, c->vreg[n][1]); else c->vreg[d][1] = X(c, n);
            return 1;
        }
        if (ptype > 1) return 0;
        a = lane(c->vreg[n], 0, fsz);
        switch (op) {
        case 0: case 1:                                    /* fcvt[npmz][su] */
            setX(c, d, fcvt_int(a, dbl, rmode == 0 ? 0 : rmode == 1 ? 1 : rmode == 2 ? 2 : 3, op, sf, 0));
            return 1;
        case 4: case 5:                                    /* fcvta[su] */
            if (rmode) return 0;
            setX(c, d, fcvt_int(a, dbl, 4, op == 5, sf, 0));
            return 1;
        case 2: case 3:                                    /* scvtf / ucvtf */
            if (rmode) return 0;
            SETF(int_fcvt(X(c, n), sf, op == 3, dbl, 0));
            return 1;
        case 6:                                            /* fmov w/x <- s/d */
            if (rmode || sf != dbl) return 0;
            setX(c, d, a);
            return 1;
        case 7:                                            /* fmov s/d <- w/x */
            if (rmode || sf != dbl) return 0;
            SETF(X(c, n) & emask(fsz));
            return 1;
        }
        return 0;
    }
    if ((insn & 0x7f200000u) == 0x1e000000u) {         /* FP <-> fixed point */
        int sf = insn >> 31, rmode = insn >> 19 & 3, op = insn >> 16 & 7, fbits = 64 - (insn >> 10 & 0x3f);
        if (ptype > 1 || (!sf && fbits > 32)) return 0;
        if (rmode == 3 && op < 2) { setX(c, d, fcvt_int(a, dbl, 3, op, sf, fbits)); return 1; }
        if (rmode == 0 && (op == 2 || op == 3)) { SETF(int_fcvt(X(c, n), sf, op == 3, dbl, fbits)); return 1; }
        return 0;
    }
    if (insn >> 31 || (insn >> 29 & 1)) return 0;
    if ((insn & 0xff000000u) == 0x1f000000u) {         /* fmadd/fmsub/fnmadd/fnmsub */
        int o1 = insn >> 21 & 1, o0 = insn >> 15 & 1, ra = insn >> 10 & 31;
        uint64_t acc = lane(c->vreg[ra], 0, fsz);
        if (ptype > 1) return 0;
        r = fma_bits(acc, a, b, dbl, o0 != o1, o1);     /* fmsub/fnmadd negate the product, fn* the addend */
        SETF(r);
        return 1;
    }
    if ((insn & 0xff207c00u) == 0x1e204000u) {         /* 1-source */
        int op = insn >> 15 & 0x3f;
        if (ptype > 1) return 0;
        switch (op) {
        case 0: SETF(a); return 1;                                            /* fmov */
        case 1: SETF(a & (emask(fsz) >> 1)); return 1;                        /* fabs */
        case 2: SETF(a ^ (dbl ? 1ULL << 63 : 1ULL << 31)); return 1;          /* fneg */
        case 3:                                                               /* fsqrt */
            if (pick_nan(a, 0, 1, dbl, &r)) { SETF(r); return 1; }
            SETF(dbl ? fixnan(db(sqrt(bd(a))), 1) : fixnan(fb(sqrtf(bf((uint32_t)a))), 0));
            return 1;
        case 4: case 5: {                                                     /* fcvt s<->d */
            int to_dbl = op == 5;
            if (to_dbl == dbl) return 0;
            r = cvt_sd(a, to_dbl);
            c->vreg[d][0] = r & emask(to_dbl ? 8 : 4); c->vreg[d][1] = 0;
            return 1; }
        case 8: case 9: case 10: case 11: case 12: case 14: case 15: {       /* frint* */
            int mode = op == 8 ? 0 : op == 9 ? 1 : op == 10 ? 2 : op == 11 ? 3 : op == 12 ? 4 : 0;
            if (pick_nan(a, 0, 1, dbl, &r)) { SETF(r); return 1; }
            if (dbl) { double x = bd(a); SETF(isinf(x) ? a : db(copysign(round_mode(x, mode), x))); }
            else { float x = bf((uint32_t)a); SETF(isinf(x) ? a : fb((float)copysign(round_mode(x, mode), x))); }
            return 1; }
        }
        return 0;
    }
    if ((insn & 0xff203c00u) == 0x1e202000u) {         /* fcmp / fcmpe */
        if (ptype > 1 || (insn & 7)) return 0;
        fcmp_flags(c, a, insn & 8 ? 0 : b, dbl);
        return 1;
    }
    if ((insn & 0xff201fe0u) == 0x1e201000u) {         /* fmov (immediate) */
        if (ptype > 1) return 0;
        SETF(vfp_expand(insn >> 13 & 0xff, dbl));
        return 1;
    }
    if ((insn & 0xff200c00u) == 0x1e200800u) {         /* 2-source */
        int op = insn >> 12 & 0xf;
        if (ptype > 1 || op > 8) return 0;
        SETF(fp2(op, a, b, dbl));
        return 1;
    }
    if ((insn & 0xff200c00u) == 0x1e200c00u) {         /* fcsel */
        if (ptype > 1) return 0;
        SETF(cond_holds(c, insn >> 12 & 0xf) ? a : b);
        return 1;
    }
    if ((insn & 0xff200c00u) == 0x1e200400u) {         /* fccmp / fccmpe */
        if (ptype > 1) return 0;
        if (cond_holds(c, insn >> 12 & 0xf)) fcmp_flags(c, a, b, dbl);
        else { unsigned f = insn & 0xf; c->n = f >> 3 & 1; c->z = f >> 2 & 1; c->c = f >> 1 & 1; c->v = f & 1; }
        return 1;
    }
    return 0;
    #undef SETF
}

int aoi_simd_step(struct aoi_cpu *c, uint32_t insn)
{
    int ok;
    if ((insn & 0x9f200400u) == 0x0e200400u) ok = three_same(c, insn);
    else if ((insn & 0x9f3e0c00u) == 0x0e200800u) ok = two_misc(c, insn);
    else if ((insn & 0x9f3e0c00u) == 0x0e300800u) ok = across(c, insn);
    else if ((insn & 0x9f800400u) == 0x0f000400u && (insn >> 19 & 0xf)) ok = shift_imm(c, insn, 0);
    else if ((insn & 0xdf800400u) == 0x5f000400u && (insn >> 19 & 0xf)) ok = shift_imm(c, insn, 1);
    else if ((insn & 0x9f200c00u) == 0x0e200000u) ok = three_diff(c, insn);
    else if ((insn & 0x9fe08400u) == 0x0e000400u) ok = copy(c, insn);
    else if ((insn & 0xbf208c00u) == 0x0e000800u) ok = permute(c, insn);
    else if ((insn & 0xbfe08400u) == 0x2e000000u) ok = ext(c, insn);
    else if ((insn & 0xbfe08c00u) == 0x0e000000u) ok = tbl(c, insn);
    else if ((insn & 0xbf000000u) == 0x0d000000u) ok = ldst_single(c, insn);
    else if ((insn & 0xdf3e0c00u) == 0x5e200800u) ok = scalar_misc(c, insn);
    else if ((insn & 0xffe0fc00u) == 0x5e000400u) ok = scalar_dup(c, insn);
    else if ((insn & 0xdf3e0c00u) == 0x5e300800u) ok = scalar_pairwise(c, insn);
    else if ((insn & 0x9f000400u) == 0x0f000000u) ok = fp_elem(c, insn, 0);
    else if ((insn & 0xdf000400u) == 0x5f000000u) ok = fp_elem(c, insn, 1);
    else if ((insn & 0x5f000000u) == 0x1e000000u || (insn & 0xff000000u) == 0x1f000000u) ok = fp_scalar(c, insn);
    else return 0;
    if (!ok) UNDEF();
    return 1;
}
