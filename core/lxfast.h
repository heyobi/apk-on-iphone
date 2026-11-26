/* glibc's string and memory functions at host speed (core/lxfast.c). */
#ifndef AOI_LXFAST_H
#define AOI_LXFAST_H

#include <stdint.h>

struct aoi_proc;
struct aoi_cpu;

/* A Linux process mapped a file at guest address a (len bytes from file offset off,
 * executable): if it is glibc's libc.so.6, its string functions are routed to the host
 * before the dynamic linker relocates it. fd: the host file. Returns the guest page it
 * added next to libc for that (to note in the process's maps), or 0. */
uint64_t aoi_lxfast_mapped(struct aoi_proc *p, uint64_t a, uint64_t len, uint64_t off, int fd, const char *path);

/* The cpu's host_call while a process has routed functions (also after a snapshot is
 * restored: core/snap.c). */
uint64_t aoi_lxfast_call(struct aoi_cpu *c, unsigned slot);

/* Calls answered on the host, and calls handed back to the guest's own code. */
extern uint64_t aoi_lxfast_hits, aoi_lxfast_declined;

#endif
