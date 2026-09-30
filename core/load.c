#include "load.h"

#include <stdlib.h>
#include <string.h>

#define PAGE 0x4000                 /* iOS page size; the guest's working granule */
#define STACK_SIZE (256 * 1024)
#define STACK_TOP  0x7000000000ULL

static uint64_t roundup(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
static uint64_t rounddown(uint64_t v, uint64_t a) { return v & ~(a - 1); }

static uint8_t *add_region(struct aoi_image *img, uint64_t base, uint64_t size)
{
    struct aoi_region *r;
    uint8_t *host;
    if (img->mem.n == AOI_MAX_REGIONS) return NULL;
    host = calloc(1, size);
    if (!host) return NULL;
    r = &img->mem.r[img->mem.n++];
    r->base = base; r->size = size; r->host = host;
    img->blocks[img->nblocks++] = host;
    return host;
}

const char *aoi_load(struct aoi_image *img, const struct aoi_elf *elf, uint64_t base,
                     int argc, const char *const argv[])
{
    uint64_t bias = elf->is_shared ? base : 0;
    uint64_t lo = rounddown(elf->min_vaddr + bias, PAGE);
    uint64_t hi = roundup(elf->max_vaddr + bias, PAGE);
    uint8_t *img_host;
    uint8_t *stack;
    uint64_t sp;
    int i;

    memset(img, 0, sizeof(*img));

    /* One contiguous region covering all PT_LOAD segments. */
    img_host = add_region(img, lo, hi - lo);
    if (!img_host) return "out of memory for image";
    for (i = 0; i < elf->nseg; i++) {
        const struct aoi_segment *s = &elf->seg[i];
        memcpy(img_host + (s->vaddr + bias - lo), elf->data + s->offset, s->filesz);
    }
    img->entry = elf->entry + bias;

    /* Initial stack: [argc][argv...][NULL][envp NULL][auxv AT_NULL]. */
    stack = add_region(img, STACK_TOP - STACK_SIZE, STACK_SIZE);
    if (!stack) return "out of memory for stack";
    {
        /* Lay strings at the top, then the pointer array below. */
        uint64_t strtop = STACK_TOP;
        uint64_t *ptrs = calloc((size_t)argc + 8, sizeof(uint64_t));
        int np = 0, k;
        uint64_t argp[64];
        if (!ptrs) return "out of memory";
        for (k = argc - 1; k >= 0; k--) {
            size_t n = strlen(argv[k]) + 1;
            strtop -= n;
            memcpy(stack + (strtop - (STACK_TOP - STACK_SIZE)), argv[k], n);
            argp[k] = strtop;
        }
        sp = rounddown(strtop, 16);
        /* auxv: AT_NULL (0,0) */
        sp -= 16;
        /* envp: just NULL */
        sp -= 8;
        /* argv NULL terminator */
        sp -= 8;
        /* argv pointers */
        sp -= (uint64_t)argc * 8;
        for (k = 0; k < argc; k++) ptrs[np++] = argp[k];
        /* argc */
        sp -= 8;
        {
            uint8_t *base_host = stack + (sp - (STACK_TOP - STACK_SIZE));
            uint64_t v = (uint64_t)argc, cur = sp;
            memcpy(stack + (cur - (STACK_TOP - STACK_SIZE)), &v, 8); cur += 8;
            for (k = 0; k < argc; k++) { memcpy(stack + (cur - (STACK_TOP - STACK_SIZE)), &argp[k], 8); cur += 8; }
            v = 0; memcpy(stack + (cur - (STACK_TOP - STACK_SIZE)), &v, 8); cur += 8; /* argv NULL */
            memcpy(stack + (cur - (STACK_TOP - STACK_SIZE)), &v, 8); cur += 8;        /* envp NULL */
            memcpy(stack + (cur - (STACK_TOP - STACK_SIZE)), &v, 8); cur += 8;        /* auxv AT_NULL key */
            memcpy(stack + (cur - (STACK_TOP - STACK_SIZE)), &v, 8); cur += 8;        /* auxv AT_NULL val */
            (void)base_host;
        }
        free(ptrs);
    }
    img->sp = sp;
    return NULL;
}

void aoi_image_free(struct aoi_image *img)
{
    int i;
    for (i = 0; i < img->nblocks; i++) free(img->blocks[i]);
    img->nblocks = 0;
    img->mem.n = 0;
}
