#include "native.h"
#include "elf.h"

#include <string.h>

#define PAGE 0x4000
#define SHT_DYNSYM 11
#define R_AARCH64_ABS64     257
#define R_AARCH64_GLOB_DAT  1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE  1027

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }
static uint64_t rup(uint64_t v) { return (v + PAGE - 1) & ~(uint64_t)(PAGE - 1); }

const char *aoi_native_layout(const void *so, size_t size, struct aoi_native_layout *out)
{
    struct aoi_elf elf;
    const char *err;
    uint64_t text_end = 0, first_w = ~0ULL;
    int k;
    if ((err = aoi_elf_parse(&elf, so, size))) return err;
    if (!elf.is_shared) return "not a shared object";
    for (k = 0; k < elf.nseg; k++) {
        if (elf.seg[k].exec && elf.seg[k].vaddr + elf.seg[k].memsz > text_end) text_end = elf.seg[k].vaddr + elf.seg[k].memsz;
        if (elf.seg[k].write && elf.seg[k].vaddr < first_w) first_w = elf.seg[k].vaddr;
    }
    out->span = rup(elf.max_vaddr);
    out->text_end = rup(text_end);
    out->text_ok = first_w >= out->text_end;
    return NULL;
}

/* .dynsym and .dynstr file offsets from the section headers */
static int dynsym(const uint8_t *d, size_t size, uint64_t *symtab, uint64_t *nsyms, uint64_t *strtab)
{
    uint64_t shoff = u64(d + 40), i;
    uint16_t shentsize = u16(d + 58), shnum = u16(d + 60);
    for (i = 0; i < shnum; i++) {
        const uint8_t *sh = d + shoff + i * shentsize;
        if (shoff + (i + 1) * shentsize > size) return 0;
        if (u32(sh + 4) == SHT_DYNSYM) {
            *symtab = u64(sh + 24);
            *nsyms = u64(sh + 32) / 24;
            *strtab = u64(d + shoff + (uint64_t)u32(sh + 40) * shentsize + 24);
            return 1;
        }
    }
    return 0;
}

static const char *rela(const uint8_t *d, uint8_t *buf, uint8_t *base, uint64_t off, uint64_t sz, uint64_t symtab,
                        uint64_t strtab, aoi_native_resolve resolve, void *ctx, const char **missing)
{
    uint64_t i;
    for (i = 0; i + 24 <= sz; i += 24) {
        const uint8_t *r = d + off + i;
        uint64_t where = u64(r), info = u64(r + 8), symi = info >> 32, S = 0, val;
        uint32_t type = (uint32_t)info;
        int64_t addend = (int64_t)u64(r + 16);
        if (symi) {
            const uint8_t *s = d + symtab + symi * 24;
            const char *name = (const char *)d + strtab + u32(s);
            if (u16(s + 6)) S = (uint64_t)(uintptr_t)base + u64(s + 8);      /* defined here */
            else if (!(S = (uint64_t)(uintptr_t)resolve(name, ctx)) && (s[4] >> 4) != 2) {  /* not weak */
                *missing = name;
                return "missing import";
            }
        }
        switch (type) {
        case R_AARCH64_RELATIVE: val = (uint64_t)(uintptr_t)base + (uint64_t)addend; break;
        case R_AARCH64_ABS64: case R_AARCH64_GLOB_DAT: case R_AARCH64_JUMP_SLOT: val = S + (uint64_t)addend; break;
        default: return "unsupported relocation type";
        }
        memcpy(buf + where, &val, 8);
    }
    return NULL;
}

const char *aoi_native_link(const void *so, size_t size, uint8_t *buf, uint8_t *base,
                            aoi_native_resolve resolve, void *ctx, const char **missing)
{
    struct aoi_elf elf;
    const uint8_t *d = so;
    const char *err;
    uint64_t symtab, nsyms, strtab, i, dyn_off = 0, dyn_sz = 0;
    uint64_t rela_off = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
    uint16_t phnum = u16(d + 56);
    int k;

    *missing = NULL;
    if ((err = aoi_elf_parse(&elf, so, size))) return err;
    for (k = 0; k < elf.nseg; k++)
        memcpy(buf + elf.seg[k].vaddr, d + elf.seg[k].offset, elf.seg[k].filesz);
    if (!dynsym(d, size, &symtab, &nsyms, &strtab)) return "no .dynsym";
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = d + u64(d + 32) + i * u16(d + 54);
        if (u32(ph) == 2) { dyn_off = u64(ph + 8); dyn_sz = u64(ph + 32); }
    }
    for (i = 0; i + 16 <= dyn_sz; i += 16) {
        uint64_t tag = u64(d + dyn_off + i), val = u64(d + dyn_off + i + 8);
        if (tag == 7) rela_off = val; else if (tag == 8) relasz = val;
        else if (tag == 23) jmprel = val; else if (tag == 2) pltrelsz = val;
        else if (tag == 0) break;
    }
    if (rela_off && (err = rela(d, buf, base, rela_off, relasz, symtab, strtab, resolve, ctx, missing))) return err;
    if (jmprel && (err = rela(d, buf, base, jmprel, pltrelsz, symtab, strtab, resolve, ctx, missing))) return err;
    (void)nsyms;
    return NULL;
}

void *aoi_native_sym(const void *so, size_t size, uint8_t *base, const char *name)
{
    const uint8_t *d = so;
    uint64_t symtab, nsyms, strtab, i;
    if (!dynsym(d, size, &symtab, &nsyms, &strtab)) return NULL;
    for (i = 1; i < nsyms; i++) {
        const uint8_t *s = d + symtab + i * 24;
        uint8_t bind = s[4] >> 4;
        if (u16(s + 6) && (bind == 1 || bind == 2) && !strcmp((const char *)d + strtab + u32(s), name))
            return base + u64(s + 8);
    }
    return NULL;
}
