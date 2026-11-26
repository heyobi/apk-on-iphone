/* glibc's string and memory functions at host speed.
 *
 * On AlmaLinux a third of dnf's metadata work is strcmp, memcpy, strchr, strlen and
 * friends, each of them dozens of interpreted instructions per call. When a Linux
 * process maps glibc's libc.so.6 (executable, before the dynamic linker relocates it),
 * the entries of these functions are routed to host calls (core/cpu.c's thunks):
 *
 * - A plain function (strcmp, strchr...): its first instruction becomes a branch to its
 *   thunk. Declined calls go to a stub that runs that first instruction and jumps back.
 * - An IFUNC (memcpy, strlen...: glibc picks a variant per CPU): the resolver's first
 *   instruction branches to a thunk that runs the real resolver once, keeps the variant
 *   it picked, and hands back the function's thunk instead. Declined calls go to the
 *   variant.
 *
 * A host call works only on memory the guest could touch (every byte checked first, the
 * page permissions as the interpreter sees them); anything else - a pointer into an
 * unmapped or protected page, a copy that overlaps across host chunks - is declined and
 * runs as the guest's own code, which faults exactly where it would have. Results are
 * the ones glibc returns (memcmp/strcmp: the difference of the first differing bytes,
 * as unsigned chars). The routing lives in one guest page next to libc (thunks, stubs,
 * the variants), so fork, threads and snapshots carry it with the process.
 * AOI_LXFAST=0 turns it off. */
#include "lxfast.h"
#include "cpu_impl.h"
#include "proc.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint64_t aoi_lxfast_hits, aoi_lxfast_declined;

enum { F_MEMCPY, F_MEMMOVE, F_MEMSET, F_MEMCHR, F_RAWMEMCHR, F_MEMCMP, F_STRLEN, F_STRNLEN,
       F_STRCHR, F_STRCHRNUL, F_STRRCHR, F_STRCMP, F_STRNCMP, F_STRCPY, F_STPCPY, NF };
static const char *const names[NF] = { "memcpy", "memmove", "memset", "memchr", "rawmemchr", "memcmp", "strlen",
                                       "strnlen", "strchr", "strchrnul", "strrchr", "strcmp", "strncmp", "strcpy", "stpcpy" };

/* The page: thunk slots (slot 0 is the cpu's "return"), stubs, variants. */
#define SLOTS     64
#define SLOT_F(i) (1 + (i))                  /* the function */
#define SLOT_R(i) (1 + NF + (i))             /* its IFUNC resolver */
#define SLOT_C(i) (1 + 2 * NF + (i))         /* where that resolver returns to */
#define STUB(j)   (0x100 + 32 * (j))         /* j < NF: function j; NF + i: resolver i */
#define VARIANT(i) (0x800 + 8 * (i))
#define SAVED_LR  0x900

/* ---------- guest memory ---------- */

static uint8_t *span(struct aoi_vm *vm, uint64_t a, uint64_t len, int need, uint64_t *n)
{
    return len ? aoi_vm_span(vm, AOI_TBI(a), len, need, n) : NULL;
}

/* Every byte of [a, a+len) accessible with need. */
static int all_ok(struct aoi_vm *vm, uint64_t a, uint64_t len, int need)
{
    uint64_t n;
    while (len) {
        if (!span(vm, a, len, need, &n)) return 0;
        a += n; len -= n;
    }
    return 1;
}

/* How far to ask for in one span: to the end of this page first (short strings), then 64 KiB. */
static uint64_t step(uint64_t a, uint64_t done) { return done ? 65536 : AOI_VM_PAGE - (AOI_TBI(a) & (AOI_VM_PAGE - 1)); }

/* strlen/strnlen: 1 and *len, or 0 (an inaccessible byte comes first). */
static int str_len(struct aoi_vm *vm, uint64_t a, uint64_t max, uint64_t *len)
{
    uint64_t done = 0, n;
    while (done < max) {
        uint64_t want = step(a + done, done);
        const uint8_t *p, *z;
        if (want > max - done) want = max - done;
        if (!(p = span(vm, a + done, want, AOI_PROT_R, &n))) return 0;
        if ((z = memchr(p, 0, n))) { *len = done + (uint64_t)(z - p); return 1; }
        done += n;
    }
    *len = max;
    return 1;
}

/* memcpy/memmove of n bytes; 0 = decline. */
static int copy(struct aoi_vm *vm, uint64_t d, uint64_t s, uint64_t n)
{
    uint8_t *pd, *ps;
    uint64_t nd, ns, k;
    if (!n) return 1;
    if ((pd = span(vm, d, n, AOI_PROT_W, &nd)) && nd == n && (ps = span(vm, s, n, AOI_PROT_R, &ns)) && ns == n) {
        memmove(pd, ps, n);                                        /* both in one host chunk */
        return 1;
    }
    if (AOI_TBI(d) < AOI_TBI(s) + n && AOI_TBI(s) < AOI_TBI(d) + n) return 0;   /* overlapping across chunks */
    if (!all_ok(vm, d, n, AOI_PROT_W) || !all_ok(vm, s, n, AOI_PROT_R)) return 0;
    while (n) {
        pd = span(vm, d, n, AOI_PROT_W, &nd);
        ps = span(vm, s, n, AOI_PROT_R, &ns);
        k = nd < ns ? nd : ns;
        memcpy(pd, ps, k);
        d += k; s += k; n -= k;
    }
    return 1;
}

/* ---------- the functions: 1 with *r, or 0 to decline ---------- */

static int run(struct aoi_cpu *c, int f, uint64_t *r)
{
    struct aoi_vm *vm = c->mem->vm;
    uint64_t a = c->x[0], b = c->x[1], n = c->x[2], len, done, k, na, nb;
    const uint8_t *pa, *pb;
    switch (f) {
    case F_MEMCPY: case F_MEMMOVE:
        if (!copy(vm, a, b, n)) return 0;
        *r = a;
        return 1;
    case F_MEMSET: {
        uint8_t *p;
        if (!all_ok(vm, a, n, AOI_PROT_W)) return 0;
        for (done = 0; done < n; done += k) {
            p = span(vm, a + done, n - done, AOI_PROT_W, &k);
            memset(p, (int)(b & 0xff), k);
        }
        *r = a;
        return 1;
    }
    case F_MEMCHR: case F_RAWMEMCHR: {
        uint64_t max = f == F_MEMCHR ? n : ~0ULL;
        for (done = 0; done < max; done += k) {
            uint64_t want = step(a + done, done);
            const uint8_t *q;
            if (want > max - done) want = max - done;
            if (!(pa = span(vm, a + done, want, AOI_PROT_R, &k))) return 0;
            if ((q = memchr(pa, (int)(b & 0xff), k))) { *r = a + done + (uint64_t)(q - pa); return 1; }
        }
        *r = 0;
        return 1;
    }
    case F_MEMCMP:
        for (done = 0; done < n; done += k) {
            if (!(pa = span(vm, a + done, n - done, AOI_PROT_R, &na)) || !(pb = span(vm, b + done, n - done, AOI_PROT_R, &nb))) return 0;
            k = na < nb ? na : nb;
            if (memcmp(pa, pb, k)) {
                uint64_t i = 0;
                while (pa[i] == pb[i]) i++;
                *r = (uint32_t)((int)pa[i] - (int)pb[i]);
                return 1;
            }
        }
        *r = 0;
        return 1;
    case F_STRLEN: case F_STRNLEN:
        if (!str_len(vm, a, f == F_STRLEN ? ~0ULL : b, &len)) return 0;
        *r = len;
        return 1;
    case F_STRCHR: case F_STRCHRNUL: case F_STRRCHR: {
        int ch = (int)(b & 0xff);
        uint64_t last = 0;
        int found = 0;
        for (done = 0;; done += k) {
            const uint8_t *z, *q;
            uint64_t lim;
            if (!(pa = span(vm, a + done, step(a + done, done), AOI_PROT_R, &k))) return 0;
            z = memchr(pa, 0, k);
            lim = z ? (uint64_t)(z - pa) + 1 : k;
            if (f == F_STRRCHR) {
                for (q = pa; q < pa + lim; q++) if (*q == ch) { last = done + (uint64_t)(q - pa); found = 1; }
                if (z) { *r = found ? a + last : 0; return 1; }
            } else if ((q = memchr(pa, ch, lim))) {
                *r = a + done + (uint64_t)(q - pa);
                return 1;
            } else if (z) {
                *r = f == F_STRCHRNUL ? a + done + (uint64_t)(z - pa) : 0;
                return 1;
            }
        }
    }
    case F_STRCMP: case F_STRNCMP: {
        uint64_t max = f == F_STRCMP ? ~0ULL : n;
        for (done = 0; done < max; done += k) {
            uint64_t want = step(a + done, done), wb = step(b + done, done), lim, i;
            const uint8_t *z;
            if (want > wb) want = wb;
            if (want > max - done) want = max - done;
            if (!(pa = span(vm, a + done, want, AOI_PROT_R, &na)) || !(pb = span(vm, b + done, want, AOI_PROT_R, &nb))) return 0;
            k = na < nb ? na : nb;
            z = memchr(pa, 0, k);
            lim = z ? (uint64_t)(z - pa) + 1 : k;
            for (i = 0; i < lim; i++)
                if (pa[i] != pb[i]) { *r = (uint32_t)((int)pa[i] - (int)pb[i]); return 1; }
            if (z) { *r = 0; return 1; }
        }
        *r = 0;
        return 1;
    }
    case F_STRCPY: case F_STPCPY:
        if (!str_len(vm, b, ~0ULL, &len) || !copy(vm, a, b, len + 1)) return 0;
        *r = f == F_STRCPY ? a : a + len;
        return 1;
    }
    return 0;
}

/* ---------- host calls ---------- */

static uint64_t page_rd(struct aoi_vm *vm, uint64_t a)
{
    uint64_t v = 0;
    aoi_vm_read(vm, a, &v, 8, 0);
    return v;
}
static void page_wr(struct aoi_vm *vm, uint64_t a, uint64_t v) { aoi_vm_write(vm, a, &v, 8, 0); }

uint64_t aoi_lxfast_call(struct aoi_cpu *c, unsigned slot)
{
    struct aoi_vm *vm = c->mem->vm;
    uint64_t P = c->thunk_base, r;
    if (slot >= SLOT_F(0) && slot < SLOT_F(NF)) {
        int f = (int)slot - SLOT_F(0);
        uint64_t v;
        if (run(c, f, &r)) {
            aoi_lxfast_hits++;
            c->steps += 4 + (c->x[2] < 4096 ? c->x[2] : 4096) / 16;   /* (the scheduler's time slice) */
            return r;
        }
        aoi_lxfast_declined++;
        v = page_rd(vm, P + VARIANT(f));
        c->thunk_redirect = v ? v : P + STUB(f);                   /* the guest's own code, registers as they were */
        return 0;
    }
    if (slot >= SLOT_R(0) && slot < SLOT_R(NF)) {                  /* an IFUNC resolver called */
        int f = (int)slot - SLOT_R(0);
        if (page_rd(vm, P + VARIANT(f))) return P + 4 * SLOT_F(f);
        page_wr(vm, P + SAVED_LR, c->x[30]);                       /* first time: run it, see what it picks */
        c->x[30] = P + 4 * SLOT_C(f);
        c->thunk_redirect = P + STUB(NF + f);
        return 0;
    }
    if (slot >= SLOT_C(0) && slot < SLOT_C(NF)) {                  /* ... and it returned its variant */
        int f = (int)slot - SLOT_C(0);
        page_wr(vm, P + VARIANT(f), c->x[0]);
        c->x[30] = page_rd(vm, P + SAVED_LR);
        return P + 4 * SLOT_F(f);
    }
    c->stop = AOI_STOP_UNDEF;
    c->fault_insn = 0;
    return 0;
}

/* ---------- routing, when libc.so.6 is mapped ---------- */

static int pc_relative(uint32_t w)
{
    return (w & 0x7c000000u) == 0x14000000u || (w & 0xff000010u) == 0x54000000u ||   /* b bl, b.cond */
           (w & 0x7e000000u) == 0x34000000u || (w & 0x7e000000u) == 0x36000000u ||   /* cbz cbnz, tbz tbnz */
           (w & 0x1f000000u) == 0x10000000u || (w & 0x3b000000u) == 0x18000000u;     /* adr adrp, ldr literal */
}

static int rd_at(int fd, uint64_t off, void *buf, size_t n) { return pread(fd, buf, n, (off_t)off) == (ssize_t)n; }

struct sym { uint64_t value; int ifunc; };

/* .dynsym of the file: the routed functions' values (0: absent). */
static int find_syms(int fd, struct sym out[NF], uint64_t ph[][4], int *nph)
{
    uint8_t eh[64];
    uint64_t shoff, phoff;
    uint16_t shnum, phnum, shentsize, phentsize;
    int i, ok = 0;
    memset(out, 0, sizeof(struct sym) * NF);
    if (!rd_at(fd, 0, eh, 64) || memcmp(eh, "\177ELF\2\1", 6) || *(uint16_t *)(eh + 18) != 183) return 0;   /* ELF64, LE, aarch64 */
    memcpy(&phoff, eh + 32, 8); memcpy(&shoff, eh + 40, 8);
    memcpy(&phentsize, eh + 54, 2); memcpy(&phnum, eh + 56, 2);
    memcpy(&shentsize, eh + 58, 2); memcpy(&shnum, eh + 60, 2);
    *nph = 0;
    for (i = 0; i < phnum && *nph < 16; i++) {                     /* PT_LOAD: offset, vaddr, filesz, flags */
        uint8_t p[56];
        uint32_t type, flags;
        if (!rd_at(fd, phoff + (uint64_t)i * phentsize, p, 56)) return 0;
        memcpy(&type, p, 4); memcpy(&flags, p + 4, 4);
        if (type != 1) continue;
        memcpy(&ph[*nph][0], p + 8, 8); memcpy(&ph[*nph][1], p + 16, 8); memcpy(&ph[*nph][2], p + 32, 8);
        ph[*nph][3] = flags;
        (*nph)++;
    }
    for (i = 0; i < shnum; i++) {
        uint8_t s[64], l[64];
        uint32_t type, link;
        uint64_t off, size, loff, lsize, k;
        char *str;
        uint8_t *syms;
        if (!rd_at(fd, shoff + (uint64_t)i * shentsize, s, 64)) return 0;
        memcpy(&type, s + 4, 4);
        if (type != 11) continue;                                  /* SHT_DYNSYM */
        memcpy(&off, s + 24, 8); memcpy(&size, s + 32, 8); memcpy(&link, s + 40, 4);
        if (!rd_at(fd, shoff + (uint64_t)link * shentsize, l, 64)) return 0;
        memcpy(&loff, l + 24, 8); memcpy(&lsize, l + 32, 8);
        if (size > (64u << 20) || lsize > (64u << 20)) return 0;
        syms = malloc(size); str = malloc(lsize + 1);
        if (syms && str && rd_at(fd, off, syms, size) && rd_at(fd, loff, str, lsize)) {
            str[lsize] = 0;
            for (k = 0; k + 24 <= size; k += 24) {
                uint32_t name;
                uint8_t info = syms[k + 4];
                uint16_t shndx;
                uint64_t value;
                int f;
                memcpy(&name, syms + k, 4); memcpy(&shndx, syms + k + 6, 2); memcpy(&value, syms + k + 8, 8);
                if (!shndx || !value || name >= lsize || ((info & 0xf) != 2 && (info & 0xf) != 10)) continue;
                for (f = 0; f < NF; f++)
                    if (!strcmp(str + name, names[f]) && !out[f].value) { out[f].value = value; out[f].ifunc = (info & 0xf) == 10; ok = 1; }
            }
        }
        free(syms); free(str);
        break;
    }
    return ok;
}

uint64_t aoi_lxfast_mapped(struct aoi_proc *p, uint64_t a, uint64_t len, uint64_t off, int fd, const char *path)
{
    static const uint32_t jump_back[3] = { 0x58000070u, 0xd61f0200u, 0xd503201fu };   /* ldr x16, .+12; br x16; nop */
    struct sym s[NF];
    uint64_t ph[16][4], P, done[NF];
    uint8_t *pg;
    size_t n = strlen(path);
    int nph, f, k, routed = 0, nd = 0;
    const char *e = getenv("AOI_LXFAST");
    if ((e && *e == '0') || !p->linux_mode || p->cpu.thunk_slots) return 0;     /* (one libc per process) */
    if (n < 10 || strcmp(path + n - 10, "/libc.so.6")) return 0;
    if (!find_syms(fd, s, ph, &nph)) return 0;
    P = aoi_vm_map(&p->vm, (AOI_TBI(a) + len + 0xffff) & ~0xffffULL, AOI_VM_PAGE, AOI_PROT_R | AOI_PROT_X, 0);
    if ((int64_t)P < 0 || !(pg = aoi_vm_ptr(&p->vm, P, AOI_VM_PAGE, 0))) return 0;
    memset(pg, 0, AOI_VM_PAGE);
    for (f = 0; f < NF; f++) {
        uint64_t v = s[f].value, fo = 0, g, d;
        uint32_t w, b;
        uint8_t *at;
        int j = s[f].ifunc ? NF + f : f, dup = 0;
        if (!v) continue;
        for (k = 0; k < nph; k++)
            if (v >= ph[k][1] && v - ph[k][1] < ph[k][2] && (ph[k][3] & 1)) { fo = v - ph[k][1] + ph[k][0]; break; }
        if (k == nph || fo < off || fo - off + 4 > len) continue;
        g = a + (fo - off);
        for (k = 0; k < nd; k++) if (done[k] == g) dup = 1;        /* (two names, one entry: route it once) */
        if (dup || !(at = aoi_vm_ptr(&p->vm, g, 4, 0))) continue;
        memcpy(&w, at, 4);
        d = P + 4 * (s[f].ifunc ? SLOT_R(f) : SLOT_F(f)) - g;
        if ((int64_t)d >= (1LL << 27) || (int64_t)d < -(1LL << 27)) continue;
        if ((w & 0x1f000000u) == 0x10000000u) {                    /* adr/adrp (resolvers start so): its value as a literal */
            int64_t imm = (int64_t)(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3));
            uint64_t val, back = g + 4;
            uint32_t ld = 0x58000080u | (w & 31), ld16 = 0x580000b0u;   /* ldr xd, .+16; ldr x16, .+20 */
            imm = (imm ^ (1 << 20)) - (1 << 20);
            val = (w >> 31) ? (g & ~0xfffULL) + ((uint64_t)imm << 12) : g + (uint64_t)imm;
            memcpy(pg + STUB(j), &ld, 4); memcpy(pg + STUB(j) + 4, &ld16, 4);
            memcpy(pg + STUB(j) + 8, jump_back + 1, 8);           /* br x16; nop */
            memcpy(pg + STUB(j) + 16, &val, 8); memcpy(pg + STUB(j) + 24, &back, 8);
        } else if (pc_relative(w)) {
            continue;
        } else {
            uint64_t back = g + 4;
            memcpy(pg + STUB(j), &w, 4);                           /* stub: its first instruction, then back */
            memcpy(pg + STUB(j) + 4, jump_back, 12);
            memcpy(pg + STUB(j) + 16, &back, 8);
        }
        b = 0x14000000u | (uint32_t)((d >> 2) & 0x3ffffff);        /* b <thunk> */
        memcpy(at, &b, 4);
        done[nd++] = g;
        routed++;
    }
    if (!routed) { aoi_vm_unmap(&p->vm, P, AOI_VM_PAGE); return 0; }
    for (k = -1; k < AOI_PROC_THREADS; k++) {
        struct aoi_cpu *c = k < 0 ? &p->cpu : &p->th[k].cpu;
        c->thunk_base = P;
        c->thunk_slots = SLOTS;
        c->host_call = aoi_lxfast_call;
    }
    return P;
}
