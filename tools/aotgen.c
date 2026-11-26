/* aotgen: a guest aarch64 shared library -> C, translated ahead of time (core/aot.h).
 *
 * usage: aotgen LIBRARY NAME OUT.c [PAGES]
 *   PAGES: a file of page addresses (hex, one per line) to translate, from a profile
 *   (tests/linux/prof_resolve.py --pages); the others stay interpreted. Default: all.
 *
 * Every instruction of the library's .text becomes C in a function per 4 KiB page; a jump
 * table enters the page at any instruction. Integer data processing, loads and stores,
 * branches and a few system instructions are translated (the accelerator's subset,
 * core/jit.c, with the same meaning); at anything else the function returns and the
 * interpreter takes over. Branches inside the page are gotos (a budget check on each, so
 * loops return to the interpreter now and then); others leave with cpu->pc set. Guest
 * addresses are the library's virtual addresses plus cpu->aot_bias (its load bias). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t sextn(uint64_t v, int bits) { return (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits)); }

static int decode_bitmask(int n, int immr, int imms, int is64, uint64_t *wmask, uint64_t *tmask, int imm)
{
    int len, levels, s, r, esize, d, i;
    uint64_t welem, telem, w = 0, t = 0;
    int x = (n << 6) | (~imms & 0x3f);
    if (!x) return 0;
    for (len = 6; len >= 0 && !(x >> len & 1); len--) {}
    if (len < 1) return 0;
    levels = (1 << len) - 1;
    if (imm && (imms & levels) == levels) return 0;
    s = imms & levels; r = immr & levels; esize = 1 << len;
    d = (s - r) & levels;
    welem = s + 1 == 64 ? ~0ULL : ((1ULL << (s + 1)) - 1);
    telem = d + 1 == 64 ? ~0ULL : ((1ULL << (d + 1)) - 1);
    if (r) welem = ((welem >> r) | (welem << (esize - r))) & (esize == 64 ? ~0ULL : ((1ULL << esize) - 1));
    for (i = 0; i < 64; i += esize) { w |= welem << i; t |= telem << i; }
    if (!is64) { w &= 0xffffffffu; t &= 0xffffffffu; }
    *wmask = w; *tmask = t;
    return 1;
}

static FILE *o;
static uint64_t page_lo, page_hi;   /* the page being written */

/* ---- operands as C expressions ---- */

static char rbuf[8][64];
static int rnext;
static const char *X(int r, int sp_ok)          /* 64-bit read */
{
    char *b = rbuf[rnext++ & 7];
    if (r == 31) snprintf(b, 64, "%s", sp_ok ? "c->sp" : "0ULL");
    else snprintf(b, 64, "c->x[%d]", r);
    return b;
}
static const char *W(int r, int sp_ok)          /* 32-bit read */
{
    char *b = rbuf[rnext++ & 7];
    if (r == 31) snprintf(b, 64, "%s", sp_ok ? "(uint32_t)c->sp" : "0u");
    else snprintf(b, 64, "(uint32_t)c->x[%d]", r);
    return b;
}
/* rd = value (an expression), zero-extended from 32 bits if w32 */
static void set(int r, int sp_ok, int w32, const char *expr)
{
    const char *z = w32 ? "(uint64_t)(uint32_t)" : "(uint64_t)";
    if (r == 31 && !sp_ok) fprintf(o, "(void)(%s);", expr);
    else if (r == 31) fprintf(o, "c->sp = %s(%s);", z, expr);
    else fprintf(o, "c->x[%d] = %s(%s);", r, z, expr);
}

static const char *cond(unsigned c)
{
    static const char *t[16] = { "c->z", "!c->z", "c->c", "!c->c", "c->n", "!c->n", "c->v", "!c->v",
                                 "(c->c && !c->z)", "!(c->c && !c->z)", "(c->n == c->v)", "(c->n != c->v)",
                                 "(!c->z && c->n == c->v)", "!(!c->z && c->n == c->v)", "1", "1" };
    return t[c & 15];
}

/* shifted register operand: typ 0 lsl, 1 lsr, 2 asr, 3 ror */
static void shifted(char *b, size_t n, int r, int w32, int typ, int amt)
{
    const char *v = w32 ? W(r, 0) : X(r, 0);
    int bits = w32 ? 32 : 64;
    if (!amt) { snprintf(b, n, "%s", v); return; }
    switch (typ) {
    case 0: snprintf(b, n, "(%s << %d)", v, amt); break;
    case 1: snprintf(b, n, "(%s >> %d)", v, amt); break;
    case 2: snprintf(b, n, w32 ? "(uint32_t)((int32_t)%s >> %d)" : "(uint64_t)((int64_t)%s >> %d)", v, amt); break;
    default: snprintf(b, n, "((%s >> %d) | (%s << %d))", v, amt, v, bits - amt); break;
    }
}
static void extended(char *b, size_t n, int r, int option, int sh)
{
    static const char *f[8] = { "(uint64_t)(uint8_t)%s", "(uint64_t)(uint16_t)%s", "(uint64_t)(uint32_t)%s", "%s",
                                "(uint64_t)(int64_t)(int8_t)%s", "(uint64_t)(int64_t)(int16_t)%s", "(uint64_t)(int64_t)(int32_t)%s", "%s" };
    char t[96];
    snprintf(t, sizeof t, f[option & 7], X(r, 0));
    if (sh) snprintf(b, n, "(%s << %d)", t, sh); else snprintf(b, n, "%s", t);
}

/* a + b or a - b, flags if setf, result to rd */
static void addsub(int w32, int sub, int setf, int rd, int rd_sp, const char *a, const char *b)
{
    const char *T = w32 ? "uint32_t" : "uint64_t";
    int top = w32 ? 31 : 63;
    fprintf(o, "{ %s a_ = %s, b_ = %s, r_ = a_ %c b_; ", T, a, b, sub ? '-' : '+');
    if (setf)
        fprintf(o, "c->n = (int)(r_ >> %d); c->z = !r_; c->c = %s; c->v = (int)((((%s) & (a_ ^ r_)) >> %d) & 1); ",
                top, sub ? "a_ >= b_" : "r_ < a_", sub ? "a_ ^ b_" : "~(a_ ^ b_)", top);
    if (rd != 31 || rd_sp) set(rd, rd_sp, w32, "r_");
    fprintf(o, " }");
}
static void logic_flags(int w32)
{
    fprintf(o, "c->n = (int)(r_ >> %d); c->z = !r_; c->c = 0; c->v = 0; ", w32 ? 31 : 63);
}

/* ---- control flow ---- */

static void go(uint64_t t)                       /* taken branch to t (this instruction counted) */
{
    if (t >= page_lo && t < page_hi)
        fprintf(o, "{ if (++n >= AOT_BUDGET) { c->pc = B + 0x%llxULL; return n; } goto L%u; }",
                (unsigned long long)t, (unsigned)((t - page_lo) >> 2));
    else fprintf(o, "{ n++; c->pc = B + 0x%llxULL; return n; }", (unsigned long long)t);
}

/* load: dst (uint64_t lvalue name) = len bytes at address expr, sign-extended if sign */
static void load(const char *dst, const char *addr, int len, int sign, uint64_t pc)
{
    fprintf(o, "{ uint64_t la_ = %s; %s = rd(c, la_, %d); if (c->stop != AOI_RUN) { c->pc = B + 0x%llxULL; return n; } }",
            addr, dst, len, (unsigned long long)pc);
    if (sign && len < 8) fprintf(o, " %s = (uint64_t)(int64_t)(int%d_t)%s;", dst, len * 8, dst);
}
static void store(const char *addr, const char *val, int len, uint64_t pc)
{
    fprintf(o, "{ wr(c, %s, %s, %d); if (c->stop != AOI_RUN) { c->pc = B + 0x%llxULL; return n; } }",
            addr, val, len, (unsigned long long)pc);
}
static void ld_st(int size, int opc, int rt, const char *addr, uint64_t pc)
{
    char a[160];
    snprintf(a, sizeof a, "%s", addr);
    fprintf(o, "{ uint64_t ea_ = %s; ", a);
    if (opc == 0) store("ea_", X(rt, 0), 1 << size, pc);
    else {
        fprintf(o, "uint64_t v_; ");
        load("v_", "ea_", 1 << size, opc != 1, pc);
        set(rt, 0, opc == 3, "v_");
    }
    fprintf(o, " }");
}

/* One instruction at pc. 1: translated, falls through to the next (counted); 2: ended in a
 * branch (handled); 0: outside the subset. */
static int insn(uint32_t w, uint64_t pc)
{
    uint64_t nxt = pc + 4;
    int sf = w >> 31, w32 = !sf;
    int rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
    char e1[256], e2[256];

    /* ---- system ---- */
    if ((w & 0xfffff01fu) == 0xd503201fu) return 1;                /* nop, hints (bti, paciasp...) */
    if ((w & 0xfffff09fu) == 0xd503309fu) return 1;                /* dsb, dmb, isb */
    if (w == 0xd503305fu) { fprintf(o, "c->excl_valid = 0;"); return 1; }   /* clrex */
    if ((w & 0xffffffe0u) == 0xd53bd040u) { set(rd, 0, 0, "c->tpidr"); return 1; }   /* mrs tpidr_el0 */
    if ((w & 0xffffffe0u) == 0xd51bd040u) { fprintf(o, "c->tpidr = %s;", X(rd, 0)); return 1; }

    /* ---- branches ---- */
    if ((w & 0x7c000000u) == 0x14000000u) {                       /* b, bl */
        uint64_t t = pc + (sextn(w & 0x3ffffff, 26) << 2);
        if (w >> 31) fprintf(o, "c->x[30] = B + 0x%llxULL; ", (unsigned long long)nxt);
        go(t);
        return 2;
    }
    if ((w & 0xff000010u) == 0x54000000u) {                       /* b.cond */
        uint64_t t = pc + (sextn((w >> 5) & 0x7ffff, 19) << 2);
        fprintf(o, "if (%s) ", cond(w & 15)); go(t);
        return 1;
    }
    if ((w & 0x7e000000u) == 0x34000000u) {                       /* cbz, cbnz */
        uint64_t t = pc + (sextn((w >> 5) & 0x7ffff, 19) << 2);
        fprintf(o, "if (%s%s) ", (w >> 24 & 1) ? "" : "!", w32 ? W(rd, 0) : X(rd, 0)); go(t);
        return 1;
    }
    if ((w & 0x7e000000u) == 0x36000000u) {                       /* tbz, tbnz */
        int b = (int)((w >> 26 & 0x20) | ((w >> 19) & 0x1f));
        uint64_t t = pc + (sextn((w >> 5) & 0x3fff, 14) << 2);
        fprintf(o, "if (%s((%s >> %d) & 1)) ", (w >> 24 & 1) ? "" : "!", X(rd, 0), b); go(t);
        return 1;
    }
    if ((w & 0xfffffc1fu) == 0xd65f0000u || (w & 0xfffffc1fu) == 0xd61f0000u) {   /* ret, br */
        fprintf(o, "{ n++; c->pc = %s; return n; }", X(rn, 0));
        return 2;
    }
    if ((w & 0xfffffc1fu) == 0xd63f0000u) {                       /* blr */
        fprintf(o, "{ uint64_t t_ = %s; c->x[30] = B + 0x%llxULL; n++; c->pc = t_; return n; }", X(rn, 0), (unsigned long long)nxt);
        return 2;
    }

    /* ---- data processing, immediate ---- */
    if ((w & 0x1f000000u) == 0x11000000u && !(w >> 23 & 1)) {     /* add/sub immediate */
        int opn = w >> 30 & 1, s = w >> 29 & 1;
        uint64_t imm = (uint64_t)((w >> 10) & 0xfff) << ((w >> 22 & 1) ? 12 : 0);
        snprintf(e2, sizeof e2, w32 ? "%lluu" : "%lluULL", (unsigned long long)imm);
        addsub(w32, opn, s, rd, !s, w32 ? W(rn, 1) : X(rn, 1), e2);
        return 1;
    }
    if ((w & 0x1f800000u) == 0x12000000u) {                       /* logical immediate */
        int opc = w >> 29 & 3;
        uint64_t m, t;
        static const char ops[4] = { '&', '|', '^', '&' };
        if (!decode_bitmask(w >> 22 & 1, (w >> 16) & 63, (w >> 10) & 63, sf, &m, &t, 1)) return 0;
        fprintf(o, "{ %s r_ = %s %c %lluULL; ", w32 ? "uint32_t" : "uint64_t", w32 ? W(rn, 0) : X(rn, 0), ops[opc], (unsigned long long)m);
        if (opc == 3) { logic_flags(w32); set(rd, 0, w32, "r_"); }
        else set(rd, 1, w32, "r_");
        fprintf(o, " }");
        return 1;
    }
    if ((w & 0x1f800000u) == 0x12800000u) {                       /* movn, movz, movk */
        int opc = w >> 29 & 3, hw = (w >> 21) & 3;
        uint64_t imm = (uint64_t)((w >> 5) & 0xffff) << (16 * hw);
        if (opc == 1) return 0;
        if (opc == 3) {
            snprintf(e1, sizeof e1, "(%s & 0x%llxULL) | 0x%llxULL", X(rd, 0), (unsigned long long)~(0xffffULL << (16 * hw)), (unsigned long long)imm);
            set(rd, 0, w32, e1);
            return 1;
        }
        if (opc == 0) imm = ~imm;
        if (w32) imm &= 0xffffffffu;
        snprintf(e1, sizeof e1, "0x%llxULL", (unsigned long long)imm);
        set(rd, 0, 0, e1);
        return 1;
    }
    if ((w & 0x1f800000u) == 0x13000000u) {                       /* sbfm, bfm, ubfm */
        int opc = w >> 29 & 3, immr = (w >> 16) & 63, imms = (w >> 10) & 63, width = w32 ? 32 : 64;
        uint64_t wm, tm;
        const char *T = w32 ? "uint32_t" : "uint64_t", *S = w32 ? "int32_t" : "int64_t";
        if (opc == 3 || !decode_bitmask(w >> 22 & 1, immr, imms, sf, &wm, &tm, 0)) return 0;
        fprintf(o, "{ %s src_ = %s, bot_ = (%s)(%s & 0x%llxULL); ", T, w32 ? W(rn, 0) : X(rn, 0), T,
                immr ? (snprintf(e1, sizeof e1, "((src_ >> %d) | (src_ << %d))", immr, width - immr), e1) : "src_",
                (unsigned long long)wm);
        if (opc == 2) fprintf(o, "%s r_ = bot_ & 0x%llxULL; ", T, (unsigned long long)tm);
        else if (opc == 0)
            fprintf(o, "%s top_ = (%s)((%s)(src_ << %d) >> %d); %s r_ = (top_ & 0x%llxULL) | (bot_ & 0x%llxULL); ",
                    T, T, S, width - 1 - imms, width - 1, T, (unsigned long long)~tm & (w32 ? 0xffffffffULL : ~0ULL), (unsigned long long)tm);
        else
            fprintf(o, "%s dst_ = %s; bot_ = (dst_ & 0x%llxULL) | bot_; %s r_ = (dst_ & 0x%llxULL) | (bot_ & 0x%llxULL); ",
                    T, w32 ? W(rd, 0) : X(rd, 0), (unsigned long long)~wm & (w32 ? 0xffffffffULL : ~0ULL), T,
                    (unsigned long long)~tm & (w32 ? 0xffffffffULL : ~0ULL), (unsigned long long)tm);
        set(rd, 0, w32, "r_");
        fprintf(o, " }");
        return 1;
    }
    if ((w & 0x7fa00000u) == 0x13800000u) {                       /* extr */
        int lsb = (w >> 10) & 63;
        if (!lsb) { set(rd, 0, w32, w32 ? W(rm, 0) : X(rm, 0)); return 1; }
        snprintf(e1, sizeof e1, "(%s >> %d) | (%s << %d)", w32 ? W(rm, 0) : X(rm, 0), lsb, w32 ? W(rn, 0) : X(rn, 0), (w32 ? 32 : 64) - lsb);
        set(rd, 0, w32, e1);
        return 1;
    }
    if ((w & 0x1f000000u) == 0x10000000u) {                       /* adr, adrp */
        int64_t imm = (int64_t)sextn(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3), 21);
        if (w >> 31) snprintf(e1, sizeof e1, "((B + 0x%llxULL) & ~0xfffULL) + 0x%llxULL", (unsigned long long)pc, (unsigned long long)((uint64_t)imm << 12));
        else snprintf(e1, sizeof e1, "B + 0x%llxULL", (unsigned long long)(pc + (uint64_t)imm));
        set(rd, 0, 0, e1);
        return 1;
    }

    /* ---- loads and stores (integer registers) ---- */
    if ((w & 0x3b000000u) == 0x39000000u) {                       /* unsigned offset */
        int size = w >> 30, opc = (w >> 22) & 3;
        if (w >> 26 & 1) return 0;
        if (size == 3 && opc == 2) return 1;                       /* prfm */
        if ((size == 3 && opc == 3) || (size == 2 && opc == 3)) return 0;
        snprintf(e1, sizeof e1, "%s + %lluULL", X(rn, 1), (unsigned long long)(((w >> 10) & 0xfff) << size));
        ld_st(size, opc, rd, e1, pc);
        return 1;
    }
    if ((w & 0x3b200000u) == 0x38000000u) {                       /* unscaled, post-, pre-index, unprivileged */
        int size = w >> 30, opc = (w >> 22) & 3, mode = (w >> 10) & 3;
        int64_t imm = (int64_t)sextn((w >> 12) & 0x1ff, 9);
        if (w >> 26 & 1) return 0;
        if (size == 3 && opc == 2) return 1;
        if ((size == 3 && opc == 3) || (size == 2 && opc == 3)) return 0;
        if (mode == 0 || mode == 2) { snprintf(e1, sizeof e1, "%s + 0x%llxULL", X(rn, 1), (unsigned long long)imm); ld_st(size, opc, rd, e1, pc); return 1; }
        fprintf(o, "{ uint64_t base_ = %s, wb_ = base_ + 0x%llxULL; ", X(rn, 1), (unsigned long long)imm);
        ld_st(size, opc, rd, mode == 3 ? "wb_" : "base_", pc);
        set(rn, 1, 0, "wb_");
        fprintf(o, " }");
        return 1;
    }
    if ((w & 0x3b200c00u) == 0x38200800u) {                       /* register offset */
        int size = w >> 30, opc = (w >> 22) & 3, option = (w >> 13) & 7, s = w >> 12 & 1;
        if (w >> 26 & 1) return 0;
        if (size == 3 && opc == 2) return 1;
        if ((size == 3 && opc == 3) || (size == 2 && opc == 3)) return 0;
        extended(e2, sizeof e2, rm, option, s ? size : 0);
        snprintf(e1, sizeof e1, "%s + %s", X(rn, 1), e2);
        ld_st(size, opc, rd, e1, pc);
        return 1;
    }
    if ((w & 0x3b000000u) == 0x18000000u) {                       /* ldr literal */
        int opc = w >> 30;
        if (w >> 26 & 1) return 0;
        if (opc == 3) return 1;                                    /* prfm */
        snprintf(e1, sizeof e1, "B + 0x%llxULL", (unsigned long long)(pc + (sextn((w >> 5) & 0x7ffff, 19) << 2)));
        fprintf(o, "{ uint64_t v_; ");
        load("v_", e1, opc == 1 ? 8 : 4, opc == 2, pc);
        set(rd, 0, 0, "v_");
        fprintf(o, " }");
        return 1;
    }
    if ((w & 0x3a000000u) == 0x28000000u) {                       /* ldp, stp (and ldnp, stnp, ldpsw) */
        int opc = w >> 30, typ = (w >> 23) & 3, L = w >> 22 & 1, rt2 = (w >> 10) & 31;
        int size = opc == 2 ? 3 : 2, bytes = 1 << size;
        int64_t imm = (int64_t)sextn((w >> 15) & 0x7f, 7) * bytes;
        if (w >> 26 & 1) return 0;
        if (opc == 3 || (opc == 1 && !L)) return 0;
        fprintf(o, "{ uint64_t base_ = %s, a_ = base_ + 0x%llxULL, wb_ = a_; ", X(rn, 1), (unsigned long long)(typ == 1 ? 0 : (uint64_t)imm));
        if (typ == 1) fprintf(o, "wb_ = base_ + 0x%llxULL; ", (unsigned long long)imm);
        if (L) {
            fprintf(o, "uint64_t v1_, v2_; ");
            load("v1_", "a_", bytes, opc == 1, pc);
            snprintf(e1, sizeof e1, "a_ + %d", bytes);
            load("v2_", e1, bytes, opc == 1, pc);
        } else {
            fprintf(o, "uint64_t s1_ = %s, s2_ = %s; ", X(rd, 0), X(rt2, 0));
            store("a_", "s1_", bytes, pc);
            snprintf(e1, sizeof e1, "a_ + %d", bytes);
            store(e1, "s2_", bytes, pc);
        }
        if (typ == 1 || typ == 3) set(rn, 1, 0, "wb_");
        if (L) { set(rd, 0, 0, "v1_"); set(rt2, 0, 0, "v2_"); }
        fprintf(o, " }");
        return 1;
    }

    /* ---- data processing, register ---- */
    if ((w & 0x1f000000u) == 0x0a000000u) {                       /* logical shifted register */
        int opc = w >> 29 & 3, typ = (w >> 22) & 3, nb = w >> 21 & 1, amt = (w >> 10) & 63;
        static const char ops[4] = { '&', '|', '^', '&' };
        shifted(e2, sizeof e2, rm, w32, typ, amt);
        fprintf(o, "{ %s r_ = %s %c (%s%s); ", w32 ? "uint32_t" : "uint64_t", w32 ? W(rn, 0) : X(rn, 0), ops[opc], nb ? "~" : "", e2);
        if (opc == 3) logic_flags(w32);
        set(rd, 0, w32, "r_");
        fprintf(o, " }");
        return 1;
    }
    if ((w & 0x1f200000u) == 0x0b000000u) {                       /* add/sub shifted register */
        int opn = w >> 30 & 1, s = w >> 29 & 1, typ = (w >> 22) & 3, amt = (w >> 10) & 63;
        if (typ == 3) return 0;
        shifted(e2, sizeof e2, rm, w32, typ, amt);
        snprintf(e1, sizeof e1, "%s", w32 ? W(rn, 0) : X(rn, 0));
        addsub(w32, opn, s, rd, 0, e1, e2);
        return 1;
    }
    if ((w & 0x1fe00000u) == 0x0b200000u) {                       /* add/sub extended register */
        int opn = w >> 30 & 1, s = w >> 29 & 1, option = (w >> 13) & 7, sh = (w >> 10) & 7;
        extended(e1, sizeof e1, rm, option, sh);
        if (w32) { snprintf(e2, sizeof e2, "(uint32_t)%s", e1); addsub(1, opn, s, rd, !s, W(rn, 1), e2); }
        else addsub(0, opn, s, rd, !s, X(rn, 1), e1);
        return 1;
    }
    if ((w & 0x1fe0fc00u) == 0x1a000000u) {                       /* adc, adcs, sbc, sbcs */
        int opn = w >> 30 & 1, s = w >> 29 & 1;
        const char *T = w32 ? "uint32_t" : "uint64_t";
        int top = w32 ? 31 : 63;
        fprintf(o, "{ %s a_ = %s, b_ = %s%s, r_ = a_ + b_ + (%s)c->c; ", T, w32 ? W(rn, 0) : X(rn, 0), opn ? "~" : "", w32 ? W(rm, 0) : X(rm, 0), T);
        if (s) fprintf(o, "c->n = (int)(r_ >> %d); c->z = !r_; c->c = c->c ? r_ <= a_ : r_ < a_; c->v = (int)(((~(a_ ^ b_) & (a_ ^ r_)) >> %d) & 1); ", top, top);
        set(rd, 0, w32, "r_");
        fprintf(o, " }");
        return 1;
    }
    if ((w & 0x3fe00410u) == 0x3a400000u) {                       /* ccmp, ccmn (register, immediate) */
        int opn = w >> 30 & 1, nzcv = w & 15;
        if (w >> 11 & 1) snprintf(e2, sizeof e2, w32 ? "%du" : "%dULL", rm);
        else snprintf(e2, sizeof e2, "%s", w32 ? W(rm, 0) : X(rm, 0));
        snprintf(e1, sizeof e1, "%s", w32 ? W(rn, 0) : X(rn, 0));
        fprintf(o, "if (%s) ", cond((w >> 12) & 15));
        addsub(w32, opn, 1, 31, 0, e1, e2);                         /* ccmp: a - b; ccmn: a + b */
        fprintf(o, " else { c->n = %d; c->z = %d; c->c = %d; c->v = %d; }", nzcv >> 3 & 1, nzcv >> 2 & 1, nzcv >> 1 & 1, nzcv & 1);
        return 1;
    }
    if ((w & 0x3fe00000u) == 0x1a800000u) {                       /* csel, csinc, csinv, csneg */
        int opn = w >> 30 & 1, op2 = (w >> 10) & 3;
        const char *T = w32 ? "uint32_t" : "uint64_t";
        const char *b = w32 ? W(rm, 0) : X(rm, 0);
        if (!opn && op2 == 0) snprintf(e2, sizeof e2, "%s", b);
        else if (!opn && op2 == 1) snprintf(e2, sizeof e2, "(%s)(%s + 1)", T, b);
        else if (opn && op2 == 0) snprintf(e2, sizeof e2, "(%s)~%s", T, b);
        else if (opn && op2 == 1) snprintf(e2, sizeof e2, "(%s)(0 - %s)", T, b);
        else return 0;
        snprintf(e1, sizeof e1, "%s ? %s : %s", cond((w >> 12) & 15), w32 ? W(rn, 0) : X(rn, 0), e2);
        set(rd, 0, w32, e1);
        return 1;
    }
    if ((w & 0x5fe00000u) == 0x1ac00000u) {                       /* 2-source */
        int opcode = (w >> 10) & 63, bits = w32 ? 32 : 64;
        const char *a = w32 ? W(rn, 0) : X(rn, 0), *b = w32 ? W(rm, 0) : X(rm, 0);
        if (opcode >= 8 && opcode <= 11) {                         /* lslv lsrv asrv rorv: the count modulo the width */
            fprintf(o, "{ unsigned s_ = (unsigned)(%s & %d); %s r_ = ", b, bits - 1, w32 ? "uint32_t" : "uint64_t");
            if (opcode == 8) fprintf(o, "%s << s_; ", a);
            else if (opcode == 9) fprintf(o, "%s >> s_; ", a);
            else if (opcode == 10) fprintf(o, w32 ? "(uint32_t)((int32_t)%s >> s_); " : "(uint64_t)((int64_t)%s >> s_); ", a);
            else fprintf(o, "s_ ? ((%s >> s_) | (%s << (%d - s_))) : %s; ", a, a, bits, a);
            set(rd, 0, w32, "r_");
            fprintf(o, " }");
            return 1;
        }
        if (opcode == 2 || opcode == 3) {                          /* udiv, sdiv: x/0 = 0, MIN/-1 = MIN */
            const char *T = w32 ? "uint32_t" : "uint64_t", *S = w32 ? "int32_t" : "int64_t";
            if (opcode == 2) fprintf(o, "{ %s a_ = %s, b_ = %s, r_ = b_ ? a_ / b_ : 0; ", T, a, b);
            else fprintf(o, "{ %s a_ = %s, b_ = %s, r_ = !b_ ? 0 : (%s)b_ == -1 ? (%s)(0 - a_) : (%s)((%s)a_ / (%s)b_); ", T, a, b, S, T, T, S, S);
            set(rd, 0, w32, "r_");
            fprintf(o, " }");
            return 1;
        }
        return 0;
    }
    if ((w & 0x7ffffc00u) == 0x5ac01000u) {                       /* clz */
        if (w32) snprintf(e1, sizeof e1, "%s ? (uint32_t)__builtin_clz(%s) : 32u", W(rn, 0), W(rn, 0));
        else snprintf(e1, sizeof e1, "%s ? (uint64_t)__builtin_clzll(%s) : 64ULL", X(rn, 0), X(rn, 0));
        set(rd, 0, w32, e1);
        return 1;
    }
    if ((w & 0x7ffff000u) == 0x5ac00000u) {                       /* rbit, rev16, rev32 / rev (w), rev (x) */
        int opc = (w >> 10) & 3;
        if (w32 && opc == 3) return 0;
        if (opc == 0) snprintf(e1, sizeof e1, w32 ? "aot_rbit32(%s)" : "aot_rbit64(%s)", w32 ? W(rn, 0) : X(rn, 0));
        else if (opc == 1) snprintf(e1, sizeof e1, w32 ? "aot_rev16_32(%s)" : "aot_rev16_64(%s)", w32 ? W(rn, 0) : X(rn, 0));
        else if (opc == 2) snprintf(e1, sizeof e1, w32 ? "__builtin_bswap32(%s)" : "aot_rev32_64(%s)", w32 ? W(rn, 0) : X(rn, 0));
        else snprintf(e1, sizeof e1, "__builtin_bswap64(%s)", X(rn, 0));
        set(rd, 0, w32, e1);
        return 1;
    }
    if ((w & 0x1f000000u) == 0x1b000000u) {                       /* 3-source */
        int op31 = (w >> 21) & 7, o0 = w >> 15 & 1, ra = (w >> 10) & 31;
        if (op31 == 0) {                                           /* madd, msub */
            if (w32) snprintf(e1, sizeof e1, "%s %c %s * %s", W(ra, 0), o0 ? '-' : '+', W(rn, 0), W(rm, 0));
            else snprintf(e1, sizeof e1, "%s %c %s * %s", X(ra, 0), o0 ? '-' : '+', X(rn, 0), X(rm, 0));
            set(rd, 0, w32, e1);
            return 1;
        }
        if ((op31 == 1 || op31 == 5) && sf) {                      /* smaddl/smsubl, umaddl/umsubl */
            const char *ext = op31 == 1 ? "(uint64_t)(int64_t)(int32_t)" : "(uint64_t)";
            snprintf(e1, sizeof e1, "%s %c %s%s * %s%s", X(ra, 0), o0 ? '-' : '+', ext, W(rn, 0), ext, W(rm, 0));
            set(rd, 0, 0, e1);
            return 1;
        }
        if ((op31 == 2 || op31 == 6) && sf) {                      /* smulh, umulh */
            if (op31 == 6) snprintf(e1, sizeof e1, "(uint64_t)(((unsigned __int128)%s * %s) >> 64)", X(rn, 0), X(rm, 0));
            else snprintf(e1, sizeof e1, "(uint64_t)(((__int128)(int64_t)%s * (int64_t)%s) >> 64)", X(rn, 0), X(rm, 0));
            set(rd, 0, 0, e1);
            return 1;
        }
        return 0;
    }
    return 0;
}

/* ---- the library ---- */

static uint8_t *img;
static size_t img_len;

int main(int argc, char **argv)
{
    FILE *f;
    uint64_t phoff, shoff, text_lo = 0, text_hi = 0, x_off = 0, x_vaddr = 0, x_sz = 0, pg;
    uint16_t phnum, phentsize, shnum, shentsize, shstrndx;
    uint8_t id[20];
    int idlen = 0, i, npages, translated = 0, total = 0;
    static uint64_t want[65536];
    int nwant = 0;
    if (argc != 4 && argc != 5) { fprintf(stderr, "usage: aotgen LIBRARY NAME OUT.c [PAGES]\n"); return 2; }
    if (argc == 5) {
        FILE *pf = fopen(argv[4], "r");
        unsigned long long v;
        if (!pf) { perror(argv[4]); return 1; }
        while (nwant < 65536 && fscanf(pf, "%llx", &v) == 1) want[nwant++] = v & ~0xfffULL;
        fclose(pf);
    }
    if (!(f = fopen(argv[1], "rb"))) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); img_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    img = malloc(img_len);
    if (!img || fread(img, 1, img_len, f) != img_len) { perror("read"); return 1; }
    fclose(f);
    if (memcmp(img, "\177ELF\2\1", 6) || *(uint16_t *)(img + 18) != 183) { fprintf(stderr, "not an aarch64 ELF64\n"); return 1; }
    memcpy(&phoff, img + 32, 8); memcpy(&shoff, img + 40, 8);
    memcpy(&phentsize, img + 54, 2); memcpy(&phnum, img + 56, 2);
    memcpy(&shentsize, img + 58, 2); memcpy(&shnum, img + 60, 2); memcpy(&shstrndx, img + 62, 2);
    for (i = 0; i < phnum; i++) {
        const uint8_t *p = img + phoff + (uint64_t)i * phentsize;
        uint32_t type, flags;
        uint64_t off, va, sz;
        memcpy(&type, p, 4); memcpy(&flags, p + 4, 4); memcpy(&off, p + 8, 8); memcpy(&va, p + 16, 8); memcpy(&sz, p + 32, 8);
        if (type == 1 && (flags & 1) && !x_sz) { x_off = off; x_vaddr = va; x_sz = sz; }
        if (type == 4) {
            uint64_t q = off;
            while (q + 12 <= off + sz) {
                uint32_t namesz, descsz, nt;
                memcpy(&namesz, img + q, 4); memcpy(&descsz, img + q + 4, 4); memcpy(&nt, img + q + 8, 4);
                if (nt == 3 && namesz == 4 && !memcmp(img + q + 12, "GNU", 4) && descsz <= 20) { memcpy(id, img + q + 16, descsz); idlen = (int)descsz; }
                q += 12 + ((namesz + 3) & ~3u) + ((descsz + 3) & ~3u);
            }
        }
    }
    if (!x_sz || x_off != x_vaddr || !idlen) { fprintf(stderr, "needs an executable segment at vaddr = offset and a build-id\n"); return 1; }
    {   /* .text */
        const uint8_t *sh = img + shoff, *strs;
        uint64_t stroff;
        memcpy(&stroff, sh + (uint64_t)shstrndx * shentsize + 24, 8);
        strs = img + stroff;
        for (i = 0; i < shnum; i++) {
            uint32_t name;
            uint64_t addr, size;
            memcpy(&name, sh + (uint64_t)i * shentsize, 4);
            memcpy(&addr, sh + (uint64_t)i * shentsize + 16, 8); memcpy(&size, sh + (uint64_t)i * shentsize + 32, 8);
            if (!strcmp((const char *)strs + name, ".text")) { text_lo = addr; text_hi = addr + size; }
        }
    }
    if (!text_hi) { fprintf(stderr, "no .text\n"); return 1; }
    if (!(o = fopen(argv[3], "w"))) { perror(argv[3]); return 1; }
    fprintf(o, "/* %s translated ahead of time by tools/aotgen.c - generated, do not edit */\n", argv[2]);
    fprintf(o, "#include \"cpu_impl.h\"\n#include \"aot.h\"\n#define B (c->aot_bias)\n#define AOT_BUDGET 200000\n");
    fprintf(o, "#pragma GCC diagnostic ignored \"-Wunused-label\"\n#pragma GCC diagnostic ignored \"-Wunused-variable\"\n");
    fprintf(o, "static inline uint32_t aot_rbit32(uint32_t v) { uint32_t r = 0; int i; for (i = 0; i < 32; i++) r |= ((v >> i) & 1u) << (31 - i); return r; }\n"
               "static inline uint64_t aot_rbit64(uint64_t v) { uint64_t r = 0; int i; for (i = 0; i < 64; i++) r |= ((v >> i) & 1ULL) << (63 - i); return r; }\n"
               "static inline uint32_t aot_rev16_32(uint32_t v) { return ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu); }\n"
               "static inline uint64_t aot_rev16_64(uint64_t v) { return ((v & 0x00ff00ff00ff00ffULL) << 8) | ((v >> 8) & 0x00ff00ff00ff00ffULL); }\n"
               "static inline uint64_t aot_rev32_64(uint64_t v) { return ((uint64_t)__builtin_bswap32((uint32_t)v)) | ((uint64_t)__builtin_bswap32((uint32_t)(v >> 32)) << 32); }\n");
    text_lo &= ~0xfffULL;
    text_hi = (text_hi + 0xfff) & ~0xfffULL;
    npages = (int)((text_hi - text_lo) >> 12);
    for (pg = text_lo; pg < text_hi; pg += 4096) {
        int k, nins = 0, j, wanted = !nwant;
        for (j = 0; j < nwant && !wanted; j++) wanted = want[j] == pg;
        if (!wanted) continue;
        page_lo = pg; page_hi = pg + 4096;
        fprintf(o, "static int p_%llx(struct aoi_cpu *c)\n{\n    int n = 0;\n    switch ((unsigned)((c->pc - B - 0x%llxULL) >> 2)) {\n",
                (unsigned long long)pg, (unsigned long long)pg);
        for (k = 0; k < 1024; k++) fprintf(o, "    case %d: goto L%d;\n", k, k);
        fprintf(o, "    default: return 0;\n    }\n");
        for (k = 0; k < 1024; k++) {
            uint64_t pc = pg + 4u * (unsigned)k;
            uint32_t w = 0;
            int r;
            if (pc >= x_vaddr && pc + 4 <= x_vaddr + x_sz) memcpy(&w, img + (pc - x_vaddr + x_off), 4);
            fprintf(o, "L%d: ", k);
            total++;
            r = (pc >= x_vaddr && pc + 4 <= x_vaddr + x_sz) ? insn(w, pc) : 0;
            if (r == 1) { fprintf(o, " n++;\n"); translated++; nins++; }
            else if (r == 2) { fprintf(o, "\n"); translated++; nins++; }
            else fprintf(o, "c->pc = B + 0x%llxULL; return n;   /* %08x */\n", (unsigned long long)pc, w);
        }
        fprintf(o, "    c->pc = B + 0x%llxULL; return n;\n}\n", (unsigned long long)page_hi);
    }
    fprintf(o, "static const aoi_aot_fn pages[%d] = {\n", npages);
    for (pg = text_lo; pg < text_hi; pg += 4096) {
        int j, wanted = !nwant;
        for (j = 0; j < nwant && !wanted; j++) wanted = want[j] == pg;
        if (wanted) fprintf(o, "    p_%llx,\n", (unsigned long long)pg); else fprintf(o, "    0,\n");
    }
    fprintf(o, "};\nconst struct aoi_aot_lib aot_%s = { \"%s\", {", argv[2], argv[2]);
    for (i = 0; i < idlen; i++) fprintf(o, "%s0x%02x", i ? ", " : "", id[i]);
    fprintf(o, "}, %d, 0x%llxULL, 0x%llxULL, pages };\n", idlen, (unsigned long long)text_lo, (unsigned long long)text_hi);
    fclose(o);
    fprintf(stderr, "aotgen: %s: %d pages, %d of %d instructions translated (%.1f%%)\n", argv[2], npages, translated, total,
            100.0 * translated / (total ? total : 1));
    return 0;
}
