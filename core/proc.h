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

#define AOI_PROC_FDS 1024               /* RLIMIT_NOFILE (1024, as on Linux; 256 was too few for asyncio and Python's test suite) */
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
    int pair, end, ptype;           /* AOI_FD_PIPE: one end (0/1) of host pair `pair` (0: none);
                                     * ptype 0 pipe, else a socket type: a snapshot rebuilds it */
    int connecting;                 /* AOI_FD_INET: a blocking connect waits for the host's */
    int v4;                         /* AOI_FD_INET: an AF_INET6 guest socket on an AF_INET host one */
    int peer;                       /* AOI_FD_DNS: host end the answer is written to (-1 once sent) */
    int nreq;                       /* AOI_FD_DNS: bytes of the request so far, in req */
    char *req;
    int seals;                      /* memfd: F_ADD_SEALS so far (not enforced) */
    int64_t tnext, tint;            /* AOI_FD_TIMERFD: next expiry (monotonic ns, 0: disarmed), interval */
    int tclock;                     /* AOI_FD_TIMERFD: its clock (0 realtime, else monotonic) */
    int evid;                       /* AOI_FD_EVENTFD: its counter in aoi_proc.ev (0: f->count, unshared) */
    int cloexec;                    /* FD_CLOEXEC as the guest set it: not passed to a child's execve */
};

enum { AOI_FD_FILE = 0, AOI_FD_SOCKET, AOI_FD_LOGD, AOI_FD_UFFD, AOI_FD_PIPE, AOI_FD_BINDER,
       AOI_FD_EVENTFD, AOI_FD_EPOLL,
       AOI_FD_INET,                 /* AF_INET/AF_INET6: a host socket, non-blocking underneath */
       AOI_FD_DNS,                  /* netd's /dev/socket/dnsproxyd, answered with the host's resolver */
       AOI_FD_TIMERFD,              /* timerfd: expirations counted in `count` */
       AOI_FD_DMAHEAP,              /* /dev/dma_heap/system: allocations are memfds */
       AOI_FD_TTY,                  /* Linux mode: a terminal's slave side (core/tty.c) */
       AOI_FD_PTM,                  /* Linux mode: a pty's master side (/dev/ptmx) */
       AOI_FD_ULISTEN,              /* Linux mode: a listening AF_UNIX socket with a file name */
       AOI_FD_SIGNALFD };           /* Linux mode: signalfd (its mask in count) */

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
    char comm[16];                  /* prctl PR_SET_NAME (pthread_setname_np); "" until named */
    uint64_t susp_old;              /* rt_sigsuspend: the mask a handler's frame restores */
    int susp;                       /* ... and whether the next delivered frame takes it */
};

/* An eventfd's counter, shared by every fd for it (dup, SCM_RIGHTS), as Linux's is. */
struct aoi_evfd { int refs, sem; uint64_t count; };
#define AOI_PROC_EVFDS 256

struct aoi_vfork;
struct aoi_child;

struct aoi_proc {
    struct aoi_vm vm;
    struct aoi_mem mem;
    struct aoi_cpu cpu;
    struct aoi_evfd ev[AOI_PROC_EVFDS];   /* eventfd counters (index 0 unused) */
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
    struct aoi_af *af;              /* AudioFlinger state (core/af.c), or NULL */
    void (*audio)(void *ctx, const int16_t *lr, unsigned frames);   /* the mixed output: 48 kHz stereo (or NULL) */
    void *audio_ctx;
    struct aoi_gralloc *gralloc;    /* graphics buffers (core/gralloc.c), or NULL */
    /* if set: called with each new frame SurfaceFlinger puts on screen, as tightly
     * packed 4-byte pixels (R, G, B, X/A) */
    void (*frame)(void *ctx, const uint8_t *pixels, uint32_t width, uint32_t height);
    void *frame_ctx;
    /* if set: called (with frame_ctx) when the app leaves for the launcher: aoi.Main
     * opens "/dev/aoi_home" (back on its root activity, finish) */
    void (*home)(void *ctx);
    /* if set: the host's clipboard, for aoi.Clipboard, which opens "/dev/aoi_clip/OP":
     * 's' set it to the UTF-8 text in the file `path` (guest /data/local/tmp/aoi.clip),
     * 'g' write its text there (or remove the file: no text), 'h' write "1" or "0"
     * there: whether it has text (without reading it: iOS asks the user on a read),
     * 'u' open the URL in the file (a link no activity of the app handles) */
    void (*clip)(void *ctx, int op, const char *path);
    /* if set: the host's keyboard, for aoi.InputMethodManager, which opens
     * "/dev/aoi_ime/1" to show it and "/dev/aoi_ime/0" to hide it; what is typed
     * comes back through aoi_proc_key. */
    void (*ime)(void *ctx, int show);
    /* if set: Android started before its app is chosen (the iOS app's warm process):
     * aoi.Main waits in open("/dev/aoi_warm") until this returns 1, on the guest's
     * thread, having made the process the app's (its /data, log, frames). */
    int (*warm)(struct aoi_proc *p);
    /* The guest's OpenGL ES (core/gpu.h): AOI_SYS_GL goes here (gpu/host.c), or is
     * ENOSYS when the host has no GPU for it. */
    uint64_t (*gpu)(void *ctx, struct aoi_proc *p, uint64_t op, uint64_t args);
    void *gpu_ctx;
    int gpu_live;                   /* the host's contexts and surfaces it holds: no snapshot while any */
    int snap_gpu_free;              /* aoi.Snapshot: take it the moment gpu_live reaches 0 */
    int input_w;                    /* host write end of /dev/aoi_input (touches for aoi.Input), or 0 */
    int input_pair;                 /* its pair id (the guest holds end 0) */
    int next_pair;                  /* pair ids of host pipes and socket pairs */
    /* writable MAP_SHARED file mappings: their pages go back to the file (a dup of
     * the host fd) on msync, munmap and exit; the guest's view is a copy */
    struct { uint64_t addr, len, off; int fd; } shm[32];
    int nshm;
    volatile int stop_request;      /* set from another host thread: aoi_proc_run returns AOI_RUN */
    int one_resched;                /* one-thread build: its last turn ended with every thread waiting */
    unsigned wake_seen;             /* the aoi_wake_seq this process last saw */
    uint32_t uid;                   /* getuid & co. and every file's owner: an app's (10100), or root */
    volatile int pause_request;     /* set from another host thread: it waits between time slices */
    char snap_path[AOI_PATH];       /* if set: where the guest's open("/dev/aoi_snapshot") saves a snapshot */
    volatile int snap_request;
    volatile int redraw_request;
    int inotify_wd;                 /* the last inotify watch descriptor handed out */
    uint32_t posix_timers;          /* timer_create ids handed out */    /* the screen to p->frame again at the next time slice (a new frame target) */      /* that open happened: saved at the next time slice (core/snap.c) */
    unsigned char unknown[512];     /* syscalls already reported as unimplemented */
    struct aoi_proc_map maps[AOI_PROC_MAPS];
    int nmaps;
    struct aoi_thread th[AOI_PROC_THREADS];
    int cur;                        /* index of the thread whose registers are in cpu */
    int next_tid;
    struct aoi_vfork *vf;           /* a vfork()ed child, run here until its execve (core/proc.c) */
    struct aoi_child **child;       /* child processes (Runtime.exec): guest processes on host threads */
    int nchild, next_pid;
    int own_stdio;                  /* fds 0-2 are this process's own host fds (a child's pipes): close them */
    int thread_exit;                /* the running thread called exit (not exit_group) */
    /* Linux userland mode (aoi_proc_linux): an ordinary Linux root (Alpine, Debian) run as
     * root, with real fork() and execve(), process ids, groups and signals between processes.
     * Off for Android, whose code paths stay as they were. */
    int linux_mode;
    int pid, ppid, pgid, sid;       /* 0: GUEST_PID / 1 (Android's single process) */
    volatile uint64_t xsig;         /* signals sent by other processes (bit sig-1), taken between slices */
    int term_sig;                   /* the signal that ended the process (wait status), or 0 */
    int trace_shared;               /* trace is the parent's FILE: not closed here */
    uint32_t cred[6];               /* real, effective, saved uid; the same for gid (0: root) */
    int ctty;                       /* controlling terminal: /dev/pts/(ctty-1), 0: the console */
    uint32_t umask;                 /* Linux mode: the file mode creation mask */
    int64_t itimer_at, itimer_iv;   /* ITIMER_REAL: when SIGALRM is due (0: off), its interval */
    volatile int stopped;           /* job control: stopped by this signal (SIGTSTP...), 0: running */
    void *me_child;                 /* its struct aoi_child in the parent (stop/continue reports) */
    int64_t start_ns;               /* when it started (/proc/<pid>/stat) */
    uint64_t *samples;              /* if set: pc at the end of each time slice (profiling) */
    size_t nsamples, maxsamples;
};

/* Every guest thread's state, pc, lr and frame-pointer backtrace, to p->log
 * (/dev/aoi_threads; the host after the app stops on a fault). */
void aoi_proc_log_threads(struct aoi_proc *p);

/* Loads `path` (a guest path) with argv/envp into a fresh process. NULL on success. */
const char *aoi_proc_exec(struct aoi_proc *p, const char *root, const char *path,
                          int argc, const char *const *argv, const char *const *envp);

/* Runs all guest threads until the process exits or one stops (fault,
 * undefined instruction, deadlock); max_steps 0 = no limit. */
enum aoi_stop aoi_proc_run(struct aoi_proc *p, uint64_t max_steps);

void aoi_proc_free(struct aoi_proc *p);

/* Linux userland mode, right after aoi_proc_exec: root (uid 0), starting directory `cwd`,
 * fork/execve/process groups/signals between processes, /proc for every process. */
void aoi_proc_linux(struct aoi_proc *p, const char *cwd);
/* Every process: the guest directory `guest` (e.g. /root/Files) is the host directory `host`
 * (e.g. the app's folder shared with the Files app), not the one inside the root. */
void aoi_proc_bind(const char *guest, const char *host);
/* Linux mode: a new top-level process (after aoi_proc_exec + aoi_proc_linux) runs beside the
 * others, on its own thread or, in one-thread builds, taking turns; it frees itself when it
 * ends. 0, or -1. (Another session in the same address-space registry: a terminal tab.) */
int aoi_proc_adopt(struct aoi_proc *np);
/* Linux mode: is a process with this pid alive? */
int aoi_proc_alive(int pid);
/* One-thread builds: gives every adopted or child process one turn (when no aoi_proc_run is
 * active, e.g. after the first process ended). 1 if any ran. */
int aoi_proc_turn_others(void);

/* Linux mode: sends sig to every process of group pgid (the terminal's Ctrl-C), from any
 * host thread. Returns how many got it. */
int aoi_proc_kill_group(int pgid, int sig);
/* Linux mode: fds 0-2 become the terminal (core/tty.c's aoi_console, or tty). */
void aoi_proc_attach_tty(struct aoi_proc *p);
struct aoi_tty;
void aoi_proc_attach_tty_to(struct aoi_proc *p, struct aoi_tty *tty);
/* Linux mode: a process restored from a snapshot (core/snap.c) rejoins the registry. */
void aoi_proc_linux_resume(struct aoi_proc *p);
/* Linux mode: live child processes; SIGs to a whole session; the name of a group's leader. */
int aoi_proc_children(struct aoi_proc *p);
int aoi_proc_kill_session(int sid, int sig);
int aoi_proc_name_of_pgrp(int pgrp, char *out, size_t n);
/* The program that last started listening on TCP `port` (0), or -1 if none was noted. */
int aoi_proc_listener_name(int port, char *out, size_t n);
/* Linux mode: the terminal's foreground process group (TIOCSPGRP), or 0. */
extern volatile int aoi_tty_pgrp;

/* "path+0xoff" for a guest code address, or "?" (for crash reports). */
const char *aoi_proc_where(struct aoi_proc *p, uint64_t addr, char *buf, size_t n);

/* A host fd becomes a guest fd of the given kind (AOI_FD_PIPE: read/write and
 * readiness through the host, non-blocking underneath). The guest fd, or -1. */
int aoi_proc_fd_install(struct aoi_proc *p, int host, const char *path, int kind);

/* A memfd of `size` zero bytes made for the guest by a host-side service (AudioFlinger's
 * track buffers), named "memfd:<name>": the guest fd, or -1. */
int aoi_proc_memfd(struct aoi_proc *p, const char *name, uint64_t size);
/* The host fd behind guest fd `fd`, or -1. */
int aoi_proc_host_fd(struct aoi_proc *p, int fd);

/* Wakes up to n guest threads waiting on the futex word at guest address addr. */
int aoi_proc_futex_wake(struct aoi_proc *p, uint64_t addr, int n);

/* Read-write anonymous guest memory for a host-side service (gralloc buffers), named
 * `name` in crash reports: its address, or an error value (>= -4096). */
/* A touch for the app (any host thread): action 0 down, 1 up, 2 move, at (x, y) in
 * screen pixels. Goes to /dev/aoi_input, which java/src/aoi/Input.java reads and
 * turns into MotionEvents on the window's input channel. Dropped until it is open. */
void aoi_proc_touch(struct aoi_proc *p, int action, float x, float y);
/* A key from the host's keyboard to aoi.InputMethodManager: action 6 types the
 * character `value` (a Unicode code point), 7 is backspace, 8 enter, 9 says the user
 * closed the keyboard. */
void aoi_proc_key(struct aoi_proc *p, int action, int32_t value);

/* A host AF_UNIX pair that keeps message boundaries: SOCK_SEQPACKET, or where the
 * host has none (Darwin) SOCK_DGRAM with room for many messages. 0 or -1. */
int aoi_host_msgpair(int sv[2]);

/* The guest's CLOCK_MONOTONIC (the host's plus an offset a restored snapshot sets, so
 * guest time never goes back), in ns. Used for every guest-visible clock and deadline. */
int64_t aoi_mono_ns(void);
extern int64_t aoi_mono_offset;

/* A host fd for guest path `guest` opened with host open() flags (synthetic /proc
 * files included), or -1 (core/snap.c reopens files with it). */
int aoi_proc_open_host(struct aoi_proc *p, const char *guest, int flags);
/* The host path of guest path `guest` (AOI_PATH bytes; "" if it has none). */
void aoi_proc_host_path(struct aoi_proc *p, const char *guest, char *out);

/* Snapshots (core/snap.c): the whole process (memory, threads, fds, binder,
 * SurfaceFlinger, gralloc) saved to a file, and a process made from one, ready for
 * aoi_proc_run. NULL, or what went wrong. A snapshot is only valid for the same
 * build of this code. */
const char *aoi_snap_save(struct aoi_proc *p, const char *path);
const char *aoi_snap_load(struct aoi_proc *p, const char *path, const char *root, const char *data);
/* 1 if the snapshot at path was saved by this build (aoi_snap_load would take it). */
int aoi_snap_this_build(const char *path);
/* Only the snapshot's memory and registers into p (for an exec from a saved process,
 * "zygote"): no fds, not registered as a process, the guest clock untouched. */
const char *aoi_snap_load_image(struct aoi_proc *p, const char *path, const char *root);
/* Fast start for an interpreted program (Linux mode): exec of EXE (guest path, as resolved)
 * runs INTERP SCRIPT GIVEN ARGS...; SCRIPT imports what the program needs and opens
 * /dev/aoi_snapshot, which saves the process under ROOT/var/cache/lxzygote/; later execs of
 * EXE resume that snapshot instead while SCRIPT, every file the snapshot maps and the guest
 * paths in WATCH (comma-separated: the program's Python folders) are unchanged. Without
 * SCRIPT, EXE runs as usual. The script reads its arguments from /proc/self/cmdline (INTERP
 * SCRIPT [--env=FILE] GIVEN ARGS...) and, when given, the environment from FILE.
 * Off when exe is NULL. AOI_ZYGOTE=0 in the host's environment turns every rule off. */
void aoi_proc_zygote(const char *exe, const char *interp, const char *script, const char *watch);
/* Raises the host process's file descriptor limit as far as the system allows: every guest
 * process's fds are host fds of this one process (a guest has up to AOI_PROC_FDS of its own). */
void aoi_proc_raise_nofile(void);

/* Gives guest fds a and b (ends 0 and 1 of one host pair of type ptype) a new pair id,
 * so a snapshot can rebuild the pair. Returns the id. */
int aoi_proc_pair(struct aoi_proc *p, int a, int b, int ptype);

uint64_t aoi_proc_map_anon(struct aoi_proc *p, uint64_t len, const char *name);
void aoi_proc_unmap_anon(struct aoi_proc *p, uint64_t addr, uint64_t len);

/* One-thread (WebAssembly) builds: the host's turn while guest processes wait or run (core/proc.c). */
extern void (*aoi_host_poll)(int64_t wait_ns);
extern volatile unsigned aoi_wake_seq;

#endif
