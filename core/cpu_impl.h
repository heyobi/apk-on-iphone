/* Helpers shared by the interpreter's decoders (core/cpu.c, core/simd.c). Internal. */
#ifndef AOI_CPU_IMPL_H
#define AOI_CPU_IMPL_H

#include "cpu.h"

#include <string.h>

static inline uint64_t rd(struct aoi_cpu *c, uint64_t a, int len)
{
    uint8_t *p = aoi_mem_ptr(c->mem, a, (uint64_t)len);
    uint64_t v = 0;
    int i;
    if (!p) { c->stop = AOI_STOP_FAULT; c->fault_addr = a; return 0; }
    for (i = 0; i < len; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static inline void wr(struct aoi_cpu *c, uint64_t a, uint64_t v, int len)
{
    uint8_t *p = aoi_mem_ptr(c->mem, a, (uint64_t)len);
    int i;
    if (!p) { c->stop = AOI_STOP_FAULT; c->fault_addr = a; return; }
    if (c->trace && c->nwlog < 4) { c->wlog_addr[c->nwlog] = a; c->wlog_len[c->nwlog++] = len; }
    for (i = 0; i < len; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static inline uint64_t sextn(uint64_t v, int bits)
{
    uint64_t m = (uint64_t)1 << (bits - 1);
    return (v ^ m) - m;
}

/* SIMD&FP register load/store of 1, 2, 4, 8 or 16 bytes. A load zeroes the rest of Vt. */
static inline void vld(struct aoi_cpu *c, int t, uint64_t a, int bytes)
{
    uint64_t lo = rd(c, a, bytes > 8 ? 8 : bytes), hi = bytes > 8 ? rd(c, a + 8, 8) : 0;
    if (c->stop != AOI_RUN) return;
    c->vreg[t][0] = lo; c->vreg[t][1] = hi;
}
static inline void vst(struct aoi_cpu *c, int t, uint64_t a, int bytes)
{
    wr(c, a, c->vreg[t][0], bytes > 8 ? 8 : bytes);
    if (bytes > 8) wr(c, a + 8, c->vreg[t][1], 8);
}

/* X registers: index 31 reads as zero (XZR) except where the encoding means SP. */
static inline uint64_t X(struct aoi_cpu *c, int i) { return i == 31 ? 0 : c->x[i]; }
static inline void setX(struct aoi_cpu *c, int i, uint64_t v) { if (i != 31) c->x[i] = v; }

static inline int cond_holds(struct aoi_cpu *c, unsigned cond)
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

/* core/simd.c: SIMD&FP data processing. Returns 1 if it decoded insn (it may
 * still stop the CPU), 0 if the encoding is not one of its classes. */
int aoi_simd_step(struct aoi_cpu *c, uint32_t insn);

#endif
