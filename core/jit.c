/* The accelerator (WebAssembly builds only): hot guest basic blocks become WebAssembly
 * functions, compiled by the JavaScript engine's JIT (in a web view: the only JIT iOS
 * gives ordinary apps) and called from the interpreter through the function table.
 *
 * aoi_cpu_run asks aoi_jit_find at every block start (after a taken branch); a block
 * seen JIT_HOT times is translated: a run of integer instructions (data processing,
 * loads and stores, branches) up to the first branch or the first instruction outside
 * that subset, which ends the block early (the interpreter does it). The function gets
 * the CPU state's address, keeps the registers it uses in locals, and returns how many
 * guest instructions it ran with cpu->pc at the next one. Guest memory accesses take an
 * inline fast path (in-range, one page, permission byte, host chunk); anything else goes
 * through rd()/wr(), and a fault stops the block before the faulting instruction with
 * cpu->stop set, exactly where the interpreter would stop.
 *
 * Generated code depends only on the instruction words and their addresses, and is
 * keyed by (address space, pc); every unmap, protect and address-space change flushes
 * the cache (aoi_jit_flush), so code that is replaced is never run stale. */
#include "jit.h"

#ifdef AOI_JIT
#include "cpu_impl.h"
#include <emscripten.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int aoi_jit_hot = 200000;             /* block entries into a page before it is translated (AOI_JIT_HOT): JavaScriptCore
                                         compiles a page in ~11 ms, so only pages that run long pay off */
#define JIT_HOT aoi_jit_hot
#define JIT_MISS 16                  /* new distinct starts seen before the page is translated again */
#define JIT_BUDGET 200000         /* guest instructions per call before going back to the interpreter */
#define JIT_SLOTS (1u << 14)

/* one per (address space, guest page): the page's function and the block starts seen in it */
struct jit_slot { uint64_t base; const void *vm; int32_t fn; int32_t count; int32_t need, redo; uint32_t roots[AOI_VM_PAGE / 4 / 32]; };
static struct jit_slot slots[JIT_SLOTS];
static unsigned slot_used;
uint64_t aoi_jit_blocks, aoi_jit_failed, aoi_jit_runs, aoi_jit_insns;
/* instructions that ended a block because the translator does not do them: by encoding
 * class (top 10 bits), to see what to add next (AOI_JIT_STATS) */
static uint32_t refused_key[64], refused_n[64];
static void note_refused(uint32_t w)
{
    uint32_t k = w >> 22;
    int i, low = 0;
    for (i = 0; i < 64; i++) {
        if (refused_n[i] && refused_key[i] == k) { refused_n[i]++; return; }
        if (refused_n[i] < refused_n[low]) low = i;
    }
    refused_key[low] = k; refused_n[low] = 1;
}
void aoi_jit_report(FILE *f)
{
    int i, j, best;
    uint32_t used[64] = { 0 };
    fprintf(f, "[jit] %llu of the instructions ran translated; refused (class: top 10 bits, count):",
            (unsigned long long)aoi_jit_insns);
    for (j = 0; j < 12; j++) {
        best = -1;
        for (i = 0; i < 64; i++) if (refused_n[i] && !used[i] && (best < 0 || refused_n[i] > refused_n[best])) best = i;
        if (best < 0) break;
        used[best] = 1;
        fprintf(f, " %08x:%u", refused_key[best] << 22, refused_n[best]);
    }
    fprintf(f, "\n");
}
int aoi_jit_enabled = 1;

EM_JS(int, jit_install, (const uint8_t *code, int len), {
    var t0 = performance.now();
    Module.jitBytes = (Module.jitBytes || 0) + len;
    try {
        var tbl = typeof wasmTable != 'undefined' ? wasmTable : wasmExports['__indirect_function_table'];
        var mem = typeof wasmMemory != 'undefined' ? wasmMemory : wasmExports['memory'];
        var mod = new WebAssembly.Module(HEAPU8.slice(code, code + len));
        var inst = new WebAssembly.Instance(mod, { env: { mem: mem, tbl: tbl } });
        var idx = tbl.length;
        tbl.grow(1);
        tbl.set(idx, inst.exports.f);
        Module.jitMs = (Module.jitMs || 0) + (performance.now() - t0);
        return idx;
    } catch (e) {
        if (!Module.jitErrors) Module.jitErrors = 0;
        if (Module.jitErrors++ < 3) err('[jit] ' + e);
        return 0;
    }
});

/* Compiled regions by what they were made from (the page's bytes, the entry): a new process
 * running the same library code at the same address (every bash, dpkg, apt started from a
 * shell) reuses the function instead of compiling it again. */
#define MOD_SLOTS 8192
struct mod_slot { uint64_t base, hash; int32_t fn; };
static struct mod_slot mods[MOD_SLOTS];
static uint64_t page_hash(const uint8_t *p)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;
    for (i = 0; i < AOI_VM_PAGE; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001b3ULL; }
    return h;
}
uint64_t aoi_jit_reused;

EM_JS(int, aoi_jit_compile_ms, (void), { return Math.round(Module.jitMs || 0); });
EM_JS(int, aoi_jit_kbytes, (void), { return Math.round((Module.jitBytes || 0) / 1024); });

void aoi_jit_flush(void)
{
    if (slot_used) memset(slots, 0, sizeof slots);
    slot_used = 0;
}

/* One instruction through the interpreter, for what the translator does not do itself
 * (SIMD, floating point, atomics, system registers ...): registers already live in the
 * CPU structure, so nothing needs syncing. */
static int jit_stepping;
static void jit_step(struct aoi_cpu *c, uint64_t pc)
{
    c->pc = pc;
    jit_stepping = 1;
    aoi_cpu_run(c, c->steps + 1);
    jit_stepping = 0;
}

/* slow paths, called from generated code through the table */
static uint64_t jit_ld(struct aoi_cpu *c, uint64_t a, int len) { return rd(c, a, len); }
static void jit_st(struct aoi_cpu *c, uint64_t a, uint64_t v, int len) { wr(c, a, v, len); }

/* ---------- WebAssembly binary emitter ---------- */
struct buf { uint8_t *p; size_t n, cap; };
static void put(struct buf *b, uint8_t v)
{
    if (b->n == b->cap) { b->cap = b->cap ? 2 * b->cap : 4096; b->p = realloc(b->p, b->cap); }
    b->p[b->n++] = v;
}
static void putu(struct buf *b, uint64_t v) { do { uint8_t c = v & 0x7f; v >>= 7; put(b, c | (v ? 0x80 : 0)); } while (v); }
static void puts_(struct buf *b, int64_t v)
{
    for (;;) {
        uint8_t c = v & 0x7f;
        v >>= 7;
        if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) { put(b, c); return; }
        put(b, c | 0x80);
    }
}
static void putbuf(struct buf *b, const struct buf *s) { size_t i; for (i = 0; i < s->n; i++) put(b, s->p[i]); }
static void putname(struct buf *b, const char *s) { putu(b, strlen(s)); while (*s) put(b, (uint8_t)*s++); }

enum {
    UNREACH = 0x00, BLOCK = 0x02, IF = 0x04, ELSE = 0x05, END = 0x0b, BR = 0x0c, BR_IF = 0x0d, RETURN = 0x0f,
    CALL_IND = 0x11, DROP = 0x1a, SELECT = 0x1b, LGET = 0x20, LSET = 0x21, LTEE = 0x22,
    I32_LOAD = 0x28, I64_LOAD = 0x29, I32_LOAD8U = 0x2d, I64_LOAD8S = 0x30, I64_LOAD8U = 0x31, I64_LOAD16S = 0x32,
    I64_LOAD16U = 0x33, I64_LOAD32S = 0x34, I64_LOAD32U = 0x35, I32_STORE = 0x36, I64_STORE = 0x37,
    I64_STORE8 = 0x3c, I64_STORE16 = 0x3d, I64_STORE32 = 0x3e,
    I32_CONST = 0x41, I64_CONST = 0x42,
    I32_EQZ = 0x45, I32_EQ = 0x46, I32_NE = 0x47, I32_LTS = 0x48, I32_LTU = 0x49, I32_GEU = 0x4f,
    I64_EQZ = 0x50, I64_EQ = 0x51, I64_NE = 0x52, I64_LTS = 0x53, I64_LTU = 0x54, I64_LEU = 0x58, I64_GEU = 0x5a,
    I32_CLZ = 0x67, I32_ADD = 0x6a, I32_SUB = 0x6b, I32_MUL = 0x6c, I32_DIVS = 0x6d, I32_DIVU = 0x6e,
    I32_AND = 0x71, I32_OR = 0x72, I32_XOR = 0x73, I32_SHL = 0x74, I32_SHRS = 0x75, I32_SHRU = 0x76, I32_ROTR = 0x78,
    I64_CLZ = 0x79, I64_ADD = 0x7c, I64_SUB = 0x7d, I64_MUL = 0x7e, I64_DIVS = 0x7f, I64_DIVU = 0x80,
    I64_AND = 0x83, I64_OR = 0x84, I64_XOR = 0x85, I64_SHL = 0x86, I64_SHRS = 0x87, I64_SHRU = 0x88, I64_ROTR = 0x8a,
    WRAP = 0xa7, EXT_S = 0xac, EXT_U = 0xad, I64_EXT8S = 0xc2, I64_EXT16S = 0xc3, I64_EXT32S = 0xc4,
    T_I32 = 0x7f, T_I64 = 0x7e, T_VOID = 0x40,
    F64_EQ = 0x61, F64_LT = 0x63, F64_GT = 0x64, F64_ABS = 0x99, F64_NEG = 0x9a, F64_SQRT = 0x9f,
    F64_ADD = 0xa0, F64_SUB = 0xa1, F64_MUL = 0xa2, F64_DIV = 0xa3,
    F64_CVT_I64S = 0xb9, F64_CVT_I64U = 0xba, I64_REINT_F64 = 0xbd, F64_REINT_I64 = 0xbf,
};
#define OFF(f) ((uint32_t)offsetof(struct aoi_cpu, f))

/* locals: 0 = cpu (param); then these */
enum { L_X0 = 1, L_SP = 32, L_N = 33, L_Z, L_C, L_V, L_TA, L_TB, L_TR, L_TC, L_A, L_UA, L_UB, L_UR, L_H,
       L_VSIZE, L_PROT, L_CHUNK, L_NPC, L_CNT, L_IDX, L_SV, L_UF, L_COUNT_ };
/* local groups for the declaration: i64 x0-x30, sp; i32 n z c v; i64 ta tb tr tc a; i32 ua ub ur h;
 * i64 vsize; i32 prot chunk; i64 npc; i32 cnt idx */

struct jit {
    struct buf b;                   /* the body */
    uint32_t rd_mask, wr_mask;      /* x0-x30 read / written (bit 31: sp) */
    int flags_rd, flags_wr, mem;    /* NZCV read before written / written; guest memory used */
    int flags_known;                /* NZCV set in this block already (no need to load) */
    int ninsn;                      /* instructions of the current block so far (counted at an exit) */
    uint64_t pc;                    /* the current instruction's address */
    /* the region: blocks of one guest page in one function, a dispatch loop between them */
    uint64_t base;                  /* the page */
    const uint8_t *start;           /* per instruction slot: a block starts there */
    int d_out;                      /* br depth from block-level code to the function's exit block */
};

static void op(struct jit *j, uint8_t o) { put(&j->b, o); }
static void lget(struct jit *j, int l) { put(&j->b, LGET); putu(&j->b, (unsigned)l); }
static void lset(struct jit *j, int l) { put(&j->b, LSET); putu(&j->b, (unsigned)l); }
static void ltee(struct jit *j, int l) { put(&j->b, LTEE); putu(&j->b, (unsigned)l); }
static void i32c(struct jit *j, int32_t v) { put(&j->b, I32_CONST); puts_(&j->b, v); }
static void i64c(struct jit *j, int64_t v) { put(&j->b, I64_CONST); puts_(&j->b, v); }
static void mem_op(struct jit *j, uint8_t o, uint32_t off) { put(&j->b, o); putu(&j->b, 0); putu(&j->b, off); }

/* registers: r 31 is sp where sp_ok, else the zero register */
static void reg_get(struct jit *j, int r, int sp_ok)
{
    if (r == 31 && !sp_ok) { i64c(j, 0); return; }
    lget(j, 0); mem_op(j, I64_LOAD, r == 31 ? OFF(sp) : OFF(x) + 8u * (unsigned)r);
}
static void reg_get32(struct jit *j, int r, int sp_ok)
{
    if (r == 31 && !sp_ok) { i32c(j, 0); return; }
    reg_get(j, r, sp_ok); op(j, WRAP);
}
/* value (i64, or i32 if w32: zero-extended) on the stack -> register */
static void reg_set(struct jit *j, int r, int sp_ok, int w32)
{
    if (w32) op(j, EXT_U);
    if (r == 31 && !sp_ok) { op(j, DROP); return; }
    lset(j, L_SV); lget(j, 0); lget(j, L_SV);
    mem_op(j, I64_STORE, r == 31 ? OFF(sp) : OFF(x) + 8u * (unsigned)r);
}
/* NZCV in the CPU structure (ints) */
static uint32_t flag_off(int l) { return l == L_N ? OFF(n) : l == L_Z ? OFF(z) : l == L_C ? OFF(c) : OFF(v); }
static void fget(struct jit *j, int l) { lget(j, 0); mem_op(j, I32_LOAD, flag_off(l)); }
static void fset(struct jit *j, int l) { lset(j, L_UF); lget(j, 0); lget(j, L_UF); mem_op(j, I32_STORE, flag_off(l)); }
static void flags_use(struct jit *j) { if (!j->flags_known) j->flags_rd = 1; }
static void flags_def(struct jit *j) { j->flags_known = 1; j->flags_wr = 1; }

static void cnt_add(struct jit *j, int n) { lget(j, L_CNT); i32c(j, n); op(j, I32_ADD); lset(j, L_CNT); }
static int in_region(struct jit *j, uint64_t pc) { return pc - j->base < AOI_VM_PAGE && j->start[(pc - j->base) >> 2]; }

/* Go on at pc, after this instruction: inside the region another turn of the dispatch loop
 * (while the budget lasts), else out to the interpreter. extra: blocks opened since
 * block level. */
static void exit_to(struct jit *j, uint64_t pc, int extra)
{
    cnt_add(j, j->ninsn + 1);
    i64c(j, (int64_t)pc); lset(j, L_NPC);
    if (in_region(j, pc)) {
        lget(j, L_CNT); i32c(j, JIT_BUDGET); op(j, I32_LTS);
        put(&j->b, IF); put(&j->b, T_VOID);
          i32c(j, (int32_t)((pc - j->base) >> 2)); lset(j, L_IDX);
          put(&j->b, BR); putu(&j->b, (unsigned)(j->d_out + extra));   /* (+1 for this if, -1: the loop is inside the exit block) */
        put(&j->b, END);
    }
    put(&j->b, BR); putu(&j->b, (unsigned)(j->d_out + extra));
}
/* an indirect branch: target on the stack (i64); a block start of this page loops, anything else leaves */
static void exit_dyn(struct jit *j)
{
    lset(j, L_NPC);
    cnt_add(j, j->ninsn + 1);
    lget(j, L_NPC); i64c(j, (int64_t)j->base); op(j, I64_SUB); i64c(j, AOI_VM_PAGE); op(j, I64_LTU);
    lget(j, L_CNT); i32c(j, JIT_BUDGET); op(j, I32_LTS); op(j, I32_AND);
    put(&j->b, IF); put(&j->b, T_VOID);
      lget(j, L_NPC); i64c(j, (int64_t)j->base); op(j, I64_SUB); i64c(j, 2); op(j, I64_SHRU); op(j, WRAP); lset(j, L_IDX);
      put(&j->b, BR); putu(&j->b, (unsigned)j->d_out);             /* the dispatch: non-starts leave there */
    put(&j->b, END);
    put(&j->b, BR); putu(&j->b, (unsigned)j->d_out);
}

static void cond(struct jit *j, unsigned c)
{
    flags_use(j);
    switch (c >> 1) {
    case 0: fget(j, L_Z); break;
    case 1: fget(j, L_C); break;
    case 2: fget(j, L_N); break;
    case 3: fget(j, L_V); break;
    case 4: fget(j, L_C); fget(j, L_Z); op(j, I32_EQZ); op(j, I32_AND); break;
    case 5: fget(j, L_N); fget(j, L_V); op(j, I32_EQ); break;
    case 6: fget(j, L_Z); op(j, I32_EQZ); fget(j, L_N); fget(j, L_V); op(j, I32_EQ); op(j, I32_AND); break;
    default: i32c(j, 1); return;
    }
    if (c & 1) op(j, I32_EQZ);
}

/* operands in ta/tb (or ua/ub), result in tr (ur) and on rd; flags if setf */
static void addsub(struct jit *j, int w32, int sub, int setf, int rd, int rd_sp)
{
    int A = w32 ? L_UA : L_TA, B = w32 ? L_UB : L_TB, R = w32 ? L_UR : L_TR;
    lset(j, B); lset(j, A);
    lget(j, A); lget(j, B); op(j, w32 ? (sub ? I32_SUB : I32_ADD) : (sub ? I64_SUB : I64_ADD)); lset(j, R);
    if (setf) {
        flags_def(j);
        lget(j, R); if (w32) i32c(j, 0); else i64c(j, 0); op(j, w32 ? I32_LTS : I64_LTS); fset(j, L_N);
        lget(j, R); op(j, w32 ? I32_EQZ : I64_EQZ); fset(j, L_Z);
        if (sub) { lget(j, A); lget(j, B); op(j, w32 ? I32_GEU : I64_GEU); fset(j, L_C); }
        else { lget(j, R); lget(j, A); op(j, w32 ? I32_LTU : I64_LTU); fset(j, L_C); }
        /* v: sub ((a^b)&(a^r)) < 0, add (~(a^b)&(a^r)) < 0 */
        lget(j, A); lget(j, B); op(j, w32 ? I32_XOR : I64_XOR);
        if (!sub) { if (w32) i32c(j, -1); else i64c(j, -1); op(j, w32 ? I32_XOR : I64_XOR); }
        lget(j, A); lget(j, R); op(j, w32 ? I32_XOR : I64_XOR); op(j, w32 ? I32_AND : I64_AND);
        if (w32) i32c(j, 0); else i64c(j, 0); op(j, w32 ? I32_LTS : I64_LTS); fset(j, L_V);
    }
    if (rd != 31 || rd_sp) { lget(j, R); reg_set(j, rd, rd_sp, w32); }
}

static void logic_flags(struct jit *j, int w32)
{
    int R = w32 ? L_UR : L_TR;
    flags_def(j);
    lget(j, R); if (w32) i32c(j, 0); else i64c(j, 0); op(j, w32 ? I32_LTS : I64_LTS); fset(j, L_N);
    lget(j, R); op(j, w32 ? I32_EQZ : I64_EQZ); fset(j, L_Z);
    i32c(j, 0); fset(j, L_C); i32c(j, 0); fset(j, L_V);
}

static void shifted(struct jit *j, int r, int w32, int typ, int amt)
{
    static const uint8_t ops64[4] = { I64_SHL, I64_SHRU, I64_SHRS, I64_ROTR }, ops32[4] = { I32_SHL, I32_SHRU, I32_SHRS, I32_ROTR };
    if (w32) reg_get32(j, r, 0); else reg_get(j, r, 0);
    if (amt) { if (w32) i32c(j, amt); else i64c(j, amt); op(j, w32 ? ops32[typ] : ops64[typ]); }
}

static void extended(struct jit *j, int r, int w32, int option, int sh)
{
    reg_get(j, r, 0);
    switch (option) {
    case 0: i64c(j, 0xff); op(j, I64_AND); break;
    case 1: i64c(j, 0xffff); op(j, I64_AND); break;
    case 2: i64c(j, 0xffffffffLL); op(j, I64_AND); break;
    case 4: op(j, I64_EXT8S); break;
    case 5: op(j, I64_EXT16S); break;
    case 6: op(j, I64_EXT32S); break;
    default: break;
    }
    if (sh) { i64c(j, sh); op(j, I64_SHL); }
    if (w32) op(j, WRAP);
}

/* Guest address (i64) in L_A. Leaves the host address (i32) in L_H, or 0 for the slow path. */
static void fast_addr(struct jit *j, int len, int need)
{
    /* the module's function 1 (tr_helper below): host address or 0; small code per access */
    j->mem = 1;
    lget(j, L_A); i32c(j, len); i32c(j, need); lget(j, L_PROT); lget(j, L_CHUNK); lget(j, L_VSIZE);
    put(&j->b, 0x10); putu(&j->b, 1);                              /* call 1 */
    lset(j, L_H);
}

/* function 1 of every module: (a i64, len i32, need i32, prot i32, chunk i32, vsize i64) -> host i32 or 0:
 * top byte ignored, inside the space, one page, permission byte, the chunk's host memory */
static void tr_helper(struct buf *out)
{
    struct jit h;
    memset(&h, 0, sizeof h);
    putu(&h.b, 1); putu(&h.b, 1); put(&h.b, T_I32);                /* local 6: h */
    lget(&h, 0); i64c(&h, 0x00ffffffffffffffLL); op(&h, I64_AND); lset(&h, 0);
    lget(&h, 0); lget(&h, 1); op(&h, EXT_U); op(&h, I64_ADD); lget(&h, 5); op(&h, I64_LEU);
    lget(&h, 0); lget(&h, 0); lget(&h, 1); op(&h, EXT_U); op(&h, I64_ADD); i64c(&h, 1); op(&h, I64_SUB); op(&h, I64_XOR);
    i64c(&h, AOI_VM_PAGE); op(&h, I64_LTU);
    op(&h, I32_AND);
    put(&h.b, IF); put(&h.b, T_VOID);
      lget(&h, 3); lget(&h, 0); i64c(&h, 12); op(&h, I64_SHRU); op(&h, WRAP); op(&h, I32_ADD); mem_op(&h, I32_LOAD8U, 0);
      lget(&h, 2); i32c(&h, AOI_PROT_MISSING); op(&h, I32_OR); op(&h, I32_AND); lget(&h, 2); op(&h, I32_EQ);
      put(&h.b, IF); put(&h.b, T_VOID);
        lget(&h, 4); lget(&h, 0); i64c(&h, AOI_VM_CHUNK_SHIFT); op(&h, I64_SHRU); op(&h, WRAP); i32c(&h, 2); op(&h, I32_SHL); op(&h, I32_ADD);
        mem_op(&h, I32_LOAD, 0); ltee(&h, 6);
        put(&h.b, IF); put(&h.b, T_VOID);
          lget(&h, 6); lget(&h, 0); i64c(&h, AOI_VM_CHUNK - 1); op(&h, I64_AND); op(&h, WRAP); op(&h, I32_ADD);
          op(&h, RETURN);
        put(&h.b, END);
      put(&h.b, END);
    put(&h.b, END);
    i32c(&h, 0);
    put(&h.b, END);
    putu(out, h.b.n); putbuf(out, &h.b);
    free(h.b.p);
}

/* after a slow path: stop before this instruction if it faulted */
static void check_stop(struct jit *j, int depth)
{
    lget(j, 0); mem_op(j, I32_LOAD, OFF(stop));
    put(&j->b, IF); put(&j->b, T_VOID);
      i64c(j, (int64_t)j->pc); lset(j, L_NPC); cnt_add(j, j->ninsn);
      put(&j->b, BR); putu(&j->b, (unsigned)(j->d_out + depth + 1));
    put(&j->b, END);
}

/* load len bytes from guest address L_A; value (i64, zero- or sign-extended) on the stack.
 * depth: blocks open around this point inside the body block. */
static void load(struct jit *j, int len, int sign, int depth)
{
    static const uint8_t lu[4] = { I64_LOAD8U, I64_LOAD16U, I64_LOAD32U, I64_LOAD }, ls[4] = { I64_LOAD8S, I64_LOAD16S, I64_LOAD32S, I64_LOAD };
    int k = len == 1 ? 0 : len == 2 ? 1 : len == 4 ? 2 : 3;
    fast_addr(j, len, AOI_PROT_R);
    lget(j, L_H);
    put(&j->b, IF); put(&j->b, T_I64);
      lget(j, L_H); mem_op(j, sign ? ls[k] : lu[k], 0);
    put(&j->b, ELSE);
      lget(j, 0); lget(j, L_A); i32c(j, len);
      i32c(j, (int32_t)(uintptr_t)jit_ld); put(&j->b, CALL_IND); putu(&j->b, 1); put(&j->b, 0);
      if (sign && len < 8) op(j, len == 1 ? I64_EXT8S : len == 2 ? I64_EXT16S : I64_EXT32S);
      lset(j, L_TC);
      check_stop(j, depth + 1);
      lget(j, L_TC);
    put(&j->b, END);
}

/* store: value (i64) in L_TC, address in L_A */
static void store(struct jit *j, int len, int depth)
{
    static const uint8_t st[4] = { I64_STORE8, I64_STORE16, I64_STORE32, I64_STORE };
    int k = len == 1 ? 0 : len == 2 ? 1 : len == 4 ? 2 : 3;
    fast_addr(j, len, AOI_PROT_W);
    lget(j, L_H);
    put(&j->b, IF); put(&j->b, T_VOID);
      lget(j, L_H); lget(j, L_TC); mem_op(j, st[k], 0);
    put(&j->b, ELSE);
      lget(j, 0); lget(j, L_A); lget(j, L_TC); i32c(j, len);
      i32c(j, (int32_t)(uintptr_t)jit_st); put(&j->b, CALL_IND); putu(&j->b, 2); put(&j->b, 0);
      check_stop(j, depth + 1);
    put(&j->b, END);
}

/* SIMD&FP registers: the CPU structure's vreg[n][0] (low 64 bits) and [1] (high) */
static void vget(struct jit *j, int r, int hi) { lget(j, 0); mem_op(j, I64_LOAD, OFF(vreg) + 16u * (unsigned)r + (hi ? 8u : 0u)); }
static void vset(struct jit *j, int r, int hi)   /* value (i64) on the stack */
{
    lset(j, L_SV); lget(j, 0); lget(j, L_SV); mem_op(j, I64_STORE, OFF(vreg) + 16u * (unsigned)r + (hi ? 8u : 0u));
}
static void vzero_hi(struct jit *j, int r) { i64c(j, 0); vset(j, r, 1); }

/* bytes len (1..16) between guest memory at L_A and vector register rt (a load clears the rest) */
static void vmem(struct jit *j, int load_, int rt, int len, int depth)
{
    if (load_) {
        load(j, len < 16 ? len : 8, 0, depth); vset(j, rt, 0);
        if (len == 16) { lget(j, L_A); i64c(j, 8); op(j, I64_ADD); lset(j, L_A); load(j, 8, 0, depth); vset(j, rt, 1); }
        else vzero_hi(j, rt);
    } else {
        vget(j, rt, 0); lset(j, L_TC); store(j, len < 16 ? len : 8, depth);
        if (len == 16) { lget(j, L_A); i64c(j, 8); op(j, I64_ADD); lset(j, L_A); vget(j, rt, 1); lset(j, L_TC); store(j, 8, depth); }
    }
}

/* SIMD&FP loads and stores, floating point (double) arithmetic, a few system and atomic
 * instructions. 1 translated, 0 not here. */
static int insn_fp(struct jit *j, uint32_t w)
{
    uint64_t pc = j->pc;
    int rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
    const int D = 0;
    /* ---- system ---- */
    if ((w & 0xffffffe0u) == 0xd53bd040u) { lget(j, 0); mem_op(j, I64_LOAD, OFF(tpidr)); reg_set(j, rd, 0, 0); return 1; }   /* mrs tpidr_el0 */
    if ((w & 0xffffffe0u) == 0xd51bd040u) { reg_get(j, rd, 0); lset(j, L_SV); lget(j, 0); lget(j, L_SV); mem_op(j, I64_STORE, OFF(tpidr)); return 1; }
    if ((w & 0xfffff09fu) == 0xd503309fu) return 1;                /* dsb, dmb, isb: one thread at a time */
    if (w == 0xd503305fu) { lget(j, 0); i32c(j, 0); mem_op(j, I32_STORE, OFF(excl_valid)); return 1; }   /* clrex */

    /* ---- load-acquire / store-release, exclusives (one guest thread runs at a time) ---- */
    if ((w & 0x3fbffc00u) == 0x089ffc00u) {                       /* ldar (L=1) / stlr (L=0) */
        int size = w >> 30, L = w >> 22 & 1;
        reg_get(j, rn, 1); lset(j, L_A);
        if (L) { load(j, 1 << size, 0, D); reg_set(j, rd, 0, 0); }
        else { reg_get(j, rd, 0); lset(j, L_TC); store(j, 1 << size, D); }
        return 1;
    }
    if ((w & 0x3fe07c00u) == 0x08407c00u) {                       /* ldxr / ldaxr */
        int size = w >> 30;
        reg_get(j, rn, 1); ltee(j, L_TB); lset(j, L_A);
        load(j, 1 << size, 0, D); reg_set(j, rd, 0, 0);
        lget(j, 0); lget(j, L_TB); mem_op(j, I64_STORE, OFF(excl_addr));
        lget(j, 0); i32c(j, 1); mem_op(j, I32_STORE, OFF(excl_valid));
        return 1;
    }
    if ((w & 0x3fe07c00u) == 0x08007c00u) {                       /* stxr / stlxr: status in ws (rm field) */
        int size = w >> 30, rs = rm;
        if (rs == rd || rs == rn) return 0;                        /* (constrained unpredictable: the interpreter) */
        reg_get(j, rn, 1); ltee(j, L_TB); lset(j, L_A);
        lget(j, 0); mem_op(j, I32_LOAD, OFF(excl_valid));
        lget(j, 0); mem_op(j, I64_LOAD, OFF(excl_addr)); lget(j, L_TB); op(j, I64_EQ); op(j, I32_AND);
        put(&j->b, IF); put(&j->b, T_VOID);
          reg_get(j, rd, 0); lset(j, L_TC); store(j, 1 << size, D + 1);
          i64c(j, 0); reg_set(j, rs, 0, 0);
        put(&j->b, ELSE);
          i64c(j, 1); reg_set(j, rs, 0, 0);
        put(&j->b, END);
        lget(j, 0); i32c(j, 0); mem_op(j, I32_STORE, OFF(excl_valid));
        return 1;
    }

    /* ---- SIMD&FP loads and stores ---- */
    if ((w & 0x3f000000u) == 0x3d000000u) {                       /* unsigned offset */
        int size = w >> 30, opc = (w >> 22) & 3, len, sc;
        if ((opc & 2) && size) return 0;
        len = (opc & 2) ? 16 : 1 << size; sc = (opc & 2) ? 4 : size;
        reg_get(j, rn, 1); i64c(j, (int64_t)(((w >> 10) & 0xfff) << sc)); op(j, I64_ADD); lset(j, L_A);
        vmem(j, opc & 1, rd, len, D);
        return 1;
    }
    if ((w & 0x3f200000u) == 0x3c000000u) {                       /* unscaled, pre-, post-index */
        int size = w >> 30, opc = (w >> 22) & 3, mode = (w >> 10) & 3, len;
        int64_t imm = (int64_t)sextn((w >> 12) & 0x1ff, 9);
        if ((opc & 2) && size) return 0;
        len = (opc & 2) ? 16 : 1 << size;
        if (mode == 0 || mode == 2) { reg_get(j, rn, 1); i64c(j, imm); op(j, I64_ADD); lset(j, L_A); vmem(j, opc & 1, rd, len, D); return 1; }
        reg_get(j, rn, 1); if (mode == 3) { i64c(j, imm); op(j, I64_ADD); }
        ltee(j, L_TB); lset(j, L_A);
        vmem(j, opc & 1, rd, len, D);
        lget(j, L_TB); if (mode == 1) { i64c(j, imm); op(j, I64_ADD); }
        reg_set(j, rn, 1, 0);
        return 1;
    }
    if ((w & 0x3f200c00u) == 0x3c200800u) {                       /* register offset */
        int size = w >> 30, opc = (w >> 22) & 3, option = (w >> 13) & 7, sbit = w >> 12 & 1, len, sc;
        if ((opc & 2) && size) return 0;
        len = (opc & 2) ? 16 : 1 << size; sc = (opc & 2) ? 4 : size;
        reg_get(j, rn, 1); extended(j, rm, 0, option, sbit ? sc : 0); op(j, I64_ADD); lset(j, L_A);
        vmem(j, opc & 1, rd, len, D);
        return 1;
    }
    if ((w & 0x3f000000u) == 0x1c000000u) {                       /* ldr (literal) s/d/q */
        int opc = w >> 30;
        if (opc == 3) return 0;
        i64c(j, (int64_t)(pc + (uint64_t)(sextn((w >> 5) & 0x7ffff, 19) << 2))); lset(j, L_A);
        vmem(j, 1, rd, 4 << opc, D);
        return 1;
    }
    if ((w & 0x3e000000u) == 0x2c000000u) {                       /* ldp / stp s/d/q */
        int opc = w >> 30, typ = (w >> 23) & 3, L = w >> 22 & 1, rt2 = (w >> 10) & 31, len;
        int64_t imm;
        if (opc == 3) return 0;
        len = 4 << opc;
        imm = (int64_t)sextn((w >> 15) & 0x7f, 7) * len;
        if (L && rd == rt2) return 0;
        reg_get(j, rn, 1); if (typ != 1) { i64c(j, imm); op(j, I64_ADD); } lset(j, L_TB);
        lget(j, L_TB); lset(j, L_A); vmem(j, L, rd, len, D);
        lget(j, L_TB); i64c(j, len); op(j, I64_ADD); lset(j, L_A); vmem(j, L, rt2, len, D);
        if (typ == 1 || typ == 3) { lget(j, L_TB); if (typ == 1) { i64c(j, imm); op(j, I64_ADD); } reg_set(j, rn, 1, 0); }
        return 1;
    }

    /* ---- moves between general and FP registers, conversions (double, 64-bit) ---- */
    if ((w & 0xfffffc00u) == 0x9e660000u) { vget(j, rn, 0); reg_set(j, rd, 0, 0); return 1; }                 /* fmov xd, dn */
    if ((w & 0xfffffc00u) == 0x9e670000u) { reg_get(j, rn, 0); vset(j, rd, 0); vzero_hi(j, rd); return 1; }  /* fmov dd, xn */
    if ((w & 0xfffffc00u) == 0x1e260000u) { vget(j, rn, 0); i64c(j, 0xffffffffLL); op(j, I64_AND); reg_set(j, rd, 0, 0); return 1; }   /* fmov wd, sn */
    if ((w & 0xfffffc00u) == 0x1e270000u) { reg_get32(j, rn, 0); op(j, EXT_U); vset(j, rd, 0); vzero_hi(j, rd); return 1; }            /* fmov sd, wn */
    if ((w & 0xfffffc00u) == 0x9e620000u || (w & 0xfffffc00u) == 0x9e630000u) {                                 /* scvtf / ucvtf dd, xn */
        reg_get(j, rn, 0); op(j, (w >> 16 & 1) ? F64_CVT_I64U : F64_CVT_I64S); op(j, I64_REINT_F64); vset(j, rd, 0); vzero_hi(j, rd);
        return 1;
    }
    if ((w & 0xfffffc00u) == 0x9e780000u || (w & 0xfffffc00u) == 0x9e790000u) {                                 /* fcvtzs / fcvtzu xd, dn (saturating) */
        vget(j, rn, 0); op(j, F64_REINT_I64); put(&j->b, 0xfc); putu(&j->b, (w >> 16 & 1) ? 7u : 6u); reg_set(j, rd, 0, 0);
        return 1;
    }
    /* ---- double arithmetic (default FPCR: round to nearest, as the interpreter's host does) ---- */
    if ((w & 0xffe0fc00u) == 0x1e602800u || (w & 0xffe0fc00u) == 0x1e603800u ||
        (w & 0xffe0fc00u) == 0x1e600800u || (w & 0xffe0fc00u) == 0x1e601800u) {                                 /* fadd fsub fmul fdiv d */
        int o = (w >> 12) & 0xf;
        vget(j, rn, 0); op(j, F64_REINT_I64); vget(j, rm, 0); op(j, F64_REINT_I64);
        op(j, o == 2 ? F64_ADD : o == 3 ? F64_SUB : o == 0 ? F64_MUL : F64_DIV);
        op(j, I64_REINT_F64); vset(j, rd, 0); vzero_hi(j, rd);
        return 1;
    }
    if ((w & 0xfffffc00u) == 0x1e604000u || (w & 0xfffffc00u) == 0x1e614000u ||
        (w & 0xfffffc00u) == 0x1e60c000u || (w & 0xfffffc00u) == 0x1e61c000u) {                                 /* fmov fneg fabs fsqrt d */
        int o = (w >> 15) & 3;
        vget(j, rn, 0);
        if (o == 2) { i64c(j, (int64_t)0x8000000000000000ULL); op(j, I64_XOR); }
        else if (o == 1) { i64c(j, 0x7fffffffffffffffLL); op(j, I64_AND); }
        else if (o == 3) { op(j, F64_REINT_I64); op(j, F64_SQRT); op(j, I64_REINT_F64); }
        vset(j, rd, 0); vzero_hi(j, rd);
        return 1;
    }
    if ((w & 0xffe0fc17u) == 0x1e602000u) {                       /* fcmp dn, dm / dn, #0 (not fcmpe's signalling) */
        vget(j, rn, 0); lset(j, L_TA);                             /* raw bits; reinterpreted as doubles below */
        if (w & 8) i64c(j, 0); else vget(j, rm, 0);
        lset(j, L_TB);
        /* a = TA, b = TB as doubles */
        lget(j, L_TA); op(j, F64_REINT_I64); lget(j, L_TB); op(j, F64_REINT_I64); op(j, F64_LT); fset(j, L_N);
        lget(j, L_TA); op(j, F64_REINT_I64); lget(j, L_TB); op(j, F64_REINT_I64); op(j, F64_EQ); fset(j, L_Z);
        /* c: not less (equal, greater or unordered); v: unordered */
        lget(j, L_TA); op(j, F64_REINT_I64); lget(j, L_TA); op(j, F64_REINT_I64); op(j, F64_EQ);
        lget(j, L_TB); op(j, F64_REINT_I64); lget(j, L_TB); op(j, F64_REINT_I64); op(j, F64_EQ); op(j, I32_AND); op(j, I32_EQZ);
        lset(j, L_UR);
        fget(j, L_N); op(j, I32_EQZ); fset(j, L_C);
        lget(j, L_UR); fset(j, L_V);
        flags_def(j);
        return 1;
    }
    return 0;
}

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

static void ld_st(struct jit *j, int size, int opc, int rt, int depth)
{
    /* address in L_A */
    if (opc == 0) { reg_get(j, rt, 0); lset(j, L_TC); store(j, 1 << size, depth); return; }
    if (opc == 1) { load(j, 1 << size, 0, depth); reg_set(j, rt, 0, 0); return; }
    load(j, 1 << size, 1, depth);
    if (opc == 3) { op(j, WRAP); reg_set(j, rt, 0, 1); }   /* sign-extended to 32 bits */
    else reg_set(j, rt, 0, 0);
}

/* One instruction. Returns 1 if translated, 0 if it is outside the subset (the block ends
 * before it), 2 if it ended the block (a branch). */
static int insn(struct jit *j, uint32_t w)
{
    uint64_t pc = j->pc, nxt = pc + 4;
    int sf = w >> 31, w32 = !sf;
    int rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
    const int D = 0;                  /* blocks open inside the body block at instruction level */

    {
        int r = insn_fp(j, w);
        if (r) return r;
    }
    /* ---- branches ---- */
    if ((w & 0x7c000000u) == 0x14000000u) {                       /* b, bl */
        uint64_t t = pc + (uint64_t)(sextn(w & 0x3ffffff, 26) << 2);
        if (w >> 31) { i64c(j, (int64_t)nxt); reg_set(j, 30, 0, 0); }
        exit_to(j, t, D);
        return 2;
    }
    if ((w & 0xff000010u) == 0x54000000u) {                       /* b.cond */
        uint64_t t = pc + (uint64_t)(sextn((w >> 5) & 0x7ffff, 19) << 2);
        cond(j, w & 15);
        put(&j->b, IF); put(&j->b, T_VOID); exit_to(j, t, D + 1); put(&j->b, END);
        exit_to(j, nxt, D);
        return 2;
    }
    if ((w & 0x7e000000u) == 0x34000000u) {                       /* cbz, cbnz */
        uint64_t t = pc + (uint64_t)(sextn((w >> 5) & 0x7ffff, 19) << 2);
        if (w32) { reg_get32(j, rd, 0); op(j, I32_EQZ); } else { reg_get(j, rd, 0); op(j, I64_EQZ); }
        if (w >> 24 & 1) op(j, I32_EQZ);
        put(&j->b, IF); put(&j->b, T_VOID); exit_to(j, t, D + 1); put(&j->b, END);
        exit_to(j, nxt, D);
        return 2;
    }
    if ((w & 0x7e000000u) == 0x36000000u) {                       /* tbz, tbnz */
        int b = (int)((w >> 26 & 0x20) | ((w >> 19) & 0x1f));
        uint64_t t = pc + (uint64_t)(sextn((w >> 5) & 0x3fff, 14) << 2);
        reg_get(j, rd, 0); i64c(j, (int64_t)(1ULL << b)); op(j, I64_AND); op(j, I64_EQZ);
        if (w >> 24 & 1) op(j, I32_EQZ);
        put(&j->b, IF); put(&j->b, T_VOID); exit_to(j, t, D + 1); put(&j->b, END);
        exit_to(j, nxt, D);
        return 2;
    }
    if ((w & 0xfffffc1fu) == 0xd65f0000u || (w & 0xfffffc1fu) == 0xd61f0000u) {   /* ret, br */
        reg_get(j, rn, 0); exit_dyn(j);
        return 2;
    }
    if ((w & 0xfffffc1fu) == 0xd63f0000u) {                       /* blr */
        reg_get(j, rn, 0); lset(j, L_TA);
        i64c(j, (int64_t)nxt); reg_set(j, 30, 0, 0);
        lget(j, L_TA); exit_dyn(j);
        return 2;
    }
    if ((w & 0xfffff01fu) == 0xd503201fu) return 1;               /* nop and hints */

    /* ---- data processing, immediate ---- */
    if ((w & 0x1f000000u) == 0x11000000u && !(w >> 23 & 1)) {     /* add/sub immediate */
        int opn = w >> 30 & 1, s = w >> 29 & 1;
        uint64_t imm = (uint64_t)((w >> 10) & 0xfff) << ((w >> 22 & 1) ? 12 : 0);
        if (w32) { reg_get32(j, rn, 1); i32c(j, (int32_t)imm); } else { reg_get(j, rn, 1); i64c(j, (int64_t)imm); }
        addsub(j, w32, opn, s, rd, !s);
        return 1;
    }
    if ((w & 0x1f800000u) == 0x12000000u) {                       /* logical immediate */
        int opc = w >> 29 & 3;
        uint64_t m, t;
        static const uint8_t o64[4] = { I64_AND, I64_OR, I64_XOR, I64_AND }, o32[4] = { I32_AND, I32_OR, I32_XOR, I32_AND };
        if (!decode_bitmask(w >> 22 & 1, (w >> 16) & 63, (w >> 10) & 63, sf, &m, &t, 1)) return 0;
        if (w32) { reg_get32(j, rn, 0); i32c(j, (int32_t)m); op(j, o32[opc]); }
        else { reg_get(j, rn, 0); i64c(j, (int64_t)m); op(j, o64[opc]); }
        if (opc == 3) { lset(j, w32 ? L_UR : L_TR); logic_flags(j, w32); lget(j, w32 ? L_UR : L_TR); reg_set(j, rd, 0, w32); }
        else reg_set(j, rd, 1, w32);
        return 1;
    }
    if ((w & 0x1f800000u) == 0x12800000u) {                       /* movn, movz, movk */
        int opc = w >> 29 & 3, hw = (w >> 21) & 3;
        uint64_t imm = (uint64_t)((w >> 5) & 0xffff) << (16 * hw);
        if (opc == 1) return 0;
        if (opc == 3) {
            reg_get(j, rd, 0); i64c(j, (int64_t)~(0xffffULL << (16 * hw))); op(j, I64_AND); i64c(j, (int64_t)imm); op(j, I64_OR);
            if (w32) { op(j, WRAP); reg_set(j, rd, 0, 1); } else reg_set(j, rd, 0, 0);
            return 1;
        }
        if (opc == 0) imm = ~imm;
        if (w32) imm &= 0xffffffffu;
        i64c(j, (int64_t)imm); reg_set(j, rd, 0, 0);
        return 1;
    }
    if ((w & 0x1f800000u) == 0x13000000u) {                       /* sbfm, bfm, ubfm */
        int opc = w >> 29 & 3, immr = (w >> 16) & 63, imms = (w >> 10) & 63, width = w32 ? 32 : 64;
        uint64_t wm, tm;
        if (opc == 3 || !decode_bitmask(w >> 22 & 1, immr, imms, sf, &wm, &tm, 0)) return 0;
        if (w32) {
            reg_get32(j, rn, 0); i32c(j, immr); op(j, I32_ROTR); i32c(j, (int32_t)wm); op(j, I32_AND); lset(j, L_UA);   /* bot */
            if (opc == 2) { lget(j, L_UA); i32c(j, (int32_t)tm); op(j, I32_AND); }
            else if (opc == 0) {
                reg_get32(j, rn, 0); i32c(j, width - 1 - imms); op(j, I32_SHL); i32c(j, width - 1); op(j, I32_SHRS);
                i32c(j, (int32_t)~tm); op(j, I32_AND); lget(j, L_UA); i32c(j, (int32_t)tm); op(j, I32_AND); op(j, I32_OR);
            } else {
                reg_get32(j, rd, 0); i32c(j, (int32_t)~wm); op(j, I32_AND); lget(j, L_UA); op(j, I32_OR); lset(j, L_UA);
                reg_get32(j, rd, 0); i32c(j, (int32_t)~tm); op(j, I32_AND); lget(j, L_UA); i32c(j, (int32_t)tm); op(j, I32_AND); op(j, I32_OR);
            }
            reg_set(j, rd, 0, 1);
        } else {
            reg_get(j, rn, 0); i64c(j, immr); op(j, I64_ROTR); i64c(j, (int64_t)wm); op(j, I64_AND); lset(j, L_TA);
            if (opc == 2) { lget(j, L_TA); i64c(j, (int64_t)tm); op(j, I64_AND); }
            else if (opc == 0) {
                reg_get(j, rn, 0); i64c(j, width - 1 - imms); op(j, I64_SHL); i64c(j, width - 1); op(j, I64_SHRS);
                i64c(j, (int64_t)~tm); op(j, I64_AND); lget(j, L_TA); i64c(j, (int64_t)tm); op(j, I64_AND); op(j, I64_OR);
            } else {
                reg_get(j, rd, 0); i64c(j, (int64_t)~wm); op(j, I64_AND); lget(j, L_TA); op(j, I64_OR); lset(j, L_TA);
                reg_get(j, rd, 0); i64c(j, (int64_t)~tm); op(j, I64_AND); lget(j, L_TA); i64c(j, (int64_t)tm); op(j, I64_AND); op(j, I64_OR);
            }
            reg_set(j, rd, 0, 0);
        }
        return 1;
    }
    if ((w & 0x7fa00000u) == 0x13800000u) {                       /* extr */
        int lsb = (w >> 10) & 63;
        if (w32) {
            if (!lsb) { reg_get32(j, rm, 0); reg_set(j, rd, 0, 1); return 1; }
            reg_get32(j, rm, 0); i32c(j, lsb); op(j, I32_SHRU); reg_get32(j, rn, 0); i32c(j, 32 - lsb); op(j, I32_SHL); op(j, I32_OR);
            reg_set(j, rd, 0, 1);
        } else {
            if (!lsb) { reg_get(j, rm, 0); reg_set(j, rd, 0, 0); return 1; }
            reg_get(j, rm, 0); i64c(j, lsb); op(j, I64_SHRU); reg_get(j, rn, 0); i64c(j, 64 - lsb); op(j, I64_SHL); op(j, I64_OR);
            reg_set(j, rd, 0, 0);
        }
        return 1;
    }
    if ((w & 0x1f000000u) == 0x10000000u) {                       /* adr, adrp */
        int64_t imm = (int64_t)sextn(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3), 21);
        uint64_t v = (w >> 31) ? (pc & ~(uint64_t)0xfff) + (uint64_t)(imm << 12) : pc + (uint64_t)imm;
        i64c(j, (int64_t)v); reg_set(j, rd, 0, 0);
        return 1;
    }

    /* ---- loads and stores (integer registers) ---- */
    if ((w & 0x3b000000u) == 0x39000000u) {                       /* ldr/str unsigned offset */
        int size = w >> 30, opc = (w >> 22) & 3;
        if (w >> 26 & 1) return 0;
        if (size == 3 && opc == 2) return 1;                       /* prfm */
        if (size == 3 && opc == 3) return 0;
        if (size == 2 && opc == 3) return 0;
        reg_get(j, rn, 1); i64c(j, (int64_t)(((w >> 10) & 0xfff) << size)); op(j, I64_ADD); lset(j, L_A);
        ld_st(j, size, opc, rd, D);
        return 1;
    }
    if ((w & 0x3b200000u) == 0x38000000u) {                       /* unscaled, pre-, post-index */
        int size = w >> 30, opc = (w >> 22) & 3, mode = (w >> 10) & 3;
        int64_t imm = (int64_t)sextn((w >> 12) & 0x1ff, 9);
        if (w >> 26 & 1) return 0;
        if (size == 3 && opc == 2) return 1;
        if ((size == 3 && opc == 3) || (size == 2 && opc == 3)) return 0;
        if (mode == 0 || mode == 2) { reg_get(j, rn, 1); i64c(j, imm); op(j, I64_ADD); lset(j, L_A); ld_st(j, size, opc, rd, D); return 1; }
        if (mode == 3) { reg_get(j, rn, 1); i64c(j, imm); op(j, I64_ADD); ltee(j, L_TB); lset(j, L_A); }
        else { reg_get(j, rn, 1); ltee(j, L_TB); lset(j, L_A); }
        ld_st(j, size, opc, rd, D);
        lget(j, L_TB); if (mode == 1) { i64c(j, imm); op(j, I64_ADD); }
        reg_set(j, rn, 1, 0);
        return 1;
    }
    if ((w & 0x3b200c00u) == 0x38200800u) {                       /* register offset */
        int size = w >> 30, opc = (w >> 22) & 3, option = (w >> 13) & 7, s = w >> 12 & 1;
        if (w >> 26 & 1) return 0;
        if (size == 3 && opc == 2) return 1;
        if ((size == 3 && opc == 3) || (size == 2 && opc == 3)) return 0;
        reg_get(j, rn, 1); extended(j, rm, 0, option, s ? size : 0); op(j, I64_ADD); lset(j, L_A);
        ld_st(j, size, opc, rd, D);
        return 1;
    }
    if ((w & 0x3b000000u) == 0x18000000u) {                       /* ldr literal */
        int opc = w >> 30;
        if (w >> 26 & 1) return 0;
        if (opc == 3) return 1;                                    /* prfm */
        i64c(j, (int64_t)(pc + (uint64_t)(sextn((w >> 5) & 0x7ffff, 19) << 2))); lset(j, L_A);
        load(j, opc == 1 ? 8 : 4, opc == 2, D); reg_set(j, rd, 0, 0);
        return 1;
    }
    if ((w & 0x3a000000u) == 0x28000000u) {                       /* ldp, stp */
        int opc = w >> 30, typ = (w >> 23) & 3, L = w >> 22 & 1, rt2 = (w >> 10) & 31;
        int size = opc == 2 ? 3 : 2, bytes = 1 << size;
        int64_t imm = (int64_t)sextn((w >> 15) & 0x7f, 7) * bytes;
        if (w >> 26 & 1) return 0;
        if (opc == 3 || (opc == 1 && !L)) return 0;
        reg_get(j, rn, 1); if (typ != 1) { i64c(j, imm); op(j, I64_ADD); } lset(j, L_TB);
        if (L) {
            lget(j, L_TB); lset(j, L_A); load(j, bytes, opc == 1, D); lset(j, L_TR);
            lget(j, L_TB); i64c(j, bytes); op(j, I64_ADD); lset(j, L_A); load(j, bytes, opc == 1, D); lset(j, L_TA);
        } else {
            lget(j, L_TB); lset(j, L_A); reg_get(j, rd, 0); lset(j, L_TC); store(j, bytes, D);
            lget(j, L_TB); i64c(j, bytes); op(j, I64_ADD); lset(j, L_A); reg_get(j, rt2, 0); lset(j, L_TC); store(j, bytes, D);
        }
        if (typ == 1 || typ == 3) { lget(j, L_TB); if (typ == 1) { i64c(j, imm); op(j, I64_ADD); } reg_set(j, rn, 1, 0); }
        if (L) { lget(j, L_TR); reg_set(j, rd, 0, 0); lget(j, L_TA); reg_set(j, rt2, 0, 0); }
        return 1;
    }

    /* ---- data processing, register ---- */
    if ((w & 0x1f000000u) == 0x0a000000u) {                       /* logical shifted register */
        int opc = w >> 29 & 3, typ = (w >> 22) & 3, nb = w >> 21 & 1, amt = (w >> 10) & 63;
        static const uint8_t o64[4] = { I64_AND, I64_OR, I64_XOR, I64_AND }, o32[4] = { I32_AND, I32_OR, I32_XOR, I32_AND };
        if (w32) reg_get32(j, rn, 0); else reg_get(j, rn, 0);
        shifted(j, rm, w32, typ, amt);
        if (nb) { if (w32) i32c(j, -1); else i64c(j, -1); op(j, w32 ? I32_XOR : I64_XOR); }
        op(j, w32 ? o32[opc] : o64[opc]);
        if (opc == 3) { lset(j, w32 ? L_UR : L_TR); logic_flags(j, w32); lget(j, w32 ? L_UR : L_TR); }
        reg_set(j, rd, 0, w32);
        return 1;
    }
    if ((w & 0x1f200000u) == 0x0b000000u) {                       /* add/sub shifted register */
        int opn = w >> 30 & 1, s = w >> 29 & 1, typ = (w >> 22) & 3, amt = (w >> 10) & 63;
        if (typ == 3) return 0;
        if (w32) reg_get32(j, rn, 0); else reg_get(j, rn, 0);
        shifted(j, rm, w32, typ, amt);
        addsub(j, w32, opn, s, rd, 0);
        return 1;
    }
    if ((w & 0x1fe00000u) == 0x0b200000u) {                       /* add/sub extended register */
        int opn = w >> 30 & 1, s = w >> 29 & 1, option = (w >> 13) & 7, sh = (w >> 10) & 7;
        if (w32) reg_get32(j, rn, 1); else reg_get(j, rn, 1);
        extended(j, rm, w32, option, sh);
        addsub(j, w32, opn, s, rd, !s);
        return 1;
    }
    if ((w & 0x3fe00410u) == 0x3a400000u) {                       /* ccmp, ccmn */
        int opn = w >> 30 & 1, nzcv = w & 15;
        cond(j, (w >> 12) & 15);
        put(&j->b, IF); put(&j->b, T_VOID);
          if (w32) reg_get32(j, rn, 0); else reg_get(j, rn, 0);
          if (w >> 11 & 1) { if (w32) i32c(j, rm); else i64c(j, rm); } else { if (w32) reg_get32(j, rm, 0); else reg_get(j, rm, 0); }
          addsub(j, w32, opn, 1, 31, 0);
        put(&j->b, ELSE);
          i32c(j, nzcv >> 3 & 1); fset(j, L_N); i32c(j, nzcv >> 2 & 1); fset(j, L_Z);
          i32c(j, nzcv >> 1 & 1); fset(j, L_C); i32c(j, nzcv & 1); fset(j, L_V);
        put(&j->b, END);
        flags_def(j);
        return 1;
    }
    if ((w & 0x3fe00000u) == 0x1a800000u) {                       /* csel, csinc, csinv, csneg */
        int opn = w >> 30 & 1, op2 = (w >> 10) & 3;
        if (w32) reg_get32(j, rn, 0); else reg_get(j, rn, 0);
        if (w32) reg_get32(j, rm, 0); else reg_get(j, rm, 0);
        if (!opn && op2 == 1) { if (w32) i32c(j, 1); else i64c(j, 1); op(j, w32 ? I32_ADD : I64_ADD); }
        else if (opn && op2 == 0) { if (w32) i32c(j, -1); else i64c(j, -1); op(j, w32 ? I32_XOR : I64_XOR); }
        else if (opn && op2 == 1) {
            lset(j, w32 ? L_UB : L_TB); if (w32) i32c(j, 0); else i64c(j, 0); lget(j, w32 ? L_UB : L_TB); op(j, w32 ? I32_SUB : I64_SUB);
        }
        cond(j, (w >> 12) & 15);
        op(j, SELECT);
        reg_set(j, rd, 0, w32);
        return 1;
    }
    if ((w & 0x5fe00000u) == 0x1ac00000u) {                       /* 2-source */
        int opcode = (w >> 10) & 63;
        if (opcode >= 8 && opcode <= 11) {                         /* lslv lsrv asrv rorv: Wasm masks the count as ARM does */
            static const uint8_t o64[4] = { I64_SHL, I64_SHRU, I64_SHRS, I64_ROTR }, o32[4] = { I32_SHL, I32_SHRU, I32_SHRS, I32_ROTR };
            if (w32) { reg_get32(j, rn, 0); reg_get32(j, rm, 0); op(j, o32[opcode - 8]); }
            else { reg_get(j, rn, 0); reg_get(j, rm, 0); op(j, o64[opcode - 8]); }
            reg_set(j, rd, 0, w32);
            return 1;
        }
        if (opcode == 2 || opcode == 3) {                          /* udiv, sdiv: x/0 = 0, INT_MIN/-1 = INT_MIN */
            int A = w32 ? L_UA : L_TA, B = w32 ? L_UB : L_TB;
            if (w32) { reg_get32(j, rn, 0); lset(j, A); reg_get32(j, rm, 0); lset(j, B); }
            else { reg_get(j, rn, 0); lset(j, A); reg_get(j, rm, 0); lset(j, B); }
            lget(j, B); op(j, w32 ? I32_EQZ : I64_EQZ);
            put(&j->b, IF); put(&j->b, w32 ? T_I32 : T_I64);
              if (w32) i32c(j, 0); else i64c(j, 0);
            put(&j->b, ELSE);
              if (opcode == 3) {
                  lget(j, B); if (w32) i32c(j, -1); else i64c(j, -1); op(j, w32 ? I32_EQ : I64_EQ);
                  put(&j->b, IF); put(&j->b, w32 ? T_I32 : T_I64);
                    if (w32) i32c(j, 0); else i64c(j, 0); lget(j, A); op(j, w32 ? I32_SUB : I64_SUB);
                  put(&j->b, ELSE);
                    lget(j, A); lget(j, B); op(j, w32 ? I32_DIVS : I64_DIVS);
                  put(&j->b, END);
              } else { lget(j, A); lget(j, B); op(j, w32 ? I32_DIVU : I64_DIVU); }
            put(&j->b, END);
            reg_set(j, rd, 0, w32);
            return 1;
        }
        return 0;
    }
    if ((w & 0x7ffffc00u) == 0x5ac01000u) {                       /* clz */
        if (w32) { reg_get32(j, rn, 0); op(j, I32_CLZ); } else { reg_get(j, rn, 0); op(j, I64_CLZ); }
        reg_set(j, rd, 0, w32);
        return 1;
    }
    if ((w & 0x1f000000u) == 0x1b000000u) {                       /* 3-source */
        int op31 = (w >> 21) & 7, o0 = w >> 15 & 1, ra = (w >> 10) & 31;
        if (op31 == 0) {                                           /* madd, msub */
            if (w32) { reg_get32(j, ra, 0); reg_get32(j, rn, 0); reg_get32(j, rm, 0); op(j, I32_MUL); op(j, o0 ? I32_SUB : I32_ADD); }
            else { reg_get(j, ra, 0); reg_get(j, rn, 0); reg_get(j, rm, 0); op(j, I64_MUL); op(j, o0 ? I64_SUB : I64_ADD); }
            reg_set(j, rd, 0, w32);
            return 1;
        }
        if ((op31 == 1 || op31 == 5) && sf) {                      /* smaddl/smsubl, umaddl/umsubl */
            uint8_t ext = op31 == 1 ? EXT_S : EXT_U;
            reg_get(j, ra, 0);
            reg_get32(j, rn, 0); op(j, ext); reg_get32(j, rm, 0); op(j, ext); op(j, I64_MUL);
            op(j, o0 ? I64_SUB : I64_ADD);
            reg_set(j, rd, 0, 0);
            return 1;
        }
        if (op31 == 6) {                                           /* umulh via 32-bit halves */
            reg_get(j, rn, 0); lset(j, L_TA); reg_get(j, rm, 0); lset(j, L_TB);
            lget(j, L_TA); i64c(j, 0xffffffffLL); op(j, I64_AND); lget(j, L_TB); i64c(j, 0xffffffffLL); op(j, I64_AND); op(j, I64_MUL); i64c(j, 32); op(j, I64_SHRU);
            lget(j, L_TA); i64c(j, 32); op(j, I64_SHRU); lget(j, L_TB); i64c(j, 0xffffffffLL); op(j, I64_AND); op(j, I64_MUL); op(j, I64_ADD); lset(j, L_TR);
            lget(j, L_TR); i64c(j, 0xffffffffLL); op(j, I64_AND); lget(j, L_TA); i64c(j, 0xffffffffLL); op(j, I64_AND); lget(j, L_TB); i64c(j, 32); op(j, I64_SHRU); op(j, I64_MUL); op(j, I64_ADD); lset(j, L_TC);
            lget(j, L_TA); i64c(j, 32); op(j, I64_SHRU); lget(j, L_TB); i64c(j, 32); op(j, I64_SHRU); op(j, I64_MUL);
            lget(j, L_TR); i64c(j, 32); op(j, I64_SHRU); op(j, I64_ADD); lget(j, L_TC); i64c(j, 32); op(j, I64_SHRU); op(j, I64_ADD);
            reg_set(j, rd, 0, 0);
            return 1;
        }
        return 0;                                                  /* smulh: the interpreter */
    }
    return 0;
}

/* instructions that must go back to the interpreter loop rather than through jit_step:
 * system calls (they may unmap code), exception-generating ones */
static int region_ender(uint32_t w)
{
    return (w & 0xff000000u) == 0xd4000000u;                       /* svc, hvc, smc, brk, hlt, dcps */
}

/* direct targets of a branch word (ends the block): returns how many, in t[] */
static int targets(uint32_t w, uint64_t pc, uint64_t t[2])
{
    if ((w & 0x7c000000u) == 0x14000000u) {                       /* b, bl (bl: the return comes back after it) */
        t[0] = pc + (uint64_t)(sextn(w & 0x3ffffff, 26) << 2);
        if (w >> 31) { t[1] = pc + 4; return 2; }
        return 1;
    }
    if ((w & 0xff000010u) == 0x54000000u || (w & 0x7e000000u) == 0x34000000u) {
        t[0] = pc + (uint64_t)(sextn((w >> 5) & 0x7ffff, 19) << 2); t[1] = pc + 4; return 2;
    }
    if ((w & 0x7e000000u) == 0x36000000u) { t[0] = pc + (uint64_t)(sextn((w >> 5) & 0x3fff, 14) << 2); t[1] = pc + 4; return 2; }
    if ((w & 0xfffffc1fu) == 0xd63f0000u) { t[0] = pc + 4; return 1; }   /* blr */
    return 0;                                                      /* ret, br */
}

static int32_t translate(struct aoi_cpu *c, uint64_t base, const uint32_t *roots)
{
    struct jit j, probe;
    struct buf m = { 0 }, sec = { 0 }, code = { 0 }, body = { 0 };
    uint8_t start[AOI_VM_PAGE / 4], seen[AOI_VM_PAGE / 4];
    uint16_t work[AOI_VM_PAGE / 4], order[AOI_VM_PAGE / 4];
    const uint32_t *words;
    int nw = 0, nb = 0, total = 0, i, r, kb, nlocals;
    int32_t fn;
    {
        uint8_t *p = gptr(c, base, AOI_VM_PAGE, AOI_PROT_X);
        if (!p) return 0;
        words = (const uint32_t *)p;
    }
    memset(start, 0, sizeof start); memset(seen, 0, sizeof seen);
    memset(&probe, 0, sizeof probe);
    /* discover the blocks reachable inside the page from every start seen there */
    for (i = 0; i < AOI_VM_PAGE / 4; i++)
        if (roots[i >> 5] >> (i & 31) & 1) { start[i] = 1; work[nw++] = (uint16_t)i; }
    while (nw) {
        int s0 = work[--nw], si;
        if (seen[s0]) continue;
        seen[s0] = 1;
        for (si = s0; si < AOI_VM_PAGE / 4; si++) {
            uint64_t t[2];
            int nt, q;
            probe.b.n = 0; probe.pc = base + 4u * (unsigned)si; probe.base = base; probe.start = start;
            r = insn(&probe, words[si]);
            if (!r && !region_ender(words[si])) r = 1;              /* stepped through the interpreter inline */
            if (!r) break;
            if (r == 2) {
                /* only code that ran: the starts seen (roots) and the fall-through after a
                 * conditional branch or a call; a taken target nobody reached yet stays out */
                nt = targets(words[si], probe.pc, t);
                for (q = 0; q < nt; q++)
                    if (t[q] == probe.pc + 4 && t[q] - base < AOI_VM_PAGE) {   /* fall-through only: taken targets come in as seen starts */
                        int ti = (int)((t[q] - base) >> 2);
                        start[ti] = 1;
                        if (!seen[ti] && nw < AOI_VM_PAGE / 4) work[nw++] = (uint16_t)ti;
                    }
                break;
            }
            if (si + 1 < AOI_VM_PAGE / 4 && start[si + 1]) {       /* runs into a known block: it continues there */
                if (!seen[si + 1] && nw < AOI_VM_PAGE / 4) work[nw++] = (uint16_t)(si + 1);
                break;
            }
        }
    }
    free(probe.b.p);
    for (i = 0; i < AOI_VM_PAGE / 4; i++) if (start[i]) order[nb++] = (uint16_t)i;

    /* emit: block $out { loop $top { block $B[nb-1] ... block $B0 { block $trap { br_table } leave } B0 code } B1 code ... } } */
    memset(&j, 0, sizeof j);
    j.base = base; j.start = start;
    put(&j.b, BLOCK); put(&j.b, T_VOID);                           /* $out */
    put(&j.b, 0x03); put(&j.b, T_VOID);                            /* loop $top */
    for (kb = nb - 1; kb >= 0; kb--) { put(&j.b, BLOCK); put(&j.b, T_VOID); }
    put(&j.b, BLOCK); put(&j.b, T_VOID);                           /* $trap */
    lget(&j, L_IDX);
    put(&j.b, 0x0e);                                               /* br_table over the page's slots */
    putu(&j.b, AOI_VM_PAGE / 4);
    {
        int slot, bi = 0;
        for (slot = 0; slot < AOI_VM_PAGE / 4; slot++) {
            if (start[slot]) { while (order[bi] != slot) bi++; putu(&j.b, (unsigned)(bi + 1)); }
            else putu(&j.b, 0);
        }
        putu(&j.b, 0);
    }
    put(&j.b, END);                                                /* $trap: not a block start (an indirect target) */
    put(&j.b, BR); putu(&j.b, (unsigned)(nb + 1));                 /* L_NPC is set: leave */
    for (kb = 0; kb < nb; kb++) {
        int si;
        put(&j.b, END);                                            /* end of $B[kb]: its code follows */
        j.d_out = nb - kb;                                         /* $B[kb+1..nb-1], $top -> br nb-kb is $out... */
        j.flags_known = 0;
        for (si = order[kb]; ; si++) {
            size_t before;
            uint32_t rm0, wm0;
            int fr0, fw0, fk0, mem0;
            j.pc = base + 4u * (unsigned)si;
            if (si >= AOI_VM_PAGE / 4) { i64c(&j, (int64_t)j.pc); lset(&j, L_NPC); cnt_add(&j, j.ninsn); put(&j.b, BR); putu(&j.b, (unsigned)j.d_out); break; }
            if (si != order[kb] && start[si]) {                     /* the next block: fall into it */
                cnt_add(&j, j.ninsn);
                break;
            }
            before = j.b.n; rm0 = j.rd_mask; wm0 = j.wr_mask; fr0 = j.flags_rd; fw0 = j.flags_wr; fk0 = j.flags_known; mem0 = j.mem;
            r = insn(&j, words[si]);
            if (!r && !region_ender(words[si])) {                  /* through the interpreter, inline */
                note_refused(words[si]);
                lget(&j, 0); i64c(&j, (int64_t)j.pc);
                i32c(&j, (int32_t)(uintptr_t)jit_step); put(&j.b, CALL_IND); putu(&j.b, 3); put(&j.b, 0);
                lget(&j, 0); mem_op(&j, I32_LOAD, OFF(stop));          /* a fault (or anything that stops): leave */
                lget(&j, 0); mem_op(&j, I64_LOAD, OFF(pc)); i64c(&j, (int64_t)(j.pc + 4)); op(&j, I64_NE);
                op(&j, I32_OR);
                put(&j.b, IF); put(&j.b, T_VOID);
                  lget(&j, 0); mem_op(&j, I64_LOAD, OFF(pc)); lset(&j, L_NPC);
                  cnt_add(&j, j.ninsn + 1);
                  put(&j.b, BR); putu(&j.b, (unsigned)(j.d_out + 1));
                put(&j.b, END);
                r = 1;
            }
            if (!r) {                                              /* the interpreter goes on from here */
                note_refused(words[si]);
                j.b.n = before; j.rd_mask = rm0; j.wr_mask = wm0; j.flags_rd = fr0; j.flags_wr = fw0; j.flags_known = fk0; j.mem = mem0;
                i64c(&j, (int64_t)j.pc); lset(&j, L_NPC); cnt_add(&j, j.ninsn); put(&j.b, BR); putu(&j.b, (unsigned)j.d_out);
                break;
            }
            j.ninsn++; total++;
            if (r == 2) break;
        }
        j.ninsn = 0;
    }
    put(&j.b, END);                                                /* loop */
    put(&j.b, END);                                                /* $out */
    if (total < 2) { free(j.b.p); return 0; }

    /* the function: registers in, the region, registers out */
    nlocals = 10;
    putu(&body, (unsigned)nlocals);
    putu(&body, 32); put(&body, T_I64);                            /* x0-x30, sp */
    putu(&body, 4); put(&body, T_I32);                             /* n z c v */
    putu(&body, 5); put(&body, T_I64);                             /* ta tb tr tc a */
    putu(&body, 4); put(&body, T_I32);                             /* ua ub ur h */
    putu(&body, 1); put(&body, T_I64);                             /* vsize */
    putu(&body, 2); put(&body, T_I32);                             /* prot chunk */
    putu(&body, 1); put(&body, T_I64);                             /* npc */
    putu(&body, 2); put(&body, T_I32);                             /* cnt idx */
    putu(&body, 1); put(&body, T_I64);                             /* sv */
    putu(&body, 1); put(&body, T_I32);                             /* uf */
    {
        struct jit pj; memset(&pj, 0, sizeof pj);
        pj.b = body;
        if (j.mem) {                                               /* the address space: cpu->mem->vm */
            lget(&pj, 0); mem_op(&pj, I32_LOAD, OFF(mem)); mem_op(&pj, I32_LOAD, (uint32_t)offsetof(struct aoi_mem, vm)); lset(&pj, L_H);
            lget(&pj, L_H); mem_op(&pj, I64_LOAD, (uint32_t)offsetof(struct aoi_vm, size)); lset(&pj, L_VSIZE);
            lget(&pj, L_H); mem_op(&pj, I32_LOAD, (uint32_t)offsetof(struct aoi_vm, prot)); lset(&pj, L_PROT);
            lget(&pj, L_H); mem_op(&pj, I32_LOAD, (uint32_t)offsetof(struct aoi_vm, chunk)); lset(&pj, L_CHUNK);
        }
        lget(&pj, 0); mem_op(&pj, I64_LOAD, OFF(pc)); ltee(&pj, L_NPC);           /* entered at cpu->pc: */
        i64c(&pj, (int64_t)base); op(&pj, I64_SUB); i64c(&pj, 2); op(&pj, I64_SHRU); op(&pj, WRAP); lset(&pj, L_IDX);
        /* (a pc that is not a block start lands in $trap and returns 0: the interpreter takes it) */
        putbuf(&pj.b, &j.b);
        lget(&pj, 0); lget(&pj, L_NPC); mem_op(&pj, I64_STORE, OFF(pc));
        lget(&pj, L_CNT);
        put(&pj.b, END);
        body = pj.b;
    }
    free(j.b.p);

    /* module */
    put(&m, 0); put(&m, 'a'); put(&m, 's'); put(&m, 'm'); put(&m, 1); put(&m, 0); put(&m, 0); put(&m, 0);
    /* types: 0 (i32)->i32, 1 (i32 i64 i32)->i64, 2 (i32 i64 i64 i32)->() */
    sec.n = 0;
    putu(&sec, 5);
    put(&sec, 0x60); putu(&sec, 1); put(&sec, T_I32); putu(&sec, 1); put(&sec, T_I32);
    put(&sec, 0x60); putu(&sec, 3); put(&sec, T_I32); put(&sec, T_I64); put(&sec, T_I32); putu(&sec, 1); put(&sec, T_I64);
    put(&sec, 0x60); putu(&sec, 4); put(&sec, T_I32); put(&sec, T_I64); put(&sec, T_I64); put(&sec, T_I32); putu(&sec, 0);
    put(&sec, 0x60); putu(&sec, 2); put(&sec, T_I32); put(&sec, T_I64); putu(&sec, 0);
    put(&sec, 0x60); putu(&sec, 6); put(&sec, T_I64); put(&sec, T_I32); put(&sec, T_I32); put(&sec, T_I32); put(&sec, T_I32); put(&sec, T_I64);
    putu(&sec, 1); put(&sec, T_I32);
    put(&m, 1); putu(&m, sec.n); putbuf(&m, &sec);
    /* imports: env.mem (memory), env.tbl (table) */
    sec.n = 0;
    putu(&sec, 2);
    putname(&sec, "env"); putname(&sec, "mem"); put(&sec, 0x02); put(&sec, 0x00); putu(&sec, 1);
    putname(&sec, "env"); putname(&sec, "tbl"); put(&sec, 0x01); put(&sec, 0x70); put(&sec, 0x00); putu(&sec, 1);
    put(&m, 2); putu(&m, sec.n); putbuf(&m, &sec);
    /* function 0: type 0 */
    sec.n = 0; putu(&sec, 2); putu(&sec, 0); putu(&sec, 4);
    put(&m, 3); putu(&m, sec.n); putbuf(&m, &sec);
    /* export "f" */
    sec.n = 0; putu(&sec, 1); putname(&sec, "f"); put(&sec, 0x00); putu(&sec, 0);
    put(&m, 7); putu(&m, sec.n); putbuf(&m, &sec);
    /* code */
    putu(&code, 2); putu(&code, body.n); putbuf(&code, &body); tr_helper(&code);
    put(&m, 10); putu(&m, code.n); putbuf(&m, &code);

    fn = jit_install(m.p, (int)m.n);
    free(m.p); free(sec.p); free(code.p); free(body.p);
    if (fn) aoi_jit_blocks++; else aoi_jit_failed++;
    return fn;
}

static int32_t translate_cached(struct aoi_cpu *c, uint64_t base, const uint32_t *roots)
{
    uint8_t *p = gptr(c, base, AOI_VM_PAGE, AOI_PROT_X);
    uint64_t h;
    unsigned i, k;
    int32_t fn;
    if (!p) return 0;
    h = page_hash(p);
    for (i = 0; i < AOI_VM_PAGE / 4 / 32; i++) h = (h ^ roots[i]) * 0x100000001b3ULL;
    i = (unsigned)((h ^ (base >> 12) * 0x9e3779b97f4a7c15ULL) >> 40) & (MOD_SLOTS - 1);
    for (k = 0; k < 8; k++) {
        struct mod_slot *m = &mods[(i + k) & (MOD_SLOTS - 1)];
        if (m->fn && m->base == base && m->hash == h) { aoi_jit_reused++; return m->fn; }
    }
    fn = translate(c, base, roots);
    if (fn > 0) {
        for (k = 0; k < 8 && mods[(i + k) & (MOD_SLOTS - 1)].fn; k++) {}
        if (k == 8) k = 0;
        mods[(i + k) & (MOD_SLOTS - 1)] = (struct mod_slot){ base, h, fn };
    }
    return fn;
}

static struct jit_slot *slot_of(struct aoi_cpu *c, uint64_t base, int make)
{
    const void *vm = c->mem->vm;
    unsigned h = (unsigned)((base >> 12) * 0x9e3779b1u ^ (uintptr_t)vm) & (JIT_SLOTS - 1), k;
    for (k = 0; k < 16; k++, h = (h + 1) & (JIT_SLOTS - 1)) {
        struct jit_slot *s = &slots[h];
        if (s->base == base && s->vm == vm) return s;
        if (!s->vm) {
            if (!make) return NULL;
            if (slot_used > JIT_SLOTS / 2) { aoi_jit_flush(); return NULL; }
            memset(s, 0, sizeof *s);
            s->base = base; s->vm = vm; slot_used++;
            return s;
        }
    }
    return NULL;
}

aoi_jit_fn aoi_jit_find(struct aoi_cpu *c, uint64_t pc)
{
    struct jit_slot *s;
    unsigned i = (unsigned)(pc & (AOI_VM_PAGE - 1)) >> 2;
    if (!aoi_jit_enabled || jit_stepping || !c->mem->vm || (pc & 3)) return NULL;
    if (!(s = slot_of(c, pc & ~(uint64_t)(AOI_VM_PAGE - 1), 1))) return NULL;
    if (s->fn > 0) { aoi_jit_runs++; return (aoi_jit_fn)(uintptr_t)s->fn; }
    if (s->fn < 0) return NULL;                                    /* nothing to translate here */
    s->roots[i >> 5] |= 1u << (i & 31);
    if (++s->count < JIT_HOT) return NULL;
    s->count = 0;
    s->fn = translate_cached(c, s->base, s->roots);
    if (!s->fn) { s->fn = -1; return NULL; }
    return (aoi_jit_fn)(uintptr_t)s->fn;
}

void aoi_jit_miss(struct aoi_cpu *c, uint64_t pc)
{
    struct jit_slot *s = slot_of(c, pc & ~(uint64_t)(AOI_VM_PAGE - 1), 0);
    unsigned i = (unsigned)(pc & (AOI_VM_PAGE - 1)) >> 2;
    int32_t fn;
    if (!s || s->fn <= 0) return;
    if (s->roots[i >> 5] >> (i & 31) & 1) return;                  /* known: a start the function cut off (e.g. a refused instruction) */
    s->roots[i >> 5] |= 1u << (i & 31);
    if (s->redo >= 64) return;                                     /* translated enough times: the rest stays interpreted */
    if (++s->count < JIT_MISS) return;                             /* (counts distinct new starts: an indirect branch's targets, */
    s->count = 0; s->redo++;                                       /*  e.g. CPython's opcode handlers, come in quickly) */
    fn = translate_cached(c, s->base, s->roots);                   /* again, with the new starts */
    if (fn > 0) s->fn = fn;
}
#endif
