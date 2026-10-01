/* Address-space probe: can this device give the guest the address space that
 * Android programs need? core/proc.c uses 64 GiB of guest addresses (scudo alone
 * reserves 8+ GiB). iOS caps contiguous reservations (6 GiB on an iPhone 16 Pro,
 * iOS 27, without the extended-virtual-addressing entitlement), which is why
 * core/vm.c is sparse. This measures both on the device. Shared by the iOS app
 * and tools/iostest.c. */
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "vmprobe.h"
#include "../core/vm.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <os/proc.h>
#endif

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define GiB (1ULL << 30)

static void say(aoi_log_fn log, void *ctx, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log(ctx, buf);
}

/* Reserve `size` PROT_NONE, then make the first and the last page writable and
 * use them: a reservation the kernel accepts but cannot back is no use to us. */
static int try_reserve(uint64_t size, int *err)
{
    long pg = sysconf(_SC_PAGESIZE);
    uint8_t *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    uint8_t *last;
    int ok;
    if (p == MAP_FAILED) { *err = errno; return 0; }
    last = p + size - (uint64_t)pg;
    ok = mprotect(p, (size_t)pg, PROT_READ | PROT_WRITE) == 0 &&
         mprotect(last, (size_t)pg, PROT_READ | PROT_WRITE) == 0;
    if (ok) { p[0] = 0x5a; last[pg - 1] = 0xa5; ok = p[0] == 0x5a && last[pg - 1] == 0xa5; }
    else *err = errno;
    munmap(p, size);
    return ok;
}

int aoi_vm_probe(aoi_log_fn log, void *ctx)
{
    static const unsigned sizes[] = { 64, 48, 32, 24, 16, 12, 8, 6, 4, 2 };
    unsigned i, best = 0;
    int e = 0, ok_path = 0;
    struct aoi_vm vm;
    const char *verr;

#ifdef __APPLE__
    {
        task_vm_info_data_t vi;
        mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
        memset(&vi, 0, sizeof vi);
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS)
            say(log, ctx, "vm: user addresses 0x%llx-0x%llx (%.1f GiB), footprint %.0f MB, limit %.0f MB",
                (unsigned long long)vi.min_address, (unsigned long long)vi.max_address,
                (double)(vi.max_address - vi.min_address) / (double)GiB, vi.phys_footprint / 1e6,
                vi.limit_bytes_remaining ? (vi.phys_footprint + vi.limit_bytes_remaining) / 1e6 : 0.0);
        say(log, ctx, "vm: available memory %.0f MB", os_proc_available_memory() / 1e6);
    }
#endif
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        if (try_reserve(sizes[i] * GiB, &e)) { best = sizes[i]; break; }
        say(log, ctx, "vm: %u GiB reservation refused (%s)", sizes[i], strerror(e));
    }
    say(log, ctx, "vm: largest usable reservation %u GiB", best);

    /* the real path: core/vm.c's sparse 64 GiB guest space, as core/proc.c uses it */
    if ((verr = aoi_vm_init(&vm, 64 * GiB))) {
        say(log, ctx, "vm: aoi_vm_init(64 GiB): %s", verr);
        ok_path = 0;
    } else {
        uint8_t one[4] = { 1, 2, 3, 4 }, back[4];
        uint64_t scudo = aoi_vm_map(&vm, 4 * GiB, 8 * GiB, 0, 0);    /* PROT_NONE, like scudo */
        uint64_t high = aoi_vm_map(&vm, 0, 1 << 20, AOI_PROT_R | AOI_PROT_W, 0);
        uint64_t low = aoi_vm_map(&vm, 0x70000000, 1 << 20, AOI_PROT_R | AOI_PROT_W, 1);
        int pr = scudo < vm.size && aoi_vm_protect(&vm, scudo + 7 * GiB, 1 << 20, AOI_PROT_R | AOI_PROT_W) == 0;
        ok_path = high < vm.size && low < vm.size && pr &&
                  aoi_vm_write(&vm, high + (1 << 20) - 4, one, 4, AOI_PROT_W) &&
                  aoi_vm_write(&vm, low, one, 4, AOI_PROT_W) &&
                  aoi_vm_write(&vm, scudo + 7 * GiB + (1 << 20) - 4, one, 4, AOI_PROT_W) &&
                  aoi_vm_read(&vm, scudo + 7 * GiB + (1 << 20) - 4, back, 4, AOI_PROT_R) && back[3] == 4;
        say(log, ctx, "vm: sparse 64 GiB guest space, 8 GiB scudo-style reservation, writes high/low/inside: %s "
            "(%llu host chunks of 2 MiB)", ok_path ? "OK" : "FAILED", (unsigned long long)vm.nchunks);
        aoi_vm_free(&vm);
    }
    say(log, ctx, "vm: %s", ok_path ? "Android's address-space needs fit on this device"
                                    : "address space is a blocker here");
    return best;
}
