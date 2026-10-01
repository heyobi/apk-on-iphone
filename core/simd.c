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

/* Element i of esz bytes (little-endian host), without memcpy calls. */
static inline uint64_t lane(const uint64_t *v, int i, int esz)
{
    const uint8_t *p = (const uint8_t *)v + i * esz;
    switch (esz) {
    case 1: return *p;
    case 2: return *(const aoi_u16u *)p;
    case 4: return *(const aoi_u32u *)p;
    default: return *(const aoi_u64u *)p;
    }
}
static inline void setlane(uint64_t *v, int i, int esz, uint64_t x)
{
    uint8_t *p = (uint8_t *)v + i * esz;
    switch (esz) {
    case 1: *p = (uint8_t)x; break;
    case 2: *(aoi_u16u *)p = (uint16_t)x; break;
    case 4: *(aoi_u32u *)p = (uint32_t)x; break;
    default: *(aoi_u64u *)p = x; break;
    }
}
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

/* frint*: mode as round_mode; infinities and zeros come back unchanged */
static uint64_t frint_bits(uint64_t a, int dbl, int mode)
{
    uint64_t r;
    if (pick_nan(a, 0, 1, dbl, &r)) return r;
    if (dbl) { double x = bd(a); return isinf(x) ? a : db(copysign(round_mode(x, mode), x)); }
    else { float x = bf((uint32_t)a); return isinf(x) ? a : fb((float)copysign(round_mode(x, mode), x)); }
}

static uint64_t fsqrt_bits(uint64_t a, int dbl)
{
    uint64_t r;
    if (pick_nan(a, 0, 1, dbl, &r)) return r;
    return dbl ? fixnan(db(sqrt(bd(a))), 1) : fixnan(fb(sqrtf(bf((uint32_t)a))), 0);
}

/* frecps (2 - a*b) / frsqrts ((3 - a*b) / 2), fused, and fmulx (fmul with 0*inf = +-2) */
static uint64_t fp_step(int op, uint64_t a, uint64_t b, int dbl)
{
    uint64_t sgn = dbl ? 1ULL << 63 : 1ULL << 31, r;
    double x, y;
    if (op != 2) a ^= sgn;                              /* FPNeg(op1) before the NaN checks */
    if (pick_nan(a, b, 2, dbl, &r)) return r;
    x = dbl ? bd(a) : bf((uint32_t)a); y = dbl ? bd(b) : bf((uint32_t)b);
    if ((isinf(x) && y == 0) || (x == 0 && isinf(y))) {
        uint64_t two = dbl ? 0x4000000000000000ULL : 0x40000000u, onehalf = dbl ? 0x3ff8000000000000ULL : 0x3fc00000u;
        return op == 2 ? (two | ((a ^ b) & sgn)) : op == 0 ? two : onehalf;
    }
    if (op == 2) return fp2(0, a, b, dbl);
    if (isinf(x) || isinf(y)) return (dbl ? 0x7ff0000000000000ULL : 0x7f800000u) | ((a ^ b) & sgn);
    if (op == 1) {                                       /* halve the larger operand: exact, one rounding */
        if (fabs(x) >= fabs(y)) x *= 0.5; else y *= 0.5;
        return dbl ? db(fma(x, y, 1.5)) : fb(fmaf((float)x, (float)y, 1.5f));
    }
    return dbl ? db(fma(x, y, 2.0)) : fb(fmaf((float)x, (float)y, 2.0f));
}

/* RecipEstimate / RecipSqrtEstimate of the ARM ARM: 8-bit estimates, 256..511 */
static int recip_est(int a) { a = a * 2 + 1; return ((1 << 19) / a + 1) / 2; }
static int rsqrt_est(int a)
{
    long b = 512;
    if (a < 256) a = a * 2 + 1; else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    while ((long)a * (b + 1) * (b + 1) < (1L << 28)) b++;
    return (int)((b + 1) / 2);
}

/* FPRecipEstimate (frecpe) and FPRSqrtEstimate (frsqrte), FPCR default (RNE, no FZ) */
static uint64_t frecpe_bits(uint64_t a, int dbl, int sqrt_)
{
    int N = dbl ? 64 : 32, fbits = dbl ? 52 : 23, ebits = N - 1 - fbits, ex, est, rexp;
    uint64_t sign = a >> (N - 1) & 1, frac, r, inf = (uint64_t)((1 << ebits) - 1) << fbits;
    ex = (int)(a >> fbits & ((1u << ebits) - 1));
    frac = (a & (((uint64_t)1 << fbits) - 1)) << (52 - fbits);   /* as a 52-bit fraction */
    if (pick_nan(a, 0, 1, dbl, &r)) return r;
    if (ex == 0 && !frac) return sign << (N - 1) | inf;               /* 0 -> inf */
    if (sqrt_) {
        if (sign) return defnan(dbl);
        if (ex == (1 << ebits) - 1) return 0;                         /* +inf -> +0 */
        if (ex == 0) { while (!(frac >> 51 & 1)) { frac = (frac << 1) & ((1ULL << 52) - 1); ex--; }
                       frac = (frac << 1) & ((1ULL << 52) - 1); }
        est = rsqrt_est(ex & 1 ? (int)(128 | frac >> 45) : (int)(256 | frac >> 44));
        rexp = ((dbl ? 3068 : 380) - ex) / 2;
        return (uint64_t)rexp << fbits | (uint64_t)(est & 0xff) << (fbits - 8);
    }
    if (ex == (1 << ebits) - 1) return sign << (N - 1);              /* inf -> 0 */
    if (fabs(dbl ? bd(a) : bf((uint32_t)a)) < (dbl ? ldexp(1.0, -1024) : ldexp(1.0, -128)))
        return sign << (N - 1) | inf;                                 /* overflows: inf (RNE) */
    if (ex == 0) {
        if (!(frac >> 51 & 1)) { ex = -1; frac = (frac << 2) & ((1ULL << 52) - 1); }
        else frac = (frac << 1) & ((1ULL << 52) - 1);
    }
    est = recip_est((int)(256 | frac >> 44));
    rexp = (dbl ? 2045 : 253) - ex;
    frac = (uint64_t)(est & 0xff) << 44;
    if (rexp == 0) frac = 1ULL << 51 | frac >> 1;
    else if (rexp == -1) { frac = 1ULL << 50 | frac >> 2; rexp = 0; }
    return sign << (N - 1) | (uint64_t)rexp << fbits | frac >> (52 - fbits);
}

/* IEEE half precision <-> single/double (FPCR.AHP = 0, RNE), NaN payloads kept and quieted */
static uint64_t half_to(uint64_t h, int dbl)
{
    int sign = h >> 15 & 1, ex = h >> 10 & 31, man = h & 0x3ff;
    double v;
    if (ex == 31) {
        if (!man) return dbl ? (uint64_t)sign << 63 | 0x7ff0000000000000ULL : (uint64_t)sign << 31 | 0x7f800000u;
        return dbl ? (uint64_t)sign << 63 | 0x7ff8000000000000ULL | (uint64_t)man << 42
                   : (uint64_t)sign << 31 | 0x7fc00000u | (uint64_t)man << 13;
    }
    v = ex ? ldexp(1024 + man, ex - 25) : ldexp(man, -24);
    if (sign) v = -v;
    return dbl ? db(v) : fb((float)v);
}
static uint64_t to_half(uint64_t a, int dbl)
{
    uint64_t sign = (a >> (dbl ? 63 : 31) & 1) << 15;
    double x;
    int e;
    if (isnan_b(a, dbl)) return sign | 0x7e00 | ((dbl ? a >> 42 : a >> 13) & 0x1ff);
    x = fabs(dbl ? bd(a) : bf((uint32_t)a));
    if (x >= 65520.0) return sign | 0x7c00;           /* also inf; 65520 rounds up to 2^16 */
    if (x < ldexp(1.0, -14)) return sign | (uint64_t)round_mode(ldexp(x, 24), 0);
    frexp(x, &e);                                     /* x = f * 2^e, 0.5 <= f < 1 */
    return sign | (((uint64_t)(e + 14) << 10) + ((uint64_t)round_mode(ldexp(x, 11 - e), 0) - 1024));
}

/* saturate a wide result to a signed / unsigned element */
static uint64_t sat_s(__int128 v, int esz)
{
    int64_t mx = (int64_t)(emask(esz) >> 1);
    return (uint64_t)(v > mx ? mx : v < -mx - 1 ? -mx - 1 : (int64_t)v) & emask(esz);
}
static uint64_t sat_u(__int128 v, int esz) { return v < 0 ? 0 : v > (__int128)emask(esz) ? emask(esz) : (uint64_t)v; }

/* sqdmulh / sqrdmulh on one element */
static uint64_t sqdmulh(uint64_t x, uint64_t y, int esz, int round)
{
    __int128 p = (__int128)sx(x, esz) * sx(y, esz) * 2 + (round ? (__int128)1 << (8 * esz - 1) : 0);
    return sat_s(p >> (8 * esz), esz);
}

/* FP two-register-misc ops shared by the vector and scalar forms; key = U:a:opcode */
static int fp_misc(int key, uint64_t x, int dbl, uint64_t *out, int scalar)
{
    int fesz = dbl ? 8 : 4;
    double v = dbl ? bd(x) : bf((uint32_t)x);
    int nan = isnan_b(x, dbl);
    if (!scalar) switch (key) {                       /* vector only */
    case 0x2f: *out = x & (emask(fesz) >> 1); return 1;              /* fabs */
    case 0x6f: *out = x ^ (1ULL << (8 * fesz - 1)); return 1;       /* fneg */
    case 0x7f: *out = fsqrt_bits(x, dbl); return 1;                  /* fsqrt */
    case 0x18: *out = frint_bits(x, dbl, 0); return 1;               /* frintn */
    case 0x19: *out = frint_bits(x, dbl, 2); return 1;               /* frintm */
    case 0x38: *out = frint_bits(x, dbl, 1); return 1;               /* frintp */
    case 0x39: *out = frint_bits(x, dbl, 3); return 1;               /* frintz */
    case 0x58: *out = frint_bits(x, dbl, 4); return 1;               /* frinta */
    case 0x59: case 0x79: *out = frint_bits(x, dbl, 0); return 1;    /* frintx / frinti (FPCR: RNE) */
    case 0x3c: case 0x7c:                                             /* urecpe / ursqrte */
        if (dbl) return 0;
        if (key == 0x3c ? !(x >> 31 & 1) : !(x >> 30 & 3)) *out = 0xffffffffu;
        else *out = (uint64_t)(key == 0x3c ? recip_est((int)(x >> 23 & 0x1ff)) : rsqrt_est((int)(x >> 23 & 0x1ff))) << 23;
        return 1;
    }
    if (scalar && key == 0x3f) {                      /* frecpx: sign, inverted exponent, zero fraction */
        int fbits = dbl ? 52 : 23;
        uint64_t em = (dbl ? 0x7ffULL : 0xffULL) << fbits, ex = x & em;
        if (pick_nan(x, 0, 1, dbl, out)) return 1;
        *out = (x & ~(emask(fesz) >> 1)) | (ex ? ~ex & em : em - (1ULL << fbits));
        return 1;
    }
    switch (key) {
    case 0x2c: *out = !nan && v > 0 ? emask(fesz) : 0; return 1;    /* fcmgt #0 */
    case 0x6c: *out = !nan && v >= 0 ? emask(fesz) : 0; return 1;   /* fcmge #0 */
    case 0x2d: *out = !nan && v == 0 ? emask(fesz) : 0; return 1;   /* fcmeq #0 */
    case 0x6d: *out = !nan && v <= 0 ? emask(fesz) : 0; return 1;   /* fcmle #0 */
    case 0x2e: *out = !nan && v < 0 ? emask(fesz) : 0; return 1;    /* fcmlt #0 */
    case 0x3d: *out = frecpe_bits(x, dbl, 0); return 1;              /* frecpe */
    case 0x7d: *out = frecpe_bits(x, dbl, 1); return 1;              /* frsqrte */
    case 0x5a: *out = fcvt_int(x, dbl, 0, 1, dbl, 0); return 1;      /* fcvtnu */
    case 0x5b: *out = fcvt_int(x, dbl, 2, 1, dbl, 0); return 1;      /* fcvtmu */
    case 0x7a: *out = fcvt_int(x, dbl, 1, 1, dbl, 0); return 1;      /* fcvtpu */
    case 0x5c: *out = fcvt_int(x, dbl, 4, 1, dbl, 0); return 1;      /* fcvtau */
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

static __attribute__((noinline)) int three_same(struct aoi_cpu *c, uint32_t insn, int scalar)
{
    int q = scalar ? 1 : insn >> 30 & 1, u = insn >> 29 & 1, size = insn >> 22 & 3, op = insn >> 11 & 0x1f;
    int m = insn >> 16 & 31, n = insn >> 5 & 31, d = insn & 31, esz = 1 << size, ne = scalar ? 1 : (q ? 16 : 8) / esz, i;
    const uint64_t *a = c->vreg[n], *b = c->vreg[m];
    vec r = {0, 0};

    if (op == 0x03) {                                /* logical, on whole halves */
        if (scalar) return 0;
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
    if (op >= 0x18) {                                 /* FP three-same */
        int dbl = size & 1, fesz = dbl ? 8 : 4, fne = scalar ? 1 : (q ? 16 : 8) / fesz, hi = size >> 1;
        int k = u << 6 | hi << 5 | op;
        if (dbl && !q) return 0;
        /* fast path: 4 x single fadd/fsub/fmul/fmax/fmin with the default FPCR (round to
         * nearest, no flush-to-zero, no default NaN) is the host's IEEE arithmetic; a NaN
         * anywhere takes the general path below (ARM's NaN propagation rules). Skia's
         * raster pipeline is mostly these. */
        if (!scalar && q && !dbl && !(c->fpcr & 0x03c00000u) &&
            (k == 0x1a || k == 0x3a || k == 0x5b || k == 0x1e || k == 0x3e)) {
            float x[4], y[4], z[4];
            int nan = 0;
            memcpy(x, a, 16); memcpy(y, b, 16);
            switch (k) {                                /* one loop per op: the compiler vectorizes them */
            case 0x1a: for (i = 0; i < 4; i++) z[i] = x[i] + y[i]; break;
            case 0x3a: for (i = 0; i < 4; i++) z[i] = x[i] - y[i]; break;
            case 0x5b: for (i = 0; i < 4; i++) z[i] = x[i] * y[i]; break;
            default: {                                  /* fmax / fmin: a NaN operand need not reach z */
                uint32_t xb[4], yb[4], zb[4];
                memcpy(xb, a, 16); memcpy(yb, b, 16);
                for (i = 0; i < 4; i++) nan |= x[i] != x[i] || y[i] != y[i];
                for (i = 0; i < 4; i++)                 /* equal: +0 vs -0, max(+0, -0) = +0 */
                    zb[i] = x[i] == y[i] ? (k == 0x1e ? xb[i] & yb[i] : xb[i] | yb[i])
                          : (k == 0x1e ? x[i] > y[i] : x[i] < y[i]) ? xb[i] : yb[i];
                memcpy(z, zb, 16);
                break;
            }
            }
            /* a NaN result (from a NaN operand, inf - inf, 0 * inf) takes the general
             * path: ARM's NaN rules */
            for (i = 0; i < 4; i++) nan |= z[i] != z[i];
            if (!nan) { memcpy(c->vreg[d], z, 16); return 1; }
        }
        /* scalar: fmulx, fcmeq/ge/gt, facge/gt, fabd, frecps, frsqrts */
        if (scalar && k != 0x1b && k != 0x1c && k != 0x5c && k != 0x7c && k != 0x5d && k != 0x7d
            && k != 0x7a && k != 0x1f && k != 0x3f) return 0;
        for (i = 0; i < fne; i++) {
            uint64_t x = lane(a, i, fesz), y = lane(b, i, fesz), z = 0;
            if (k == 0x5a || k == 0x58 || k == 0x78 || k == 0x5e || k == 0x7e) {   /* pairwise, over m:n */
                const uint64_t *src = i < fne / 2 ? a : b;
                int j = (i % (fne / 2)) * 2;
                x = lane(src, j, fesz); y = lane(src, j + 1, fesz);
            }
            switch (k) {
            case 0x1a: case 0x5a: z = fp2(2, x, y, dbl); break;      /* fadd / faddp */
            case 0x3a: z = fp2(3, x, y, dbl); break;                 /* fsub */
            case 0x7a: z = fp2(3, x, y, dbl) & (emask(fesz) >> 1); break;   /* fabd */
            case 0x5b: z = fp2(0, x, y, dbl); break;                 /* fmul */
            case 0x1b: z = fp_step(2, x, y, dbl); break;             /* fmulx */
            case 0x5f: z = fp2(1, x, y, dbl); break;                 /* fdiv */
            case 0x1f: z = fp_step(0, x, y, dbl); break;             /* frecps */
            case 0x3f: z = fp_step(1, x, y, dbl); break;             /* frsqrts */
            case 0x19: case 0x39: {                                   /* fmla / fmls (fused) */
                uint64_t acc = lane(c->vreg[d], i, fesz);
                z = fma_bits(acc, k == 0x39 ? x ^ (1ULL << (8 * fesz - 1)) : x, y, dbl, 0, 0);
                break; }
            case 0x1e: case 0x5e: z = fp2(4, x, y, dbl); break;      /* fmax / fmaxp */
            case 0x3e: case 0x7e: z = fp2(5, x, y, dbl); break;      /* fmin / fminp */
            case 0x18: case 0x58: z = fp2(6, x, y, dbl); break;      /* fmaxnm / fmaxnmp */
            case 0x38: case 0x78: z = fp2(7, x, y, dbl); break;      /* fminnm / fminnmp */
            case 0x5d: case 0x7d:                                     /* facge / facgt: on |x|, |y| */
                x &= emask(fesz) >> 1; y &= emask(fesz) >> 1;
                /* fall through */
            case 0x1c: case 0x5c: case 0x7c: {                        /* fcmeq / fcmge / fcmgt */
                double xv = dbl ? bd(x) : bf((uint32_t)x), yv = dbl ? bd(y) : bf((uint32_t)y);
                int t = isnan_b(x, dbl) || isnan_b(y, dbl) ? 0 : k == 0x1c ? xv == yv
                        : k == 0x5c || k == 0x5d ? xv >= yv : xv > yv;
                z = t ? emask(fesz) : 0; break; }
            default: return 0;
            }
            setlane(r, i, fesz, z);
        }
        setv(c, d, r, scalar ? 0 : q);
        return 1;
    }
    if (size == 3 && !q) return 0;
    if (scalar && !(op == 0x01 || op == 0x05 || (op == 0x16 && (size == 1 || size == 2))
                    || (size == 3 && (op == 0x06 || op == 0x07 || op == 0x08 || op == 0x10 || op == 0x11)))) return 0;
    for (i = 0; i < ne; i++) {
        uint64_t x = lane(a, i, esz), y = lane(b, i, esz), z;
        int64_t sxv = sx(x, esz), syv = sx(y, esz);
        switch (op) {
        case 0x10: z = u ? x - y : x + y; break;                                   /* add/sub */
        case 0x01: z = u ? sat_u((__int128)x + y, esz) : sat_s((__int128)sxv + syv, esz); break;   /* [su]qadd */
        case 0x05: z = u ? sat_u((__int128)x - y, esz) : sat_s((__int128)sxv - syv, esz); break;   /* [su]qsub */
        case 0x16: if (size == 0 || size == 3) return 0; z = sqdmulh(x, y, esz, u); break;   /* sq[r]dmulh */
        case 0x06: z = (u ? x > y : sxv > syv) ? ~0ULL : 0; break;                 /* cmhi/cmgt */
        case 0x07: z = (u ? x >= y : sxv >= syv) ? ~0ULL : 0; break;               /* cmhs/cmge */
        case 0x11: z = (u ? x == y : (x & y) != 0) ? ~0ULL : 0; break;             /* cmeq/cmtst */
        case 0x0c: if (size == 3) return 0; z = u ? (x > y ? x : y) : (uint64_t)(sxv > syv ? sxv : syv); break;   /* max */
        case 0x0d: if (size == 3) return 0; z = u ? (x < y ? x : y) : (uint64_t)(sxv < syv ? sxv : syv); break;   /* min */
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
        case 0x17: case 0x14: case 0x15: {                                           /* addp / [su]maxp / [su]minp */
            const uint64_t *src = i < ne / 2 ? a : b;
            int j = (i % (ne / 2)) * 2;
            uint64_t p0 = lane(src, j, esz), p1 = lane(src, j + 1, esz);
            int64_t s0 = sx(p0, esz), s1 = sx(p1, esz);
            if (op == 0x17) { if (u) return 0; z = p0 + p1; break; }
            if (size == 3) return 0;
            if (op == 0x14) z = u ? (p0 > p1 ? p0 : p1) : (uint64_t)(s0 > s1 ? s0 : s1);
            else z = u ? (p0 < p1 ? p0 : p1) : (uint64_t)(s0 < s1 ? s0 : s1);
            break; }
        case 0x04:                                                                   /* [su]hsub */
            if (size == 3) return 0;
            z = u ? (x - y) >> 1 : (uint64_t)((sxv - syv) >> 1); break;
        case 0x0e: case 0x0f:                                                        /* [su]abd / [su]aba */
            if (size == 3) return 0;
            z = u ? (x > y ? x - y : y - x) : (uint64_t)(sxv > syv ? sxv - syv : syv - sxv);
            if (op == 0x0f) z += lane(c->vreg[d], i, esz);
            break;
        default: return 0;
        }
        setlane(r, i, esz, z & emask(esz));
    }
    setv(c, d, r, scalar ? 0 : q);
    return 1;
}

static __attribute__((noinline)) int two_misc(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, size = insn >> 22 & 3, op = insn >> 12 & 0x1f;
    int n = insn >> 5 & 31, d = insn & 31, esz = 1 << size, ne = (q ? 16 : 8) / esz, i, j;
    const uint64_t *a = c->vreg[n];
    vec r = {0, 0};

    if ((op == 0x16 || op == 0x17) && !u && size < 2) {   /* fcvtn{2} d->s / s->h, fcvtl{2} s->d / h->s */
        int wsz = size ? 8 : 4, nsz = wsz / 2, k = 8 / nsz;
        if (op == 0x16) {
            r[0] = q ? c->vreg[d][0] : 0; r[1] = 0;
            for (i = 0; i < k; i++)
                setlane(r, i + (q ? k : 0), nsz, size ? cvt_sd(lane(a, i, 8), 0) : to_half(lane(a, i, 4), 0));
            c->vreg[d][0] = r[0]; c->vreg[d][1] = q ? r[1] : 0;
            return 1;
        }
        for (i = 0; i < k; i++) {
            uint64_t x = lane(a, i + (q ? k : 0), nsz);
            setlane(r, i, wsz, size ? cvt_sd(x, 1) : half_to(x, 0));
        }
        setv(c, d, r, 1);
        return 1;
    }
    if ((op == 0x12 && u) || op == 0x14) {            /* sqxtun{2} / sqxtn{2} / uqxtn{2} */
        if (size == 3) return 0;
        r[0] = q ? c->vreg[d][0] : 0;
        for (i = 0; i < 8 / esz; i++) {
            uint64_t x = lane(a, i, 2 * esz);
            setlane(r, i + (q ? 8 / esz : 0), esz, op == 0x14 && u ? sat_u(x, esz)
                    : op == 0x14 ? sat_s(sx(x, 2 * esz), esz) : sat_u(sx(x, 2 * esz), esz));
        }
        c->vreg[d][0] = r[0]; c->vreg[d][1] = q ? r[1] : 0;
        return 1;
    }
    if (op == 0x13 && u) {                            /* shll{2}: widen, shift left by the element size */
        if (size == 3) return 0;
        for (i = 0; i < 8 / esz; i++)
            setlane(r, i, 2 * esz, lane(a, i + (q ? 8 / esz : 0), esz) << (8 * esz));
        setv(c, d, r, 1);
        return 1;
    }
    if (op >= 0x0c && op != 0x12) {                   /* FP forms: size = a:sz */
        int dbl = size & 1, fesz = dbl ? 8 : 4, key = u << 6 | (size >> 1) << 5 | op;
        uint64_t z;
        if (dbl && !q) return 0;
        /* fast path, single precision with the default FPCR (round to nearest even, as the
         * host): conversions to and from 32-bit integers, which Skia does per pixel */
        if (!dbl && !(c->fpcr & 0x03c00000u) &&
            (key == 0x1d || key == 0x5d || key == 0x3b || key == 0x7b || key == 0x1a || key == 0x5a)) {
            uint32_t xb[4], zb[4] = {0, 0, 0, 0};
            float f;
            memcpy(xb, a, 16);
            for (i = 0; i < (q ? 4 : 2); i++) {
                switch (key) {
                case 0x1d: f = (float)(int32_t)xb[i]; memcpy(&zb[i], &f, 4); break;              /* scvtf */
                case 0x5d: f = (float)xb[i]; memcpy(&zb[i], &f, 4); break;                       /* ucvtf */
                default:
                    memcpy(&f, &xb[i], 4);
                    if (key == 0x1a || key == 0x5a) f = rintf(f);                                 /* fcvtn*: to even */
                    else f = truncf(f);                                                           /* fcvtz* */
                    if (key & 0x40) zb[i] = !(f > 0) ? 0 : f >= 4294967296.0f ? 0xffffffffu : (uint32_t)f;
                    else zb[i] = f != f ? 0 : f >= 2147483648.0f ? 0x7fffffffu
                               : f < -2147483648.0f ? 0x80000000u : (uint32_t)(int32_t)f;
                    break;
                }
            }
            memcpy(c->vreg[d], zb, 16);
            if (!q) c->vreg[d][1] = 0;
            return 1;
        }
        for (i = 0; i < (q ? 16 : 8) / fesz; i++) {
            if (!fp_misc(key, lane(a, i, fesz), dbl, &z, 0)) return 0;
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
    case 0x04:                                        /* cls / clz */
        if (size == 3) return 0;
        for (i = 0; i < ne; i++) {
            uint64_t x = lane(a, i, esz);
            int bits = 8 * esz, k, cnt = 0;
            if (u) { for (k = bits - 1; k >= 0 && !(x >> k & 1); k--) cnt++; }
            else { int top = (int)(x >> (bits - 1) & 1); for (k = bits - 2; k >= 0 && (int)(x >> k & 1) == top; k--) cnt++; }
            setlane(r, i, esz, (uint64_t)cnt);
        }
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
    case 0x03: case 0x07:                             /* suqadd / usqadd, sqabs / sqneg */
        if (size == 3 && !q) return 0;
        for (i = 0; i < ne; i++) {
            uint64_t x = lane(a, i, esz), y = lane(c->vreg[d], i, esz);
            __int128 sxv = sx(x, esz);
            setlane(r, i, esz, op == 7 ? sat_s(u ? -sxv : sxv < 0 ? -sxv : sxv, esz)
                    : u ? sat_u((__int128)y + sxv, esz) : sat_s((__int128)sx(y, esz) + x, esz));
        }
        break;
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

static __attribute__((noinline)) int across(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, size = insn >> 22 & 3, op = insn >> 12 & 0x1f;
    int n = insn >> 5 & 31, d = insn & 31, esz = 1 << size, ne = (q ? 16 : 8) / esz, i;
    const uint64_t *a = c->vreg[n];
    uint64_t acc;
    int resz = esz;
    if (u && (op == 0x0c || op == 0x0f)) {            /* fmaxnmv / fminnmv / fmaxv / fminv: 4s only */
        int k = (op == 0x0c ? 6 : 4) + (size >> 1);
        if ((size & 1) || !q) return 0;
        acc = fp2(k, fp2(k, lane(a, 0, 4), lane(a, 1, 4), 0), fp2(k, lane(a, 2, 4), lane(a, 3, 4), 0), 0);
        c->vreg[d][0] = acc; c->vreg[d][1] = 0;
        return 1;
    }
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

static __attribute__((noinline)) int shift_imm(struct aoi_cpu *c, uint32_t insn, int scalar)
{
    int q = scalar ? 1 : insn >> 30 & 1, u = insn >> 29 & 1, immh = insn >> 19 & 0xf, op = insn >> 11 & 0x1f;
    int n = insn >> 5 & 31, d = insn & 31, immhb = insn >> 16 & 0x7f, i;
    int esz = immh & 8 ? 8 : immh & 4 ? 4 : immh & 2 ? 2 : 1, bits = 8 * esz, ne;
    int rsh = 2 * bits - immhb, lsh = immhb - bits;
    const uint64_t *a = c->vreg[n];
    vec r = {0, 0};

    if (scalar && esz != 8 && op != 0x0c && op != 0x0e && (op < 0x10 || op > 0x13) && op != 0x1c && op != 0x1f) return 0;
    if (op == 0x14) {                                  /* [su]shll{2}: widen */
        if (esz == 8 || scalar) return 0;
        for (i = 0; i < 8 / esz; i++) {
            uint64_t x = lane(a, i + (q ? 8 / esz : 0), esz);
            uint64_t w = u ? x : (uint64_t)sx(x, esz);
            setlane(r, i, 2 * esz, (w << lsh) & emask(2 * esz));
        }
        setv(c, d, r, 1);
        return 1;
    }
    if (op >= 0x10 && op <= 0x13) {                    /* narrowing right shifts (esz is the result size) */
        int dsz = esz, sq = op >= 0x12 || u;           /* shrn/rshrn truncate, the others saturate */
        if (esz == 8 || (scalar && !sq)) return 0;
        if (!scalar && q) r[0] = c->vreg[d][0];
        for (i = 0; i < (scalar ? 1 : 8 / dsz); i++) {
            uint64_t x = lane(a, i, 2 * dsz), z;
            __int128 v = (op >= 0x12 && u) || !sq ? (__int128)x : (__int128)sx(x, 2 * dsz);
            v = (v + (op & 1 ? (__int128)1 << (rsh - 1) : 0)) >> rsh;     /* r*: round */
            z = !sq ? (uint64_t)v & emask(dsz) : op < 0x12 || u ? sat_u(v, dsz) : sat_s(v, dsz);
            setlane(r, i + (q && !scalar ? 8 / dsz : 0), dsz, z);
        }
        setv(c, d, r, 0);
        if (!scalar && q) c->vreg[d][1] = r[1];
        return 1;
    }
    if ((op == 0x1c || op == 0x1f) && esz < 4) return 0;   /* fixed-point conversions: s/d only (no fp16) */
    if (esz == 8 && !q) return 0;
    ne = scalar ? 1 : (q ? 16 : 8) / esz;
    if (!scalar && (op == 0x00 || (op == 0x0a && !u))) {   /* ushr / sshr / shl: lanes typed, no helpers */
        #define SHIFT_LANES(T, ST, N)                                                               \
        { T x[N], z[N]; memcpy(x, a, sizeof x);                                                      \
          for (i = 0; i < ne; i++)                                                                   \
              z[i] = op == 0x0a ? (T)(x[i] << lsh) : u ? (T)(rsh >= bits ? 0 : x[i] >> rsh)          \
                   : (T)((ST)x[i] >> (rsh >= bits ? bits - 1 : rsh));                                \
          for (; i < N; i++) z[i] = 0;                                                               \
          memcpy(c->vreg[d], z, sizeof z); }
        switch (esz) {
        case 1: SHIFT_LANES(uint8_t, int8_t, 16) break;
        case 2: SHIFT_LANES(uint16_t, int16_t, 8) break;
        case 4: SHIFT_LANES(uint32_t, int32_t, 4) break;
        default: SHIFT_LANES(uint64_t, int64_t, 2) break;
        }
        #undef SHIFT_LANES
        return 1;
    }
    for (i = 0; i < ne; i++) {
        uint64_t x = lane(a, i, esz), z, dv = lane(c->vreg[d], i, esz);
        __int128 sxv = sx(x, esz);
        switch (op) {
        case 0x00: case 0x02:                          /* [su]shr / [su]sra */
            z = u ? (rsh >= 64 ? 0 : x >> rsh) : (uint64_t)(rsh >= 64 ? (sx(x, esz) < 0 ? -1 : 0) : sx(x, esz) >> rsh);
            if (op == 2) z += dv;
            break;
        case 0x04: case 0x06:                          /* [su]rshr / [su]rsra */
            z = (uint64_t)(((u ? (__int128)x : sxv) + ((__int128)1 << (rsh - 1))) >> rsh);
            if (op == 6) z += dv;
            break;
        case 0x0c:                                     /* sqshlu */
            if (!u) return 0;
            z = sat_u(sxv * ((__int128)1 << lsh), esz);
            break;
        case 0x0e:                                     /* sqshl / uqshl (immediate) */
            z = u ? sat_u((__int128)x << lsh, esz) : sat_s(sxv * ((__int128)1 << lsh), esz);
            break;
        case 0x1c: z = int_fcvt(x, esz == 8, u, esz == 8, rsh); break;        /* scvtf / ucvtf, fixed */
        case 0x1f: z = fcvt_int(x, esz == 8, 3, u, esz == 8, rsh); break;     /* fcvtzs / fcvtzu, fixed */
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

static __attribute__((noinline)) int three_diff(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, size = insn >> 22 & 3, op = insn >> 12 & 0xf;
    int m = insn >> 16 & 31, n = insn >> 5 & 31, d = insn & 31, esz = 1 << size, i, half = q ? 8 / esz : 0;
    vec r = {0, 0};
    if (op == 0xe && !u && (size == 0 || size == 3)) {   /* pmull{2}: carry-less multiply */
        if (size == 0) {
            for (i = 0; i < 8; i++) {
                uint64_t x = lane(c->vreg[n], i + (q ? 8 : 0), 1), y = lane(c->vreg[m], i + (q ? 8 : 0), 1), z = 0;
                int k;
                for (k = 0; k < 8; k++) if (y >> k & 1) z ^= x << k;
                setlane(r, i, 2, z);
            }
        } else {                                       /* 1d x 1d -> 1q */
            uint64_t x = c->vreg[n][q], y = c->vreg[m][q];
            int k;
            for (k = 0; k < 64; k++)
                if (y >> k & 1) { r[0] ^= x << k; if (k) r[1] ^= x >> (64 - k); }
        }
        setv(c, d, r, 1);
        return 1;
    }
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
        case 0x5: case 0x7:                            /* [su]abal / [su]abdl */
            z = u ? (x > y ? x - y : y - x) : (uint64_t)((int64_t)x > (int64_t)y ? x - y : y - x);
            if (op == 5) z += acc;
            break;
        default: return 0;
        }
        setlane(r, i, 2 * esz, z & emask(2 * esz));
    }
    setv(c, d, r, 1);
    return 1;
}

static __attribute__((noinline)) int copy(struct aoi_cpu *c, uint32_t insn)
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

static __attribute__((noinline)) int permute(struct aoi_cpu *c, uint32_t insn)
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

static __attribute__((noinline)) int ext(struct aoi_cpu *c, uint32_t insn)
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

static __attribute__((noinline)) int tbl(struct aoi_cpu *c, uint32_t insn)
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

static __attribute__((noinline)) int scalar_misc(struct aoi_cpu *c, uint32_t insn)
{
    int u = insn >> 29 & 1, size = insn >> 22 & 3, op = insn >> 12 & 0x1f, n = insn >> 5 & 31, d = insn & 31;
    int dbl = size & 1, key = u << 6 | (size >> 1) << 5 | op;
    uint64_t z;
    if (op < 0x0c || !fp_misc(key, lane(c->vreg[n], 0, dbl ? 8 : 4), dbl, &z, 1)) return 0;
    c->vreg[d][0] = z; c->vreg[d][1] = 0;
    return 1;
}

static __attribute__((noinline)) int scalar_pairwise(struct aoi_cpu *c, uint32_t insn)
{
    int u = insn >> 29 & 1, size = insn >> 22 & 3, op = insn >> 12 & 0x1f, n = insn >> 5 & 31, d = insn & 31;
    if (u && (op == 0x0c || op == 0x0d || op == 0x0f)) {   /* fmaxnmp / fminnmp / faddp / fmaxp / fminp */
        int dbl = size & 1, fesz = dbl ? 8 : 4, k = op == 0x0d ? 2 : (op == 0x0c ? 6 : 4) + (size >> 1);
        if (op == 0x0d && size >> 1) return 0;
        c->vreg[d][0] = fp2(k, lane(c->vreg[n], 0, fesz), lane(c->vreg[n], 1, fesz), dbl);
        c->vreg[d][1] = 0;
        return 1;
    }
    if (u || size != 3 || op != 0x1b) return 0;       /* addp d, v.2d */
    c->vreg[d][0] = c->vreg[n][0] + c->vreg[n][1];
    c->vreg[d][1] = 0;
    return 1;
}

/* integer ops by element: mul/mla/mls, [su]mull/[su]mlal/[su]mlsl{2}, sqdmull/sqdmlal/sqdmlsl{2},
 * sq[r]dmulh (also scalar). size 1: Vm is v0-v15, index H:L:M; size 2: index H:L. */
static int int_elem(struct aoi_cpu *c, uint32_t insn, int scalar)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, size = insn >> 22 & 3, L = insn >> 21 & 1, M = insn >> 20 & 1;
    int rm = insn >> 16 & 15, op = insn >> 12 & 0xf, H = insn >> 11 & 1, n = insn >> 5 & 31, d = insn & 31;
    int esz = 1 << size, idx, i, half = q ? 8 / esz : 0, longop = (op & 3) >= 2 && op != 0xe && op != 0xf;
    uint64_t e;
    vec r = {0, 0};
    if (size == 0 || size == 3) return 0;
    if (scalar && op != 0xc && op != 0xd && op != 0x3 && op != 0x7 && op != 0xb) return 0;
    if (u && (op & 1)) return 0;                       /* sqdmlal etc. and sqdmulh are U=0 only */
    if (size == 1) { idx = H << 2 | L << 1 | M; e = lane(c->vreg[rm], idx, 2); }
    else { idx = H << 1 | L; e = lane(c->vreg[M << 4 | rm], idx, 4); }
    if (!longop) {                                     /* same size: mul, mla, mls, sqdmulh, sqrdmulh */
        for (i = 0; i < (scalar ? 1 : (q ? 16 : 8) / esz); i++) {
            uint64_t x = lane(c->vreg[n], i, esz), acc = lane(c->vreg[d], i, esz), z;
            switch (u << 4 | op) {
            case 0x08: z = x * e; break;                                   /* mul */
            case 0x10: z = acc + x * e; break;                             /* mla */
            case 0x14: z = acc - x * e; break;                             /* mls */
            case 0x0c: case 0x0d: z = sqdmulh(x, e, esz, op & 1); break;   /* sqdmulh / sqrdmulh */
            default: return 0;
            }
            setlane(r, i, esz, z & emask(esz));
        }
        setv(c, d, r, scalar ? 0 : q);
        return 1;
    }
    for (i = 0; i < (scalar ? 1 : 8 / esz); i++) {    /* widening: the {2} forms take the upper half */
        uint64_t x = lane(c->vreg[n], i + (scalar ? 0 : half), esz), acc = lane(c->vreg[d], i, 2 * esz), z;
        __int128 p = u ? (__int128)(x * e) : (__int128)sx(x, esz) * sx(e, esz);
        switch (u << 4 | op) {
        case 0x0a: case 0x1a: z = (uint64_t)p; break;                     /* [su]mull */
        case 0x02: case 0x12: z = acc + (uint64_t)p; break;               /* [su]mlal */
        case 0x06: case 0x16: z = acc - (uint64_t)p; break;               /* [su]mlsl */
        case 0x0b: z = sat_s(2 * p, 2 * esz); break;                      /* sqdmull */
        case 0x03: z = sat_s((__int128)sx(acc, 2 * esz) + sx(sat_s(2 * p, 2 * esz), 2 * esz), 2 * esz); break;  /* sqdmlal */
        case 0x07: z = sat_s((__int128)sx(acc, 2 * esz) - sx(sat_s(2 * p, 2 * esz), 2 * esz), 2 * esz); break;  /* sqdmlsl */
        default: return 0;
        }
        setlane(r, i, 2 * esz, z & emask(2 * esz));
    }
    setv(c, d, r, !scalar);
    return 1;
}

/* fmul / fmulx / fmla / fmls by element, vector and scalar */
static __attribute__((noinline)) int fp_elem(struct aoi_cpu *c, uint32_t insn, int scalar)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, sz = insn >> 22 & 1, L = insn >> 21 & 1, M = insn >> 20 & 1;
    int rm = insn >> 16 & 15, op = insn >> 12 & 0xf, H = insn >> 11 & 1, n = insn >> 5 & 31, d = insn & 31;
    int dbl = sz, fesz = dbl ? 8 : 4, idx, ne, i;
    uint64_t e;
    vec r = {0, 0};
    if (op != 0x1 && op != 0x5 && op != 0x9) return int_elem(c, insn, scalar);
    if (!(insn >> 23 & 1) || (u && op != 0x9) || (dbl && L) || (!scalar && dbl && !q)) return 0;
    idx = dbl ? H : H << 1 | L;
    e = lane(c->vreg[M << 4 | rm], idx, fesz);
    ne = scalar ? 1 : (q ? 16 : 8) / fesz;
    /* fast path: 4 x single fmla / fmls / fmul by element, default FPCR, no NaN in or out
     * (ARM's NaN rules take the general path) */
    if (!scalar && q && !dbl && !u && !(c->fpcr & 0x03c00000u)) {
        float x[4], acc[4], z[4], f;
        int nan;
        memcpy(&f, &e, 4); memcpy(x, c->vreg[n], 16); memcpy(acc, c->vreg[d], 16);
        nan = f != f;
        for (i = 0; i < 4; i++) nan |= x[i] != x[i] || (op != 0x9 && acc[i] != acc[i]);
        if (!nan) {
            for (i = 0; i < 4; i++)
                z[i] = op == 0x9 ? x[i] * f : fmaf(op == 0x5 ? -x[i] : x[i], f, acc[i]);
            for (i = 0; i < 4; i++) nan |= z[i] != z[i];
            if (!nan) { memcpy(c->vreg[d], z, 16); return 1; }
        }
    }
    for (i = 0; i < ne; i++) {
        uint64_t x = lane(c->vreg[n], i, fesz), acc = lane(c->vreg[d], i, fesz), z;
        switch (op) {
        case 0x9: z = u ? fp_step(2, x, e, dbl) : fp2(0, x, e, dbl); break;   /* fmulx / fmul */
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
static __attribute__((noinline)) int scalar_dup(struct aoi_cpu *c, uint32_t insn)
{
    int imm5 = insn >> 16 & 31, n = insn >> 5 & 31, d = insn & 31, size, esz;
    if (!(imm5 & 0xf)) return 0;
    size = __builtin_ctz((unsigned)imm5); esz = 1 << size;
    c->vreg[d][0] = lane(c->vreg[n], imm5 >> (size + 1), esz);
    c->vreg[d][1] = 0;
    return 1;
}

/* ld1-ld4 / st1-st4 (single structure, one lane) and ld1r-ld4r */
static __attribute__((noinline)) int ldst_single(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, post = insn >> 23 & 1, load = insn >> 22 & 1, R = insn >> 21 & 1;
    int rm = insn >> 16 & 31, opcode = insn >> 13 & 7, S = insn >> 12 & 1, size = insn >> 10 & 3;
    int rn = insn >> 5 & 31, t = insn & 31, selem = ((opcode & 1) << 1 | R) + 1, esz, idx, i, k;
    uint64_t base = rn == 31 ? c->sp : c->x[rn], v[4];
    if (!post && rm) return 0;
    switch (opcode >> 1) {
    case 0: esz = 1; idx = q << 3 | S << 2 | size; break;
    case 1: if (size & 1) return 0; esz = 2; idx = q << 2 | S << 1 | size >> 1; break;
    case 2: if (size == 0) { esz = 4; idx = q << 1 | S; }
            else if (size == 1 && !S) { esz = 8; idx = q; }
            else return 0;
            break;
    default:                                           /* ld1r-ld4r: replicate to every lane */
        if (!load || S) return 0;
        esz = 1 << size;
        for (k = 0; k < selem; k++) { v[k] = rd(c, base + (uint64_t)(k * esz), esz); if (c->stop != AOI_RUN) return 1; }
        for (k = 0; k < selem; k++) {
            vec r = {0, 0};
            for (i = 0; i < (q ? 16 : 8) / esz; i++) setlane(r, i, esz, v[k]);
            setv(c, (t + k) & 31, r, q);
        }
        goto wb;
    }
    if (load) {
        for (k = 0; k < selem; k++) { v[k] = rd(c, base + (uint64_t)(k * esz), esz); if (c->stop != AOI_RUN) return 1; }
        for (k = 0; k < selem; k++) setlane(c->vreg[(t + k) & 31], idx, esz, v[k]);
    } else
        for (k = 0; k < selem && c->stop == AOI_RUN; k++) wr(c, base + (uint64_t)(k * esz), lane(c->vreg[(t + k) & 31], idx, esz), esz);
wb:
    if (post && c->stop == AOI_RUN) {
        uint64_t nb = base + (rm == 31 ? (uint64_t)(selem * esz) : c->x[rm]);
        if (rn == 31) c->sp = nb; else c->x[rn] = nb;
    }
    return 1;
}

/* ---------------- scalar floating point ---------------- */

static __attribute__((noinline)) int fp_scalar(struct aoi_cpu *c, uint32_t insn)
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
        if ((op == 4 || op == 5 || op == 7) && ptype != 2 && ptype != op - 4) {   /* fcvt between s, d, h */
            int to = op - 4, from = ptype;             /* 0 single, 1 double, 3 half */
            uint64_t x = lane(c->vreg[n], 0, from == 3 ? 2 : from ? 8 : 4);
            if (to == 3) r = to_half(x, from);
            else if (from == 3) r = half_to(x, to);
            else r = cvt_sd(x, to);
            c->vreg[d][0] = r; c->vreg[d][1] = 0;
            return 1;
        }
        if (ptype > 1) return 0;
        switch (op) {
        case 0: SETF(a); return 1;                                            /* fmov */
        case 1: SETF(a & (emask(fsz) >> 1)); return 1;                        /* fabs */
        case 2: SETF(a ^ (dbl ? 1ULL << 63 : 1ULL << 31)); return 1;          /* fneg */
        case 3: SETF(fsqrt_bits(a, dbl)); return 1;                           /* fsqrt */
        case 8: case 9: case 10: case 11: case 12: case 14: case 15:          /* frint* */
            SETF(frint_bits(a, dbl, op == 8 ? 0 : op == 9 ? 1 : op == 10 ? 2 : op == 11 ? 3 : op == 12 ? 4 : 0));
            return 1;
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

/* The commonest vector forms (Skia's raster pipeline: 4 x float arithmetic, bitwise
 * selects), recognised by their whole opcode before the class decoders. 1: done; 0:
 * not one of them, or a case the class decoder must handle (NaN, non-default FPCR). */
static inline int simd_fast(struct aoi_cpu *c, uint32_t insn)
{
    int n = insn >> 5 & 31, m = insn >> 16 & 31, d = insn & 31, q = insn >> 30 & 1, i;
    uint32_t key = insn & 0xbfe0fc00u;
    switch (key) {
    case 0x0e201c00u: case 0x0e601c00u: case 0x0ea01c00u: case 0x0ee01c00u:   /* and bic orr orn */
    case 0x2e201c00u: case 0x2e601c00u: case 0x2ea01c00u: case 0x2ee01c00u: { /* eor bsl bit bif */
        uint64_t r[2];
        for (i = 0; i < 2; i++) {
            uint64_t x = c->vreg[n][i], y = c->vreg[m][i], z = c->vreg[d][i];
            switch (key >> 22 & 0x83) {                                            /* U, size */
            case 0x00: r[i] = x & y; break;               case 0x01: r[i] = x & ~y; break;
            case 0x02: r[i] = x | y; break;               case 0x03: r[i] = x | ~y; break;
            case 0x80: r[i] = x ^ y; break;
            case 0x81: r[i] = (z & x) | (~z & y); break;                           /* bsl */
            case 0x82: r[i] = (z & ~y) | (x & y); break;                           /* bit */
            default: r[i] = (z & y) | (x & ~y); break;                             /* bif */
            }
        }
        c->vreg[d][0] = r[0]; c->vreg[d][1] = q ? r[1] : 0;
        return 1;
    }
    case 0x0e20d400u: case 0x0ea0d400u: case 0x2e20dc00u:                     /* fadd fsub fmul .4s */
    case 0x0e20f400u: case 0x0ea0f400u: {                                     /* fmax fmin .4s */
        float x[4], y[4], z[4];
        int nan = 0;
        if (!q || (c->fpcr & 0x03c00000u)) return 0;
        memcpy(x, c->vreg[n], 16); memcpy(y, c->vreg[m], 16);
        switch (key) {
        case 0x0e20d400u: for (i = 0; i < 4; i++) z[i] = x[i] + y[i]; break;
        case 0x0ea0d400u: for (i = 0; i < 4; i++) z[i] = x[i] - y[i]; break;
        case 0x2e20dc00u: for (i = 0; i < 4; i++) z[i] = x[i] * y[i]; break;
        default: {
            uint32_t xb[4], yb[4], zb[4];
            int mx = key == 0x0e20f400u;
            memcpy(xb, x, 16); memcpy(yb, y, 16);
            for (i = 0; i < 4; i++) nan |= x[i] != x[i] || y[i] != y[i];
            for (i = 0; i < 4; i++)                                  /* max(+0, -0) = +0, min = -0 */
                zb[i] = x[i] == y[i] ? (mx ? xb[i] & yb[i] : xb[i] | yb[i])
                      : (mx ? x[i] > y[i] : x[i] < y[i]) ? xb[i] : yb[i];
            memcpy(z, zb, 16);
            break;
        }
        }
        for (i = 0; i < 4; i++) nan |= z[i] != z[i];                 /* ARM's NaN rules: the class decoder */
        if (nan) return 0;
        memcpy(c->vreg[d], z, 16);
        return 1;
    }
    }
    return 0;
}

/* Which class decoder an instruction word belongs to (0: none), and a cache of it, as
 * core/cpu.c's decode cache: word << 32 | class, one 64-bit store. */
static int simd_class(uint32_t insn)
{
    if ((insn & 0x9f200400u) == 0x0e200400u) return 1;
    if ((insn & 0xdf200400u) == 0x5e200400u) return 2;
    if ((insn & 0x9f3e0c00u) == 0x0e200800u) return 3;
    if ((insn & 0x9f3e0c00u) == 0x0e300800u) return 4;
    if ((insn & 0x9f800400u) == 0x0f000400u && (insn >> 19 & 0xf)) return 5;
    if ((insn & 0xdf800400u) == 0x5f000400u && (insn >> 19 & 0xf)) return 6;
    if ((insn & 0x9f200c00u) == 0x0e200000u) return 7;
    if ((insn & 0x9fe08400u) == 0x0e000400u) return 8;
    if ((insn & 0xbf208c00u) == 0x0e000800u) return 9;
    if ((insn & 0xbfe08400u) == 0x2e000000u) return 10;
    if ((insn & 0xbfe08c00u) == 0x0e000000u) return 11;
    if ((insn & 0xbf000000u) == 0x0d000000u) return 12;
    if ((insn & 0xdf3e0c00u) == 0x5e200800u) return 13;
    if ((insn & 0xffe0fc00u) == 0x5e000400u) return 14;
    if ((insn & 0xdf3e0c00u) == 0x5e300800u) return 15;
    if ((insn & 0x9f000400u) == 0x0f000000u) return 16;
    if ((insn & 0xdf000400u) == 0x5f000000u) return 17;
    if ((insn & 0x5f000000u) == 0x1e000000u || (insn & 0xff000000u) == 0x1f000000u) return 18;
    return 0;
}

#define SCACHE_BITS 13
static uint64_t scache[1u << SCACHE_BITS];

int aoi_simd_step(struct aoi_cpu *c, uint32_t insn)
{
    int ok, cls;
    uint64_t *slot = &scache[(insn * 0x9e3779b1u) >> (32 - SCACHE_BITS)], e;
    if (simd_fast(c, insn)) return 1;
    e = *slot;
    if ((uint32_t)(e >> 32) == insn && (uint32_t)e) cls = (int)(uint32_t)e;
    else if ((cls = simd_class(insn))) *slot = (uint64_t)insn << 32 | (uint32_t)cls;
    switch (cls) {
    case 1: ok = three_same(c, insn, 0); break;
    case 2: ok = three_same(c, insn, 1); break;
    case 3: ok = two_misc(c, insn); break;
    case 4: ok = across(c, insn); break;
    case 5: ok = shift_imm(c, insn, 0); break;
    case 6: ok = shift_imm(c, insn, 1); break;
    case 7: ok = three_diff(c, insn); break;
    case 8: ok = copy(c, insn); break;
    case 9: ok = permute(c, insn); break;
    case 10: ok = ext(c, insn); break;
    case 11: ok = tbl(c, insn); break;
    case 12: ok = ldst_single(c, insn); break;
    case 13: ok = scalar_misc(c, insn); break;
    case 14: ok = scalar_dup(c, insn); break;
    case 15: ok = scalar_pairwise(c, insn); break;
    case 16: ok = fp_elem(c, insn, 0); break;
    case 17: ok = fp_elem(c, insn, 1); break;
    case 18: ok = fp_scalar(c, insn); break;
    default: return 0;
    }
    if (!ok) UNDEF();
    return 1;
}
