/* Minimal ELF64 / AArch64 reader: enough to plan loading a Linux (bionic)
 * executable or shared object into an iOS process. No libc beyond string.h. */
#ifndef AOI_ELF_H
#define AOI_ELF_H

#include <stddef.h>
#include <stdint.h>

#define AOI_MAX_SEGMENTS 16

struct aoi_segment {
    uint64_t vaddr, memsz;      /* where it goes */
    uint64_t offset, filesz;    /* where it comes from */
    uint64_t align;
    int      exec, write;
};

struct aoi_elf {
    const uint8_t *data;
    size_t size;
    int is_shared;              /* ET_DYN (.so or PIE) vs ET_EXEC */
    uint64_t entry;
    int nseg;
    struct aoi_segment seg[AOI_MAX_SEGMENTS];
    uint64_t min_vaddr, max_vaddr;  /* page-rounded span of all PT_LOAD */
    uint64_t min_align;             /* smallest PT_LOAD alignment */
};

/* Returns NULL on success, or a static string naming the first problem. */
const char *aoi_elf_parse(struct aoi_elf *elf, const void *data, size_t size);

#endif
