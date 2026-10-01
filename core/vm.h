/* Sparse guest address space.
 *
 * Guest virtual addresses [0, size) are described by a per-page table of the
 * guest's own R/W/X, so a stray access becomes a guest fault, never a host crash.
 * Host memory backs the space in 2 MiB chunks that exist only while some page in
 * them is accessible: reserving gigabytes PROT_NONE (scudo does 8+ GiB at start)
 * costs a page-table entry, not address space. iOS grants a process at most a
 * few GiB of contiguous reservation (6 GiB measured on an iPhone 16 Pro, iOS 27),
 * which is why the space is not one host mapping.
 *
 * Invariant: every page with any of R/W/X has its chunk allocated, so an access
 * is one prot lookup plus one chunk lookup. Host pointers are only contiguous
 * within a chunk: aoi_vm_ptr() refuses ranges that cross one; use
 * aoi_vm_read/aoi_vm_write/aoi_vm_span for anything larger. */
#ifndef AOI_VM_H
#define AOI_VM_H

#include <stddef.h>
#include <stdint.h>

#define AOI_VM_PAGE 4096u           /* the guest's page size (AT_PAGESZ) */
#define AOI_VM_CHUNK_SHIFT 21       /* 2 MiB host chunks */
#define AOI_VM_CHUNK (1ULL << AOI_VM_CHUNK_SHIFT)
#define AOI_PROT_R 1
#define AOI_PROT_W 2
#define AOI_PROT_X 4

struct aoi_vm {
    uint8_t **chunk;                /* host memory per 2 MiB of guest space, or NULL */
    uint64_t size;                  /* bytes of guest address space */
    uint8_t *prot;                  /* one byte per guest page; 0 = unmapped */
    uint64_t hint;                  /* where the next non-fixed mapping is searched from */
    uint64_t nchunks;               /* chunks currently allocated (diagnostics) */
};

const char *aoi_vm_init(struct aoi_vm *vm, uint64_t size);
void aoi_vm_free(struct aoi_vm *vm);

/* mmap: with fixed, exactly at addr (replacing what is there); otherwise the
 * first free range at or after addr (or the hint). Memory comes back zeroed.
 * Returns the guest address, or (uint64_t)-errno. */
uint64_t aoi_vm_map(struct aoi_vm *vm, uint64_t addr, uint64_t len, int prot, int fixed);
int aoi_vm_unmap(struct aoi_vm *vm, uint64_t addr, uint64_t len);       /* 0 or -errno */
int aoi_vm_protect(struct aoi_vm *vm, uint64_t addr, uint64_t len, int prot);

/* Host pointer for [addr, addr+len) if every page has `need` access (0: mapped at
 * all, with host memory) and the range lies in one chunk; else NULL. */
uint8_t *aoi_vm_ptr(struct aoi_vm *vm, uint64_t addr, uint64_t len, int need);

/* Host pointer for the longest prefix of [addr, addr+len) inside one chunk, its
 * length in *n; NULL if the first byte is not accessible with `need`. */
uint8_t *aoi_vm_span(struct aoi_vm *vm, uint64_t addr, uint64_t len, int need, uint64_t *n);

/* Copies across chunks. 1 on success, 0 if any page lacks `need` access. */
int aoi_vm_read(struct aoi_vm *vm, uint64_t addr, void *dst, uint64_t len, int need);
int aoi_vm_write(struct aoi_vm *vm, uint64_t addr, const void *src, uint64_t len, int need);

/* Zeroes the mapped pages of a range (madvise DONTNEED). */
void aoi_vm_zero(struct aoi_vm *vm, uint64_t addr, uint64_t len);

#endif
