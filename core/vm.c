#include "vm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

#define HOST_PAGE 0x4000u           /* covers both 4 KB (Linux) and 16 KB (iOS) hosts */
#define PG(a) ((a) / AOI_VM_PAGE)
#define CI(a) ((a) >> AOI_VM_CHUNK_SHIFT)
#define PAGES_PER_CHUNK (AOI_VM_CHUNK / AOI_VM_PAGE)

static uint64_t down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static uint64_t up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

const char *aoi_vm_init(struct aoi_vm *vm, uint64_t size)
{
    memset(vm, 0, sizeof *vm);
    size = up(size, AOI_VM_CHUNK);
    if (!(vm->prot = calloc(PG(size), 1))) return "no memory for the page table";
    if (!(vm->chunk = calloc(CI(size), sizeof *vm->chunk))) { free(vm->prot); vm->prot = NULL; return "no memory for the chunk table"; }
    vm->size = size;
    vm->hint = 0x10000000;          /* keep low addresses free (null page, fixed loads) */
    return NULL;
}

void aoi_vm_free(struct aoi_vm *vm)
{
    uint64_t i;
    if (vm->chunk)
        for (i = 0; i < CI(vm->size); i++)
            if (vm->chunk[i]) munmap(vm->chunk[i], AOI_VM_CHUNK);
    free(vm->chunk);
    free(vm->prot);
    memset(vm, 0, sizeof *vm);
}

static int chunk_get(struct aoi_vm *vm, uint64_t ci)
{
    void *p;
    if (vm->chunk[ci]) return 1;
    p = mmap(NULL, AOI_VM_CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 0;
    vm->chunk[ci] = p;
    vm->nchunks++;
    return 1;
}

/* Fresh zero bytes for [addr, addr+len) inside one existing chunk: whole host
 * pages are replaced by new anonymous memory (which also returns them to the
 * system), the ragged edges are cleared. */
static void chunk_clear(struct aoi_vm *vm, uint64_t addr, uint64_t len)
{
    uint8_t *base = vm->chunk[CI(addr)];
    uint64_t off = addr & (AOI_VM_CHUNK - 1), end = off + len;
    uint64_t ia = up(off, HOST_PAGE), ie = down(end, HOST_PAGE);
    if (ie > ia) {
        if (mmap(base + ia, ie - ia, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
            memset(base + ia, 0, ie - ia);
        memset(base + off, 0, ia - off);
        memset(base + ie, 0, end - ie);
    } else memset(base + off, 0, len);
}

/* After pages lose their mapping: release a chunk none of whose pages is mapped. */
static void chunk_release(struct aoi_vm *vm, uint64_t ci)
{
    uint64_t p0 = ci * PAGES_PER_CHUNK, i;
    if (!vm->chunk[ci]) return;
    for (i = 0; i < PAGES_PER_CHUNK; i++)
        if (vm->prot[p0 + i]) return;
    munmap(vm->chunk[ci], AOI_VM_CHUNK);
    vm->chunk[ci] = NULL;
    vm->nchunks--;
}

static int range_ok(struct aoi_vm *vm, uint64_t addr, uint64_t len)
{
    return len && addr + len >= addr && addr + len <= vm->size;
}

/* Every page in the range with access must have its chunk. */
static int back(struct aoi_vm *vm, uint64_t addr, uint64_t len, int prot)
{
    uint64_t c;
    if (!(prot & 7)) return 1;
    for (c = CI(addr); c <= CI(addr + len - 1); c++)
        if (!chunk_get(vm, c)) return 0;
    return 1;
}

uint64_t aoi_vm_map(struct aoi_vm *vm, uint64_t addr, uint64_t len, int prot, int fixed)
{
    uint64_t n, a, e;
    if (!len) return (uint64_t)-EINVAL;
    len = up(len, AOI_VM_PAGE);
    if (fixed) {
        if (addr % AOI_VM_PAGE || !range_ok(vm, addr, len)) return (uint64_t)-EINVAL;
    } else {
        /* first fit from the hint, wrapping once */
        uint64_t start = up(addr && range_ok(vm, addr, len) ? addr : vm->hint, AOI_VM_PAGE);
        int wrapped = 0;
        a = start;
        for (;;) {
            if (a + len > vm->size) {
                if (wrapped) return (uint64_t)-ENOMEM;
                a = 0x10000000; wrapped = 1;
                continue;
            }
            for (n = 0; n < len && !vm->prot[PG(a + n)]; n += AOI_VM_PAGE) {}
            if (n == len) break;
            a += n + AOI_VM_PAGE;
        }
        addr = a;
        vm->hint = addr + len;
    }
    if (!back(vm, addr, len, prot)) return (uint64_t)-ENOMEM;
    /* zero whatever host memory already sits under the range */
    for (a = addr, e = addr + len; a < e; a = n) {
        n = down(a, AOI_VM_CHUNK) + AOI_VM_CHUNK;
        if (n > e) n = e;
        if (vm->chunk[CI(a)]) chunk_clear(vm, a, n - a);
    }
    memset(vm->prot + PG(addr), prot | 0x80, PG(len));
    return addr;
}

int aoi_vm_unmap(struct aoi_vm *vm, uint64_t addr, uint64_t len)
{
    uint64_t c;
    if (addr % AOI_VM_PAGE || !len) return -EINVAL;
    len = up(len, AOI_VM_PAGE);
    if (!range_ok(vm, addr, len)) return -EINVAL;
    memset(vm->prot + PG(addr), 0, PG(len));
    for (c = CI(addr); c <= CI(addr + len - 1); c++) chunk_release(vm, c);
    return 0;
}

int aoi_vm_protect(struct aoi_vm *vm, uint64_t addr, uint64_t len, int prot)
{
    uint64_t p;
    if (addr % AOI_VM_PAGE) return -EINVAL;
    len = up(len, AOI_VM_PAGE);
    if (!range_ok(vm, addr, len)) return -ENOMEM;
    for (p = addr; p < addr + len; p += AOI_VM_PAGE)
        if (!vm->prot[PG(p)]) return -ENOMEM;
    if (!back(vm, addr, len, prot)) return -ENOMEM;
    memset(vm->prot + PG(addr), prot | 0x80, PG(len));
    return 0;
}

static int pages_ok(struct aoi_vm *vm, uint64_t addr, uint64_t len, int need)
{
    uint64_t p, e;
    if (!len) len = 1;
    if (addr + len < addr || addr + len > vm->size) return 0;
    for (p = down(addr, AOI_VM_PAGE), e = addr + len; p < e; p += AOI_VM_PAGE) {
        uint8_t f = vm->prot[PG(p)];
        if (!f || (f & need) != need || !vm->chunk[CI(p)]) return 0;
    }
    return 1;
}

uint8_t *aoi_vm_ptr(struct aoi_vm *vm, uint64_t addr, uint64_t len, int need)
{
    if (!len) len = 1;
    if (CI(addr) != CI(addr + len - 1) || !pages_ok(vm, addr, len, need)) return NULL;
    return vm->chunk[CI(addr)] + (addr & (AOI_VM_CHUNK - 1));
}

uint8_t *aoi_vm_span(struct aoi_vm *vm, uint64_t addr, uint64_t len, int need, uint64_t *n)
{
    uint64_t room = AOI_VM_CHUNK - (addr & (AOI_VM_CHUNK - 1));
    if (len > room) len = room;
    if (!len || !pages_ok(vm, addr, len, need)) return NULL;
    *n = len;
    return vm->chunk[CI(addr)] + (addr & (AOI_VM_CHUNK - 1));
}

int aoi_vm_read(struct aoi_vm *vm, uint64_t addr, void *dst, uint64_t len, int need)
{
    uint8_t *d = dst, *s;
    uint64_t n;
    if (!pages_ok(vm, addr, len, need)) return 0;
    while (len) {
        if (!(s = aoi_vm_span(vm, addr, len, need, &n))) return 0;
        memcpy(d, s, n);
        d += n; addr += n; len -= n;
    }
    return 1;
}

int aoi_vm_write(struct aoi_vm *vm, uint64_t addr, const void *src, uint64_t len, int need)
{
    const uint8_t *s = src;
    uint8_t *d;
    uint64_t n;
    if (!pages_ok(vm, addr, len, need)) return 0;
    while (len) {
        if (!(d = aoi_vm_span(vm, addr, len, need, &n))) return 0;
        memcpy(d, s, n);
        s += n; addr += n; len -= n;
    }
    return 1;
}

void aoi_vm_zero(struct aoi_vm *vm, uint64_t addr, uint64_t len)
{
    uint64_t p, e;
    if (!range_ok(vm, addr, len ? len : 1)) return;
    for (p = down(addr, AOI_VM_PAGE), e = up(addr + len, AOI_VM_PAGE); p < e; p += AOI_VM_PAGE)
        if (vm->prot[PG(p)] && vm->chunk[CI(p)]) memset(vm->chunk[CI(p)] + (p & (AOI_VM_CHUNK - 1)), 0, AOI_VM_PAGE);
}
