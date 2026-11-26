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
#include "../core/lxfast.h"

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

#ifndef __EMSCRIPTEN__
/* LXRUN_PROF=FILE: a sampling profile of the main process's guest code. Every millisecond
 * of CPU time (SIGPROF), the guest pc of the main process's running thread is noted as
 * (mapped file, offset); at exit FILE gets one "count path offset" line per sample site,
 * for tests/linux/prof_resolve.py to turn into functions. The handler only reads memory
 * and copies a path into its own table the first time it sees it. */
#include <signal.h>
#include <sys/time.h>
#define PROF_PATHS 512
#define PROF_SLOTS (1u << 20)
static char prof_path[PROF_PATHS][160];
static int prof_npaths;
static uint64_t *prof_key;                                         /* (path index + 1) << 40 | offset */
static uint32_t *prof_cnt;
static uint64_t prof_samples, prof_lost;
static pthread_t prof_main;

static void prof_tick(int sig)
{
    uint64_t pc, off = 0, key, h;
    int i, k = -1;
    (void)sig;
    if (!pthread_equal(pthread_self(), prof_main)) return;         /* (other guest processes' threads) */
    pc = proc.cpu.pc;
    for (i = proc.nmaps - 1; i >= 0; i--) {
        struct aoi_proc_map *m = &proc.maps[i];
        if (pc >= m->start && pc < m->start + m->len) {
            off = pc - m->start + m->off;
            for (k = 0; k < prof_npaths && strcmp(prof_path[k], m->path); k++) {}
            if (k == prof_npaths) {
                if (k >= PROF_PATHS - 1) { prof_lost++; return; }  /* (the last: outside every mapping) */
                memcpy(prof_path[k], m->path, sizeof prof_path[k]);
                prof_path[k][sizeof prof_path[k] - 1] = 0;
                prof_npaths++;
            }
            break;
        }
    }
    if (k < 0) { k = PROF_PATHS - 1; off = pc; }                   /* (outside every mapping) */
    key = ((uint64_t)(k + 1) << 40) | (off & 0xffffffffffULL);
    for (h = (key * 0x9e3779b97f4a7c15ULL) >> 44, i = 0; i < 64; i++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (prof_key[h] == key) { prof_cnt[h]++; prof_samples++; return; }
        if (!prof_key[h]) { prof_key[h] = key; prof_cnt[h] = 1; prof_samples++; return; }
    }
    prof_lost++;
}

static void prof_start(void)
{
    struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
    if (!getenv("LXRUN_PROF")) return;
    prof_key = calloc(PROF_SLOTS, sizeof *prof_key);
    prof_cnt = calloc(PROF_SLOTS, sizeof *prof_cnt);
    if (!prof_key || !prof_cnt) return;
    prof_main = pthread_self();
    signal(SIGPROF, prof_tick);
    setitimer(ITIMER_PROF, &it, NULL);
}

static void prof_write(void)
{
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    FILE *f;
    uint32_t h;
    if (!prof_key) return;
    setitimer(ITIMER_PROF, &off, NULL);
    if (!(f = fopen(getenv("LXRUN_PROF"), "w"))) return;
    fprintf(f, "# samples %" PRIu64 " lost %" PRIu64 " (1 ms of CPU time each)\n", prof_samples, prof_lost);
    for (h = 0; h < PROF_SLOTS; h++) {
        uint64_t k;
        if (!(k = prof_key[h])) continue;
        fprintf(f, "%u %s %#llx\n", prof_cnt[h], (int)(k >> 40) - 1 == PROF_PATHS - 1 ? "?" : prof_path[(k >> 40) - 1],
                (unsigned long long)(k & 0xffffffffffULL));
    }
    fclose(f);
}
#else
static void prof_start(void) {}
static void prof_write(void) {}
#endif

#ifdef __EMSCRIPTEN__
/* In a web view (tests/wasm/web): the terminal's output collects in JavaScript and the
 * scheduler's host turn (aoi_host_poll) sends it to the app and brings back the keys. */
#include <emscripten.h>
/* Several terminals (tabs) in one core: each is a session (a tty and the bash on it),
 * known to the host by its id. Output goes out as (id, bytes) pairs; the host's answer is
 * frames of keys, [u32 id][u32 length][bytes]..., plus control words in Module.lxCtl:
 * "N id cols rows" a new session, "S id cols rows" a new size, "C id" close it. The host
 * hears "T id name" (the tab title: the foreground program) and "E id" (the session ended).
 * Module.lxPollMulti(out, waitMs, meta) speaks this; a host with only Module.lxPoll(out,
 * waitMs) (one terminal, Module.lxSize) gets session 0. */
EM_JS(void, js_tty_out, (int id, const char *s, int n), {
    if (!Module.lxOut) Module.lxOut = [];
    Module.lxOut.push([id, HEAPU8.slice(s, s + n)]);
});
EM_JS(int, js_tty_poll, (char *buf, int max, int wait_ms, const char *meta), {
    var out = Module.lxOut || [], r = null;
    Module.lxOut = [];
    if (Module.lxPollMulti) r = Module.lxPollMulti(out, wait_ms, UTF8ToString(meta));
    else if (Module.lxPoll) {
        var k = Module.lxPoll(out.map(function (x) { return x[1]; }), wait_ms);
        if (k && k.length) {
            r = new Uint8Array(8 + k.length);
            var dv = new DataView(r.buffer);
            dv.setUint32(0, 0, true); dv.setUint32(4, k.length, true);
            r.set(k, 8);
        }
        if (Module.lxSize) { Module.lxCtl = (Module.lxCtl || '') + 'S 0 ' + Module.lxSize[0] + ' ' + Module.lxSize[1] + ';'; Module.lxSize = null; }
    }
    if (!r || !r.length) return 0;
    var n = Math.min(r.length, max);
    HEAPU8.set(r.subarray(0, n), buf);
    return n;
});
EM_JS(int, js_tty_ctl, (char *buf, int max), {
    var s = Module.lxCtl || '';
    Module.lxCtl = '';
    if (!s) return 0;
    stringToUTF8(s, buf, max);
    return 1;
});

struct wsess { int used, id, sid, gone; struct aoi_tty *t; char title[64]; };
static struct wsess ws[AOI_TTYS];
static const char *web_root, *const *web_argv, *const *web_envp;
static int web_argc;

static void web_out(void *ctx, const char *s, size_t n) { js_tty_out(((struct wsess *)ctx)->id, s, (int)n); }

static struct wsess *web_find(int id)
{
    int i;
    for (i = 0; i < AOI_TTYS; i++) if (ws[i].used && ws[i].id == id) return &ws[i];
    return NULL;
}

static void web_new(int id, int cols, int rows)
{
    struct wsess *w = NULL;
    struct aoi_proc *np;
    int i;
    if (web_find(id)) return;
    for (i = 0; i < AOI_TTYS; i++) if (!ws[i].used) { w = &ws[i]; break; }
    if (!w || !(np = calloc(1, sizeof *np))) return;
    {
        struct aoi_tty *t = w->t;                    /* a closed tab's terminal is used again (there are AOI_TTYS) */
        memset(w, 0, sizeof *w);
        w->t = t;
    }
    w->id = id;
    {
        const char *e = aoi_proc_exec(np, web_root, web_argv[0], web_argc, web_argv, web_envp);
        if (e) { fprintf(stderr, "[lxrun] session %d: %s\n", id, e); free(np); return; }
    }
    aoi_proc_linux(np, "/root");
    if (w->t) aoi_tty_resize(w->t, cols, rows);
    else if (!(w->t = aoi_tty_new(web_out, w, cols, rows))) { aoi_proc_free(np); free(np); return; }
    aoi_proc_attach_tty_to(np, w->t);
    np->log = proc.log;
    if (getenv("LXRUN_DEBUG") && getenv("LXRUN_DEBUG")[0] == '2') { np->trace = stderr; np->trace_shared = 1; }
    w->sid = np->sid;
    w->used = 1;
    if (aoi_proc_adopt(np)) { w->used = 0; aoi_proc_free(np); free(np); }
    if (getenv("LXRUN_DEBUG")) fprintf(stderr, "[lxrun] session %d: pid %d, pts/%d, %s\n", id, w->sid, aoi_tty_index(w->t), w->used ? "running" : "failed");
}

static void web_poll(int64_t wait_ns)
{
    static char b[65536];
    char meta[2048], ctl[2048];
    size_t ml = 0;
    int i, n;
    meta[0] = 0;
    for (i = 0; i < AOI_TTYS; i++) {
        struct wsess *w = &ws[i];
        char name[64];
        if (!w->used) continue;
        if (w->gone || !aoi_proc_alive(w->sid)) {
            ml += (size_t)snprintf(meta + ml, sizeof meta - ml, "E %d;", w->id);
            w->used = 0;
            continue;
        }
        if (aoi_proc_name_of_pgrp(aoi_tty_fg(w->t), name, sizeof name)) snprintf(name, sizeof name, "bash");
        for (n = 0; name[n]; n++) if (name[n] == ';' || name[n] < ' ') name[n] = '_';
        if (strcmp(name, w->title)) {
            snprintf(w->title, sizeof w->title, "%s", name);
            ml += (size_t)snprintf(meta + ml, sizeof meta - ml, "T %d %s;", w->id, name);
        }
        if (ml >= sizeof meta - 80) break;
    }
    n = js_tty_poll(b, (int)sizeof b, (int)(wait_ns / 1000000), meta);
    for (i = 0; i + 8 <= n;) {
        uint32_t id, len;
        struct wsess *w;
        memcpy(&id, b + i, 4); memcpy(&len, b + i + 4, 4);
        i += 8;
        if (len > (uint32_t)(n - i)) len = (uint32_t)(n - i);
        if ((w = web_find((int)id)) && len) { aoi_tty_input(w->t, b + i, len); aoi_wake_seq++; }
        i += (int)len;
    }
    if (js_tty_ctl(ctl, (int)sizeof ctl)) {
        char *s = ctl, *e;
        for (; *s; s = e + 1) {
            int id, c, r;
            struct wsess *w;
            if (!(e = strchr(s, ';'))) break;
            *e = 0;
            if (getenv("LXRUN_DEBUG")) fprintf(stderr, "[lxrun] ctl %s\n", s);
            if (sscanf(s, "N %d %d %d", &id, &c, &r) == 3) web_new(id, c, r);
            else if (sscanf(s, "S %d %d %d", &id, &c, &r) == 3) { if ((w = web_find(id))) aoi_tty_resize(w->t, c, r); }
            else if (sscanf(s, "C %d", &id) == 1) { if ((w = web_find(id))) aoi_proc_kill_session(w->sid, 9); }
        }
        aoi_wake_seq++;
    }
}
#endif

static void tty_out(void *ctx, const char *s, size_t n)
{
    (void)ctx;
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
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games",
        "HOME=/root", "USER=root", "LOGNAME=root", "SHELL=/bin/sh", "LANG=C.UTF-8", NULL };
    const char *envp[80], *cwd = "/root", *err;
    char term[64];
    int ne = 0, a = 1, trace = 0, quiet = getenv("LXRUN_QUIET") != NULL, use_tty = 0;
#ifdef AOI_JIT
    if (getenv("AOI_JIT") && !atoi(getenv("AOI_JIT"))) aoi_jit_enabled = 0;   /* AOI_JIT=0: interpreter only */
    if (getenv("AOI_JIT_HOT")) aoi_jit_hot = atoi(getenv("AOI_JIT_HOT"));
#endif
    if (getenv("LXRUN_BIND") && strchr(getenv("LXRUN_BIND"), '=')) {   /* GUEST=HOST: a host directory in the guest */
        char b[1024], *eq;
        snprintf(b, sizeof b, "%s", getenv("LXRUN_BIND"));
        eq = strchr(b, '='); *eq = 0;
        aoi_proc_bind(b, eq + 1);
    }
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
        ws[0].id = getenv("LXRUN_SESSION") ? atoi(getenv("LXRUN_SESSION")) : 0;
        ws[0].t = aoi_tty_new(web_out, &ws[0], 80, 24);
        ws[0].sid = proc.sid;
        ws[0].used = 1;
        aoi_proc_attach_tty(&proc);
        web_root = argv[a];
        web_argv = (const char *const *)argv + a + 1;
        web_argc = argc - a - 1;
        web_envp = (const char *const *)envp;
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
    prof_start();
    st = aoi_proc_run(&proc, 0);
    prof_write();
    t1 = now_s();
#ifdef __EMSCRIPTEN__
    if (aoi_host_poll) {
        ws[0].gone = 1;                              /* (its process is no longer on the list) */
        aoi_host_poll(0);                            /* the last output */
        if (getenv("LXRUN_MULTI"))                   /* the other tabs, and new ones, until the page goes */
            for (;;) if (!aoi_proc_turn_others()) aoi_host_poll(100000000);
    }
#endif
    fflush(stdout);
    if (st == AOI_STOP_EXIT) {
        if (!quiet || getenv("LXRUN_STATS"))
            fprintf(stderr, "[lxrun] exit %d, %" PRIu64 " instructions, %.2f s, peak RSS %ld MiB, %" PRIu64 " syscalls (all processes)\n",
                    proc.cpu.exit_code, proc.cpu.steps, t1 - t0, peak_rss_mib(), aoi_syscall_count);
        if (getenv("LXRUN_STATS"))
            fprintf(stderr, "[lxrun] libc on the host: %" PRIu64 " calls, %" PRIu64 " handed back to the guest's code (all processes)\n",
                    aoi_lxfast_hits, aoi_lxfast_declined);
#ifdef AOI_JIT
        if (!quiet || getenv("LXRUN_STATS"))
            fprintf(stderr, "[lxrun] accelerator: %" PRIu64 " blocks translated, %" PRIu64 " refused by the engine, %" PRIu64 " reused, %" PRIu64 " region runs, %d KiB compiled in %d ms\n",
                    aoi_jit_blocks, aoi_jit_failed, aoi_jit_reused, aoi_jit_runs, aoi_jit_kbytes(), aoi_jit_compile_ms());
        if (getenv("LXRUN_STATS")) aoi_jit_report(stderr);
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
