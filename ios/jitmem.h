/* Executable memory on iOS for the native backend. Three ways to get it, since
 * which one works depends on the iOS version, the device and how JIT was
 * enabled (StikDebug sets CS_DEBUGGED). Each is probed safely before use. */
#ifndef AOI_JITMEM_H
#define AOI_JITMEM_H

#include <stddef.h>
#include <stdint.h>

enum { AOI_JIT_MAPJIT, AOI_JIT_MPROTECT, AOI_JIT_REMAP, AOI_JIT_COUNT };

const char *aoi_jit_name(int strategy);
/* 1 if the process is being (or was) debugged: CS_DEBUGGED, the precondition for JIT. */
int aoi_jit_debugged(void);
/* Writable, zeroed memory for a whole image of `span` bytes. */
uint8_t *aoi_jit_reserve(int strategy, size_t span, const char **err);
/* Makes [base, base+text) executable (the rest stays read-write) and checks the
 * kernel really granted execute. NULL on success. */
const char *aoi_jit_seal(int strategy, uint8_t *base, size_t text, size_t span);
void aoi_jit_release(int strategy, uint8_t *base, size_t span);

#endif
