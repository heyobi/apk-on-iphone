/* On-device check that the guest address space Android programs need can be
 * reserved and used (see vmprobe.c). */
#ifndef AOI_VMPROBE_H
#define AOI_VMPROBE_H

#include "gmptest.h"

/* Logs what it finds; returns the largest usable reservation in GiB. */
int aoi_vm_probe(aoi_log_fn log, void *ctx);

#endif
