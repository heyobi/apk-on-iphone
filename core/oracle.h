/* Reference-CPU oracle (debug builds only, needs libunicorn). */
#ifndef AOI_ORACLE_H
#define AOI_ORACLE_H

#include "cpu.h"

/* Cross-check every instruction the CPU runs against Unicorn. Map all guest
 * memory before calling. Returns NULL or an error. */
const char *aoi_oracle_attach(struct aoi_cpu *cpu);
/* Prints a summary; call when a run ends. */
void aoi_oracle_report(struct aoi_cpu *cpu, enum aoi_stop st);

/* For single-instruction testing (tools/isacheck.c): clear the mismatch state
 * (quiet = no printing), then ask whether the last step mismatched and whether
 * Unicorn could execute it at all. */
void aoi_oracle_reset(int quiet);
int aoi_oracle_mismatch(void);
int aoi_oracle_ref_ran(void);
const char *aoi_oracle_what(void);   /* first differing register/field */

#endif
