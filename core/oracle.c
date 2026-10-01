/* Reference-CPU oracle: runs every guest instruction a second time in Unicorn
 * (QEMU's AArch64 core) and stops at the first instruction where our
 * interpreter disagrees — registers, flags, pc or stored bytes.
 *
 * Both CPUs share the same host memory (uc_mem_map_ptr). Before our step,
 * Unicorn executes the instruction with a write hook that saves the old bytes;
 * its result is recorded and memory is restored, then our interpreter runs
 * the same instruction and the two results are compared.
 *
 * Debug-only: built into the *-check binaries, never into the iOS app. */
#include "oracle.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unicorn/unicorn.h>

#define MAXW 8

struct oracle {
    uc_engine *uc;
    uint64_t pc;
    uint32_t insn;
    int uc_ok;
    uint64_t x[31], sp, npc, nzcv, v[32][2];
    int nw;
    uint64_t waddr[MAXW];
    int wlen[MAXW];
    uint8_t wold[MAXW][16], wnew[MAXW][16];
    uint64_t checked;
    int failed, quiet;
    char what[16];
};

static struct oracle O;

static const int xreg[31] = {
    UC_ARM64_REG_X0,  UC_ARM64_REG_X1,  UC_ARM64_REG_X2,  UC_ARM64_REG_X3,  UC_ARM64_REG_X4,
    UC_ARM64_REG_X5,  UC_ARM64_REG_X6,  UC_ARM64_REG_X7,  UC_ARM64_REG_X8,  UC_ARM64_REG_X9,
    UC_ARM64_REG_X10, UC_ARM64_REG_X11, UC_ARM64_REG_X12, UC_ARM64_REG_X13, UC_ARM64_REG_X14,
    UC_ARM64_REG_X15, UC_ARM64_REG_X16, UC_ARM64_REG_X17, UC_ARM64_REG_X18, UC_ARM64_REG_X19,
    UC_ARM64_REG_X20, UC_ARM64_REG_X21, UC_ARM64_REG_X22, UC_ARM64_REG_X23, UC_ARM64_REG_X24,
    UC_ARM64_REG_X25, UC_ARM64_REG_X26, UC_ARM64_REG_X27, UC_ARM64_REG_X28, UC_ARM64_REG_X29,
    UC_ARM64_REG_X30,
};

static void on_write(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void *ud)
{
    struct aoi_cpu *c = ud;
    uint8_t *p = aoi_mem_ptr(c->mem, addr, (uint64_t)size);
    (void)uc; (void)type; (void)value;
    if (O.nw == MAXW || size > 16 || !p) return;
    O.waddr[O.nw] = addr;
    O.wlen[O.nw] = size;
    memcpy(O.wold[O.nw], p, (size_t)size);
    O.nw++;
}

static uint64_t nzcv_of(struct aoi_cpu *c)
{
    return (uint64_t)c->n << 31 | (uint64_t)c->z << 30 | (uint64_t)c->c << 29 | (uint64_t)c->v << 28;
}

static void before(struct aoi_cpu *c)
{
    uint64_t v;
    int i;
    O.pc = c->pc;
    O.nw = 0;
    memcpy(&O.insn, aoi_mem_ptr(c->mem, c->pc, 4), 4);
    for (i = 0; i < 31; i++) uc_reg_write(O.uc, xreg[i], &c->x[i]);
    uc_reg_write(O.uc, UC_ARM64_REG_SP, &c->sp);
    v = nzcv_of(c);
    uc_reg_write(O.uc, UC_ARM64_REG_NZCV, &v);
    uc_reg_write(O.uc, UC_ARM64_REG_TPIDR_EL0, &c->tpidr);
    for (i = 0; i < 32; i++) uc_reg_write(O.uc, UC_ARM64_REG_Q0 + i, c->vreg[i]);
    O.uc_ok = uc_emu_start(O.uc, c->pc, (uint64_t)-1, 0, 1) == UC_ERR_OK;
    for (i = 0; i < 31; i++) uc_reg_read(O.uc, xreg[i], &O.x[i]);
    uc_reg_read(O.uc, UC_ARM64_REG_SP, &O.sp);
    uc_reg_read(O.uc, UC_ARM64_REG_PC, &O.npc);
    uc_reg_read(O.uc, UC_ARM64_REG_NZCV, &O.nzcv);
    for (i = 0; i < 32; i++) uc_reg_read(O.uc, UC_ARM64_REG_Q0 + i, O.v[i]);
    /* keep what Unicorn stored, then undo it so our CPU starts from the same memory */
    for (i = O.nw - 1; i >= 0; i--) {
        uint8_t *p = aoi_mem_ptr(c->mem, O.waddr[i], (uint64_t)O.wlen[i]);
        memcpy(O.wnew[i], p, (size_t)O.wlen[i]);
        memcpy(p, O.wold[i], (size_t)O.wlen[i]);
    }
}

static void fail(struct aoi_cpu *c, const char *what, uint64_t ours, uint64_t ref)
{
    int first = !O.failed;
    if (first) snprintf(O.what, sizeof O.what, "%s", what);
    O.failed = 1;
    c->stop = AOI_STOP_UNDEF;       /* stop the run where it went wrong */
    c->fault_insn = O.insn;
    if (O.quiet) return;
    if (first)
        fprintf(stderr, "[oracle] MISMATCH after %" PRIu64 " instructions at pc=0x%" PRIx64
                " insn=0x%08x\n", O.checked, O.pc, O.insn);
    fprintf(stderr, "[oracle]   %-6s ours=0x%016" PRIx64 " ref=0x%016" PRIx64 "\n", what, ours, ref);
}

static void after(struct aoi_cpu *c)
{
    char name[8];
    int i, k;
    if (c->stop == AOI_STOP_SYSCALL || O.insn == 0xd4000001u) return;   /* svc: host side-effects */
    O.checked++;
    if (!O.uc_ok) return;           /* Unicorn could not run it either: nothing to compare */
    for (i = 0; i < 31; i++)
        if (c->x[i] != O.x[i]) { snprintf(name, sizeof name, "x%d", i); fail(c, name, c->x[i], O.x[i]); }
    for (i = 0; i < 32; i++)
        for (k = 0; k < 2; k++)
            if (c->vreg[i][k] != O.v[i][k]) {
                snprintf(name, sizeof name, "v%d.%s", i, k ? "hi" : "lo");
                fail(c, name, c->vreg[i][k], O.v[i][k]);
            }
    if (c->sp != O.sp) fail(c, "sp", c->sp, O.sp);
    if (c->pc != O.npc) fail(c, "pc", c->pc, O.npc);
    if (nzcv_of(c) != (O.nzcv & 0xf0000000u)) fail(c, "nzcv", nzcv_of(c), O.nzcv & 0xf0000000u);
    for (i = 0; i < O.nw; i++) {
        uint8_t *p = aoi_mem_ptr(c->mem, O.waddr[i], (uint64_t)O.wlen[i]);
        if (memcmp(p, O.wnew[i], (size_t)O.wlen[i])) {
            uint64_t a = 0, b = 0;
            memcpy(&a, p, O.wlen[i] > 8 ? 8 : (size_t)O.wlen[i]);
            memcpy(&b, O.wnew[i], O.wlen[i] > 8 ? 8 : (size_t)O.wlen[i]);
            snprintf(name, sizeof name, "mem");
            fail(c, name, a, b);
            if (!O.quiet) fprintf(stderr, "[oracle]   (store to 0x%" PRIx64 ", %d bytes)\n", O.waddr[i], O.wlen[i]);
        }
    }
    for (k = 0; k < c->nwlog; k++) {
        int seen = 0;
        for (i = 0; i < O.nw; i++)
            if (c->wlog_addr[k] >= O.waddr[i] && c->wlog_addr[k] < O.waddr[i] + (uint64_t)O.wlen[i]) seen = 1;
        if (!seen) fail(c, "store", c->wlog_addr[k], 0);
    }
}

static void hook(struct aoi_cpu *c, int is_after)
{
    if (O.failed) return;
    if (is_after) after(c); else before(c);
}

const char *aoi_oracle_attach(struct aoi_cpu *c)
{
    uc_hook h;
    int i;
    memset(&O, 0, sizeof O);
    if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &O.uc) != UC_ERR_OK) return "uc_open failed";
    for (i = 0; i < c->mem->n; i++) {
        struct aoi_region *r = &c->mem->r[i];
        if (uc_mem_map_ptr(O.uc, r->base, (r->size + 0x3fff) & ~(uint64_t)0x3fff, UC_PROT_ALL, r->host) != UC_ERR_OK)
            return "uc_mem_map_ptr failed (region not page-aligned?)";
    }
    uc_hook_add(O.uc, &h, UC_HOOK_MEM_WRITE, (void *)on_write, c, 1, 0);
    c->trace = hook;
    return NULL;
}

void aoi_oracle_report(struct aoi_cpu *c, enum aoi_stop st)
{
    if (st == AOI_STOP_UNDEF && !O.failed)
        fprintf(stderr, "[oracle] our CPU lacks insn 0x%08x at pc=0x%" PRIx64 "; Unicorn %s it\n",
                c->fault_insn, c->pc, O.uc_ok ? "runs" : "also rejects");
    fprintf(stderr, "[oracle] %" PRIu64 " instructions cross-checked%s\n", O.checked,
            O.failed ? ", first mismatch above" : ", all identical");
}

void aoi_oracle_reset(int quiet) { O.failed = 0; O.quiet = quiet; O.uc_ok = 0; }
int aoi_oracle_mismatch(void) { return O.failed; }
int aoi_oracle_ref_ran(void) { return O.uc_ok; }
const char *aoi_oracle_what(void) { return O.what; }
