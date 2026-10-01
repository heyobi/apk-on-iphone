/* Advanced SIMD (NEON) and scalar floating point for the interpreter.
 *
 * aoi_simd_dp() handles the "data processing - SIMD and FP" encoding space and
 * aoi_simd_ldst() the structure loads/stores (ld1..ld4/st1..st4). Each returns 0
 * for an encoding it does not implement, which the CPU turns into AOI_STOP_UNDEF.
 *
 * Integer lanes are plain bit manipulation. Floating point uses the host's IEEE
 * arithmetic for the rounded result but applies AArch64's own rules where hosts
 * differ: NaN propagation order, the default NaN (positive, quiet), max/min of
 * signed zeros, saturating conversions, and FPSR's cumulative exception bits.
 * FPCR is taken as its reset value (round to nearest, no flush-to-zero, DN=0).
 * Everything is checked against Unicorn by tests/difftest.py. */
#include "cpu.h"

#include <fenv.h>
#include <math.h>
#include <string.h>

/* ---------- lanes ---------- */

static uint64_t getel(const uint64_t *v, int sz, int i)
{
    int bits = 8 << sz;
    unsigned pos = (unsigned)i * (unsigned)bits;
    uint64_t w = v[pos >> 6];
    return bits == 64 ? w : (w >> (pos & 63)) & (((uint64_t)1 << bits) - 1);
}

static void setel(uint64_t *v, int sz, int i, uint64_t x)
{
    int bits = 8 << sz;
    unsigned pos = (unsigned)i * (unsigned)bits;
    uint64_t m;
    if (bits == 64) { v[pos >> 6] = x; return; }
    m = (((uint64_t)1 << bits) - 1) << (pos & 63);
    v[pos >> 6] = (v[pos >> 6] & ~m) | ((x << (pos & 63)) & m);
}

static uint64_t mask_of(int sz) { return sz == 3 ? ~(uint64_t)0 : ((uint64_t)1 << (8 << sz)) - 1; }

static int64_t sx(uint64_t v, int sz)
{
    int bits = 8 << sz;
    uint64_t m;
    if (bits == 64) return (int64_t)v;
    m = (uint64_t)1 << (bits - 1);
    return (int64_t)(((v & mask_of(sz)) ^ m) - m);
}

/* Writes a result; a 64-bit (Q=0 or scalar) result clears bits 64-127. */
static void putv(struct aoi_cpu *c, int rd, const uint64_t *r, int q)
{
    c->vreg[rd][0] = r[0];
    c->vreg[rd][1] = q ? r[1] : 0;
}

static uint64_t X(struct aoi_cpu *c, int i) { return i == 31 ? 0 : c->x[i]; }
static void setX(struct aoi_cpu *c, int i, uint64_t v) { if (i != 31) c->x[i] = v; }

static uint64_t ones_if(int b, int sz) { return b ? mask_of(sz) : 0; }

/* ---------- FP helpers (type 0 = single, 1 = double) ---------- */

#define FPSR_IOC 0x01u
#define FPSR_DZC 0x02u
#define FPSR_OFC 0x04u
#define FPSR_UFC 0x08u
#define FPSR_IXC 0x10u

static int f_isnan(uint64_t v, int t)
{
    return t ? (v & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL : (v & 0x7fffffffu) > 0x7f800000u;
}
static int f_issnan(uint64_t v, int t)
{
    return f_isnan(v, t) && !(v & (t ? (uint64_t)1 << 51 : (uint64_t)1 << 22));
}
static uint64_t f_quiet(uint64_t v, int t) { return v | (t ? (uint64_t)1 << 51 : (uint64_t)1 << 22); }
static uint64_t f_defnan(int t) { return t ? 0x7ff8000000000000ULL : 0x7fc00000u; }

static double to_d(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static float to_f(uint64_t v) { uint32_t w = (uint32_t)v; float f; memcpy(&f, &w, 4); return f; }
static uint64_t of_d(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }
static uint64_t of_f(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }

/* AArch64 FPProcessNaNs for up to three operands, in operand order: any SNaN
 * wins (quietened, IOC), then the first QNaN. Returns 1 with *r set if a NaN
 * decides the result. */
static int f_nans(struct aoi_cpu *c, int t, int n, const uint64_t *ops, uint64_t *r)
{
    int i;
    for (i = 0; i < n; i++)
        if (f_issnan(ops[i], t)) { c->fpsr |= FPSR_IOC; *r = f_quiet(ops[i], t); return 1; }
    for (i = 0; i < n; i++)
        if (f_isnan(ops[i], t)) { *r = ops[i]; return 1; }
    return 0;
}

static unsigned host_flags(void)
{
    int e = fetestexcept(FE_ALL_EXCEPT);
    return (e & FE_INVALID ? FPSR_IOC : 0) | (e & FE_DIVBYZERO ? FPSR_DZC : 0) |
           (e & FE_OVERFLOW ? FPSR_OFC : 0) | (e & FE_UNDERFLOW ? FPSR_UFC : 0) |
           (e & FE_INEXACT ? FPSR_IXC : 0);
}

/* Arm detects tininess before rounding, x86 after: an inexact result that rounded
 * up to exactly the smallest normal is an underflow on Arm. err is the sign of
 * (exact - rounded) as computed by the caller with fma. */
static unsigned tiny_fix(int t, uint64_t r, double err, unsigned fl)
{
    uint64_t mag = r & (t ? 0x7fffffffffffffffULL : 0x7fffffffu);
    uint64_t minn = t ? 0x0010000000000000ULL : 0x00800000u;
    int neg = t ? (int)(r >> 63) : (int)(r >> 31 & 1);
    if (mag == minn && (fl & FPSR_IXC) && (neg ? err > 0 : err < 0)) fl |= FPSR_UFC;
    return fl;
}

enum { F_ADD, F_SUB, F_MUL, F_DIV, F_MAX, F_MIN, F_MAXNM, F_MINNM, F_NMUL };

static uint64_t f_arith(struct aoi_cpu *c, int t, int op, uint64_t a, uint64_t b);

/* fnmul is FPNeg(FPMul()): the sign flips even on a NaN result. */
static uint64_t f_nmul(struct aoi_cpu *c, int t, uint64_t a, uint64_t b)
{
    return f_arith(c, t, F_MUL, a, b) ^ (t ? 1ULL << 63 : 0x80000000u);
}

static uint64_t f_arith(struct aoi_cpu *c, int t, int op, uint64_t a, uint64_t b)
{
    uint64_t ops[2], r;
    unsigned fl;
    int an = f_isnan(a, t), bn = f_isnan(b, t);
    ops[0] = a; ops[1] = b;
    if ((op == F_MAXNM || op == F_MINNM) && (an != bn)) {
        /* a quiet NaN against a number gives the number */
        if (an && !f_issnan(a, t)) return b;
        if (bn && !f_issnan(b, t)) return a;
    }
    if (f_nans(c, t, 2, ops, &r)) return r;
    if (op >= F_MAX && op <= F_MINNM) {
        int max = op == F_MAX || op == F_MAXNM;
        double x = t ? to_d(a) : to_f(a), y = t ? to_d(b) : to_f(b);
        if (x == 0 && y == 0) {                /* +0 beats -0 for max, -0 for min */
            int sa = t ? (int)(a >> 63) : (int)(a >> 31 & 1);
            return max ? (sa ? b : a) : (sa ? a : b);
        }
        return (max ? x > y : x < y) ? a : b;
    }
    feclearexcept(FE_ALL_EXCEPT);
    if (t) {
        volatile double x = to_d(a), y = to_d(b), z;
        double err = 0;
        switch (op) {
        case F_ADD: z = x + y; break;
        case F_SUB: z = x - y; break;
        case F_DIV: z = x / y; break;
        default:    z = x * y; break;
        }
        fl = host_flags();
        if (op == F_MUL) err = fma(x, y, -z);
        else if (op == F_DIV && isfinite(z)) err = fma(-z, y, x) * (y < 0 ? -1 : 1);
        r = of_d(z);
        fl = tiny_fix(1, r, err, fl);
    } else {
        volatile float x = to_f(a), y = to_f(b), z;
        double exact = 0, zz;
        switch (op) {
        case F_ADD: z = x + y; break;
        case F_SUB: z = x - y; break;
        case F_DIV: z = x / y; break;
        default:    z = x * y; break;
        }
        fl = host_flags();
        zz = z;
        if (op == F_MUL) exact = (double)x * (double)y;                   /* exact in double */
        else if (op == F_DIV) exact = (double)x / (double)y;
        r = of_f(z);
        fl = tiny_fix(0, r, exact - zz, fl);
    }
    if (f_isnan(r, t)) r = f_defnan(t);
    c->fpsr |= fl;
    return r;
}

/* fmadd family: Ra + Rn*Rm with optional negations (o1/o0 as in the encoding). */
static uint64_t f_muladd(struct aoi_cpu *c, int t, uint64_t ra, uint64_t rn, uint64_t rm, int o1, int o0)
{
    uint64_t sign = t ? 1ULL << 63 : 0x80000000u, ops[3], r;
    unsigned fl;
    if (o1) ra ^= sign;                         /* fnmadd/fnmsub negate the addend */
    if (o0 != o1) rn ^= sign;                   /* fmsub/fnmadd negate the product */
    ops[0] = ra; ops[1] = rn; ops[2] = rm;
    {
        /* inf*0 with a quiet NaN addend is still Invalid (default NaN) */
        double n_ = t ? to_d(rn) : to_f(rn), m_ = t ? to_d(rm) : to_f(rm);
        if (f_isnan(ra, t) && !f_issnan(ra, t) && !f_issnan(rn, t) && !f_issnan(rm, t) &&
            ((isinf(n_) && m_ == 0) || (n_ == 0 && isinf(m_)))) {
            c->fpsr |= FPSR_IOC;
            return f_defnan(t);
        }
    }
    if (f_nans(c, t, 3, ops, &r)) return r;     /* a NaN keeps the sign the negation gave it */
    feclearexcept(FE_ALL_EXCEPT);
    if (t) { volatile double z = fma(to_d(rn), to_d(rm), to_d(ra)); r = of_d(z); }
    else   { volatile float z = fmaf(to_f(rn), to_f(rm), to_f(ra)); r = of_f(z); }
    fl = host_flags();
    if (f_isnan(r, t)) r = f_defnan(t);
    c->fpsr |= fl;
    return r;
}

/* NZCV of an FP compare. signal_all: fcmpe (IOC on any NaN, not just SNaN). */
static void f_compare(struct aoi_cpu *c, int t, uint64_t a, uint64_t b, int signal_all)
{
    if (f_isnan(a, t) || f_isnan(b, t)) {
        if (signal_all || f_issnan(a, t) || f_issnan(b, t)) c->fpsr |= FPSR_IOC;
        c->n = 0; c->z = 0; c->c = 1; c->v = 1;
        return;
    }
    {
        double x = t ? to_d(a) : to_f(a), y = t ? to_d(b) : to_f(b);
        c->n = x < y; c->z = x == y; c->c = x >= y; c->v = 0;
    }
}

/* Round per mode: 0 nearest-even, 1 +inf, 2 -inf, 3 zero, 4 nearest-away. */
static double f_round(double x, int mode)
{
    switch (mode) {
    case 1: return ceil(x);
    case 2: return floor(x);
    case 3: return trunc(x);
    case 4: return round(x);
    default: return nearbyint(x);
    }
}

/* FP to integer with saturation (fcvt[nzpma][su]). */
static uint64_t f_to_int(struct aoi_cpu *c, int t, uint64_t v, int mode, int is_unsigned, int bits)
{
    double x, r;
    if (f_isnan(v, t)) { c->fpsr |= FPSR_IOC; return 0; }
    x = t ? to_d(v) : to_f(v);
    r = f_round(x, mode);
    if (is_unsigned) {
        double lim = bits == 64 ? 18446744073709551616.0 : 4294967296.0;
        if (r < 0) { c->fpsr |= FPSR_IOC; return 0; }
        if (r >= lim) { c->fpsr |= FPSR_IOC; return bits == 64 ? ~0ULL : 0xffffffffu; }
        if (r != x) c->fpsr |= FPSR_IXC;
        return (uint64_t)r;
    } else {
        double lim = bits == 64 ? 9223372036854775808.0 : 2147483648.0;
        if (r >= lim) { c->fpsr |= FPSR_IOC; return bits == 64 ? 0x7fffffffffffffffULL : 0x7fffffffu; }
        if (r < -lim) { c->fpsr |= FPSR_IOC; return bits == 64 ? 0x8000000000000000ULL : 0x80000000u; }
        if (r != x) c->fpsr |= FPSR_IXC;
        return bits == 64 ? (uint64_t)(int64_t)r : (uint32_t)(int32_t)r;
    }
}

static uint64_t int_to_f(struct aoi_cpu *c, int t, uint64_t v, int is_unsigned, int bits)
{
    uint64_t r;
    feclearexcept(FE_ALL_EXCEPT);
    if (bits == 32) v = is_unsigned ? (uint32_t)v : (uint64_t)(int64_t)(int32_t)v;
    if (t) {
        volatile double d = is_unsigned ? (double)v : (double)(int64_t)v;
        r = of_d(d);
    } else {
        volatile float f = is_unsigned ? (float)v : (float)(int64_t)v;
        r = of_f(f);
    }
    c->fpsr |= host_flags() & FPSR_IXC;
    return r;
}

/* VFPExpandImm: the 8-bit FP immediate of fmov. */
static uint64_t f_imm(unsigned imm8, int t)
{
    uint64_t sign = imm8 >> 7 & 1, b6 = imm8 >> 6 & 1, low = imm8 & 0x3f;
    if (t)
        return sign << 63 | (b6 ? 0x3fc0000000000000ULL : 0x4000000000000000ULL) | (low << 48);
    return (uint32_t)(sign << 31 | (b6 ? 0x3e000000u : 0x40000000u) | (uint32_t)(low << 19));
}

/* ---------- scalar floating point ---------- */

static int fp_scalar(struct aoi_cpu *c, uint32_t insn)
{
    int t = (insn >> 22) & 3, rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    uint64_t r[2] = {0, 0}, a, b, fmask;
    int sf = insn >> 31;

    /* conversions between FP and general registers: sf 0 0 11110 type 1 rmode opcode 000000 */
    if ((insn & 0x7f20fc00u) == 0x1e200000u) {
        int rmode = (insn >> 19) & 3, op = (insn >> 16) & 7;
        if (op == 6 || op == 7) {                               /* fmov general <-> FP */
            if (rmode == 0 && ((sf == 0 && t == 0) || (sf == 1 && t == 1))) {
                if (op == 6) setX(c, rd, t ? c->vreg[rn][0] : (uint32_t)c->vreg[rn][0]);
                else { r[0] = t ? X(c, rn) : (uint32_t)X(c, rn); putv(c, rd, r, 0); }
                return 1;
            }
            if (rmode == 1 && sf == 1 && t == 2) {              /* fmov x, v.d[1] / v.d[1], x */
                if (op == 6) setX(c, rd, c->vreg[rn][1]);
                else c->vreg[rd][1] = X(c, rn);
                return 1;
            }
            return 0;
        }
        if (t > 1) return 0;
        a = t ? c->vreg[rn][0] : (uint32_t)c->vreg[rn][0];
        if (op == 2 || op == 3) {                               /* scvtf / ucvtf */
            if (rmode) return 0;
            r[0] = int_to_f(c, t, X(c, rn), op == 3, sf ? 64 : 32);
            putv(c, rd, r, 0);
            return 1;
        }
        if (op < 2 || op == 4 || op == 5) {                     /* fcvt[nzpma][su] */
            static const int mode_of[4] = { 0, 1, 2, 3 };       /* rmode: n, p, m, z */
            int mode = (op >= 4) ? 4 : mode_of[rmode];
            if (op >= 4 && rmode) return 0;                     /* fcvta* only with rmode 00 */
            setX(c, rd, f_to_int(c, t, a, mode, op & 1, sf ? 64 : 32));
            return 1;
        }
        return 0;
    }
    if (sf || (insn >> 29 & 1)) return 0;
    if (t > 1) return 0;                                        /* half precision: not yet */
    fmask = t ? ~0ULL : 0xffffffffu;
    a = c->vreg[rn][0] & fmask;
    b = c->vreg[rm][0] & fmask;

    if ((insn & 0xff207c00u & ~0x00c00000u) == 0x1e204000u) {  /* 1 source */
        int op = (insn >> 15) & 0x3f;
        switch (op) {
        case 0: r[0] = a; break;                                /* fmov */
        case 1: r[0] = a & (fmask >> 1); break;                 /* fabs */
        case 2: r[0] = a ^ (t ? 1ULL << 63 : 0x80000000u); break; /* fneg */
        case 3:                                                 /* fsqrt */
            if (f_nans(c, t, 1, &a, &r[0])) break;
            feclearexcept(FE_ALL_EXCEPT);
            if (t) { volatile double z = sqrt(to_d(a)); r[0] = of_d(z); }
            else   { volatile float z = sqrtf(to_f(a)); r[0] = of_f(z); }
            c->fpsr |= host_flags();
            if (f_isnan(r[0], t)) r[0] = f_defnan(t);
            break;
        case 4: case 5: {                                       /* fcvt to single / double */
            int to = op - 4;
            if (to == t) return 0;
            feclearexcept(FE_ALL_EXCEPT);
            if (f_isnan(a, t)) {
                if (f_issnan(a, t)) c->fpsr |= FPSR_IOC;
                if (to) r[0] = (a & 0x80000000u ? 1ULL << 63 : 0) | 0x7ff8000000000000ULL |
                               ((uint64_t)(a & 0x3fffffu) << 29);
                else r[0] = (a >> 63 ? 0x80000000u : 0) | 0x7fc00000u | (uint32_t)((a >> 29) & 0x3fffffu);
                break;
            }
            if (to) { volatile double z = to_f(a); r[0] = of_d(z); }
            else {
                volatile float z = (float)to_d(a);
                double exact = to_d(a);
                r[0] = of_f(z);
                c->fpsr |= tiny_fix(0, r[0], exact - (double)z, host_flags());
            }
            break;
        }
        case 8: case 9: case 10: case 11: case 12: case 14: case 15: { /* frint n/p/m/z/a/x/i */
            static const int mode_of[8] = { 0, 1, 2, 3, 4, -1, 0, 0 };
            double x, y;
            if (f_nans(c, t, 1, &a, &r[0])) break;
            x = t ? to_d(a) : to_f(a);
            y = f_round(x, mode_of[op - 8]);
            if (op == 14 && y != x) c->fpsr |= FPSR_IXC;
            if (t) r[0] = of_d(y);
            else r[0] = of_f((float)y);
            break;
        }
        default: return 0;
        }
        putv(c, rd, r, 0);
        return 1;
    }
    if ((insn & 0xff203c07u & ~0x00c00000u) == 0x1e202000u) {  /* fcmp / fcmpe (incl #0.0) */
        if ((insn >> 14 & 3) || (insn >> 3 & 1 ? rm != 0 : 0)) return 0;
        f_compare(c, t, a, (insn >> 3 & 1) ? 0 : b, insn >> 4 & 1);
        return 1;
    }
    if ((insn & 0xff201fe0u & ~0x00c00000u) == 0x1e201000u) {  /* fmov (scalar immediate) */
        r[0] = f_imm((insn >> 13) & 0xff, t);
        putv(c, rd, r, 0);
        return 1;
    }
    if ((insn & 0xff200c00u & ~0x00c00000u) == 0x1e200400u) {  /* fccmp / fccmpe */
        unsigned cond = (insn >> 12) & 15;
        if (aoi_cond_holds(c, cond)) f_compare(c, t, a, b, insn >> 4 & 1);
        else { c->n = insn >> 3 & 1; c->z = insn >> 2 & 1; c->c = insn >> 1 & 1; c->v = insn & 1; }
        return 1;
    }
    if ((insn & 0xff200c00u & ~0x00c00000u) == 0x1e200800u) {  /* 2 source */
        static const int ops[9] = { F_MUL, F_DIV, F_ADD, F_SUB, F_MAX, F_MIN, F_MAXNM, F_MINNM, F_NMUL };
        int op = (insn >> 12) & 15;
        if (op > 8) return 0;
        r[0] = ops[op] == F_NMUL ? f_nmul(c, t, a, b) : f_arith(c, t, ops[op], a, b);
        putv(c, rd, r, 0);
        return 1;
    }
    if ((insn & 0xff200c00u & ~0x00c00000u) == 0x1e200c00u) {  /* fcsel */
        r[0] = aoi_cond_holds(c, (insn >> 12) & 15) ? a : b;
        putv(c, rd, r, 0);
        return 1;
    }
    return 0;
}

static int fp_3source(struct aoi_cpu *c, uint32_t insn)
{
    int t = (insn >> 22) & 3, rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    int ra = (insn >> 10) & 31;
    uint64_t r[2] = {0, 0}, fmask;
    if (t > 1 || insn >> 29) return 0;
    fmask = t ? ~0ULL : 0xffffffffu;
    r[0] = f_muladd(c, t, c->vreg[ra][0] & fmask, c->vreg[rn][0] & fmask, c->vreg[rm][0] & fmask,
                    insn >> 21 & 1, insn >> 15 & 1);
    putv(c, rd, r, 0);
    return 1;
}

/* ---------- Advanced SIMD ---------- */

/* AdvSIMDExpandImm (movi/mvni/orr/bic/fmov vector immediate). */
static int simd_modimm(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, op = insn >> 29 & 1, cmode = (insn >> 12) & 15, rd = insn & 31;
    unsigned imm8 = ((insn >> 16) & 7) << 5 | ((insn >> 5) & 31);
    uint64_t imm, r[2];
    int i;
    if (insn >> 11 & 1) return 0;                               /* o2: FP16 fmov */
    switch (cmode >> 1) {
    case 0: case 1: case 2: case 3:
        imm = (uint64_t)imm8 << (8 * (cmode >> 1)); imm |= imm << 32; break;
    case 4: case 5:
        imm = (uint64_t)imm8 << (8 * ((cmode >> 1) & 1)); imm |= imm << 16; imm |= imm << 32; break;
    case 6:
        imm = (cmode & 1) ? ((uint64_t)imm8 << 16 | 0xffff) : ((uint64_t)imm8 << 8 | 0xff);
        imm |= imm << 32; break;
    default:
        if (!(cmode & 1)) {
            if (!op) { imm = imm8 * 0x0101010101010101ULL; }
            else { imm = 0; for (i = 0; i < 8; i++) if (imm8 >> i & 1) imm |= 0xffULL << (8 * i); }
        } else if (!op) {
            imm = f_imm(imm8, 0); imm |= imm << 32;
        } else {
            if (!q) return 0;
            imm = f_imm(imm8, 1);
        }
        break;
    }
    if ((cmode & 1) && cmode < 12) {                            /* orr / bic (immediate) */
        r[0] = op ? c->vreg[rd][0] & ~imm : c->vreg[rd][0] | imm;
        r[1] = op ? c->vreg[rd][1] & ~imm : c->vreg[rd][1] | imm;
    } else {
        if (op && cmode < 14) imm = ~imm;                       /* mvni */
        r[0] = r[1] = imm;
    }
    putv(c, rd, r, q);
    return 1;
}

/* Integer three-same. scalar: the 01 U 11110 form, one 64-bit lane. */
static int simd_three_same(struct aoi_cpu *c, uint32_t insn, int scalar)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, sz = (insn >> 22) & 3, op = (insn >> 11) & 31;
    int rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31, i, n;
    uint64_t *A = c->vreg[rn], *B = c->vreg[rm], *D = c->vreg[rd], r[2] = {0, 0}, cat[4];

    if (op == 3) {                                              /* logical, whole register */
        if (scalar) return 0;
        for (i = 0; i < 2; i++) {
            uint64_t a = A[i], b = B[i], d = D[i];
            if (!u) r[i] = sz == 0 ? a & b : sz == 1 ? a & ~b : sz == 2 ? a | b : a | ~b;
            else r[i] = sz == 0 ? a ^ b : sz == 1 ? (d & a) | (~d & b) :
                        sz == 2 ? (a & b) | (d & ~b) : (d & b) | (a & ~b);
        }
        putv(c, rd, r, q);
        return 1;
    }
    if (scalar) {
        if (sz != 3 || !(op == 6 || op == 7 || op == 8 || op == 16 || op == 17)) return 0;
        q = 0;
        n = 1;
    } else {
        if (sz == 3 && !q) return 0;
        n = (q ? 16 : 8) >> sz;
    }
    switch (op) {
    case 0: case 2: case 4: case 12: case 13: case 14: case 15: case 18: case 20: case 21:
        if (sz == 3) return 0;
        break;
    case 19: if (sz == 3 || u) return 0; break;                 /* mul (pmul not yet) */
    case 23: if (u) return 0; break;                            /* addp */
    case 6: case 7: case 8: case 16: case 17: break;
    default: return 0;
    }
    memcpy(cat, A, 16); memcpy(cat + 2, B, 16);
    if (!q) { cat[1] = B[0]; }                                  /* 64-bit pairwise: a.lo then b.lo */
    for (i = 0; i < n; i++) {
        uint64_t x = getel(A, sz, i), y = getel(B, sz, i), d = getel(D, sz, i), v = 0;
        int64_t sx_ = sx(x, sz), sy_ = sx(y, sz);
        switch (op) {
        case 0: v = u ? (x + y) >> 1 : (uint64_t)((sx_ + sy_) >> 1); break;          /* hadd */
        case 2: v = u ? (x + y + 1) >> 1 : (uint64_t)((sx_ + sy_ + 1) >> 1); break;  /* rhadd */
        case 4: v = u ? (x - y) >> 1 : (uint64_t)((sx_ - sy_) >> 1); break;          /* hsub */
        case 6: v = ones_if(u ? x > y : sx_ > sy_, sz); break;                       /* cmgt/cmhi */
        case 7: v = ones_if(u ? x >= y : sx_ >= sy_, sz); break;                     /* cmge/cmhs */
        case 8: {                                                                    /* sshl/ushl */
            int sh = (int8_t)(y & 0xff), bits = 8 << sz;
            if (sh >= 0) v = sh >= bits ? 0 : x << sh;
            else if (u) v = -sh >= bits ? 0 : x >> -sh;
            else v = (uint64_t)(sx_ >> (-sh >= bits ? bits - 1 : -sh));
            break;
        }
        case 12: v = u ? (x > y ? x : y) : (uint64_t)(sx_ > sy_ ? sx_ : sy_); break; /* max */
        case 13: v = u ? (x < y ? x : y) : (uint64_t)(sx_ < sy_ ? sx_ : sy_); break; /* min */
        case 14: case 15:                                                            /* abd/aba */
            v = u ? (x > y ? x - y : y - x) : (uint64_t)(sx_ > sy_ ? sx_ - sy_ : sy_ - sx_);
            if (op == 15) v += d;
            break;
        case 16: v = u ? x - y : x + y; break;                                       /* add/sub */
        case 17: v = ones_if(u ? x == y : (x & y) != 0, sz); break;                  /* cmeq/cmtst */
        case 18: v = u ? d - x * y : d + x * y; break;                               /* mla/mls */
        case 19: v = x * y; break;                                                   /* mul */
        case 20: case 21: case 23: {                                                 /* maxp/minp/addp */
            uint64_t p = getel(cat, sz, 2 * i), s2 = getel(cat, sz, 2 * i + 1);
            int64_t sp_ = sx(p, sz), ss = sx(s2, sz);
            if (op == 23) v = p + s2;
            else if (op == 20) v = u ? (p > s2 ? p : s2) : (uint64_t)(sp_ > ss ? sp_ : ss);
            else v = u ? (p < s2 ? p : s2) : (uint64_t)(sp_ < ss ? sp_ : ss);
            break;
        }
        }
        setel(r, sz, i, v & mask_of(sz));
    }
    putv(c, rd, r, q);
    return 1;
}

static int simd_two_misc(struct aoi_cpu *c, uint32_t insn, int scalar)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, sz = (insn >> 22) & 3, op = (insn >> 12) & 31;
    int rd = insn & 31, rn = (insn >> 5) & 31, i, n, k;
    uint64_t *A = c->vreg[rn], r[2] = {0, 0};

    if (scalar) {
        if (sz != 3 || !(op == 8 || op == 9 || op == 10 || op == 11) || (u && op == 10)) return 0;
        q = 0; n = 1;
    } else n = (q ? 16 : 8) >> sz;

    switch (op) {
    case 0: case 1: {                                           /* rev64 / rev32 / rev16 */
        int container = op == 1 ? 1 : (u ? 2 : 3);              /* log2 bytes: rev16, rev32, rev64 */
        int per;
        if ((op == 1 && u) || sz >= container) return 0;
        per = 1 << (container - sz);
        for (i = 0; i < n; i++) setel(r, sz, i, getel(A, sz, (i / per) * per + (per - 1 - i % per)));
        break;
    }
    case 2: case 6:                                             /* [su]addlp / [su]adalp */
        if (sz == 3) return 0;
        for (i = 0; i < n / 2; i++) {
            uint64_t a = getel(A, sz, 2 * i), b = getel(A, sz, 2 * i + 1);
            uint64_t s = u ? a + b : (uint64_t)(sx(a, sz) + sx(b, sz));
            if (op == 6) s += getel(c->vreg[rd], sz + 1, i);
            setel(r, sz + 1, i, s & mask_of(sz + 1));
        }
        break;
    case 4:                                                     /* cls / clz */
        if (sz == 3) return 0;
        for (i = 0; i < n; i++) {
            uint64_t a = getel(A, sz, i);
            int bits = 8 << sz, cnt = 0;
            if (u) { for (k = bits - 1; k >= 0 && !(a >> k & 1); k--) cnt++; }
            else { int msb = a >> (bits - 1) & 1; for (k = bits - 2; k >= 0 && (int)(a >> k & 1) == msb; k--) cnt++; }
            setel(r, sz, i, (uint64_t)cnt);
        }
        break;
    case 5:                                                     /* cnt / not / rbit (bytes) */
        if (!u && sz != 0) return 0;
        if (u && sz > 1) return 0;
        for (i = 0; i < (q ? 16 : 8); i++) {
            uint64_t a = getel(A, 0, i), v = 0;
            if (!u) { for (k = 0; k < 8; k++) v += a >> k & 1; }
            else if (sz == 0) v = ~a & 0xff;
            else { for (k = 0; k < 8; k++) if (a >> k & 1) v |= 1u << (7 - k); }
            setel(r, 0, i, v);
        }
        break;
    case 8: case 9: case 10: case 11:                           /* cm[gt,ge,eq,le,lt] #0, abs, neg */
        if (sz == 3 && !q && !scalar) return 0;
        if (op == 10 && u) return 0;
        for (i = 0; i < n; i++) {
            int64_t a = sx(getel(A, sz, i), sz);
            uint64_t v;
            if (op == 8) v = ones_if(u ? a >= 0 : a > 0, sz);
            else if (op == 9) v = ones_if(u ? a <= 0 : a == 0, sz);
            else if (op == 10) v = ones_if(a < 0, sz);
            else v = u ? (uint64_t)0 - (uint64_t)a : (uint64_t)(a < 0 ? -(uint64_t)a : (uint64_t)a);
            setel(r, sz, i, v & mask_of(sz));
        }
        break;
    case 18:                                                    /* xtn / xtn2 */
        if (u || sz == 3) return 0;
        memcpy(r, c->vreg[rd], 16);
        for (i = 0; i < (8 >> sz); i++)
            setel(r, sz, (q ? (8 >> sz) : 0) + i, getel(A, sz + 1, i) & mask_of(sz));
        if (!q) r[1] = 0;
        c->vreg[rd][0] = r[0]; c->vreg[rd][1] = r[1];
        return 1;
    case 15:                                                    /* fabs / fneg (vector) */
        if (sz < 2 || (sz == 3 && !q)) return 0;
        for (i = 0; i < n; i++) {
            uint64_t a = getel(A, sz, i), sign = (uint64_t)1 << ((8 << sz) - 1);
            setel(r, sz, i, u ? a ^ sign : a & ~sign);
        }
        break;
    default:
        return 0;
    }
    putv(c, rd, r, q);
    return 1;
}

static int simd_shift_imm(struct aoi_cpu *c, uint32_t insn, int scalar)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, immh = (insn >> 19) & 15, immhb = (insn >> 16) & 127;
    int op = (insn >> 11) & 31, rd = insn & 31, rn = (insn >> 5) & 31, i, n, sz, bits, sh;
    uint64_t *A = c->vreg[rn], *D = c->vreg[rd], r[2] = {0, 0};

    if (!immh) return 0;
    sz = 31 - __builtin_clz((unsigned)immh);
    if (op == 20) {                                             /* [su]shll / [su]shll2 (uxtl) */
        if (scalar || sz == 3) return 0;
        bits = 8 << sz;
        sh = immhb - bits;
        for (i = 0; i < (8 >> sz); i++) {
            uint64_t a = getel(A, sz, (q ? (8 >> sz) : 0) + i);
            uint64_t v = u ? a << sh : (uint64_t)(sx(a, sz) * ((int64_t)1 << sh));
            setel(r, sz + 1, i, v & mask_of(sz + 1));
        }
        putv(c, rd, r, 1);
        return 1;
    }
    if (op == 16 || op == 17) {                                 /* shrn / rshrn (narrow, U=0) */
        if (scalar || u || sz == 3) return 0;
        bits = 8 << sz;
        sh = 2 * bits - immhb;
        memcpy(r, D, 16);
        for (i = 0; i < (8 >> sz); i++) {
            uint64_t a = getel(A, sz + 1, i);
            if (op == 17) a = sz + 1 == 3 ? (uint64_t)(((unsigned __int128)a + ((uint64_t)1 << (sh - 1))) >> sh)
                                           : (a + ((uint64_t)1 << (sh - 1))) >> sh;
            else a >>= sh;
            setel(r, sz, (q ? (8 >> sz) : 0) + i, a & mask_of(sz));
        }
        if (!q) r[1] = 0;
        D[0] = r[0]; D[1] = r[1];
        return 1;
    }
    if (scalar) { if (sz != 3) return 0; q = 0; n = 1; }
    else { if (sz == 3 && !q) return 0; n = (q ? 16 : 8) >> sz; }
    bits = 8 << sz;
    for (i = 0; i < n; i++) {
        uint64_t a = getel(A, sz, i), d = getel(D, sz, i), v, m = mask_of(sz);
        if (op == 0 || op == 2 || op == 4 || op == 6) {          /* [su]shr, [su]sra, [su]rshr, [su]rsra */
            sh = 2 * bits - immhb;
            if (u) {
                unsigned __int128 w = a;
                if (op >= 4) w += (unsigned __int128)1 << (sh - 1);
                v = sh >= 128 ? 0 : (uint64_t)(w >> sh);
            } else {
                __int128 w = sx(a, sz);
                if (op >= 4) w += (__int128)1 << (sh - 1);
                v = (uint64_t)(int64_t)(w >> sh);
            }
            if (op == 2 || op == 6) v += d;
        } else if (op == 10) {                                  /* shl / sli */
            sh = immhb - bits;
            v = a << sh;
            if (u) v |= d & (((uint64_t)1 << sh) - 1);
        } else if (op == 8 && u) {                              /* sri */
            uint64_t keep;
            sh = 2 * bits - immhb;
            keep = sh == bits ? m : (m & ~(m >> sh));
            v = (sh == bits ? 0 : a >> sh) | (d & keep);
        } else return 0;
        setel(r, sz, i, v & m);
    }
    putv(c, rd, r, q);
    return 1;
}

static int simd_three_diff(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, sz = (insn >> 22) & 3, op = (insn >> 12) & 15;
    int rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31, i, n, half;
    uint64_t *A = c->vreg[rn], *B = c->vreg[rm], *D = c->vreg[rd], r[2] = {0, 0};
    if (sz == 3) return 0;
    n = 8 >> sz;                    /* narrow elements in one 64-bit half */
    half = q ? n : 0;
    if (op == 4 || op == 6) {                                   /* addhn / subhn (+r variants) */
        memcpy(r, D, 16);
        for (i = 0; i < n; i++) {
            uint64_t a = getel(A, sz + 1, i), b = getel(B, sz + 1, i);
            uint64_t s = op == 4 ? a + b : a - b;
            if (u) s += (uint64_t)1 << ((8 << sz) - 1);
            s &= mask_of(sz + 1);
            setel(r, sz, half + i, (s >> (8 << sz)) & mask_of(sz));
        }
        if (!q) r[1] = 0;
        D[0] = r[0]; D[1] = r[1];
        return 1;
    }
    if (op == 9 || op == 11 || op == 13 || op == 14 || op == 15) return 0; /* sqdmull etc, pmull */
    for (i = 0; i < n; i++) {
        uint64_t a = op == 1 || op == 3 ? getel(A, sz + 1, i) : getel(A, sz, half + i);
        uint64_t b = getel(B, sz, half + i), d = getel(D, sz + 1, i), v;
        int64_t sa = op == 1 || op == 3 ? sx(a, sz + 1) : sx(a, sz), sb = sx(b, sz);
        uint64_t xa = u ? a : (uint64_t)sa, xb = u ? b : (uint64_t)sb;
        switch (op) {
        case 0: case 1: v = xa + xb; break;                     /* [su]addl / [su]addw */
        case 2: case 3: v = xa - xb; break;                     /* [su]subl / [su]subw */
        case 5: case 7:                                         /* [su]abal / [su]abdl */
            v = u ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa);
            if (op == 5) v += d;
            break;
        case 8: v = d + xa * xb; break;                         /* [su]mlal */
        case 10: v = d - xa * xb; break;                        /* [su]mlsl */
        default: v = xa * xb; break;                            /* 12: [su]mull */
        }
        setel(r, sz + 1, i, v & mask_of(sz + 1));
    }
    putv(c, rd, r, 1);
    return 1;
}

static int simd_across(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, u = insn >> 29 & 1, sz = (insn >> 22) & 3, op = (insn >> 12) & 31;
    int rd = insn & 31, rn = (insn >> 5) & 31, i, n;
    uint64_t *A = c->vreg[rn], r[2] = {0, 0}, acc;
    if (sz == 3 || (sz == 2 && !q)) return 0;
    n = (q ? 16 : 8) >> sz;
    if (op == 3) {                                              /* [su]addlv */
        acc = 0;
        for (i = 0; i < n; i++) acc += u ? getel(A, sz, i) : (uint64_t)sx(getel(A, sz, i), sz);
        r[0] = acc & mask_of(sz + 1);
    } else if (op == 10 || op == 26) {                          /* [su]maxv / [su]minv */
        acc = getel(A, sz, 0);
        for (i = 1; i < n; i++) {
            uint64_t e = getel(A, sz, i);
            int gt = u ? e > acc : sx(e, sz) > sx(acc, sz);
            if (op == 10 ? gt : (!gt && e != acc)) acc = e;
        }
        r[0] = acc;
    } else if (op == 27 && !u) {                                /* addv */
        acc = 0;
        for (i = 0; i < n; i++) acc += getel(A, sz, i);
        r[0] = acc & mask_of(sz);
    } else return 0;
    putv(c, rd, r, 0);
    return 1;
}

static int simd_copy(struct aoi_cpu *c, uint32_t insn, int scalar)
{
    int q = insn >> 30 & 1, op = insn >> 29 & 1, imm5 = (insn >> 16) & 31, imm4 = (insn >> 11) & 15;
    int rd = insn & 31, rn = (insn >> 5) & 31, sz, idx, i, n;
    uint64_t r[2] = {0, 0}, v;
    if (!(imm5 & 15)) return 0;
    sz = __builtin_ctz((unsigned)imm5);
    idx = imm5 >> (sz + 1);
    if (scalar) {                                               /* dup (element), scalar: mov s0, v1.s[1] */
        if (op || imm4) return 0;
        r[0] = getel(c->vreg[rn], sz, idx);
        putv(c, rd, r, 0);
        return 1;
    }
    if (op) {                                                   /* ins (element) */
        if (!q) return 0;
        v = getel(c->vreg[rn], sz, imm4 >> sz);
        setel(c->vreg[rd], sz, idx, v);
        return 1;
    }
    n = (q ? 16 : 8) >> sz;
    switch (imm4) {
    case 0:                                                     /* dup (element) */
        if (sz == 3 && !q) return 0;
        v = getel(c->vreg[rn], sz, idx);
        for (i = 0; i < n; i++) setel(r, sz, i, v);
        putv(c, rd, r, q);
        return 1;
    case 1:                                                     /* dup (general) */
        if (sz == 3 && !q) return 0;
        v = X(c, rn) & mask_of(sz);
        for (i = 0; i < n; i++) setel(r, sz, i, v);
        putv(c, rd, r, q);
        return 1;
    case 3:                                                     /* ins (general) */
        if (!q) return 0;
        setel(c->vreg[rd], sz, idx, X(c, rn) & mask_of(sz));
        return 1;
    case 5:                                                     /* smov */
        if (sz >= (q ? 3 : 2)) return 0;
        v = (uint64_t)sx(getel(c->vreg[rn], sz, idx), sz);
        setX(c, rd, q ? v : (uint32_t)v);
        return 1;
    case 7:                                                     /* umov */
        if (q ? sz != 3 : sz == 3) return 0;
        setX(c, rd, getel(c->vreg[rn], sz, idx));
        return 1;
    }
    return 0;
}

static int simd_permute(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, sz = (insn >> 22) & 3, op = (insn >> 12) & 7;
    int rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31, i, n, part;
    uint64_t *A = c->vreg[rn], *B = c->vreg[rm], r[2] = {0, 0};
    if (op == 0 || op == 4 || (sz == 3 && !q)) return 0;
    n = (q ? 16 : 8) >> sz;
    part = op >> 2;                                             /* 0: uzp1/trn1/zip1, 1: the "2" forms */
    for (i = 0; i < n; i++) {
        uint64_t v;
        switch (op & 3) {
        case 1: {                                               /* uzp */
            int k = 2 * i + part;
            v = k < n ? getel(A, sz, k) : getel(B, sz, k - n);
            break;
        }
        case 2: v = (i & 1) ? getel(B, sz, (i & ~1) + part) : getel(A, sz, i + part); break; /* trn */
        default: v = getel((i & 1) ? B : A, sz, part * n / 2 + i / 2); break;               /* zip */
        }
        setel(r, sz, i, v);
    }
    putv(c, rd, r, q);
    return 1;
}

static int simd_ext(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, imm4 = (insn >> 11) & 15, rd = insn & 31, rn = (insn >> 5) & 31;
    int rm = (insn >> 16) & 31, i, n = q ? 16 : 8;
    uint64_t cat[4], r[2] = {0, 0};
    if (!q && imm4 >= 8) return 0;
    memcpy(cat, c->vreg[rn], 16);
    if (q) memcpy(cat + 2, c->vreg[rm], 16);
    else { cat[1] = c->vreg[rm][0]; }
    for (i = 0; i < n; i++) setel(r, 0, i, getel(cat, 0, i + imm4));
    putv(c, rd, r, q);
    return 1;
}

int aoi_simd_dp(struct aoi_cpu *c, uint32_t insn)
{
    /* scalar FP and FP<->integer: x0x11110 */
    if ((insn & 0x5f000000u) == 0x1e000000u && (insn >> 21 & 1)) return fp_scalar(c, insn);
    if ((insn & 0x5f000000u) == 0x1f000000u) return fp_3source(c, insn);

    if ((insn & 0x9ff80400u) == 0x0f000400u) return simd_modimm(c, insn);
    if ((insn & 0x9f800400u) == 0x0f000400u) return simd_shift_imm(c, insn, 0);
    if ((insn & 0xdf800400u) == 0x5f000400u) return simd_shift_imm(c, insn, 1);
    if ((insn & 0x9f200400u) == 0x0e200400u) return simd_three_same(c, insn, 0);
    if ((insn & 0xdf200400u) == 0x5e200400u) return simd_three_same(c, insn, 1);
    if ((insn & 0x9f3e0c00u) == 0x0e200800u) return simd_two_misc(c, insn, 0);
    if ((insn & 0xdf3e0c00u) == 0x5e200800u) return simd_two_misc(c, insn, 1);
    if ((insn & 0x9f3e0c00u) == 0x0e300800u) return simd_across(c, insn);
    if ((insn & 0x9f200c00u) == 0x0e200000u) return simd_three_diff(c, insn);
    if ((insn & 0x9fe08400u) == 0x0e000400u) return simd_copy(c, insn, 0);
    if ((insn & 0xffe08400u) == 0x5e000400u) return simd_copy(c, insn, 1);
    if ((insn & 0xbf208c00u) == 0x0e000800u) return simd_permute(c, insn);
    if ((insn & 0xbfe08400u) == 0x2e000000u) return simd_ext(c, insn);
    return 0;
}

/* ---------- structure loads/stores ---------- */

static int mem_el(struct aoi_cpu *c, int load, uint64_t a, int sz, uint64_t *v)
{
    uint8_t *p = aoi_mem_ptr(c->mem, a, (uint64_t)1 << sz);
    int i;
    if (!p) { c->stop = AOI_STOP_FAULT; c->fault_addr = a; return 0; }
    if (load) { *v = 0; for (i = 0; i < (1 << sz); i++) *v |= (uint64_t)p[i] << (8 * i); }
    else for (i = 0; i < (1 << sz); i++) p[i] = (uint8_t)(*v >> (8 * i));
    return 1;
}

int aoi_simd_ldst(struct aoi_cpu *c, uint32_t insn)
{
    int q = insn >> 30 & 1, load = insn >> 22 & 1, post = insn >> 23 & 1;
    int rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
    uint64_t base = rn == 31 ? c->sp : c->x[rn], a = base, tmp[32][2];
    int i, e, s, total = 0;

    if ((insn & 0xbf000000u) == 0x0c000000u) {                   /* multiple structures */
        static const signed char rpt_of[16] = { 1, -1, 4, -1, 1, -1, 3, 1, 1, -1, 2, -1, -1, -1, -1, -1 };
        static const signed char sel_of[16] = { 4, -1, 1, -1, 3, -1, 1, 1, 2, -1, 1, -1, -1, -1, -1, -1 };
        int op = (insn >> 12) & 15, sz = (insn >> 10) & 3, rpt = rpt_of[op], selem = sel_of[op], n, r;
        if ((insn >> 21 & 1) || (!post && rm) || rpt < 0) return 0;
        if (sz == 3 && !q && selem > 1) return 0;
        n = (q ? 16 : 8) >> sz;
        memcpy(tmp, c->vreg, sizeof tmp);
        for (r = 0; r < rpt; r++)
            for (e = 0; e < n; e++)
                for (s = 0; s < selem; s++) {
                    int t = (rt + r + s) & 31;
                    uint64_t v = load ? 0 : getel(c->vreg[t], sz, e);
                    if (!mem_el(c, load, a, sz, &v)) return 1;
                    if (load) setel(tmp[t], sz, e, v);
                    a += (uint64_t)1 << sz;
                    total += 1 << sz;
                }
        if (load)
            for (r = 0; r < rpt * selem; r++) {
                int t = (rt + r) & 31;
                c->vreg[t][0] = tmp[t][0]; c->vreg[t][1] = q ? tmp[t][1] : 0;
            }
    } else if ((insn & 0xbf000000u) == 0x0d000000u) {            /* single structure / replicate */
        int op = (insn >> 13) & 7, S = insn >> 12 & 1, sz = (insn >> 10) & 3, R = insn >> 21 & 1;
        int scale = op >> 1, selem = ((op & 1) << 1 | R) + 1, idx = 0, replicate = 0;
        if (!post && rm) return 0;
        switch (scale) {
        case 3:
            if (!load || S) return 0;
            scale = sz; replicate = 1; break;
        case 0: idx = q << 3 | S << 2 | sz; break;
        case 1: if (sz & 1) return 0; idx = q << 2 | S << 1 | sz >> 1; break;
        case 2:
            if (sz & 2) return 0;
            if (!(sz & 1)) idx = q << 1 | S;
            else { if (S) return 0; scale = 3; idx = q; }
            break;
        }
        for (s = 0; s < selem; s++) {
            int t = (rt + s) & 31;
            uint64_t v = load ? 0 : getel(c->vreg[t], scale, idx);
            if (!mem_el(c, load, a, scale, &v)) return 1;
            tmp[s][0] = v;
            a += (uint64_t)1 << scale;
            total += 1 << scale;
        }
        if (load)
            for (s = 0; s < selem; s++) {
                int t = (rt + s) & 31;
                if (replicate) {
                    uint64_t r2[2] = {0, 0};
                    for (i = 0; i < ((q ? 16 : 8) >> scale); i++) setel(r2, scale, i, tmp[s][0]);
                    putv(c, t, r2, q);
                } else setel(c->vreg[t], scale, idx, tmp[s][0]);
            }
    } else return 0;
    if (post) {
        uint64_t nb = base + (rm == 31 ? (uint64_t)total : c->x[rm]);
        if (rn == 31) c->sp = nb; else c->x[rn] = nb;
    }
    return 1;
}
