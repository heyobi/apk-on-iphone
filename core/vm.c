#include "vm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define HOST_PAGE 0x4000u           /* covers both 4 KB (Linux) and 16 KB (iOS) hosts */
#define PG(a) ((a) / AOI_VM_PAGE)

static uint64_t down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static uint64_t up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

const char *aoi_vm_init(struct aoi_vm *vm, uint64_t size)
{
    void *p;
    memset(vm, 0, sizeof *vm);
    size = up(size, HOST_PAGE);
    p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return "cannot reserve the guest address space";
    if (!(vm->prot = calloc(PG(size), 1))) { munmap(p, size); return "no memory for the page table"; }
    vm->host = p;
    vm->size = size;
    vm->hint = 0x10000000;          /* keep low addresses free (null page, fixed loads) */
    return NULL;
}

void aoi_vm_free(struct aoi_vm *vm)
{
    if (vm->host) munmap(vm->host, vm->size);
    free(vm->prot);
    memset(vm, 0, sizeof *vm);
}

/* Host side: a host page is accessible while any guest page inside it is mapped.
 * Host pages lying wholly inside [addr, addr+len) share no guest page with a
 * neighbour, so they are handled with one call; only the two edge pages need
 * the per-page check. (Per-page calls made scudo's 8 GiB reservation cost half
 * a million mmaps.) */
static void host_sync(struct aoi_vm *vm, uint64_t addr, uint64_t len, int fresh)
{
    uint64_t a = down(addr, HOST_PAGE), e = up(addr + len, HOST_PAGE), g;
    uint64_t ia = up(addr, HOST_PAGE), ie = down(addr + len, HOST_PAGE);
    if (ie > ia && !fresh) {
        mmap(vm->host + ia, ie - ia, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
    }
    for (; a < e; a += HOST_PAGE) {
        if (ie > ia && a >= ia && a < ie && !fresh) { a = ie - HOST_PAGE; continue; }
        int any = 0;
        for (g = a; g < a + HOST_PAGE; g += AOI_VM_PAGE) any |= vm->prot[PG(g)];
        if (!any)
            mmap(vm->host + a, HOST_PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
        else if (fresh)
            mprotect(vm->host + a, HOST_PAGE, PROT_READ | PROT_WRITE);
    }
}

static int range_ok(struct aoi_vm *vm, uint64_t addr, uint64_t len)
{
    return len && addr + len >= addr && addr + len <= vm->size;
}

uint64_t aoi_vm_map(struct aoi_vm *vm, uint64_t addr, uint64_t len, int prot, int fixed)
{
    uint64_t p, n;
    if (!len) return (uint64_t)-EINVAL;
    len = up(len, AOI_VM_PAGE);
    if (fixed) {
        if (addr % AOI_VM_PAGE || !range_ok(vm, addr, len)) return (uint64_t)-EINVAL;
    } else {
        /* first fit from the hint, wrapping once */
        uint64_t start = up(addr && range_ok(vm, addr, len) ? addr : vm->hint, AOI_VM_PAGE), a = start;
        int wrapped = 0;
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
    /* fresh zero pages: give the host range new anonymous memory, then mark */
    {
        uint64_t ha = down(addr, HOST_PAGE), he = up(addr + len, HOST_PAGE);
        uint64_t ia = up(addr, HOST_PAGE), ie = down(addr + len, HOST_PAGE);
        if (ie > ia)                                  /* interior: one call */
            mmap(vm->host + ia, ie - ia, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
        for (p = ha; p < he; p += HOST_PAGE) {
            int keep = 0;
            uint64_t g;
            if (ie > ia && p >= ia && p < ie) { p = ie - HOST_PAGE; continue; }
            for (g = p; g < p + HOST_PAGE; g += AOI_VM_PAGE)
                if ((g < addr || g >= addr + len) && vm->prot[PG(g)]) keep = 1;
            if (keep) {                       /* host page shared with a neighbour: zero our part */
                mprotect(vm->host + p, HOST_PAGE, PROT_READ | PROT_WRITE);
                for (g = p; g < p + HOST_PAGE; g += AOI_VM_PAGE)
                    if (g >= addr && g < addr + len) memset(vm->host + g, 0, AOI_VM_PAGE);
            } else {
                mmap(vm->host + p, HOST_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
            }
        }
    }
    memset(vm->prot + PG(addr), prot | 0x80, PG(len));
    return addr;
}

int aoi_vm_unmap(struct aoi_vm *vm, uint64_t addr, uint64_t len)
{
    if (addr % AOI_VM_PAGE || !len) return -EINVAL;
    len = up(len, AOI_VM_PAGE);
    if (!range_ok(vm, addr, len)) return -EINVAL;
    memset(vm->prot + PG(addr), 0, PG(len));
    host_sync(vm, addr, len, 0);
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
    for (p = addr; p < addr + len; p += AOI_VM_PAGE) vm->prot[PG(p)] = (uint8_t)(prot | 0x80);
    return 0;
}

uint8_t *aoi_vm_ptr(struct aoi_vm *vm, uint64_t addr, uint64_t len, int need)
{
    uint64_t p, e;
    if (!len) len = 1;
    if (addr + len < addr || addr + len > vm->size) return NULL;
    for (p = down(addr, AOI_VM_PAGE), e = addr + len; p < e; p += AOI_VM_PAGE) {
        uint8_t f = vm->prot[PG(p)];
        if (!f || (f & need) != need) return NULL;
    }
    return vm->host + addr;
}
