/* A Linux/arm64 process for unmodified Android programs.
 *
 * aoi_proc_exec() does what the kernel's execve does: maps the program and its
 * PT_INTERP (Android's own linker64) into a flat guest address space below 4 GiB,
 * builds the initial stack (argv, envp, auxv) and points the CPU at the
 * interpreter. Guest syscalls (svc #0) are served by core/proc.c on the host.
 *
 * Guest paths are confined to `root` (an Android tree, see tools/android-root.sh):
 * "/system/bin/sh" means ROOT/system/bin/sh, and absolute symlinks inside the
 * tree resolve inside it, the way they would under the real root. */
#ifndef AOI_PROC_H
#define AOI_PROC_H

#include "cpu.h"
#include "vm.h"

#include <stdio.h>

#define AOI_PROC_FDS 256
#define AOI_PATH 1024

struct aoi_proc_fd {
    int used;
    int host;                       /* host file descriptor */
    char path[AOI_PATH];            /* guest path it was opened with (resolved) */
    void *dir;                      /* host DIR* for getdents64, opened lazily */
};

/* A file mapping, kept to name code addresses in diagnostics. */
struct aoi_proc_map { uint64_t start, len, off; char path[160]; };
#define AOI_PROC_MAPS 2048

struct aoi_proc {
    struct aoi_vm vm;
    struct aoi_mem mem;
    struct aoi_cpu cpu;
    char root[AOI_PATH];            /* host directory that is the guest's "/" */
    char cwd[AOI_PATH];             /* guest path */
    char exe[AOI_PATH];             /* guest path of the program (/proc/self/exe) */
    struct aoi_proc_fd fd[AOI_PROC_FDS];
    uint64_t sigact[65][4];         /* rt_sigaction records, kept so oact reads back */
    uint64_t sigmask;
    uint64_t altstack[3];
    uint64_t brk;
    FILE *trace;                    /* strace-style log, or NULL */
    unsigned char unknown[512];     /* syscalls already reported as unimplemented */
    struct aoi_proc_map maps[AOI_PROC_MAPS];
    int nmaps;
};

/* Loads `path` (a guest path) with argv/envp into a fresh process. NULL on success. */
const char *aoi_proc_exec(struct aoi_proc *p, const char *root, const char *path,
                          int argc, const char *const *argv, const char *const *envp);

/* Runs until exit or a stop; max_steps 0 = no limit. */
enum aoi_stop aoi_proc_run(struct aoi_proc *p, uint64_t max_steps);

void aoi_proc_free(struct aoi_proc *p);

/* "path+0xoff" for a guest code address, or "?" (for crash reports). */
const char *aoi_proc_where(struct aoi_proc *p, uint64_t addr, char *buf, size_t n);

#endif
