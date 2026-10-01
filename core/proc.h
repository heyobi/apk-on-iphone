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
    int kind;                       /* AOI_FD_* */
    int nonblock;                   /* guest O_NONBLOCK (pipes: the host end is always non-blocking) */
    uint64_t count;                 /* AOI_FD_EVENTFD: the counter */
    int sem;                        /* AOI_FD_EVENTFD: EFD_SEMAPHORE (reads take 1, not all) */
    struct aoi_epoll *ep;           /* AOI_FD_EPOLL: the interest list (shared by dups) */
};

enum { AOI_FD_FILE = 0, AOI_FD_SOCKET, AOI_FD_LOGD, AOI_FD_UFFD, AOI_FD_PIPE, AOI_FD_BINDER,
       AOI_FD_EVENTFD, AOI_FD_EPOLL };

/* An epoll instance: level-triggered interest entries {fd, events, data}. */
struct aoi_epoll { int refs, n, cap; struct { int fd; uint32_t events; uint64_t data; } *e; };

/* A file mapping, kept to name code addresses in diagnostics. */
struct aoi_proc_map { uint64_t start, len, off; char path[160]; };
#define AOI_PROC_MAPS 4096

/* Guest threads are green threads: all run on the calling host thread, one at a
 * time, switched every time slice and whenever one blocks (futex, sleep). */
enum { AOI_T_FREE = 0, AOI_T_RUN, AOI_T_FUTEX, AOI_T_SLEEP };
#define AOI_PROC_THREADS 128

struct aoi_thread {
    int state;                      /* AOI_T_* */
    int tid;
    struct aoi_cpu cpu;             /* registers while not running */
    uint64_t futex_addr;            /* AOI_T_FUTEX: the word waited on */
    uint32_t futex_bitset;
    int64_t deadline;               /* monotonic ns to give up waiting / sleeping; 0 = never */
    uint64_t clear_tid;             /* CLONE_CHILD_CLEARTID / set_tid_address */
    uint64_t sigmask;               /* blocked signals (bit sig-1) */
    uint64_t pending;               /* signals waiting to be delivered to this thread */
    uint64_t altstack[3];           /* sigaltstack: sp, flags, size */
    uint64_t sigwait_mask;          /* AOI_T_SLEEP in rt_sigtimedwait: signals that end it */
    uint64_t sigwait_info;          /* its siginfo_t pointer, or 0 */
    int restart;                    /* AOI_T_SLEEP before re-running a syscall: wake without touching x0 */
    int64_t poll_deadline;          /* epoll_pwait/ppoll being retried: when it times out (0: not waiting) */
};

struct aoi_proc {
    struct aoi_vm vm;
    struct aoi_mem mem;
    struct aoi_cpu cpu;
    char root[AOI_PATH];            /* host directory that is the guest's "/" */
    char data[AOI_PATH];            /* if set: host directory that is the guest's /data (root read-only) */
    char cwd[AOI_PATH];             /* guest path */
    char exe[AOI_PATH];             /* guest path of the program (/proc/self/exe) */
    char cmdline[AOI_PATH];         /* argv joined by NULs (/proc/self/cmdline) */
    size_t cmdline_len;
    struct aoi_proc_fd fd[AOI_PROC_FDS];
    uint64_t sigact[65][4];         /* rt_sigaction: handler, flags, restorer, mask */
    uint64_t brk;
    uint64_t stack_start;           /* initial sp (/proc/self/stat startstack) */
    FILE *trace;                    /* strace-style log, or NULL */
    FILE *log;                      /* where guest liblog lines go (logd emulation), or NULL */
    int uffd;                       /* offer userfaultfd (ART then picks the CMC GC and its boot image); off: ENOSYS */
    struct { uint64_t start, len; } uffd_reg[16];   /* ranges registered with a userfaultfd (missing mode) */
    int nuffd_reg;
    struct aoi_binder *binder;      /* in-process binder driver state (core/binder.c), or NULL */
    struct aoi_sf *sf;              /* SurfaceFlinger state (core/sf.c), or NULL */
    struct aoi_gralloc *gralloc;    /* graphics buffers (core/gralloc.c), or NULL */
    /* if set: called with each new frame SurfaceFlinger puts on screen, as tightly
     * packed 4-byte pixels (R, G, B, X/A) */
    void (*frame)(void *ctx, const uint8_t *pixels, uint32_t width, uint32_t height);
    void *frame_ctx;
    unsigned char unknown[512];     /* syscalls already reported as unimplemented */
    struct aoi_proc_map maps[AOI_PROC_MAPS];
    int nmaps;
    struct aoi_thread th[AOI_PROC_THREADS];
    int cur;                        /* index of the thread whose registers are in cpu */
    int next_tid;
    int thread_exit;                /* the running thread called exit (not exit_group) */
    uint64_t *samples;              /* if set: pc at the end of each time slice (profiling) */
    size_t nsamples, maxsamples;
};

/* Loads `path` (a guest path) with argv/envp into a fresh process. NULL on success. */
const char *aoi_proc_exec(struct aoi_proc *p, const char *root, const char *path,
                          int argc, const char *const *argv, const char *const *envp);

/* Runs all guest threads until the process exits or one stops (fault,
 * undefined instruction, deadlock); max_steps 0 = no limit. */
enum aoi_stop aoi_proc_run(struct aoi_proc *p, uint64_t max_steps);

void aoi_proc_free(struct aoi_proc *p);

/* "path+0xoff" for a guest code address, or "?" (for crash reports). */
const char *aoi_proc_where(struct aoi_proc *p, uint64_t addr, char *buf, size_t n);

/* A host fd becomes a guest fd of the given kind (AOI_FD_PIPE: read/write and
 * readiness through the host, non-blocking underneath). The guest fd, or -1. */
int aoi_proc_fd_install(struct aoi_proc *p, int host, const char *path, int kind);

/* Read-write anonymous guest memory for a host-side service (gralloc buffers), named
 * `name` in crash reports: its address, or an error value (>= -4096). */
uint64_t aoi_proc_map_anon(struct aoi_proc *p, uint64_t len, const char *name);
void aoi_proc_unmap_anon(struct aoi_proc *p, uint64_t addr, uint64_t len);

#endif
