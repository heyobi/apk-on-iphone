/* Finds the AArch64 instructions that cannot run unmodified on iOS.
 *
 *  svc #0            Linux syscall. On Darwin the kernel reads the syscall number
 *                    from x16, not x8, so it would run an unrelated Darwin call.
 *  mrs/msr tpidr_el0 Bionic's thread pointer. iOS does not preserve a value the
 *                    app writes there across context switches (Darwin keeps its
 *                    own TLS in TPIDRRO_EL0).
 *  x18 shadow stack  Android builds use x18 for the shadow call stack; iOS treats
 *                    x18 as a platform register and may zero it at any time.
 *
 * Every hit is a site the loader must rewrite (or trap) before the code runs. */
#ifndef AOI_SCAN_H
#define AOI_SCAN_H

#include "elf.h"

enum aoi_site_kind { AOI_SVC, AOI_MRS_TPIDR, AOI_MSR_TPIDR, AOI_SCS_PUSH, AOI_SCS_POP, AOI_KIND_COUNT };

struct aoi_scan {
    uint64_t count[AOI_KIND_COUNT];
    uint64_t insns;             /* instructions scanned */
    uint64_t first[AOI_KIND_COUNT];  /* vaddr of the first hit, 0 if none */
};

const char *aoi_site_name(enum aoi_site_kind kind);
/* Classifies one instruction; returns AOI_KIND_COUNT for "nothing to do". */
enum aoi_site_kind aoi_classify(uint32_t insn);
void aoi_scan_elf(const struct aoi_elf *elf, struct aoi_scan *out);

#endif
