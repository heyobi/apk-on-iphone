/* Reference-CPU oracle (debug builds only, needs libunicorn). */
#ifndef AOI_ORACLE_H
#define AOI_ORACLE_H

#include "cpu.h"

/* Cross-check every instruction the CPU runs against Unicorn. Map all guest
 * memory before calling. Returns NULL or an error. */
const char *aoi_oracle_attach(struct aoi_cpu *cpu);
/* Prints a summary; call when a run ends. */
void aoi_oracle_report(struct aoi_cpu *cpu, enum aoi_stop st);

#endif
