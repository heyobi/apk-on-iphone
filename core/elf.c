#include "elf.h"

#include <string.h>

#define PT_LOAD    1
#define PF_X       1
#define PF_W       2
#define ET_EXEC    2
#define ET_DYN     3
#define EM_AARCH64 183

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

const char *aoi_elf_parse(struct aoi_elf *elf, const void *data, size_t size)
{
    const uint8_t *d = data;
    uint64_t phoff;
    uint16_t type, phentsize, phnum;
    int i;

    memset(elf, 0, sizeof(*elf));
    elf->data = d;
    elf->size = size;

    if (size < 64 || memcmp(d, "\177ELF", 4)) return "not an ELF file";
    if (d[4] != 2) return "not ELF64";
    if (d[5] != 1) return "not little-endian";
    if (rd16(d + 18) != EM_AARCH64) return "not AArch64";

    type = rd16(d + 16);
    if (type != ET_EXEC && type != ET_DYN) return "not an executable or shared object";
    elf->is_shared = type == ET_DYN;
    elf->entry = rd64(d + 24);

    phoff = rd64(d + 32);
    phentsize = rd16(d + 54);
    phnum = rd16(d + 56);
    if (phentsize < 56) return "bad program header size";
    if (phoff > size || (uint64_t)phnum * phentsize > size - phoff) return "program headers out of range";

    elf->min_vaddr = UINT64_MAX;
    elf->min_align = UINT64_MAX;
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = d + phoff + (uint64_t)i * phentsize;
        struct aoi_segment *s;
        uint32_t flags;

        if (rd32(ph) != PT_LOAD) continue;
        if (elf->nseg == AOI_MAX_SEGMENTS) return "too many PT_LOAD segments";
        s = &elf->seg[elf->nseg++];
        flags     = rd32(ph + 4);
        s->offset = rd64(ph + 8);
        s->vaddr  = rd64(ph + 16);
        s->filesz = rd64(ph + 32);
        s->memsz  = rd64(ph + 40);
        s->align  = rd64(ph + 48);
        s->exec   = !!(flags & PF_X);
        s->write  = !!(flags & PF_W);

        if (s->offset > size || s->filesz > size - s->offset) return "segment data out of range";
        if (s->filesz > s->memsz) return "segment filesz > memsz";
        if (s->vaddr < elf->min_vaddr) elf->min_vaddr = s->vaddr;
        if (s->vaddr + s->memsz > elf->max_vaddr) elf->max_vaddr = s->vaddr + s->memsz;
        if (s->align < elf->min_align) elf->min_align = s->align;
    }
    if (!elf->nseg) return "no PT_LOAD segments";
    return NULL;
}
