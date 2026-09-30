#include "cpu.h"

#include <string.h>

uint8_t *aoi_mem_ptr(struct aoi_mem *mem, uint64_t addr, uint64_t len)
{
    int i;
    for (i = 0; i < mem->n; i++) {
        struct aoi_region *r = &mem->r[i];
        if (addr >= r->base && addr + len >= addr && addr + len <= r->base + r->size)
            return r->host + (addr - r->base);
    }
    return NULL;
}

uint8_t *aoi_mem_map(struct aoi_mem *mem, uint64_t base, uint64_t size)
{
    struct aoi_region *r;
    if (mem->n == AOI_MAX_REGIONS) return NULL;
    r = &mem->r[mem->n++];
    r->base = base;
    r->size = size;
    r->host = NULL;             /* caller fills host */
    return NULL;
}

static uint64_t rd(struct aoi_cpu *c, uint64_t a, int len)
{
    uint8_t *p = aoi_mem_ptr(c->mem, a, (uint64_t)len);
    uint64_t v = 0;
    int i;
    if (!p) { c->stop = AOI_STOP_FAULT; c->fault_addr = a; return 0; }
    for (i = 0; i < len; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void wr(struct aoi_cpu *c, uint64_t a, uint64_t v, int len)
{
    uint8_t *p = aoi_mem_ptr(c->mem, a, (uint64_t)len);
    int i;
    if (!p) { c->stop = AOI_STOP_FAULT; c->fault_addr = a; return; }
    for (i = 0; i < len; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* X registers: index 31 reads as zero (XZR) except where the encoding means SP. */
static uint64_t X(struct aoi_cpu *c, int i) { return i == 31 ? 0 : c->x[i]; }
static void setX(struct aoi_cpu *c, int i, uint64_t v) { if (i != 31) c->x[i] = v; }

static uint64_t sextn(uint64_t v, int bits)
{
    uint64_t m = (uint64_t)1 << (bits - 1);
    return (v ^ m) - m;
}

/* AArch64 bitmask immediate decode (N:immr:imms) for AND/ORR/EOR/ANDS immediate. */
static int decode_bitmask(int n, int immr, int imms, int is64, uint64_t *out)
{
    int len = 31 - __builtin_clz((n << 6) | (~imms & 0x3f));
    int esize, i;
    uint64_t welem, mask, ror;
    unsigned levels, s, r;
    if (len < 1) return 0;
    esize = 1 << len;
    if (!is64 && esize > 32) return 0;
    levels = (unsigned)(esize - 1);
    s = (unsigned)imms & levels;
    r = (unsigned)immr & levels;
    if (s == levels) return 0;
    welem = ((uint64_t)1 << (s + 1)) - 1;
    ror = esize == 64 ? (welem >> r) | (r ? welem << (64 - r) : 0)
                      : ((welem >> r) | (welem << (esize - r))) & (((uint64_t)1 << esize) - 1);
    mask = is64 ? ~(uint64_t)0 : 0xffffffffu;
    ror &= esize == 64 ? mask : (((uint64_t)1 << esize) - 1);
    *out = 0;
    for (i = 0; i < (is64 ? 64 : 32); i += esize) *out |= ror << i;
    *out &= mask;
    return 1;
}

static void setflags64(struct aoi_cpu *c, uint64_t r) { c->z = r == 0; c->n = (int)(r >> 63); }

/* add/sub with carry+overflow flags. sub is add of ~b with carry_in=1. */
static uint64_t addflags(struct aoi_cpu *c, uint64_t a, uint64_t b, int carry_in, int is64, int setf)
{
    if (is64) {
        unsigned __int128 u = (unsigned __int128)a + b + carry_in;
        uint64_t r = (uint64_t)u;
        if (setf) {
            c->n = (int)(r >> 63); c->z = r == 0;
            c->c = (int)(u >> 64);
            c->v = (~(a ^ b) & (a ^ r)) >> 63 & 1;
        }
        return r;
    } else {
        uint64_t a32 = a & 0xffffffffu, b32 = b & 0xffffffffu;
        uint64_t u = a32 + b32 + carry_in;
        uint32_t r = (uint32_t)u;
        if (setf) {
            c->n = (int)(r >> 31); c->z = r == 0;
            c->c = (int)(u >> 32);
            c->v = (~(a32 ^ b32) & (a32 ^ r)) >> 31 & 1;
        }
        return r;
    }
}

static int cond_holds(struct aoi_cpu *c, unsigned cond)
{
    int r;
    switch (cond >> 1) {
    case 0: r = c->z; break;
    case 1: r = c->c; break;
    case 2: r = c->n; break;
    case 3: r = c->v; break;
    case 4: r = c->c && !c->z; break;
    case 5: r = c->n == c->v; break;
    case 6: r = c->n == c->v && !c->z; break;
    default: r = 1; break;
    }
    return (cond & 1) && cond != 0xf ? !r : r;
}

enum aoi_stop aoi_cpu_run(struct aoi_cpu *c, uint64_t max_steps)
{
    c->stop = AOI_RUN;
    while (c->stop == AOI_RUN) {
        uint32_t insn;
        uint64_t next;
        if (max_steps && c->steps >= max_steps) break;
        insn = (uint32_t)rd(c, c->pc, 4);
        if (c->stop != AOI_RUN) break;
        c->steps++;
        next = c->pc + 4;

        /* ---- branches ---- */
        if ((insn & 0xfc000000u) == 0x14000000u) {                 /* b */
            next = c->pc + (sextn(insn & 0x3ffffff, 26) << 2);
        } else if ((insn & 0xfc000000u) == 0x94000000u) {          /* bl */
            c->x[30] = c->pc + 4;
            next = c->pc + (sextn(insn & 0x3ffffff, 26) << 2);
        } else if ((insn & 0xfffffc1fu) == 0xd63f0000u) {          /* blr */
            uint64_t t = X(c, (insn >> 5) & 31); c->x[30] = c->pc + 4; next = t;
        } else if ((insn & 0xfffffc1fu) == 0xd61f0000u) {          /* br */
            next = X(c, (insn >> 5) & 31);
        } else if ((insn & 0xfffffc1fu) == 0xd65f0000u) {          /* ret */
            next = X(c, ((insn >> 5) & 31) ? (insn >> 5) & 31 : 30);
        } else if ((insn & 0xff000010u) == 0x54000000u) {          /* b.cond */
            if (cond_holds(c, insn & 0xf)) next = c->pc + (sextn((insn >> 5) & 0x7ffff, 19) << 2);
        } else if ((insn & 0x7e000000u) == 0x34000000u) {          /* cbz/cbnz */
            int is64 = insn >> 31, nz = (insn >> 24) & 1;
            uint64_t v = X(c, insn & 31); if (!is64) v &= 0xffffffffu;
            if ((v != 0) == (nz != 0)) next = c->pc + (sextn((insn >> 5) & 0x7ffff, 19) << 2);
        } else if ((insn & 0x7e000000u) == 0x36000000u) {          /* tbz/tbnz */
            int nz = (insn >> 24) & 1, b = ((insn >> 26 & 0x20)) | ((insn >> 19) & 0x1f);
            uint64_t v = X(c, insn & 31);
            if ((((v >> b) & 1) != 0) == (nz != 0)) next = c->pc + (sextn((insn >> 5) & 0x3fff, 14) << 2);
        /* ---- system ---- */
        } else if (insn == 0xd4000001u) {                          /* svc #0 */
            uint64_t r = aoi_linux_syscall(c);
            if (c->stop == AOI_RUN) c->x[0] = r;
        } else if ((insn & 0xfffffc1fu) == 0xd53bd040u) {          /* mrs xN, tpidr_el0 */
            setX(c, insn & 31, c->tpidr);
        } else if ((insn & 0xfffffc1fu) == 0xd51bd040u) {          /* msr tpidr_el0, xN */
            c->tpidr = X(c, insn & 31);
        } else if (insn == 0xd503201fu || (insn & 0xfffff01fu) == 0xd503201fu) {
            /* nop / hint */
        /* ---- moves (wide immediate) ---- */
        } else if ((insn & 0x1f800000u) == 0x12800000u ||
                   (insn & 0x1f800000u) == 0x52800000u ||
                   (insn & 0x1f800000u) == 0x72800000u) {          /* movn/movz/movk */
            int is64 = insn >> 31, opc = (insn >> 29) & 3, sh = ((insn >> 21) & 3) * 16;
            uint64_t imm = (uint64_t)((insn >> 5) & 0xffff) << sh, r;
            int rd_ = insn & 31;
            if (opc == 0) r = ~imm;                                /* movn */
            else if (opc == 2) r = imm;                            /* movz */
            else r = (X(c, rd_) & ~((uint64_t)0xffff << sh)) | imm; /* movk */
            if (!is64) r &= 0xffffffffu;
            setX(c, rd_, r);
        /* ---- add/sub immediate ---- */
        } else if ((insn & 0x1f000000u) == 0x11000000u) {          /* add/sub imm (+ADDS/SUBS) */
            int is64 = insn >> 31, sub = (insn >> 30) & 1, setf = (insn >> 29) & 1;
            int sh = (insn >> 22) & 1; uint64_t imm = (insn >> 10) & 0xfff;
            int rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t a = (rn == 31) ? c->sp : c->x[rn], r;
            if (sh) imm <<= 12;
            r = addflags(c, a, sub ? ~imm : imm, sub ? 1 : 0, is64, setf);
            if (!is64) r &= 0xffffffffu;
            if (rd_ == 31 && !setf) c->sp = r; else setX(c, rd_, r);
        /* ---- logical immediate ---- */
        } else if ((insn & 0x1f800000u) == 0x12000000u) {          /* and/orr/eor/ands imm */
            int is64 = insn >> 31, opc = (insn >> 29) & 3;
            int nn = (insn >> 22) & 1, immr = (insn >> 16) & 0x3f, imms = (insn >> 10) & 0x3f;
            int rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t m, a = X(c, rn), r;
            if (!decode_bitmask(nn, immr, imms, is64, &m)) { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; break; }
            if (opc == 0 || opc == 3) r = a & m;
            else if (opc == 1) r = a | m;
            else r = a ^ m;
            if (!is64) r &= 0xffffffffu;
            if (opc == 3) setflags64(c, is64 ? r : (uint32_t)r), c->c = 0, c->v = 0;
            if (rd_ == 31 && opc != 3) c->sp = r; else setX(c, rd_, r);
        /* ---- add/sub shifted register ---- */
        } else if ((insn & 0x1f200000u) == 0x0b000000u) {          /* add/sub/adds/subs reg */
            int is64 = insn >> 31, sub = (insn >> 30) & 1, setf = (insn >> 29) & 1;
            int shift = (insn >> 22) & 3, imm6 = (insn >> 10) & 0x3f;
            int rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t b = X(c, rm), a = X(c, rn), r;
            if (!is64) { b &= 0xffffffffu; a &= 0xffffffffu; }
            if (shift == 0) b <<= imm6;
            else if (shift == 1) b = is64 ? b >> imm6 : (uint32_t)b >> imm6;
            else if (shift == 2) b = is64 ? (uint64_t)((int64_t)b >> imm6) : (uint32_t)((int32_t)b >> imm6);
            if (!is64) b &= 0xffffffffu;
            r = addflags(c, a, sub ? ~b : b, sub ? 1 : 0, is64, setf);
            if (!is64) r &= 0xffffffffu;
            setX(c, rd_, r);
        /* ---- logical shifted register ---- */
        } else if ((insn & 0x1f000000u) == 0x0a000000u) {          /* and/orr/eor/ands/bic reg */
            int is64 = insn >> 31, opc = (insn >> 29) & 3, negate = (insn >> 21) & 1;
            int shift = (insn >> 22) & 3, imm6 = (insn >> 10) & 0x3f;
            int rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t b = X(c, rm), a = X(c, rn), r;
            if (!is64) { b &= 0xffffffffu; a &= 0xffffffffu; }
            if (shift == 0) b <<= imm6;
            else if (shift == 1) b = is64 ? b >> imm6 : (uint32_t)b >> imm6;
            else if (shift == 2) b = is64 ? (uint64_t)((int64_t)b >> imm6) : (uint32_t)((int32_t)b >> imm6);
            else b = is64 ? (b >> imm6) | (imm6 ? b << (64 - imm6) : 0) : ((uint32_t)b >> imm6) | (imm6 ? (uint32_t)b << (32 - imm6) : 0);
            if (negate) b = ~b;
            if (!is64) b &= 0xffffffffu;
            if (opc == 1) r = a | b; else if (opc == 2) r = a ^ b; else r = a & b;
            if (!is64) r &= 0xffffffffu;
            if (opc == 3) { setflags64(c, is64 ? r : (uint32_t)r); c->c = 0; c->v = 0; }
            setX(c, rd_, r);
        /* ---- load/store: register offset (integer) ---- */
        } else if ((insn & 0x3b200c00u) == 0x38200800u) {          /* ldr/str [Xn, Xm{,ext}] */
            int size = insn >> 30, opc = (insn >> 22) & 3;
            int rm = (insn >> 16) & 31, option = (insn >> 13) & 7, S = (insn >> 12) & 1;
            int rn = (insn >> 5) & 31, rt = insn & 31, bytes = 1 << size;
            uint64_t idx = X(c, rm);
            uint64_t base = (rn == 31) ? c->sp : c->x[rn], a;
            if ((option & 3) == 2) idx = (uint32_t)idx;              /* UXTW */
            else if (option == 6) idx = sextn((uint32_t)idx, 32);   /* SXTW */
            else if (option == 7) idx = (uint64_t)idx;              /* SXTX */
            if (S) idx <<= size;
            a = base + idx;
            if (opc == 0) wr(c, a, X(c, rt), bytes);
            else { uint64_t v = rd(c, a, bytes);
                   if (opc == 2 && size < 3) v = sextn(v, bytes * 8);
                   setX(c, rt, v); }
        /* ---- load/store: unsigned offset (32/64-bit integer) ---- */
        } else if ((insn & 0x3b000000u) == 0x39000000u) {          /* ldr/str [Xn, #imm] unsigned */
            int size = insn >> 30, opc = (insn >> 22) & 3;
            int rn = (insn >> 5) & 31, rt = insn & 31;
            uint64_t base = (rn == 31) ? c->sp : c->x[rn];
            uint64_t off = (uint64_t)((insn >> 10) & 0xfff) << size, a = base + off;
            int bytes = 1 << size;
            if (opc == 0) wr(c, a, X(c, rt), bytes);               /* store */
            else { uint64_t v = rd(c, a, bytes);                   /* load (zero-extend) */
                   if (opc == 2 && size < 3) v = sextn(v, bytes * 8);
                   setX(c, rt, v); }
        /* ---- load/store: signed/unscaled/pre/post (9-bit imm) ---- */
        } else if ((insn & 0x3b200000u) == 0x38000000u) {          /* ldur/stur, pre/post index */
            int size = insn >> 30, opc = (insn >> 22) & 3, mode = (insn >> 10) & 3;
            int rn = (insn >> 5) & 31, rt = insn & 31, bytes = 1 << size;
            int64_t off = (int64_t)sextn((insn >> 12) & 0x1ff, 9);
            uint64_t base = (rn == 31) ? c->sp : c->x[rn];
            uint64_t a = (mode == 1) ? base : base + off;          /* post-index uses base */
            if (mode == 3 || mode == 0) a = base + off;            /* pre-index / unscaled */
            if (mode == 1) a = base;                               /* post-index */
            if (opc == 0) wr(c, a, X(c, rt), bytes);
            else { uint64_t v = rd(c, a, bytes);
                   if (opc == 2 && size < 3) v = sextn(v, bytes * 8);
                   setX(c, rt, v); }
            if (mode == 1 || mode == 3) { uint64_t nb = base + off; if (rn == 31) c->sp = nb; else c->x[rn] = nb; }
        /* ---- load/store pair ---- */
        } else if ((insn & 0x3a000000u) == 0x28000000u && ((insn >> 26) & 1) == 0) { /* stp/ldp */
            int is64 = (insn >> 31) & 1, load = (insn >> 22) & 1, mode = (insn >> 23) & 3;
            int rt = insn & 31, rn = (insn >> 5) & 31, rt2 = (insn >> 10) & 31;
            int scale = is64 ? 3 : 2, bytes = 1 << scale;
            int64_t off = (int64_t)sextn((insn >> 15) & 0x7f, 7) << scale;
            uint64_t base = (rn == 31) ? c->sp : c->x[rn];
            uint64_t a = (mode == 1) ? base : base + off;
            if (load) { setX(c, rt, rd(c, a, bytes)); setX(c, rt2, rd(c, a + bytes, bytes)); }
            else { wr(c, a, X(c, rt), bytes); wr(c, a + bytes, X(c, rt2), bytes); }
            if (mode == 1 || mode == 3) { uint64_t nb = base + off; if (rn == 31) c->sp = nb; else c->x[rn] = nb; }
        /* ---- conditional select (csel/csinc/csinv/csneg, incl cset/csetm) ---- */
        } else if ((insn & 0x1fe00000u) == 0x1a800000u) {
            int is64 = insn >> 31, op = (insn >> 30) & 1, o2 = (insn >> 10) & 3;
            int rm = (insn >> 16) & 31, cond = (insn >> 12) & 0xf, rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t a = X(c, rn), b = X(c, rm), r;
            if (cond_holds(c, (unsigned)cond)) r = a;
            else { r = b; if (o2 == 1) r += 1; else if (o2 == 2) r = ~r; else if (o2 == 3 && op) r = (uint64_t)(-(int64_t)r); }
            if (!is64) r &= 0xffffffffu;
            setX(c, rd_, r);
        /* ---- pc-relative ---- */
        } else if ((insn & 0x9f000000u) == 0x90000000u) {          /* adrp */
            uint64_t imm = (sextn((((insn >> 5) & 0x7ffff) << 2) | ((insn >> 29) & 3), 21)) << 12;
            setX(c, insn & 31, (c->pc & ~(uint64_t)0xfff) + imm);
        } else if ((insn & 0x9f000000u) == 0x10000000u) {          /* adr */
            uint64_t imm = sextn((((insn >> 5) & 0x7ffff) << 2) | ((insn >> 29) & 3), 21);
            setX(c, insn & 31, c->pc + imm);
        } else {
            c->stop = AOI_STOP_UNDEF;
            c->fault_insn = insn;
            break;
        }
        if (c->stop == AOI_RUN) c->pc = next;
    }
    return c->stop;
}
