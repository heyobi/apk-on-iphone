#define _POSIX_C_SOURCE 200809L
#include "cpu_impl.h"
#include "hle.h"

#ifdef AOI_DEBUG
/* Debug build (make build/aoiproc-debug): AOI_WATCH=value logs every store of that
 * 32-bit value, AOI_PCRING keeps the last 1024 pcs for the SIGSEGV trace. */
uint32_t aoi_watch_val;
uint64_t *aoi_pcring;
void (*aoi_watch_fn)(struct aoi_cpu *c, uint64_t a, uint64_t v, int len);
void aoi_watch_hit(struct aoi_cpu *c, uint64_t a, uint64_t v, int len)
{
    if (aoi_watch_fn) aoi_watch_fn(c, a, v, len);
}
#endif

#include <string.h>
#include <time.h>

uint8_t *aoi_mem_ptr(struct aoi_mem *mem, uint64_t addr, uint64_t len)
{
    int i;
    if (mem->vm) return aoi_vm_ptr(mem->vm, addr, len, 0);
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

/* Integer load: opc 1 = zero-extend, 2 = sign-extend to 64, 3 = sign-extend to 32. */
static void ild(struct aoi_cpu *c, int t, uint64_t a, int bytes, int opc)
{
    uint64_t v = rd(c, a, bytes);
    if (c->stop != AOI_RUN) return;
    if (opc == 2 && bytes < 8) v = sextn(v, bytes * 8);
    else if (opc == 3) v = sextn(v, bytes * 8) & 0xffffffffu;
    t == 31 ? (void)0 : (void)(c->x[t] = v);
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

/* VFPExpandImm for fmov (vector, immediate) */
static uint64_t vfp_imm32(uint64_t i8)
{
    uint64_t b6 = i8 >> 6 & 1;
    return (i8 >> 7) << 31 | ((!b6) << 7 | (b6 ? 0x1fULL : 0) << 2 | (i8 >> 4 & 3)) << 23 | (i8 & 0xf) << 19;
}
static uint64_t vfp_imm64(uint64_t i8)
{
    uint64_t b6 = i8 >> 6 & 1;
    return (i8 >> 7) << 63 | ((!b6) << 10 | (b6 ? 0xffULL : 0) << 2 | (i8 >> 4 & 3)) << 52 | (i8 & 0xf) << 48;
}

/* DecodeBitMasks(immediate=FALSE) for sbfm/bfm/ubfm: esize is the register width. */
static void bitfield_masks(unsigned R, unsigned S, unsigned width, uint64_t *wmask, uint64_t *tmask)
{
    uint64_t mask = width == 64 ? ~0ULL : 0xffffffffu;
    uint64_t welem = S + 1 >= 64 ? ~0ULL : ((uint64_t)1 << (S + 1)) - 1;
    unsigned d = (S - R) & (width - 1);
    *wmask = R ? ((welem >> R) | (welem << (width - R))) & mask : welem & mask;
    *tmask = d + 1 >= 64 ? ~0ULL : ((uint64_t)1 << (d + 1)) - 1;
}

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

/* The generic timer as user space sees it: a 24 MHz counter (Apple's and most
 * Android phones' frequency) driven by the host's monotonic clock. */
#define AOI_CNTFRQ 24000000u
int64_t aoi_mono_offset;           /* (core/proc.c: a restored snapshot's time shift) */

static uint64_t aoi_cntvct(void)
{
    struct timespec ts;
    int64_t ns;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ns = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec + aoi_mono_offset;
    return (uint64_t)(ns / 1000000000) * AOI_CNTFRQ + (uint64_t)(ns % 1000000000) * 3 / 125;
}

#ifdef AOI_OPHIST
/* Host profiling: how often each instruction shape runs (register fields masked out),
 * printed at exit, most frequent first. */
#include <stdio.h>
#include <stdlib.h>
#define OPH (1u << 20)
static uint32_t oph_key[OPH];
static uint64_t oph_n[OPH];
static int cmp_oph(const void *x, const void *y)
{
    uint64_t a = oph_n[*(const uint32_t *)x], b = oph_n[*(const uint32_t *)y];
    return a < b ? 1 : a > b ? -1 : 0;
}
static void oph_dump(void)
{
    static uint32_t idx[OPH];
    uint32_t i, n = 0;
    uint64_t total = 0;
    for (i = 0; i < OPH; i++) if (oph_n[i]) { idx[n++] = i; total += oph_n[i]; }
    qsort(idx, n, sizeof *idx, cmp_oph);
    for (i = 0; i < n && i < 80; i++)
        fprintf(stderr, "[ophist] %08x %6.2f%% %llu\n", oph_key[idx[i]], 100.0 * (double)oph_n[idx[i]] / (double)total,
                (unsigned long long)oph_n[idx[i]]);
}
static void aoi_ophist(uint32_t insn)
{
    static int armed;
    uint32_t k = insn & 0xffe0fc00u, h;
    if (!armed) { armed = 1; atexit(oph_dump); }
    if (getenv("AOI_OPHIST_FULL")) k = insn;
    for (h = (k * 2654435761u) >> 12; ; h = (h + 1) & (OPH - 1))
        if (oph_key[h] == k && oph_n[h]) { oph_n[h]++; return; }
        else if (!oph_n[h]) { oph_key[h] = k; oph_n[h] = 1; return; }
}
#endif

/* Decode cache: which branch of the chain below an instruction word takes, so a word
 * seen before jumps straight to its code (GCC/clang labels as values) instead of being
 * tested against the patterns before it. The choice depends on the word alone. An
 * entry is word << 32 | branch + 1, one 64-bit store (two host threads may share it). */
#define DCACHE_BITS 14
static uint64_t dcache[1u << DCACHE_BITS];
#define DCACHE_SLOT(w) (((w) * 0x9e3779b1u) >> (32 - DCACHE_BITS))
#define BODY(k) L##k: if (learn) dcache[DCACHE_SLOT(insn)] = (uint64_t)insn << 32 | ((k) + 1);

enum aoi_stop aoi_cpu_run(struct aoi_cpu *c, uint64_t max_steps)
{
    static void *const body[] = { &&L0, &&L1, &&L2, &&L3, &&L4, &&L5, &&L6, &&L7, &&L8, &&L9, &&L10, &&L11, &&L12, &&L13, &&L14, &&L15, &&L16, &&L17, &&L18, &&L19, &&L20, &&L21, &&L22, &&L23, &&L24, &&L25, &&L26, &&L27, &&L28, &&L29, &&L30, &&L31, &&L32, &&L33, &&L34, &&L35, &&L36, &&L37, &&L38, &&L39, &&L40, &&L41, &&L42, &&L43, &&L44, &&L45, &&L46, &&L47, &&L48, &&L49, &&L50, &&L51, &&L52 };
    /* per call, in registers: the step limit, the trace hook, and the code page being
     * run (its host address), so most fetches skip the page-table walk; a syscall may
     * map, unmap or protect memory, so it forgets the page */
    const uint64_t limit = max_steps ? max_steps : ~0ULL;
    void (*const trace)(struct aoi_cpu *, int) = c->trace;
    const uint8_t *cpage = NULL;
    uint64_t cpage_va = 1;                                         /* no page: never matches an aligned pc */
    c->stop = AOI_RUN;
    while (c->stop == AOI_RUN) {
        uint32_t insn;
        uint64_t next;
        int learn;
        if (c->steps >= limit) break;
        if (c->pc - c->thunk_base < 4 * c->thunk_slots) {
            unsigned slot = (unsigned)((c->pc - c->thunk_base) / 4);
            uint64_t r;
            if (slot == 0) { c->stop = AOI_STOP_RETURN; break; }
            r = c->host_call(c, slot);
            cpage_va = 1;                                          /* it may have mapped memory */
            if (c->stop != AOI_RUN) break;
            c->x[0] = r;
            c->pc = c->x[30];
            continue;
        }
#ifdef AOI_DEBUG
        if (aoi_pcring) aoi_pcring[c->steps & 1023] = c->pc;
#endif
        if ((c->pc & ~(uint64_t)0xffc) == cpage_va) insn = *(const aoi_u32u *)(cpage + (c->pc & 0xffc));
        else {
            insn = (uint32_t)fetch(c, c->pc);
            if (c->stop != AOI_RUN) break;
            if (!(c->pc & 3)) {
                cpage = gptr(c, c->pc, 4, AOI_PROT_X) - (c->pc & 0xffc);
                cpage_va = c->pc & ~(uint64_t)0xfff;
            }
        }
#ifdef AOI_OPHIST
        aoi_ophist(insn);
#endif
        c->steps++;
        next = c->pc + 4;
        if (trace) { c->nwlog = 0; trace(c, 0); }

        /* ---- SIMD & FP data processing (op0 x111), except modified immediate below: straight
         * to simd.c instead of down the chain (Skia's raster pipeline is mostly these) ---- */
        if ((insn & 0x0e000000u) == 0x0e000000u && (insn & 0x9ff80400u) != 0x0f000400u) {
            if (!aoi_simd_step(c, insn)) { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; break; }
            if (c->stop != AOI_RUN) break;
            c->pc = next;
            if (trace) trace(c, 1);
            continue;
        }
        {
            uint64_t e = dcache[DCACHE_SLOT(insn)];
            if ((uint32_t)(e >> 32) == insn && (uint32_t)e) { learn = 0; goto *body[(uint32_t)e - 1]; }
            learn = 1;
        }
        /* ---- the rest: start the chain at the instruction's group (op0, bits 28-25), so
         * a load or a register op is not first compared with every branch and system
         * pattern; sections before a group's label hold none of its encodings ---- */
        switch (insn >> 25 & 0xf) {
        case 8: case 9: goto dp_imm;                               /* data processing, immediate */
        case 4: case 6: case 12: case 14: goto ldst;               /* loads and stores */
        case 5: case 13: goto dp_reg;                              /* data processing, register */
        default: break;                                            /* branches, system, the rest */
        }
        /* ---- branches ---- */
        if ((insn & 0xfc000000u) == 0x14000000u) { BODY(0)                 /* b */
            next = c->pc + (sextn(insn & 0x3ffffff, 26) << 2);
            if (next - c->hle_lo < c->hle_hi - c->hle_lo && aoi_hle_run(c, next)) next = c->pc;   /* a tail call */
        } else if ((insn & 0xfc000000u) == 0x94000000u) { BODY(1)          /* bl */
            c->x[30] = c->pc + 4;
            next = c->pc + (sextn(insn & 0x3ffffff, 26) << 2);
            if (next - c->hle_lo < c->hle_hi - c->hle_lo && aoi_hle_run(c, next)) next = c->pc;
        } else if ((insn & 0xfffffc1fu) == 0xd63f0000u) { BODY(2)          /* blr */
            uint64_t t = X(c, (insn >> 5) & 31); c->x[30] = c->pc + 4; next = t;
            if (t - c->hle_lo < c->hle_hi - c->hle_lo && aoi_hle_run(c, t)) next = c->pc;
        } else if ((insn & 0xfffffc1fu) == 0xd61f0000u) { BODY(3)          /* br */
            next = X(c, (insn >> 5) & 31);
            if (next - c->hle_lo < c->hle_hi - c->hle_lo && aoi_hle_run(c, next)) next = c->pc;
        } else if ((insn & 0xfffffc1fu) == 0xd65f0000u) { BODY(4)          /* ret */
            next = X(c, (insn >> 5) & 31);             /* Rn is always encoded; x30 is only the default */
        } else if ((insn & 0xff000010u) == 0x54000000u) { BODY(5)          /* b.cond */
            if (cond_holds(c, insn & 0xf)) {
                next = c->pc + (sextn((insn >> 5) & 0x7ffff, 19) << 2);
                if (next - c->hle_lo < c->hle_hi - c->hle_lo && aoi_hle_run(c, next)) next = c->pc;   /* a loop's back edge */
            }
        } else if ((insn & 0x7e000000u) == 0x34000000u) { BODY(6)          /* cbz/cbnz */
            int is64 = insn >> 31, nz = (insn >> 24) & 1;
            uint64_t v = X(c, insn & 31); if (!is64) v &= 0xffffffffu;
            if ((v != 0) == (nz != 0)) next = c->pc + (sextn((insn >> 5) & 0x7ffff, 19) << 2);
        } else if ((insn & 0x7e000000u) == 0x36000000u) { BODY(7)          /* tbz/tbnz */
            int nz = (insn >> 24) & 1, b = ((insn >> 26 & 0x20)) | ((insn >> 19) & 0x1f);
            uint64_t v = X(c, insn & 31);
            if ((((v >> b) & 1) != 0) == (nz != 0)) next = c->pc + (sextn((insn >> 5) & 0x3fff, 14) << 2);
        /* ---- system ---- */
        } else if (insn == 0xd4000001u) { BODY(8)                          /* svc #0 */
            uint64_t r = c->syscall ? c->syscall(c) : aoi_linux_syscall(c);
            cpage_va = 1;
            if (c->stop == AOI_RUN) c->x[0] = r;
            else if (c->stop == AOI_STOP_YIELD) { c->x[0] = r; c->pc = next; break; }
            else if (c->stop == AOI_STOP_NEWPC) { c->stop = AOI_RUN; continue; }
            else if (c->stop == AOI_STOP_RESTART) { c->stop = AOI_STOP_YIELD; break; }
        } else if ((insn & 0xffffffe0u) == 0xd53bd040u) { BODY(9)          /* mrs xN, tpidr_el0 */
            setX(c, insn & 31, c->tpidr);
        } else if ((insn & 0xffffffe0u) == 0xd53b00e0u) { BODY(10)          /* mrs xN, dczid_el0 */
            setX(c, insn & 31, 0x10);                              /* DZP: dc zva prohibited */
        } else if ((insn & 0xffffffe0u) == 0xd53b0020u) { BODY(11)          /* mrs xN, ctr_el0 */
            setX(c, insn & 31, 0x8444c004u);                       /* 64-byte I/D lines, like a Cortex-A */
        } else if ((insn & 0xffffffe0u) == 0xd53be000u) { BODY(12)          /* mrs xN, cntfrq_el0 */
            setX(c, insn & 31, AOI_CNTFRQ);
        } else if ((insn & 0xffffffe0u) == 0xd53be040u || (insn & 0xffffffe0u) == 0xd53be020u) { BODY(13) /* cntvct/cntpct */
            setX(c, insn & 31, aoi_cntvct());
        } else if ((insn & 0xffdfffe0u) == 0xd51b4200u) { BODY(14)          /* mrs/msr nzcv */
            if (insn & 0x200000u)
                setX(c, insn & 31, (uint64_t)(c->n << 31 | c->z << 30 | c->c << 29 | c->v << 28) & 0xf0000000u);
            else {
                uint64_t v = X(c, insn & 31);
                c->n = v >> 31 & 1; c->z = v >> 30 & 1; c->c = v >> 29 & 1; c->v = v >> 28 & 1;
            }
        } else if ((insn & 0xffdfffe0u) == 0xd51b4400u) { BODY(15)          /* mrs/msr fpcr */
            if (insn & 0x200000u) setX(c, insn & 31, c->fpcr); else c->fpcr = (uint32_t)X(c, insn & 31);
        } else if ((insn & 0xffdfffe0u) == 0xd51b4420u) { BODY(16)          /* mrs/msr fpsr */
            if (insn & 0x200000u) setX(c, insn & 31, c->fpsr); else c->fpsr = (uint32_t)X(c, insn & 31);
        } else if ((insn & 0xffffffe0u) == 0xd51bd040u) { BODY(17)          /* msr tpidr_el0, xN */
            c->tpidr = X(c, insn & 31);
        } else if (insn == 0xd503201fu || (insn & 0xfffff01fu) == 0xd503201fu) { BODY(18)
            /* nop / hint */
        } else if ((insn & 0xfffff09fu) == 0xd503309fu) { BODY(19)          /* dsb / dmb / isb: one thread, in order */
        } else if ((insn & 0xfffff0ffu) == 0xd503305fu) { BODY(20)          /* clrex */
            c->excl_valid = 0;
        } else if ((insn & 0xfffff000u) == 0xd50b7000u) { BODY(21)          /* dc / ic (EL0): no caches here */
            if (((insn >> 8) & 15) == 4 && ((insn >> 5) & 7) == 1) { /* dc zva: zero a 64-byte block */
                uint64_t a = X(c, insn & 31) & ~(uint64_t)63;
                int k;
                for (k = 0; k < 64 && c->stop == AOI_RUN; k += 8) wr(c, a + k, 0, 8);
            }
        /* ---- moves (wide immediate) ---- */
        } else dp_imm: if ((insn & 0x1f800000u) == 0x12800000u && ((insn >> 29) & 3) != 1) { BODY(22) /* movn/movz/movk */
            int is64 = insn >> 31, opc = (insn >> 29) & 3, sh = ((insn >> 21) & 3) * 16;
            uint64_t imm = (uint64_t)((insn >> 5) & 0xffff) << sh, r;
            int rd_ = insn & 31;
            if (opc == 0) r = ~imm;                                /* movn */
            else if (opc == 2) r = imm;                            /* movz */
            else r = (X(c, rd_) & ~((uint64_t)0xffff << sh)) | imm; /* movk */
            if (!is64) r &= 0xffffffffu;
            setX(c, rd_, r);
        /* ---- add/sub immediate ---- */
        } else if ((insn & 0x1f000000u) == 0x11000000u) { BODY(23)          /* add/sub imm (+ADDS/SUBS) */
            int is64 = insn >> 31, sub = (insn >> 30) & 1, setf = (insn >> 29) & 1;
            int sh = (insn >> 22) & 1; uint64_t imm = (insn >> 10) & 0xfff;
            int rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t a = (rn == 31) ? c->sp : c->x[rn], r;
            if (sh) imm <<= 12;
            r = addflags(c, a, sub ? ~imm : imm, sub ? 1 : 0, is64, setf);
            if (!is64) r &= 0xffffffffu;
            if (rd_ == 31 && !setf) c->sp = r; else setX(c, rd_, r);
        /* ---- logical immediate ---- */
        } else if ((insn & 0x1f800000u) == 0x12000000u) { BODY(24)          /* and/orr/eor/ands imm */
            int is64 = insn >> 31, opc = (insn >> 29) & 3;
            int nn = (insn >> 22) & 1, immr = (insn >> 16) & 0x3f, imms = (insn >> 10) & 0x3f;
            int rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t m, a = X(c, rn), r;
            if (!decode_bitmask(nn, immr, imms, is64, &m)) { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; break; }
            if (opc == 0 || opc == 3) r = a & m;
            else if (opc == 1) r = a | m;
            else r = a ^ m;
            if (!is64) r &= 0xffffffffu;
            if (opc == 3) { c->z = r == 0; c->n = (int)(r >> (is64 ? 63 : 31)) & 1; c->c = 0; c->v = 0; }
            if (rd_ == 31 && opc != 3) c->sp = r; else setX(c, rd_, r);
        /* ---- add/sub shifted register ---- */
        } else dp_reg: if ((insn & 0x1f200000u) == 0x0b000000u) { BODY(25)  /* add/sub/adds/subs reg */
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
        } else if ((insn & 0x1f000000u) == 0x0a000000u) { BODY(26)          /* and/orr/eor/ands/bic reg */
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
            if (opc == 3) { c->z = r == 0; c->n = (int)(r >> (is64 ? 63 : 31)) & 1; c->c = 0; c->v = 0; }
            setX(c, rd_, r);
        /* ---- SIMD&FP load/store (V bit set): q/d/s/h/b registers ---- */
        } else ldst: if ((insn & 0x3e000000u) == 0x2c000000u) { BODY(27)    /* stp/ldp s/d/q */
            int opc = insn >> 30, load = (insn >> 22) & 1, mode = (insn >> 23) & 3;
            int rt = insn & 31, rn = (insn >> 5) & 31, rt2 = (insn >> 10) & 31;
            int scale = 2 + opc, bytes = 1 << scale;
            int64_t off = (int64_t)sextn((insn >> 15) & 0x7f, 7) * bytes;
            uint64_t base = (rn == 31) ? c->sp : c->x[rn], a = (mode == 1) ? base : base + off;
            if (opc == 3) { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; break; }
            if (load) { vld(c, rt, a, bytes); vld(c, rt2, a + bytes, bytes); }
            else { vst(c, rt, a, bytes); vst(c, rt2, a + bytes, bytes); }
            if ((mode == 1 || mode == 3) && c->stop == AOI_RUN) { uint64_t nb = base + off; if (rn == 31) c->sp = nb; else c->x[rn] = nb; }
        } else if ((insn & 0x3f000000u) == 0x3d000000u) { BODY(28)          /* ldr/str b..q [Xn, #uimm] */
            int size = insn >> 30, opc = (insn >> 22) & 3, rn = (insn >> 5) & 31, rt = insn & 31;
            int bytes = (opc & 2) ? 16 : 1 << size;
            uint64_t a = ((rn == 31) ? c->sp : c->x[rn]) + ((uint64_t)((insn >> 10) & 0xfff) * (uint64_t)bytes);
            if (opc & 1) vld(c, rt, a, bytes); else vst(c, rt, a, bytes);
        } else if ((insn & 0x3f200000u) == 0x3c000000u) { BODY(29)          /* ldur/stur, pre/post b..q */
            int size = insn >> 30, opc = (insn >> 22) & 3, mode = (insn >> 10) & 3;
            int rn = (insn >> 5) & 31, rt = insn & 31, bytes = (opc & 2) ? 16 : 1 << size;
            int64_t off = (int64_t)sextn((insn >> 12) & 0x1ff, 9);
            uint64_t base = (rn == 31) ? c->sp : c->x[rn], a = (mode == 1) ? base : base + off;
            if (mode == 2) { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; break; }
            if (opc & 1) vld(c, rt, a, bytes); else vst(c, rt, a, bytes);
            if ((mode == 1 || mode == 3) && c->stop == AOI_RUN) { uint64_t nb = base + off; if (rn == 31) c->sp = nb; else c->x[rn] = nb; }
        } else if ((insn & 0x3f200c00u) == 0x3c200800u) { BODY(30)          /* ldr/str b..q [Xn, Xm{,ext}] */
            int size = insn >> 30, opc = (insn >> 22) & 3, rm = (insn >> 16) & 31;
            int option = (insn >> 13) & 7, S = (insn >> 12) & 1, rn = (insn >> 5) & 31, rt = insn & 31;
            int bytes = (opc & 2) ? 16 : 1 << size, sh = (opc & 2) ? 4 : size;
            uint64_t idx = X(c, rm), a;
            if (option == 2) idx = (uint32_t)idx; else if (option == 6) idx = sextn((uint32_t)idx, 32);
            a = ((rn == 31) ? c->sp : c->x[rn]) + (S ? idx << sh : idx);
            if (opc & 1) vld(c, rt, a, bytes); else vst(c, rt, a, bytes);
        } else if ((insn & 0x3f000000u) == 0x1c000000u) { BODY(31)          /* ldr s/d/q, literal */
            int opc = insn >> 30;
            vld(c, insn & 31, c->pc + (sextn((insn >> 5) & 0x7ffff, 19) << 2), 4 << opc);
        } else if ((insn & 0x3f000000u) == 0x18000000u) { BODY(32)          /* ldr w/x, ldrsw, prfm literal */
            int opc = insn >> 30;
            uint64_t a = c->pc + (sextn((insn >> 5) & 0x7ffff, 19) << 2);
            if (opc != 3) ild(c, insn & 31, a, opc == 1 ? 8 : 4, opc == 2 ? 2 : 1);
        } else if ((insn & 0xbfbf0000u) == 0x0c000000u || (insn & 0xbfa00000u) == 0x0c800000u) { BODY(33)
            /* ld1-ld4 / st1-st4 (multiple structures), optionally post-indexed */
            int q = (insn >> 30) & 1, load = (insn >> 22) & 1, post = (insn >> 23) & 1;
            int rm = (insn >> 16) & 31, opcode = (insn >> 12) & 0xf, size = (insn >> 10) & 3;
            int rn = (insn >> 5) & 31, rt = insn & 31, rpt, selem, esz = 1 << size, elems, r_, e, k;
            uint64_t base = (rn == 31) ? c->sp : c->x[rn], a = base;
            switch (opcode) {
            case 0x0: rpt = 1; selem = 4; break;  case 0x2: rpt = 4; selem = 1; break;
            case 0x4: rpt = 1; selem = 3; break;  case 0x6: rpt = 3; selem = 1; break;
            case 0x7: rpt = 1; selem = 1; break;  case 0x8: rpt = 1; selem = 2; break;
            case 0xa: rpt = 2; selem = 1; break;
            default: c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; goto ldst_done;
            }
            if (size == 3 && !q && selem > 1) { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; goto ldst_done; }
            elems = (q ? 16 : 8) / esz;
            if (load) for (r_ = 0; r_ < rpt * selem; r_++) { int t = (rt + r_) & 31; c->vreg[t][0] = 0; c->vreg[t][1] = 0; }
            if (selem == 1)                                        /* ld1/st1: whole registers, in order */
                for (r_ = 0; r_ < rpt && c->stop == AOI_RUN; r_++) {
                    int t = (rt + r_) & 31;
                    if (load) vld(c, t, a, q ? 16 : 8); else vst(c, t, a, q ? 16 : 8);
                    a += q ? 16 : 8;
                }
            else
            for (r_ = 0; r_ < rpt; r_++)
                for (e = 0; e < elems; e++)
                    for (k = 0; k < selem; k++) {
                        int t = (rt + r_ + k) & 31;
                        uint8_t *lane = (uint8_t *)c->vreg[t] + e * esz;   /* host is little-endian */
                        uint64_t v = 0;
                        if (load) {
                            v = rd(c, a, esz);
                            switch (esz) {
                            case 1: lane[0] = (uint8_t)v; break;
                            case 2: *(aoi_u16u *)lane = (uint16_t)v; break;
                            case 4: *(aoi_u32u *)lane = (uint32_t)v; break;
                            default: *(aoi_u64u *)lane = v; break;
                            }
                        } else {
                            switch (esz) {
                            case 1: v = lane[0]; break;
                            case 2: v = *(const aoi_u16u *)lane; break;
                            case 4: v = *(const aoi_u32u *)lane; break;
                            default: v = *(const aoi_u64u *)lane; break;
                            }
                            wr(c, a, v, esz);
                        }
                        a += (uint64_t)esz;
                    }
            if (post && c->stop == AOI_RUN) {
                uint64_t nb = base + (rm == 31 ? a - base : c->x[rm]);
                if (rn == 31) c->sp = nb; else c->x[rn] = nb;
            }
            if (0) { ldst_done: break; }
        /* ---- load/store: register offset (integer) ---- */
        } else if ((insn & 0x3b200c00u) == 0x38200800u) { BODY(34)          /* ldr/str [Xn, Xm{,ext}] */
            int size = insn >> 30, opc = (insn >> 22) & 3;
            int rm = (insn >> 16) & 31, option = (insn >> 13) & 7, S = (insn >> 12) & 1;
            int rn = (insn >> 5) & 31, rt = insn & 31, bytes = 1 << size;
            uint64_t idx = X(c, rm);
            uint64_t base = (rn == 31) ? c->sp : c->x[rn], a;
            if (option == 2) idx = (uint32_t)idx;                   /* UXTW */
            else if (option == 6) idx = sextn((uint32_t)idx, 32);   /* SXTW */
            if (S) idx <<= size;
            a = base + idx;
            if (opc == 0) wr(c, a, X(c, rt), bytes);
            else if (size == 3 && opc == 2) { /* prfm: no-op */ }
            else ild(c, rt, a, bytes, opc);
        /* ---- load/store: unsigned offset (32/64-bit integer) ---- */
        } else if ((insn & 0x3b000000u) == 0x39000000u) { BODY(35)          /* ldr/str [Xn, #imm] unsigned */
            int size = insn >> 30, opc = (insn >> 22) & 3;
            int rn = (insn >> 5) & 31, rt = insn & 31;
            uint64_t base = (rn == 31) ? c->sp : c->x[rn];
            uint64_t off = (uint64_t)((insn >> 10) & 0xfff) << size, a = base + off;
            int bytes = 1 << size;
            if (opc == 0) wr(c, a, X(c, rt), bytes);               /* store */
            else if (size == 3 && opc == 2) { /* prfm: no-op */ }
            else ild(c, rt, a, bytes, opc);
        /* ---- load/store: signed/unscaled/pre/post (9-bit imm) ---- */
        } else if ((insn & 0x3b200000u) == 0x38000000u) { BODY(36)          /* ldur/stur, pre/post index */
            int size = insn >> 30, opc = (insn >> 22) & 3, mode = (insn >> 10) & 3;
            int rn = (insn >> 5) & 31, rt = insn & 31, bytes = 1 << size;
            int64_t off = (int64_t)sextn((insn >> 12) & 0x1ff, 9);
            uint64_t base = (rn == 31) ? c->sp : c->x[rn];
            uint64_t a = (mode == 1) ? base : base + off;          /* post-index uses base */
            if (mode == 3 || mode == 0) a = base + off;            /* pre-index / unscaled */
            if (mode == 1) a = base;                               /* post-index */
            if (opc == 0) wr(c, a, X(c, rt), bytes);
            else if (size == 3 && opc == 2) { /* prfum: no-op */ }
            else ild(c, rt, a, bytes, opc);
            if ((mode == 1 || mode == 3) && c->stop == AOI_RUN) { uint64_t nb = base + off; if (rn == 31) c->sp = nb; else c->x[rn] = nb; }
        /* ---- load/store pair ---- */
        } else if ((insn & 0x3a000000u) == 0x28000000u && ((insn >> 26) & 1) == 0) { BODY(37) /* stp/ldp */
            int is64 = (insn >> 31) & 1, load = (insn >> 22) & 1, mode = (insn >> 23) & 3;
            int rt = insn & 31, rn = (insn >> 5) & 31, rt2 = (insn >> 10) & 31;
            int scale = is64 ? 3 : 2, bytes = 1 << scale;
            int64_t off = (int64_t)sextn((insn >> 15) & 0x7f, 7) << scale;
            uint64_t base = (rn == 31) ? c->sp : c->x[rn];
            uint64_t a = (mode == 1) ? base : base + off;
            int sw = (insn >> 30) == 1;                            /* ldpsw */
            if (load) { uint64_t v1 = rd(c, a, bytes), v2 = rd(c, a + bytes, bytes);
                        if (sw) { v1 = sextn(v1, 32); v2 = sextn(v2, 32); }
                        if (c->stop == AOI_RUN) { setX(c, rt, v1); setX(c, rt2, v2); } }
            else { wr(c, a, X(c, rt), bytes); wr(c, a + bytes, X(c, rt2), bytes); }
            if ((mode == 1 || mode == 3) && c->stop == AOI_RUN) { uint64_t nb = base + off; if (rn == 31) c->sp = nb; else c->x[rn] = nb; }
        /* ---- conditional select (csel/csinc/csinv/csneg, incl cset/csetm) ---- */
        } else if ((insn & 0x1fe00000u) == 0x1a800000u) { BODY(38)
            int is64 = insn >> 31, op = (insn >> 30) & 1, o2 = (insn >> 10) & 3;
            int rm = (insn >> 16) & 31, cond = (insn >> 12) & 0xf, rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t a = X(c, rn), b = X(c, rm), r;
            if (cond_holds(c, (unsigned)cond)) r = a;
            else if (!op) r = o2 ? b + 1 : b;                      /* csel / csinc */
            else r = o2 ? (uint64_t)(-(int64_t)b) : ~b;            /* csneg / csinv */
            if (!is64) r &= 0xffffffffu;
            setX(c, rd_, r);
        /* ---- data processing (1 source): rbit/rev/clz/cls ---- */
        } else if ((insn & 0x5fe00000u) == 0x5ac00000u && ((insn >> 16) & 0x1f) == 0) { BODY(39)
            int is64 = insn >> 31, op = (insn >> 10) & 0x3f;
            int rn = (insn >> 5) & 31, rd_ = insn & 31, width = is64 ? 64 : 32;
            uint64_t a = X(c, rn), r = 0; int i;
            if (!is64) a &= 0xffffffffu;
            switch (op) {
            case 0: for (i = 0; i < width; i++) if (a >> i & 1) r |= (uint64_t)1 << (width - 1 - i); break; /* rbit */
            case 1: for (i = 0; i < width; i += 8) r |= ((a >> i) & 0xff) << (i ^ 8); break;                /* rev16 */
            case 2: for (i = 0; i < width; i += 8) r |= ((a >> i) & 0xff) << ((i & ~31) + 24 - (i & 31)); break; /* rev32 / rev w */
            case 3: if (is64) { for (i = 0; i < 64; i += 8) r |= ((a >> i) & 0xff) << (56 - i); }
                    else { for (i = 0; i < 32; i += 8) r |= ((a >> i) & 0xff) << (24 - i); } break;         /* rev */
            case 4: { int z = 0; while (z < width && !((a >> (width - 1 - z)) & 1)) z++; r = z; } break;    /* clz */
            case 5: { int cnt = 0, msb = (a >> (width - 1)) & 1, k;
                      for (k = width - 2; k >= 0; k--) { if (((a >> k) & 1) == (uint64_t)msb) cnt++; else break; } r = cnt; } break; /* cls */
            default: c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; goto done1;
            }
            if (!is64) r &= 0xffffffffu;
            setX(c, rd_, r);
            if (0) { done1: break; }
        /* ---- add/sub with carry: adc/adcs/sbc/sbcs ---- */
        } else if ((insn & 0x1fe0fc00u) == 0x1a000000u) { BODY(40)
            int is64 = insn >> 31, sub = (insn >> 30) & 1, setf = (insn >> 29) & 1;
            int rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t a = X(c, rn), b = X(c, rm), r;
            r = addflags(c, a, sub ? ~b : b, c->c, is64, setf);
            if (!is64) r &= 0xffffffffu;
            setX(c, rd_, r);
        /* ---- data processing (3 source): madd/msub ---- */
        } else if ((insn & 0x1f000000u) == 0x1b000000u) { BODY(41)
            int is64 = insn >> 31, o0 = (insn >> 15) & 1, op31 = (insn >> 21) & 7;
            int rm = (insn >> 16) & 31, ra = (insn >> 10) & 31, rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t n_ = X(c, rn), m = X(c, rm), acc = X(c, ra), r;
            if (op31 == 0) {                           /* madd/msub */
                if (!is64) { n_ = (uint32_t)n_; m = (uint32_t)m; acc = (uint32_t)acc; }
                r = o0 ? acc - n_ * m : acc + n_ * m;
            } else if (op31 == 1) {                    /* smaddl/smsubl (32x32+64, signed) */
                int64_t prod = (int64_t)(int32_t)n_ * (int64_t)(int32_t)m;
                r = o0 ? acc - (uint64_t)prod : acc + (uint64_t)prod;
            } else if (op31 == 5) {                    /* umaddl/umsubl (32x32+64, unsigned) */
                uint64_t prod = (uint64_t)(uint32_t)n_ * (uint32_t)m;
                r = o0 ? acc - prod : acc + prod;
            } else if (op31 == 6) {                    /* umulh (U=1) */
                r = (uint64_t)(((unsigned __int128)n_ * m) >> 64);
            } else if (op31 == 2) {                    /* smulh */
                r = (uint64_t)(((__int128)(int64_t)n_ * (int64_t)m) >> 64);
            } else { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; break; }
            if (!is64) r &= 0xffffffffu;
            setX(c, rd_, r);
        /* ---- data processing (2 source): udiv/sdiv/lslv/lsrv/asrv/rorv ---- */
        } else if ((insn & 0x5fe00000u) == 0x1ac00000u) { BODY(42)
            int is64 = insn >> 31, op = (insn >> 10) & 0x3f;
            int rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t a = X(c, rn), b = X(c, rm), r; unsigned sh;
            if (!is64) { a = (uint32_t)a; b = (uint32_t)b; }
            switch (op) {
            case 2:  r = b == 0 ? 0 : a / b; break;                          /* udiv */
            case 3:  if (b == 0) r = 0;                                      /* sdiv */
                     else if (is64 ? b == ~0ULL : b == 0xffffffffu) r = 0 - a;   /* MIN / -1 wraps, no host SIGFPE */
                     else r = is64 ? (uint64_t)((int64_t)a / (int64_t)b) : (uint32_t)((int32_t)a / (int32_t)b);
                     break;
            case 8:  sh = b & (is64 ? 63 : 31); r = a << sh; break;          /* lslv */
            case 9:  sh = b & (is64 ? 63 : 31); r = a >> sh; break;          /* lsrv */
            case 10: sh = b & (is64 ? 63 : 31);
                     r = is64 ? (uint64_t)((int64_t)a >> sh) : (uint32_t)((int32_t)a >> sh); break; /* asrv */
            case 11: sh = b & (is64 ? 63 : 31);
                     r = is64 ? (a >> sh) | (sh ? a << (64 - sh) : 0)
                              : ((uint32_t)a >> sh) | (sh ? (uint32_t)a << (32 - sh) : 0); break; /* rorv */
            case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: { /* crc32[c]{b,h,w,x} */
                int sz = op & 3, k;
                uint32_t crc = (uint32_t)a, poly = op & 4 ? 0x82f63b78u : 0xedb88320u;
                if ((sz == 3) != is64) { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; goto done; }
                for (k = 0; k < (8 << sz); k++) {
                    crc ^= (uint32_t)(b >> k) & 1;
                    crc = crc & 1 ? (crc >> 1) ^ poly : crc >> 1;
                }
                r = crc;
                is64 = 0;
                break;
            }
            default: c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; goto done;
            }
            if (!is64) r &= 0xffffffffu;
            setX(c, rd_, r);
            if (0) { done: break; }
        /* ---- bitfield: sbfm/bfm/ubfm (incl lsl/lsr/asr/sxt/uxt imm) ---- */
        } else if ((insn & 0x1f800000u) == 0x13000000u) { BODY(43)
            /* ARM ARM: bot = ROR(src, R) & wmask; top = dst (bfm), sign (sbfm) or 0 (ubfm);
             * result = (top & ~tmask) | (bot & tmask) */
            int is64 = insn >> 31, opc = (insn >> 29) & 3;
            unsigned R = (insn >> 16) & 0x3f, S = (insn >> 10) & 0x3f, width = is64 ? 64 : 32;
            int rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t mask = is64 ? ~0ULL : 0xffffffffu, src = X(c, rn) & mask, dst = X(c, rd_) & mask;
            uint64_t wmask, tmask, rot, bot, top, r;
            if (opc == 3 || R >= width || S >= width) { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; break; }
            bitfield_masks(R, S, width, &wmask, &tmask);
            rot = R ? ((src >> R) | (src << (width - R))) & mask : src;
            if (opc == 1) { bot = (dst & ~wmask) | (rot & wmask); top = dst; }
            else { bot = rot & wmask; top = opc == 0 && (src >> S & 1) ? mask : 0; }
            r = ((top & ~tmask) | (bot & tmask)) & mask;
            setX(c, rd_, r);
        /* ---- extr (and ror immediate) ---- */
        } else if ((insn & 0x7fa00000u) == 0x13800000u) { BODY(44)
            int is64 = insn >> 31, lsb = (insn >> 10) & 0x3f;
            int rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t hi = X(c, rn), lo = X(c, rm), r;
            if (is64) r = lsb ? (lo >> lsb) | (hi << (64 - lsb)) : lo;
            else { lo = (uint32_t)lo; hi = (uint32_t)hi; r = lsb ? ((lo >> lsb) | (hi << (32 - lsb))) & 0xffffffffu : lo; }
            setX(c, rd_, r);
        /* ---- conditional compare: ccmp/ccmn (register and immediate) ---- */
        } else if ((insn & 0x3fe00410u) == 0x3a400000u) { BODY(45)
            int is64 = insn >> 31, sub = (insn >> 30) & 1, imm = (insn >> 11) & 1;
            int rm = (insn >> 16) & 31, cond = (insn >> 12) & 0xf, rn = (insn >> 5) & 31;
            if (cond_holds(c, (unsigned)cond)) {
                uint64_t a = X(c, rn), b = imm ? (uint64_t)rm : X(c, rm);
                addflags(c, a, sub ? ~b : b, sub ? 1 : 0, is64, 1);
            } else {
                unsigned f = insn & 0xf;
                c->n = f >> 3 & 1; c->z = f >> 2 & 1; c->c = f >> 1 & 1; c->v = f & 1;
            }
        /* ---- add/sub extended register (sp-capable) ---- */
        } else if ((insn & 0x1fe00000u) == 0x0b200000u) { BODY(46)
            int is64 = insn >> 31, sub = (insn >> 30) & 1, setf = (insn >> 29) & 1;
            int rm = (insn >> 16) & 31, option = (insn >> 13) & 7, sh = (insn >> 10) & 7;
            int rn = (insn >> 5) & 31, rd_ = insn & 31;
            uint64_t a = (rn == 31) ? c->sp : c->x[rn], b = X(c, rm), r;
            switch (option) {
            case 0: b = (uint8_t)b; break;  case 1: b = (uint16_t)b; break;  case 2: b = (uint32_t)b; break;
            case 4: b = sextn((uint8_t)b, 8); break;  case 5: b = sextn((uint16_t)b, 16); break;
            case 6: b = sextn((uint32_t)b, 32); break;  default: break;
            }
            if (sh > 4) { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; break; }
            b <<= sh;
            r = addflags(c, a, sub ? ~b : b, sub ? 1 : 0, is64, setf);
            if (!is64) r &= 0xffffffffu;
            if (rd_ == 31 && !setf) c->sp = r; else setX(c, rd_, r);
        /* ---- SIMD modified immediate: movi/mvni/orr/bic (vector), fmov (vector imm) ---- */
        } else if ((insn & 0x9ff80400u) == 0x0f000400u) { BODY(47)
            int q = (insn >> 30) & 1, op = (insn >> 29) & 1, cmode = (insn >> 12) & 0xf, rd_ = insn & 31;
            uint64_t imm8 = ((insn >> 16) & 7) << 5 | ((insn >> 5) & 0x1f), imm = 0;
            int i;
            if (insn >> 11 & 1) {                                /* o2: only fmov .4h/.8h (FP16) exists */
                uint64_t b6 = imm8 >> 6 & 1, h;
                if (cmode != 15 || op) { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; goto simdimm_done; }
                h = (imm8 >> 7) << 15 | ((b6 ^ 1) << 4 | b6 << 3 | b6 << 2 | (imm8 >> 4 & 3)) << 10 | (imm8 & 15) << 6;
                imm = h * 0x0001000100010001ULL;
                c->vreg[rd_][0] = imm; c->vreg[rd_][1] = q ? imm : 0;
            } else {
            switch (cmode >> 1) {
            case 0: case 1: case 2: case 3: imm = imm8 << (8 * (cmode >> 1)); imm |= imm << 32; break;
            case 4: case 5: imm = imm8 << (8 * ((cmode >> 1) & 1)); imm |= imm << 16; imm |= imm << 32; break;
            case 6: imm = (cmode & 1) ? (imm8 << 16) | 0xffff : (imm8 << 8) | 0xff; imm |= imm << 32; break;
            case 7:
                if (!(cmode & 1) && !op) { for (i = 0; i < 8; i++) imm |= imm8 << (8 * i); }   /* 8-bit */
                else if (!(cmode & 1) && op) { uint64_t t = (imm8 | imm8 << 28) & 0x0000000f0000000fULL;       /* bit i -> byte i */
              t = (t | t << 14) & 0x0003000300030003ULL; t = (t | t << 7) & 0x0101010101010101ULL; imm = t * 0xff; } /* 64-bit */
                else if (!op) { imm = vfp_imm32(imm8); imm |= imm << 32; }                            /* fmov .2s/.4s */
                else if (q) imm = vfp_imm64(imm8);                                                   /* fmov .2d */
                else { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; goto simdimm_done; }
                break;
            }
            if (cmode < 12 && (cmode & 1)) {                     /* orr / bic */
                uint64_t m = op ? ~imm : imm;
                if (op) { c->vreg[rd_][0] &= m; c->vreg[rd_][1] &= q ? m : 0; }
                else { c->vreg[rd_][0] |= m; c->vreg[rd_][1] = q ? c->vreg[rd_][1] | m : 0; }
                if (!q) c->vreg[rd_][1] = 0;
            } else {                                             /* movi / mvni */
                if (op && cmode < 14) imm = ~imm;
                c->vreg[rd_][0] = imm; c->vreg[rd_][1] = q ? imm : 0;
            }
            }
            if (0) { simdimm_done: break; }
        /* ---- atomics: load-acquire/store-release, exclusives, cas ---- */
        } else if ((insn & 0x3f000000u) == 0x08000000u) { BODY(48)
            int size = insn >> 30, o2 = insn >> 23 & 1, L = insn >> 22 & 1, o1 = insn >> 21 & 1;
            int rs = insn >> 16 & 31, rt2 = insn >> 10 & 31, rn = insn >> 5 & 31, rt = insn & 31, bytes = 1 << size;
            uint64_t a = rn == 31 ? c->sp : c->x[rn], old;
            (void)rt2;
            if (o2 && !o1) {                                       /* ldar / stlr (and ldlar / stllr) */
                if (L) ild(c, rt, a, bytes, 1); else wr(c, a, X(c, rt), bytes);
            } else if (!o2 && !o1) {                               /* ldxr / ldaxr / stxr / stlxr */
                if (L) { ild(c, rt, a, bytes, 1); c->excl_addr = a; c->excl_valid = 1; }
                else {
                    int ok = c->excl_valid && c->excl_addr == a;
                    if (ok) wr(c, a, X(c, rt), bytes);
                    if (c->stop == AOI_RUN) setX(c, rs, ok ? 0 : 1);
                    c->excl_valid = 0;
                }
            } else if (!o2 && o1 && insn >> 31) {                  /* ldxp / ldaxp / stxp / stlxp */
                int eb = insn >> 30 & 1 ? 8 : 4;
                if (L) {
                    uint64_t v1 = rd(c, a, eb), v2 = rd(c, a + (uint64_t)eb, eb);
                    if (c->stop == AOI_RUN) { setX(c, rt, v1); setX(c, rt2, v2); c->excl_addr = a; c->excl_valid = 1; }
                } else {
                    int ok = c->excl_valid && c->excl_addr == a;
                    if (ok) { wr(c, a, X(c, rt), eb); wr(c, a + (uint64_t)eb, X(c, rt2), eb); }
                    if (c->stop == AOI_RUN) setX(c, rs, ok ? 0 : 1);
                    c->excl_valid = 0;
                }
            } else if (!o2 && o1 && !(insn >> 31) && rt2 == 31 && !(rs & 1) && !(rt & 1)) {   /* casp{a,l,al} */
                int eb = insn >> 30 & 1 ? 8 : 4;
                uint64_t m = eb == 8 ? ~0ULL : 0xffffffffu, o1v = rd(c, a, eb), o2v = rd(c, a + (uint64_t)eb, eb);
                if (c->stop == AOI_RUN) {
                    if (o1v == (X(c, rs) & m) && o2v == (X(c, rs + 1) & m)) {
                        wr(c, a, X(c, rt), eb);
                        wr(c, a + (uint64_t)eb, X(c, rt + 1), eb);
                    }
                    if (c->stop == AOI_RUN) { setX(c, rs, o1v); setX(c, rs + 1, o2v); }
                }
            } else if (o2 && o1 && rt2 == 31) {                    /* cas{a,l,al}{b,h} */
                uint64_t m = bytes == 8 ? ~0ULL : ((uint64_t)1 << (8 * bytes)) - 1;
                old = rd(c, a, bytes);
                if (c->stop == AOI_RUN) {
                    if (old == (X(c, rs) & m)) wr(c, a, X(c, rt), bytes);
                    if (c->stop == AOI_RUN) setX(c, rs, old);
                }
            } else { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; break; }
        } else if ((insn & 0x3f20fc00u) == 0x3820c000u && ((insn >> 16) & 31) == 31 && !(insn >> 26 & 1)) { BODY(49)
            /* ldapr / ldaprb / ldaprh (RCpc load-acquire): a plain load here */
            int bytes = 1 << (insn >> 30), rn = insn >> 5 & 31;
            uint64_t v = rd(c, rn == 31 ? c->sp : c->x[rn], bytes);
            if (c->stop == AOI_RUN) setX(c, insn & 31, v);
        } else if ((insn & 0x3f200c00u) == 0x38200000u && !(insn >> 26 & 1)) { BODY(50)
            /* LSE: ldadd/ldclr/ldeor/ldset/ld[su]max/ld[su]min, swp (single-threaded: plain RMW) */
            int size = insn >> 30, o3 = insn >> 15 & 1, opc = insn >> 12 & 7;
            int rs = insn >> 16 & 31, rn = insn >> 5 & 31, rt = insn & 31, bytes = 1 << size;
            uint64_t a = rn == 31 ? c->sp : c->x[rn], m = bytes == 8 ? ~0ULL : ((uint64_t)1 << (8 * bytes)) - 1;
            uint64_t old = rd(c, a, bytes), v = X(c, rs) & m, nv;
            int64_t so = (int64_t)sextn(old, 8 * bytes), sv = (int64_t)sextn(v, 8 * bytes);
            if (c->stop != AOI_RUN) break;
            if (o3) { if (opc) { c->stop = AOI_STOP_UNDEF; c->fault_insn = insn; break; } nv = v; }
            else switch (opc) {
            case 0: nv = old + v; break;        case 1: nv = old & ~v; break;
            case 2: nv = old ^ v; break;        case 3: nv = old | v; break;
            case 4: nv = so > sv ? old : v; break;  case 5: nv = so < sv ? old : v; break;
            case 6: nv = old > v ? old : v; break;  default: nv = old < v ? old : v; break;
            }
            wr(c, a, nv, bytes);
            if (c->stop == AOI_RUN) setX(c, rt, old);
        /* ---- pc-relative ---- */
        } else if ((insn & 0x9f000000u) == 0x90000000u) { BODY(51)          /* adrp */
            uint64_t imm = (sextn((((insn >> 5) & 0x7ffff) << 2) | ((insn >> 29) & 3), 21)) << 12;
            setX(c, insn & 31, (c->pc & ~(uint64_t)0xfff) + imm);
        } else if ((insn & 0x9f000000u) == 0x10000000u) { BODY(52)          /* adr */
            uint64_t imm = sextn((((insn >> 5) & 0x7ffff) << 2) | ((insn >> 29) & 3), 21);
            setX(c, insn & 31, c->pc + imm);
        } else if (aoi_simd_step(c, insn)) {
            if (c->stop != AOI_RUN) break;
        } else {
            c->stop = AOI_STOP_UNDEF;
            c->fault_insn = insn;
            break;
        }
        if (c->stop == AOI_RUN) c->pc = next;
        if (trace) trace(c, 1);
    }
    return c->stop;
}

enum aoi_stop aoi_call(struct aoi_cpu *c, uint64_t fn, const uint64_t *args, int nargs,
                       uint64_t max_steps)
{
    int i;
    for (i = 0; i < nargs && i < 8; i++) c->x[i] = args[i];
    c->x[30] = c->thunk_base;       /* returning lands on slot 0 */
    c->pc = fn;
    return aoi_cpu_run(c, max_steps ? c->steps + max_steps : 0);
}
