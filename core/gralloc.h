/* Graphics buffers (gralloc) shared by the host allocator (core/gralloc.c) and the
 * guest mapper library (guest/mapper.c, loaded by libui as mapper.aoi.so).
 *
 * One process: a buffer's pixels are ordinary guest memory the host maps, followed
 * by one page of mutable metadata. Its native_handle carries one placeholder fd and
 * AOI_GB_INTS ints that say everything about it, so the mapper needs no lookups. */
#ifndef AOI_GRALLOC_H
#define AOI_GRALLOC_H

#define AOI_GB_MAGIC   0x47494f41u          /* 'AOIG' */
#define AOI_GB_SUFFIX  "aoi"                /* libui loads mapper.<suffix>.so */

/* native_handle ints */
enum {
    AOI_GB_I_MAGIC, AOI_GB_I_ID, AOI_GB_I_WIDTH, AOI_GB_I_HEIGHT, AOI_GB_I_STRIDE /* pixels */,
    AOI_GB_I_FORMAT /* as requested */, AOI_GB_I_LAYERS, AOI_GB_I_USAGE_LO, AOI_GB_I_USAGE_HI,
    AOI_GB_I_ADDR_LO, AOI_GB_I_ADDR_HI, AOI_GB_I_SIZE /* pixel bytes */, AOI_GB_I_BPP,
    AOI_GB_INTS
};

/* The metadata page (after the pixels, at a page boundary). */
struct aoi_gb_meta {
    unsigned magic;
    int dataspace, blend_mode;
    unsigned name_len;
    char name[128];
};

/* A private syscall the mapper makes (x8 = AOI_SYS_GRALLOC, x0 = op, x1 = id):
 * a reference to a buffer for each imported handle, so the host frees it with the
 * last one. 0, or -EINVAL for a buffer it does not know. */
#define AOI_SYS_GRALLOC 0x4f49
#define AOI_GB_RETAIN 1
#define AOI_GB_RELEASE 2

#endif
