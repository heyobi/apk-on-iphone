/* The accelerator: hot guest blocks translated to WebAssembly (core/jit.c). Built only
 * for WebAssembly (Emscripten), where the host JavaScript engine compiles them; elsewhere
 * these are no-ops and the interpreter runs everything. */
#ifndef AOI_JIT_H
#define AOI_JIT_H

#include <stdint.h>

struct aoi_cpu;

#if defined(__EMSCRIPTEN__) && !defined(AOI_NO_JIT)
#define AOI_JIT 1
/* A translated block: runs from cpu->pc, leaves cpu->pc at the next instruction (or at a
 * faulting one, with cpu->stop set) and returns how many guest instructions it ran. */
typedef int (*aoi_jit_fn)(struct aoi_cpu *c);
/* The translation for a block start, or NULL (counts it; translates it once it is hot). */
aoi_jit_fn aoi_jit_find(struct aoi_cpu *c, uint64_t pc);
/* Forget every translation (code was unmapped or changed protection, an address space ended). */
void aoi_jit_flush(void);
extern int aoi_jit_enabled;
extern uint64_t aoi_jit_blocks, aoi_jit_failed, aoi_jit_runs, aoi_jit_reused;
#else
static inline void aoi_jit_flush(void) {}
#endif

#endif
