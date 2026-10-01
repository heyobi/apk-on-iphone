#include "jitmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif

#define PAGE 0x4000

const char *aoi_jit_name(int s)
{
    switch (s) {
    case AOI_JIT_TXM:      return "TXM pool (StikDebug universal script)";
    case AOI_JIT_MAPJIT:   return "MAP_JIT + pthread_jit_write_protect_np";
    case AOI_JIT_MPROTECT: return "mprotect RW -> RX";
    case AOI_JIT_REMAP:    return "vm_remap dual mapping";
    default:               return "?";
    }
}

static void *anon_rw(size_t n)
{
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <sys/sysctl.h>

int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_DEBUGGED   0x10000000

int aoi_jit_debugged(void)
{
    uint32_t flags = 0;
    if (csops(getpid(), CS_OPS_STATUS, &flags, sizeof flags) != 0) return 0;
    return (flags & CS_DEBUGGED) != 0;
}

int aoi_jit_txm_likely(void)
{
    char machine[64] = "", os[32] = "";
    size_t n = sizeof machine, m = sizeof os;
    int major = 0, ios = 0;
    sysctlbyname("hw.machine", machine, &n, NULL, 0);
    sysctlbyname("kern.osproductversion", os, &m, NULL, 0);
    ios = atoi(os);
    if (ios < 26) return 0;
    if (!strncmp(machine, "iPhone", 6)) { major = atoi(machine + 6); return major >= 14; }   /* A15 = iPhone14,x */
    return 1;                                                   /* iPad/other on 26+: assume TXM */
}

/* StikDebug universal protocol (StikJIT INTEGRATION.md): command in x16. */
__attribute__((noinline, naked)) static void jit26_detach(void)
{
    __asm__ volatile("mov x16, #0\n brk #0xf00d\n ret\n");
}
__attribute__((noinline, naked)) static void *jit26_prepare(void *address, size_t length)
{
    __asm__ volatile("mov x16, #1\n brk #0xf00d\n ret\n");
}

static uint8_t *txm_rx, *txm_rw;
static size_t txm_size, txm_used;

const char *aoi_jit_txm_init(size_t size)
{
    vm_address_t rw = 0;
    vm_prot_t cur, max;
    uint8_t *rx;
    if (txm_rx) return NULL;
    if (!aoi_jit_debugged()) return "not attached (CS_DEBUGGED missing)";
    rx = jit26_prepare(NULL, size);              /* the debugger maps and authorizes RX */
    if (!rx) { jit26_detach(); return "JIT26PrepareRegion returned NULL"; }
    if (vm_remap(mach_task_self(), &rw, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)rx,
                 FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS) { jit26_detach(); return "vm_remap of the RX region failed"; }
    if (vm_protect(mach_task_self(), rw, size, FALSE, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) {
        jit26_detach();
        return "the RW alias was refused (max protection lacks write)";
    }
    jit26_detach();
    txm_rx = rx; txm_rw = (uint8_t *)rw; txm_size = size; txm_used = 0;
    return NULL;
}

int aoi_jit_txm_ready(void) { return txm_rx != NULL; }

/* pthread_jit_write_protect_np is declared macOS-only in some SDKs; it exists on iOS 14+. */
static void jit_write_protect(int on)
{
    static void (*fn)(int);
    static int looked;
    if (!looked) { fn = (void (*)(int))dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_np"); looked = 1; }
    if (fn) fn(on);
}

/* Did the kernel really give execute permission at addr? (TXM devices silently
 * drop +X instead of failing.) */
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

const char *aoi_jit_reserve(int s, size_t span, struct aoi_jit_mem *m)
{
    memset(m, 0, sizeof *m);
    m->strategy = s;
    m->span = span;
    switch (s) {
    case AOI_JIT_TXM:
        if (!txm_rx) return "TXM pool not prepared (enable JIT through StikDebug first)";
        if (txm_used + span > txm_size) return "TXM pool exhausted";
        m->load = txm_rx + txm_used;
        txm_used += (span + PAGE - 1) & ~(size_t)(PAGE - 1);
        if (!(m->buf = anon_rw(span))) return "mmap failed";
        return NULL;
    case AOI_JIT_MAPJIT: {
        void *p = mmap(NULL, span, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (p == MAP_FAILED) return "mmap(MAP_JIT) refused";
        jit_write_protect(0);                   /* this thread may write it now */
        m->load = m->buf = p;
        return NULL; }
    default:
        if (!(m->load = m->buf = anon_rw(span))) return "mmap failed";
        return NULL;
    }
}

/* Replace [at, at+n) by ordinary read-write memory holding src's bytes. */
static const char *rw_pages(uint8_t *at, const uint8_t *src, size_t n)
{
    uint8_t *copy = NULL;
    if (!n) return NULL;
    if (src == at) {                            /* data already there: keep a copy across the remap */
        if (!(copy = anon_rw(n))) return "mmap for the data copy failed";
        memcpy(copy, at, n);
        src = copy;
    }
    if (mmap(at, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) {
        if (copy) munmap(copy, n);
        return "could not map the data pages read-write";
    }
    memcpy(at, src, n);
    if (copy) munmap(copy, n);
    return NULL;
}

const char *aoi_jit_seal(struct aoi_jit_mem *m, size_t text)
{
    const char *err;
    switch (m->strategy) {
    case AOI_JIT_TXM:
        memcpy(txm_rw + (m->load - txm_rx), m->buf, text);          /* code through the RW alias */
        if ((err = rw_pages(m->load + text, m->buf + text, m->span - text))) return err;
        munmap(m->buf, m->span);
        m->buf = NULL;
        break;
    case AOI_JIT_MAPJIT:
        if ((err = rw_pages(m->load + text, m->load + text, m->span - text))) return err;
        jit_write_protect(1);
        break;
    case AOI_JIT_MPROTECT:
        if (mprotect(m->load, text, PROT_READ | PROT_EXEC) != 0) return "mprotect(RX) refused";
        break;
    case AOI_JIT_REMAP: {
        vm_address_t rw = 0, target = (vm_address_t)m->load;
        vm_prot_t cur, max;
        if (vm_allocate(mach_task_self(), &rw, text, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return "vm_allocate failed";
        memcpy((void *)rw, m->load, text);
        if (vm_remap(mach_task_self(), &target, text, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                     rw, FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS || target != (vm_address_t)m->load)
            return "vm_remap failed";
        if (vm_protect(mach_task_self(), target, text, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS)
            return "vm_protect(RX) refused";
        break; }
    default:
        return "unknown strategy";
    }
    sys_icache_invalidate(m->load, text);
    if (!is_executable(m->load)) return "kernel did not grant execute";
    return NULL;
}

void aoi_jit_release(struct aoi_jit_mem *m)
{
    if (m->buf && m->buf != m->load) munmap(m->buf, m->span);
    if (m->strategy != AOI_JIT_TXM && m->load) munmap(m->load, m->span);   /* pool memory is not reused */
    memset(m, 0, sizeof *m);
}

#else  /* host build (Linux): lay out and link only; never executes */

int aoi_jit_debugged(void) { return 0; }
int aoi_jit_txm_likely(void) { return 0; }
const char *aoi_jit_txm_init(size_t size) { (void)size; return "host build"; }
int aoi_jit_txm_ready(void) { return 0; }

const char *aoi_jit_reserve(int s, size_t span, struct aoi_jit_mem *m)
{
    memset(m, 0, sizeof *m);
    m->strategy = s;
    m->span = span;
    if (!(m->load = m->buf = anon_rw(span))) return "mmap failed";
    return NULL;
}

const char *aoi_jit_seal(struct aoi_jit_mem *m, size_t text)
{
    (void)m; (void)text;
    return "host build: no native execution";
}

void aoi_jit_release(struct aoi_jit_mem *m) { if (m->load) munmap(m->load, m->span); memset(m, 0, sizeof *m); }

#endif
