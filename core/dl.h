/* A small dynamic linker for Android arm64 shared objects.
 *
 * Loads .so files into one guest address space, applies RELA relocations
 * (RELATIVE, ABS64, GLOB_DAT, JUMP_SLOT) and resolves imports: first against
 * the libraries already loaded, then against host functions (the bionic shim).
 * An import nobody provides is bound to a slot that stops the CPU with its name,
 * so a missing libc function shows up by name instead of as a crash. */
#ifndef AOI_DL_H
#define AOI_DL_H

#include "cpu.h"
#include "elf.h"

#define AOI_MAX_LIBS 16
#define AOI_MAX_SLOTS 1024

struct aoi_lib {
    char name[64];
    uint64_t base;
    const uint8_t *data;        /* file image (kept for symbol lookup) */
    size_t size;
    uint64_t symtab, strtab;    /* file offsets of .dynsym / .dynstr */
    uint64_t nsyms;
};

typedef uint64_t (*aoi_host_fn)(struct aoi_cpu *cpu);

struct aoi_host_sym {
    const char *name;
    aoi_host_fn fn;             /* function import */
    uint64_t data_addr;         /* or a data import (e.g. stdout): its guest address */
};

struct aoi_dl {
    struct aoi_mem mem;
    uint8_t *blocks[AOI_MAX_REGIONS];
    int nblocks;
    struct aoi_lib lib[AOI_MAX_LIBS];
    int nlibs;
    /* host table: slot i >= 1 */
    const struct aoi_host_sym *host;
    int nhost;
    const char *slot_name[AOI_MAX_SLOTS];
    aoi_host_fn slot_fn[AOI_MAX_SLOTS];
    unsigned nslots;
    uint64_t thunk_base;
    uint64_t next_base;         /* where the next library goes */
    uint64_t heap_base, heap_top, heap_end;
    uint64_t stack_top;
};

/* Sets up the address space: thunk page, heap and stack. */
const char *aoi_dl_init(struct aoi_dl *dl, const struct aoi_host_sym *host, int nhost);
/* Maps a guest region backed by fresh zeroed host memory. */
uint8_t *aoi_dl_map(struct aoi_dl *dl, uint64_t base, uint64_t size);
/* Loads and links one library (dependencies must be loaded first). */
const char *aoi_dl_load(struct aoi_dl *dl, const char *name, const void *data, size_t size);
/* Guest address of an exported symbol, 0 if absent. */
uint64_t aoi_dl_sym(struct aoi_dl *dl, const char *name);
/* Wires a CPU to this address space (memory, thunks, stack). */
void aoi_dl_cpu(struct aoi_dl *dl, struct aoi_cpu *cpu);
/* Guest heap, used by the bionic shim. */
uint64_t aoi_dl_malloc(struct aoi_dl *dl, uint64_t size);

#endif
