#include "dl.h"

#include <stdlib.h>
#include <string.h>

#define PAGE        0x4000
#define THUNK_BASE  0x10000000ULL                 /* host-call slots */
#define LIB_BASE    0x40000000ULL                 /* first library */
#define HEAP_BASE   0x6000000000ULL
#define HEAP_SIZE   (64ULL << 20)
#define STACK_TOP   0x7000000000ULL
#define STACK_SIZE  (8ULL << 20)

#define SHT_DYNSYM 11
#define R_AARCH64_ABS64     257
#define R_AARCH64_GLOB_DAT  1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE  1027

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }
static uint64_t roundup(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

uint8_t *aoi_dl_map(struct aoi_dl *dl, uint64_t base, uint64_t size)
{
    struct aoi_region *r;
    uint8_t *host;
    if (dl->mem.n == AOI_MAX_REGIONS) return NULL;
    /* page-aligned so the same host memory can back a reference CPU (core/oracle.c) */
    if (!(host = aligned_alloc(PAGE, roundup(size, PAGE)))) return NULL;
    memset(host, 0, size);
    r = &dl->mem.r[dl->mem.n++];
    r->base = base; r->size = size; r->host = host;
    dl->blocks[dl->nblocks++] = host;
    return host;
}

const char *aoi_dl_init(struct aoi_dl *dl, const struct aoi_host_sym *host, int nhost)
{
    memset(dl, 0, sizeof(*dl));
    dl->host = host;
    dl->nhost = nhost;
    dl->thunk_base = THUNK_BASE;
    dl->nslots = 1;                                   /* slot 0 = return-to-host */
    /* the thunk page is never executed, but keep it mapped so reads don't fault */
    if (!aoi_dl_map(dl, THUNK_BASE, roundup(4 * AOI_MAX_SLOTS, PAGE))) return "no memory";
    if (!aoi_dl_map(dl, HEAP_BASE, HEAP_SIZE)) return "no memory for heap";
    dl->heap_base = dl->heap_top = HEAP_BASE;
    dl->heap_end = HEAP_BASE + HEAP_SIZE;
    if (!aoi_dl_map(dl, STACK_TOP - STACK_SIZE, STACK_SIZE)) return "no memory for stack";
    dl->stack_top = STACK_TOP;
    dl->next_base = LIB_BASE;
    return NULL;
}

/* 16-byte header holding the size, so realloc knows how much to copy. */
uint64_t aoi_dl_malloc(struct aoi_dl *dl, uint64_t size)
{
    uint64_t p = roundup(dl->heap_top, 16), total = roundup(size, 16) + 16;
    uint8_t *h;
    if (p + total > dl->heap_end) return 0;
    dl->heap_top = p + total;
    h = aoi_mem_ptr(&dl->mem, p, 16);
    memcpy(h, &size, 8);
    return p + 16;
}

static const char *sym_name(const struct aoi_lib *l, uint64_t i, const uint8_t **sym)
{
    const uint8_t *s = l->data + l->symtab + i * 24;
    if (sym) *sym = s;
    return (const char *)(l->data + l->strtab + u32(s));
}

/* Exported definition in one library: 0 if absent. */
static uint64_t lib_lookup(const struct aoi_lib *l, const char *name)
{
    uint64_t i;
    for (i = 1; i < l->nsyms; i++) {
        const uint8_t *s;
        const char *n = sym_name(l, i, &s);
        uint16_t shndx = u16(s + 6);
        uint8_t bind = s[4] >> 4;
        if (shndx != 0 && (bind == 1 || bind == 2) && !strcmp(n, name))
            return l->base + u64(s + 8);
    }
    return 0;
}

uint64_t aoi_dl_sym(struct aoi_dl *dl, const char *name)
{
    int i;
    for (i = 0; i < dl->nlibs; i++) {
        uint64_t a = lib_lookup(&dl->lib[i], name);
        if (a) return a;
    }
    return 0;
}

/* Resolve an import: loaded libraries, then host table, else a named stop slot. */
static uint64_t resolve(struct aoi_dl *dl, const char *name)
{
    uint64_t a = aoi_dl_sym(dl, name);
    unsigned s;
    int i;
    if (a) return a;
    for (s = 1; s < dl->nslots; s++)
        if (!strcmp(dl->slot_name[s], name)) return dl->thunk_base + 4 * s;
    for (i = 0; i < dl->nhost; i++)
        if (!strcmp(dl->host[i].name, name) && dl->host[i].data_addr) return dl->host[i].data_addr;
    if (dl->nslots == AOI_MAX_SLOTS) return 0;
    s = dl->nslots++;
    dl->slot_name[s] = name;
    dl->slot_fn[s] = NULL;                               /* missing unless the host has it */
    for (i = 0; i < dl->nhost; i++)
        if (!strcmp(dl->host[i].name, name)) { dl->slot_fn[s] = dl->host[i].fn; break; }
    return dl->thunk_base + 4 * s;
}

static const char *apply_rela(struct aoi_dl *dl, struct aoi_lib *l, uint64_t off, uint64_t sz)
{
    uint64_t i;
    for (i = 0; i + 24 <= sz; i += 24) {
        const uint8_t *r = l->data + off + i;
        uint64_t where = l->base + u64(r), info = u64(r + 8), symi = info >> 32;
        uint32_t type = (uint32_t)info;
        int64_t addend = (int64_t)u64(r + 16);
        uint64_t val, S = 0;
        uint8_t *p = aoi_mem_ptr(&dl->mem, where, 8);
        if (!p) return "relocation outside the image";
        if (symi) {
            const uint8_t *s;
            const char *n = sym_name(l, symi, &s);
            S = u16(s + 6) ? l->base + u64(s + 8) : resolve(dl, n);
            if (!S && u16(s + 6) == 0 && (s[4] >> 4) != 2) return "import table full";
        }
        switch (type) {
        case R_AARCH64_RELATIVE:  val = l->base + (uint64_t)addend; break;
        case R_AARCH64_ABS64:     val = S + (uint64_t)addend; break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT: val = S + (uint64_t)addend; break;
        default: return "unsupported relocation type";
        }
        memcpy(p, &val, 8);
    }
    return NULL;
}

const char *aoi_dl_load(struct aoi_dl *dl, const char *name, const void *data, size_t size)
{
    struct aoi_elf elf;
    struct aoi_lib *l;
    const uint8_t *d = data;
    const char *err;
    uint64_t span, shoff, i, dyn_off = 0, dyn_sz = 0;
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
    uint16_t shnum, shentsize, phnum;
    uint8_t *host;
    int k;

    if ((err = aoi_elf_parse(&elf, data, size))) return err;
    if (!elf.is_shared) return "not a shared object";
    if (dl->nlibs == AOI_MAX_LIBS) return "too many libraries";
    l = &dl->lib[dl->nlibs];
    memset(l, 0, sizeof(*l));
    strncpy(l->name, name, sizeof(l->name) - 1);
    l->data = d; l->size = size;
    l->base = dl->next_base;

    span = roundup(elf.max_vaddr, PAGE);
    if (!(host = aoi_dl_map(dl, l->base, span))) return "no memory for library";
    for (k = 0; k < elf.nseg; k++)
        memcpy(host + elf.seg[k].vaddr, d + elf.seg[k].offset, elf.seg[k].filesz);
    dl->next_base = roundup(l->base + span + PAGE, 0x1000000);

    /* .dynsym from the section headers (Android .so files keep them) */
    shoff = u64(d + 40); shentsize = u16(d + 58); shnum = u16(d + 60);
    for (i = 0; i < shnum; i++) {
        const uint8_t *sh = d + shoff + i * shentsize;
        if (shoff + (i + 1) * shentsize > size) return "section headers out of range";
        if (u32(sh + 4) == SHT_DYNSYM) {
            const uint8_t *strsh = d + shoff + (uint64_t)u32(sh + 40) * shentsize;
            l->symtab = u64(sh + 24);
            l->nsyms = u64(sh + 32) / 24;
            l->strtab = u64(strsh + 24);
        }
    }
    if (!l->nsyms) return "no .dynsym";

    /* PT_DYNAMIC → RELA / JMPREL */
    phnum = u16(d + 56);
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = d + u64(d + 32) + i * u16(d + 54);
        if (u32(ph) == 2) { dyn_off = u64(ph + 8); dyn_sz = u64(ph + 32); }
    }
    for (i = 0; i + 16 <= dyn_sz; i += 16) {
        uint64_t tag = u64(d + dyn_off + i), val = u64(d + dyn_off + i + 8);
        if (tag == 7) rela = val;           /* DT_RELA: a vaddr; == file offset in these .so */
        else if (tag == 8) relasz = val;
        else if (tag == 23) jmprel = val;
        else if (tag == 2) pltrelsz = val;
        else if (tag == 0) break;
    }
    dl->nlibs++;
    if (rela && (err = apply_rela(dl, l, rela, relasz))) return err;
    if (jmprel && (err = apply_rela(dl, l, jmprel, pltrelsz))) return err;
    return NULL;
}

static uint64_t dl_host_call(struct aoi_cpu *cpu, unsigned slot)
{
    struct aoi_dl *dl = cpu->host_ctx;
    if (slot >= dl->nslots || !dl->slot_fn[slot]) {
        cpu->stop = AOI_STOP_IMPORT;
        cpu->stop_name = slot < dl->nslots ? dl->slot_name[slot] : "?";
        return 0;
    }
    return dl->slot_fn[slot](cpu);
}

void aoi_dl_cpu(struct aoi_dl *dl, struct aoi_cpu *cpu)
{
    memset(cpu, 0, sizeof(*cpu));
    cpu->mem = &dl->mem;
    cpu->sp = dl->stack_top - 64;
    cpu->thunk_base = dl->thunk_base;
    cpu->thunk_slots = AOI_MAX_SLOTS;
    cpu->host_call = dl_host_call;
    cpu->host_ctx = dl;
}
