/* Executable memory on iOS for the native backend.
 *
 * Which way works depends on the device and iOS version:
 *  - TXM devices (A15+/M2+ on iOS 26+): StikDebug's "universal" script must stay
 *    attached while the app asks it, with brk #0xf00d, to prepare one RX region;
 *    we write through our own RW alias of it. All executable memory has to be
 *    requested up front, so one pool is prepared at launch (aoi_jit_txm_init).
 *  - older devices / iOS: once CS_DEBUGGED is set, MAP_JIT, mprotect or a
 *    vm_remap dual mapping work.
 * Each strategy is probed with a two-instruction function before real use. */
#ifndef AOI_JITMEM_H
#define AOI_JITMEM_H

#include <stddef.h>
#include <stdint.h>

enum { AOI_JIT_TXM, AOI_JIT_MAPJIT, AOI_JIT_MPROTECT, AOI_JIT_REMAP, AOI_JIT_COUNT };

const char *aoi_jit_name(int strategy);
/* CS_DEBUGGED: a debugger is or was attached (precondition for any JIT). */
int aoi_jit_debugged(void);
/* Heuristic: A15+/M2+ hardware on iOS 26 or later (needs the script protocol). */
int aoi_jit_txm_likely(void);

/* TXM: with StikDebug's universal script attached, prepare a `size`-byte RX pool,
 * create its RW alias and detach. Call once, after CS_DEBUGGED is set; a brk
 * without the script attached crashes the app. NULL on success. */
const char *aoi_jit_txm_init(size_t size);
int aoi_jit_txm_ready(void);

/* One image: `load` is where its code runs, `buf` where it is written. */
struct aoi_jit_mem {
    int strategy;
    uint8_t *load, *buf;
    size_t span;
};
/* Reserves room for an image of `span` bytes (buf is writable and zeroed). */
const char *aoi_jit_reserve(int strategy, size_t span, struct aoi_jit_mem *m);
/* Puts the image in place: [load, load+text) executable, the rest read-write.
 * Checks the kernel really granted execute. NULL on success. */
const char *aoi_jit_seal(struct aoi_jit_mem *m, size_t text);
void aoi_jit_release(struct aoi_jit_mem *m);

#endif
