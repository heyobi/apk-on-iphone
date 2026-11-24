#define _GNU_SOURCE                     /* cfmakeraw */
#define _DARWIN_C_SOURCE
/* lxrun: run a program from an ordinary aarch64 Linux root file system (Alpine's
 * minirootfs, a Debian rootfs) in the interpreter, as root, with a Linux
 * environment instead of Android's. The "Linux userland" mode of the core: the same
 * cpu, vm, proc and ELF loader as aoiproc, none of the Android services.
 *
 * usage: lxrun [-t] [-e NAME=VALUE]... [-C DIR] ROOT PROGRAM [args...]
 * -t logs every syscall to stderr (AOI_CHILD_TRACE=file adds child processes);
 * -C sets the starting directory (a guest path, default /root).
 * AOI_SNAPSHOT_SAVE=file saves the process when it opens /dev/aoi_snapshot;
 * AOI_SNAPSHOT_LOAD=file resumes one instead of starting PROGRAM.
 * -i: the guest gets a terminal (core/tty.c, as the iOS app's view): the host's own
 * terminal in raw mode, or, with LXRUN_KEYS, typed keys from a script: C escapes
 * (\r Enter, \x03 Ctrl-C, \e Esc, \t) and \w for a one-second pause. */
#include "../core/jit.h"
#include "../core/proc.h"
#include "../core/tty.h"

#include <pthread.h>
#include <termios.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

static struct aoi_proc proc;
static struct termios saved_tio;
static int raw_tty;

#ifdef __EMSCRIPTEN__
/* In a web view (tests/wasm/web): the terminal's output collects in JavaScript and the
 * scheduler's host turn (aoi_host_poll) sends it to the app and brings back the keys. */
#include <emscripten.h>
EM_JS(void, js_tty_out, (const char *s, int n), {
    if (!Module.lxOut) Module.lxOut = [];
    Module.lxOut.push(HEAPU8.slice(s, s + n));
});
EM_JS(int, js_tty_poll, (char *buf, int max, int wait_ms), {
    if (!Module.lxPoll) return 0;
    var out = Module.lxOut || [];
    Module.lxOut = [];
    var r = Module.lxPoll(out, wait_ms);
    if (!r || !r.length) return 0;
    var n = Math.min(r.length, max);
    HEAPU8.set(r.subarray(0, n), buf);
    return n;
});
static void web_poll(int64_t wait_ns)
{
    char b[4096];
    int n = js_tty_poll(b, (int)sizeof b, (int)(wait_ns / 1000000));
    if (n > 0) { aoi_tty_input(aoi_console, b, (size_t)n); aoi_wake_seq++; }
}
#endif

static void tty_out(void *ctx, const char *s, size_t n)
{
    (void)ctx;
#ifdef __EMSCRIPTEN__
    js_tty_out(s, (int)n);
    return;
#endif
    while (n) {
        ssize_t k = write(1, s, n);
        if (k <= 0) break;
        s += k; n -= (size_t)k;
    }
}

static void *keys_script(void *arg)
{
    const char *s = arg;
    unsigned gap = getenv("LXRUN_KEY_GAP") ? (unsigned)atoi(getenv("LXRUN_KEY_GAP")) : 200;
    struct timespec ts = { 1, 0 };
    nanosleep(&ts, NULL);                            /* let the program start */
    while (*s) {
        char c = *s++;
        if (c == '\\' && *s) {
            char e = *s++;
            if (e == 'w') { ts.tv_sec = 1; ts.tv_nsec = 0; nanosleep(&ts, NULL); continue; }
            if (e == 'r') c = '\r'; else if (e == 'n') c = '\n'; else if (e == 't') c = '\t';
            else if (e == 'e') c = 27; else if (e == '\\') c = '\\';
            else if (e == 'x' && s[0] && s[1]) { char h[3] = { s[0], s[1], 0 }; c = (char)strtol(h, NULL, 16); s += 2; }
            else c = e;
        }
        aoi_tty_input(aoi_console, &c, 1);
        ts.tv_sec = 0; ts.tv_nsec = (long)gap * 1000000L / 10;
        nanosleep(&ts, NULL);
        if (c == '\r') { ts.tv_sec = gap / 1000; ts.tv_nsec = (long)(gap % 1000) * 1000000L; nanosleep(&ts, NULL); }
    }
    return NULL;
}

static void *keys_host(void *arg)
{
    char b[256];
    (void)arg;
    for (;;) {
        ssize_t k = read(0, b, sizeof b);
        if (k <= 0) break;
        aoi_tty_input(aoi_console, b, (size_t)k);
    }
    return NULL;
}

static void restore_tty(void) { if (raw_tty) tcsetattr(0, TCSANOW, &saved_tio); }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static long peak_rss_mib(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return (long)(ru.ru_maxrss >> 20);
#else
    return ru.ru_maxrss >> 10;
#endif
}

extern uint64_t aoi_syscall_count;

int main(int argc, char **argv)
{
    static const char *base_env[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root", "USER=root", "LOGNAME=root", "SHELL=/bin/sh", "LANG=C.UTF-8", NULL };
    const char *envp[80], *cwd = "/root", *err;
    char term[64];
    int ne = 0, a = 1, trace = 0, quiet = getenv("LXRUN_QUIET") != NULL, use_tty = 0;
#ifdef AOI_JIT
    if (getenv("AOI_JIT") && !atoi(getenv("AOI_JIT"))) aoi_jit_enabled = 0;   /* AOI_JIT=0: interpreter only */
#endif
    enum aoi_stop st;
    double t0, t1;
    while (base_env[ne]) { envp[ne] = base_env[ne]; ne++; }
    snprintf(term, sizeof term, "TERM=%s", getenv("TERM") ? getenv("TERM") : "xterm-256color");
    envp[ne++] = term;
    for (;;) {
        if (argc > a && !strcmp(argv[a], "-t")) { trace = 1; a++; }
        else if (argc > a && !strcmp(argv[a], "-i")) { use_tty = 1; a++; }
        else if (argc > a + 1 && !strcmp(argv[a], "-e") && ne < 79) { envp[ne++] = argv[a + 1]; a += 2; }
        else if (argc > a + 1 && !strcmp(argv[a], "-C")) { cwd = argv[a + 1]; a += 2; }
        else break;
    }
    envp[ne] = NULL;
    if (argc - a < 2 && !(argc - a == 1 && getenv("AOI_SNAPSHOT_LOAD"))) {
        fprintf(stderr, "usage: %s [-t] [-e NAME=VALUE]... [-C DIR] ROOT PROGRAM [args...]\n", argv[0]);
        return 2;
    }
    t0 = now_s();
    if (getenv("AOI_SNAPSHOT_LOAD")) {
        if ((err = aoi_snap_load(&proc, getenv("AOI_SNAPSHOT_LOAD"), argv[a], NULL))) {
            fprintf(stderr, "[lxrun] snapshot %s: %s\n", getenv("AOI_SNAPSHOT_LOAD"), err);
            return 1;
        }
        if (!quiet) fprintf(stderr, "[lxrun] snapshot loaded in %.3f s\n", now_s() - t0);
    } else {
        if ((err = aoi_proc_exec(&proc, argv[a], argv[a + 1], argc - a - 1, (const char *const *)argv + a + 1,
                                 (const char *const *)envp))) {
            fprintf(stderr, "[lxrun] exec %s: %s\n", argv[a + 1], err);
            return 1;
        }
        aoi_proc_linux(&proc, cwd);
    }
    {
        struct rlimit rl;                            /* every process dups its fds */
        if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur < 4096) {
            rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
    }
#ifdef __EMSCRIPTEN__
    if (use_tty) {                                   /* no host terminal or threads: the host turn */
        aoi_tty_new(tty_out, NULL, 80, 24);
        aoi_proc_attach_tty(&proc);
        aoi_host_poll = web_poll;
        use_tty = 0;
    }
#endif
    if (use_tty) {
        pthread_t th;
        int cols = 80, rows = 24;
        if (!getenv("LXRUN_KEYS") && isatty(0) && !tcgetattr(0, &saved_tio)) {
            struct termios t = saved_tio;
            cfmakeraw(&t);
            tcsetattr(0, TCSANOW, &t);
            raw_tty = 1;
            atexit(restore_tty);
        }
        aoi_tty_new(tty_out, NULL, cols, rows);
        aoi_proc_attach_tty(&proc);
        if (getenv("LXRUN_KEYS")) pthread_create(&th, NULL, keys_script, (void *)getenv("LXRUN_KEYS"));
        else pthread_create(&th, NULL, keys_host, NULL);
        pthread_detach(th);
    }
    if (getenv("AOI_SNAPSHOT_SAVE")) snprintf(proc.snap_path, sizeof proc.snap_path, "%s", getenv("AOI_SNAPSHOT_SAVE"));
    if (trace) proc.trace = stderr;
    proc.log = quiet ? NULL : stderr;
    t0 = now_s();
    st = aoi_proc_run(&proc, 0);
    t1 = now_s();
#ifdef __EMSCRIPTEN__
    if (aoi_host_poll) aoi_host_poll(0);             /* the last output */
#endif
    fflush(stdout);
    if (st == AOI_STOP_EXIT) {
        if (!quiet || getenv("LXRUN_STATS"))
            fprintf(stderr, "[lxrun] exit %d, %" PRIu64 " instructions, %.2f s, peak RSS %ld MiB, %" PRIu64 " syscalls (all processes)\n",
                    proc.cpu.exit_code, proc.cpu.steps, t1 - t0, peak_rss_mib(), aoi_syscall_count);
#ifdef AOI_JIT
        if (!quiet || getenv("LXRUN_STATS"))
            fprintf(stderr, "[lxrun] accelerator: %" PRIu64 " blocks translated, %" PRIu64 " refused by the engine, %" PRIu64 " reused, %" PRIu64 " region runs\n",
                    aoi_jit_blocks, aoi_jit_failed, aoi_jit_reused, aoi_jit_runs);
#endif
        return proc.cpu.exit_code & 0xff;
    }
    {
        char w1[256], w2[256];
        fprintf(stderr, "[lxrun] stopped (%d) at pc %s, lr %s, after %" PRIu64 " instructions", (int)st,
                aoi_proc_where(&proc, proc.cpu.pc, w1, sizeof w1), aoi_proc_where(&proc, proc.cpu.x[30], w2, sizeof w2),
                proc.cpu.steps);
        if (st == AOI_STOP_UNDEF) fprintf(stderr, ": undefined instruction %#010x", proc.cpu.fault_insn);
        if (st == AOI_STOP_FAULT) fprintf(stderr, ": fault at %#" PRIx64, proc.cpu.fault_addr);
        fprintf(stderr, "\n");
    }
    return 128 + 11;
}
