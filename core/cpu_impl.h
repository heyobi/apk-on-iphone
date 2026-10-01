/* Helpers shared by the interpreter's decoders (core/cpu.c, core/simd.c). Internal. */
#ifndef AOI_CPU_IMPL_H
#define AOI_CPU_IMPL_H

#include "cpu.h"
#include "vm.h"

#include <string.h>

/* Host pointer for a guest access needing `need` (AOI_PROT_*); NULL = fault, or
 * (sparse space only) an access that straddles two 2 MiB chunks, which rd/wr
 * then do byte-wise. The common case (inside one page) is inlined. */
#define AOI_TBI(a) ((a) & 0x00ffffffffffffffULL)
static inline uint8_t *gptr(struct aoi_cpu *c, uint64_t a, int len, int need)
{
    struct aoi_vm *vm = c->mem->vm;
    if (vm) {
        /* Top Byte Ignore: Linux runs EL0 with TBI0 on, so data accesses ignore
         * bits 56-63 (Android tags heap pointers there). */
        if (need != AOI_PROT_X) a = AOI_TBI(a);
        if (a + (uint64_t)len <= vm->size && (a ^ (a + (uint64_t)len - 1)) < AOI_VM_PAGE) {
            uint8_t f = vm->prot[a / AOI_VM_PAGE];
            return f && (f & need) == need ? vm->chunk[a >> AOI_VM_CHUNK_SHIFT] + (a & (AOI_VM_CHUNK - 1)) : NULL;
        }
        return aoi_vm_ptr(vm, a, (uint64_t)len, need);
    }
    return aoi_mem_ptr(c->mem, a, (uint64_t)len);
}

static inline uint64_t rd(struct aoi_cpu *c, uint64_t a, int len)
{
    uint8_t *p = gptr(c, a, len, AOI_PROT_R), tmp[8];
    uint64_t v = 0;
    int i;
    if (!p) {
        if (c->mem->vm && aoi_vm_read(c->mem->vm, AOI_TBI(a), tmp, (uint64_t)len, AOI_PROT_R)) p = tmp;
        else { c->stop = AOI_STOP_FAULT; c->fault_addr = a; return 0; }
    }
    for (i = 0; i < len; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

#ifdef AOI_DEBUG
extern uint32_t aoi_watch_val;
void aoi_watch_hit(struct aoi_cpu *c, uint64_t a, uint64_t v, int len);
#endif
static inline void wr(struct aoi_cpu *c, uint64_t a, uint64_t v, int len)
{
    uint8_t *p = gptr(c, a, len, AOI_PROT_W), tmp[8];
    int i;
#ifdef AOI_DEBUG
    if (aoi_watch_val && len >= 4 && ((uint32_t)v == aoi_watch_val || (len == 8 && (uint32_t)(v >> 32) == aoi_watch_val)))
        aoi_watch_hit(c, a, v, len);
#endif
    if (c->trace && c->nwlog < 4) { c->wlog_addr[c->nwlog] = a; c->wlog_len[c->nwlog++] = len; }
    if (!p) {
        for (i = 0; i < len; i++) tmp[i] = (uint8_t)(v >> (8 * i));
        if (!c->mem->vm || !aoi_vm_write(c->mem->vm, AOI_TBI(a), tmp, (uint64_t)len, AOI_PROT_W)) {
            if (c->trace && c->nwlog) c->nwlog--;
            c->stop = AOI_STOP_FAULT; c->fault_addr = a;
        }
        return;
    }
    for (i = 0; i < len; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static inline uint32_t fetch(struct aoi_cpu *c, uint64_t a)
{
    uint8_t *p = gptr(c, a, 4, AOI_PROT_X);
    uint32_t v;
    if (!p) { c->stop = AOI_STOP_FAULT; c->fault_addr = a; return 0; }
    memcpy(&v, p, 4);
    return v;
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
