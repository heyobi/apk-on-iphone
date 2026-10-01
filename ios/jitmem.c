#include "jitmem.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif

const char *aoi_jit_name(int s)
{
    switch (s) {
    case AOI_JIT_MAPJIT:   return "MAP_JIT + pthread_jit_write_protect_np";
    case AOI_JIT_MPROTECT: return "mprotect RW -> RX";
    case AOI_JIT_REMAP:    return "vm_remap dual mapping (RW alias + RX)";
    default:               return "?";
    }
}

#ifdef __APPLE__
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>

/* pthread_jit_write_protect_np is declared macOS-only in some SDKs; it exists on iOS 14+. */
static void jit_write_protect(int on)
{
    static void (*fn)(int);
    static int looked;
    if (!looked) { fn = (void (*)(int))dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_np"); looked = 1; }
    if (fn) fn(on);
}

int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_DEBUGGED   0x10000000

int aoi_jit_debugged(void)
{
    uint32_t flags = 0;
    if (csops(getpid(), CS_OPS_STATUS, &flags, sizeof flags) != 0) return 0;
    return (flags & CS_DEBUGGED) != 0;
}

uint8_t *aoi_jit_reserve(int s, size_t span, const char **err)
{
    void *p;
    if (s == AOI_JIT_MAPJIT) {
        p = mmap(NULL, span, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (p == MAP_FAILED) { *err = "mmap(MAP_JIT) refused"; return NULL; }
        jit_write_protect(0);                       /* this thread may write it now */
        return p;
    }
    p = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { *err = "mmap failed"; return NULL; }
    return p;
}

/* Did the kernel really give [addr, addr+len) execute permission? (TXM devices
 * silently drop +X instead of failing.) */
static int is_executable(uint8_t *addr)
{
    vm_address_t a = (vm_address_t)addr;
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                     &count, &object) != KERN_SUCCESS) return 0;
    return a <= (vm_address_t)addr && (info.protection & VM_PROT_EXECUTE);
}

const char *aoi_jit_seal(int s, uint8_t *base, size_t text, size_t span)
{
    if (s == AOI_JIT_MAPJIT) {
        /* data must stay writable while code runs: give it ordinary memory at the same address */
        size_t dlen = span - text;
        if (dlen) {
            void *copy = mmap(NULL, dlen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (copy == MAP_FAILED) return "mmap for data copy failed";
            memcpy(copy, base + text, dlen);
            if (mmap(base + text, dlen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) {
                munmap(copy, dlen);
                return "could not replace the data pages of the MAP_JIT region";
            }
            memcpy(base + text, copy, dlen);
            munmap(copy, dlen);
        }
        jit_write_protect(1);
    } else if (s == AOI_JIT_MPROTECT) {
        if (mprotect(base, text, PROT_READ | PROT_EXEC) != 0) return "mprotect(RX) refused";
    } else if (s == AOI_JIT_REMAP) {
        vm_address_t rw = 0, target = (vm_address_t)base;
        vm_prot_t cur, max;
        if (vm_allocate(mach_task_self(), &rw, text, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return "vm_allocate failed";
        memcpy((void *)rw, base, text);
        if (vm_remap(mach_task_self(), &target, text, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                     rw, FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS || target != (vm_address_t)base)
            return "vm_remap failed";
        if (vm_protect(mach_task_self(), target, text, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS)
            return "vm_protect(RX) refused";
    } else return "unknown strategy";
    sys_icache_invalidate(base, text);
    if (!is_executable(base)) return "kernel did not grant execute (TXM device? needs the StikDebug script protocol)";
    return NULL;
}

void aoi_jit_release(int s, uint8_t *base, size_t span) { (void)s; munmap(base, span); }

#else  /* host build (Linux): lay out and link only; never executes */

int aoi_jit_debugged(void) { return 0; }

uint8_t *aoi_jit_reserve(int s, size_t span, const char **err)
{
    void *p = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    (void)s;
    if (p == MAP_FAILED) { *err = "mmap failed"; return NULL; }
    return p;
}

const char *aoi_jit_seal(int s, uint8_t *base, size_t text, size_t span)
{
    (void)s; (void)base; (void)text; (void)span;
    return "host build: no native execution";
}

void aoi_jit_release(int s, uint8_t *base, size_t span) { (void)s; munmap(base, span); }

#endif
