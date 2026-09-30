/* Load a static ET_EXEC / ET_DYN AArch64 ELF into a fresh guest address space
 * and set up an initial stack (argc/argv/envp/auxv), ready for aoi_cpu_run. */
#ifndef AOI_LOAD_H
#define AOI_LOAD_H

#include "cpu.h"
#include "elf.h"

struct aoi_image {
    struct aoi_mem mem;
    uint8_t *blocks[AOI_MAX_REGIONS];   /* owned host allocations to free */
    int nblocks;
    uint64_t entry;
    uint64_t sp;
};

/* base: where a PIE (ET_DYN) is placed; ignored for ET_EXEC. */
const char *aoi_load(struct aoi_image *img, const struct aoi_elf *elf, uint64_t base,
                     int argc, const char *const argv[]);
void aoi_image_free(struct aoi_image *img);

#endif
