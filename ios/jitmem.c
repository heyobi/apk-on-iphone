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

/* TXM layout. The debugger-prepared RX memory could not be overlaid with
 * ordinary RW pages on iOS 27 ("could not map the data pages"), so two layouts:
 *  pool  : one prepared region; an image's data pages are turned back to RW
 *          inside it, if any of the tried methods is allowed (txm_data_method);
 *  slots : regions whose first part is prepared (RX) and whose tail was never
 *          prepared (ordinary RW). An image is placed so its code ends exactly
 *          where the prepared part ends; its data then lands in the RW tail. */
struct txm_area { uint8_t *rx, *rw; size_t rx_size, tail, used; };

#define NSLOTS 9
static const size_t slot_rx[NSLOTS]   = { 24u << 20, 8u << 20, 8u << 20, 2u << 20, 2u << 20, 2u << 20, 2u << 20, 2u << 20, 2u << 20 };
static const size_t slot_tail[NSLOTS] = { 8u << 20, 4u << 20, 4u << 20, 2u << 20, 2u << 20, 2u << 20, 2u << 20, 2u << 20, 2u << 20 };

static struct txm_area pool, slots[NSLOTS];
static int txm_ok, txm_data_method;           /* 0 none, 1 mprotect, 2 vm_protect, 3 vm_allocate overwrite */
static char txm_info[512];

static const char *data_method_name(int k)
{
    return k == 1 ? "mprotect RW" : k == 2 ? "vm_protect RW" : k == 3 ? "vm_allocate overwrite" : "none";
}

/* The debugger prepares [addr, addr+n) as RX; we add an RW alias. */
static const char *prepare(uint8_t *addr, size_t n, struct txm_area *a)
{
    vm_address_t rw = 0;
    vm_prot_t cur, max;
    uint8_t *rx = jit26_prepare(addr, n);
    if (!rx) return "JIT26PrepareRegion returned NULL";
    if (addr && rx != addr) return "JIT26PrepareRegion moved the region";
    if (vm_remap(mach_task_self(), &rw, n, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)rx,
                 FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS) return "vm_remap of the RX region failed";
    if (vm_protect(mach_task_self(), rw, n, FALSE, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS)
        return "the RW alias was refused";
    a->rx = rx; a->rw = (uint8_t *)rw; a->rx_size = n;
    return NULL;
}

static int region_prot(uint8_t *addr)
{
    vm_address_t a = (vm_address_t)addr;
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                     &count, &object) != KERN_SUCCESS || a > (vm_address_t)addr) return -1;
    return info.protection;
}

/* Turn [at, at+n) of a prepared region into ordinary RW memory with method k. */
static int make_rw(uint8_t *at, size_t n, int k)
{
    vm_address_t a = (vm_address_t)at;
    int r = -1;
    if (k == 1) r = mprotect(at, n, PROT_READ | PROT_WRITE);
    else if (k == 2) r = vm_protect(mach_task_self(), a, n, FALSE, VM_PROT_READ | VM_PROT_WRITE) == KERN_SUCCESS ? 0 : -1;
    else if (k == 3) r = vm_allocate(mach_task_self(), &a, n, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) == KERN_SUCCESS && a == (vm_address_t)at ? 0 : -1;
    if (r != 0) return 0;
    r = region_prot(at);
    return r >= 0 && (r & VM_PROT_WRITE) && !(r & VM_PROT_EXECUTE);
}

const char *aoi_jit_txm_init(size_t size)
{
    const char *err = NULL, *slot_err = NULL;
    size_t total = 0, i, off;
    uint8_t *p, *q;
    int k, nslots_ok = 0;
    if (txm_ok) return NULL;
    if (!aoi_jit_debugged()) return "not attached (CS_DEBUGGED missing)";

    /* the pool: our own mapping prepared in place; if refused, let the debugger allocate (worked on iOS 27) */
    if (!(p = anon_rw(size))) { jit26_detach(); return "mmap for the pool failed"; }
    if ((err = prepare(p, size, &pool))) {
        const char *first = err;
        munmap(p, size);
        if ((err = prepare(NULL, size, &pool))) { jit26_detach(); return first; }
    }
    /* the slots; a failure here only disables them */
    for (i = 0; i < NSLOTS; i++) total += slot_rx[i] + slot_tail[i];
    if (!(q = anon_rw(total))) slot_err = "mmap for the slots failed";
    for (i = 0, off = 0; !slot_err && i < NSLOTS; off += slot_rx[i] + slot_tail[i], i++) {
        if ((slot_err = prepare(q + off, slot_rx[i], &slots[i]))) break;
        slots[i].tail = slot_tail[i];
        nslots_ok++;
    }
    jit26_detach();

    /* can a prepared page become ordinary RW again? try on the pool's last pages */
    for (k = 1; k <= 3 && !txm_data_method; k++)
        if (make_rw(pool.rx + pool.rx_size - (size_t)k * PAGE, PAGE, k)) txm_data_method = k;
    if (txm_data_method) pool.rx_size -= 3 * PAGE;          /* those test pages are not code any more */
    snprintf(txm_info, sizeof txm_info, "pool %zu MB (%s; data pages: %s), slots %d/%d%s%s",
             size >> 20, pool.rx == p ? "our mapping" : "debugger's mapping", data_method_name(txm_data_method),
             nslots_ok, NSLOTS, slot_err ? " - " : "", slot_err ? slot_err : "");
    if (!txm_data_method && !nslots_ok) return "neither layout works (see the info line)";
    txm_ok = 1;
    return NULL;
}

const char *aoi_jit_txm_info(void) { return txm_info; }

int aoi_jit_txm_ready(void) { return txm_ok; }

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

const char *aoi_jit_reserve(int s, size_t span, size_t text, struct aoi_jit_mem *m)
{
    size_t i, data = span - text;
    memset(m, 0, sizeof *m);
    m->strategy = s;
    m->span = span;
    switch (s) {
    case AOI_JIT_TXM:
        if (!txm_ok) return "TXM pool not prepared (enable JIT through StikDebug first)";
        if (txm_data_method && pool.used + span <= pool.rx_size) {
            m->load = pool.rx + pool.used;
            m->area = &pool;
            pool.used += span;
        } else {
            for (i = 0; i < NSLOTS; i++)            /* smallest free slot that fits */
                if (slots[i].rx && !slots[i].used && slots[i].rx_size >= text && slots[i].tail >= data &&
                    (!m->area || slots[i].rx_size < ((struct txm_area *)m->area)->rx_size)) m->area = &slots[i];
            if (!m->area) return "no free TXM slot large enough";
            ((struct txm_area *)m->area)->used = 1;
            m->load = ((struct txm_area *)m->area)->rx + ((struct txm_area *)m->area)->rx_size - text;
        }
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
    case AOI_JIT_TXM: {
        struct txm_area *a = m->area;
        memcpy(a->rw + (m->load - a->rx), m->buf, text);            /* code through the RW alias */
        if (m->span > text) {
            if (a == &pool && !make_rw(m->load + text, m->span - text, txm_data_method))
                return "data pages could not be made RW";
            memcpy(m->load + text, m->buf + text, m->span - text);   /* slot tail / RW pages */
        }
        munmap(m->buf, m->span);
        m->buf = NULL;
        break; }
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
    if (m->strategy != AOI_JIT_TXM && m->load) munmap(m->load, m->span);
    if (m->strategy == AOI_JIT_TXM && m->area && m->area != (void *)&pool) ((struct txm_area *)m->area)->used = 0;
    memset(m, 0, sizeof *m);
}

#else  /* host build (Linux): lay out and link only; never executes */

int aoi_jit_debugged(void) { return 0; }
int aoi_jit_txm_likely(void) { return 0; }
const char *aoi_jit_txm_init(size_t size) { (void)size; return "host build"; }
const char *aoi_jit_txm_info(void) { return ""; }
int aoi_jit_txm_ready(void) { return 0; }

const char *aoi_jit_reserve(int s, size_t span, size_t text, struct aoi_jit_mem *m)
{
    (void)text;
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
