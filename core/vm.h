/* Flat guest address space.
 *
 * One host reservation backs guest virtual addresses [0, size): guest address
 * a lives at host + a. Pages the guest has not mapped are inaccessible on the
 * host too, and a per-page table records the guest's own R/W/X, so a stray
 * access becomes a guest fault, never a host crash.
 *
 * The space is kept below 4 GiB on purpose: ART stores object references as
 * 32-bit addresses, which Darwin cannot give a native process but which every
 * address of this guest is. */
#ifndef AOI_VM_H
#define AOI_VM_H

#include <stddef.h>
#include <stdint.h>

#define AOI_VM_PAGE 4096u           /* the guest's page size (AT_PAGESZ) */
#define AOI_PROT_R 1
#define AOI_PROT_W 2
#define AOI_PROT_X 4

struct aoi_vm {
    uint8_t *host;
    uint64_t size;                  /* bytes of guest address space */
    uint8_t *prot;                  /* one byte per guest page; 0 = unmapped */
    uint64_t hint;                  /* where the next non-fixed mapping is searched from */
};

const char *aoi_vm_init(struct aoi_vm *vm, uint64_t size);
void aoi_vm_free(struct aoi_vm *vm);

/* mmap: with fixed, exactly at addr (replacing what is there); otherwise the
 * first free range at or after the hint. Memory comes back zeroed. Returns the
 * guest address, or (uint64_t)-errno. */
uint64_t aoi_vm_map(struct aoi_vm *vm, uint64_t addr, uint64_t len, int prot, int fixed);
int aoi_vm_unmap(struct aoi_vm *vm, uint64_t addr, uint64_t len);       /* 0 or -errno */
int aoi_vm_protect(struct aoi_vm *vm, uint64_t addr, uint64_t len, int prot);

/* Host pointer for [addr, addr+len) if every page has `need` access, else NULL. */
uint8_t *aoi_vm_ptr(struct aoi_vm *vm, uint64_t addr, uint64_t len, int need);

#endif
