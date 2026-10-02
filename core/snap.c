/* Snapshots of a whole guest process, so an app that took a minute to start can be
 * resumed in seconds: aoi.Main asks for one (open "/dev/aoi_snapshot") once the app
 * is idle, and the next launch loads it instead of exec-ing app_process64.
 *
 * What is saved:
 *   - struct aoi_proc as is (registers, threads, signal state, fds, map names), its
 *     host pointers and host fds fixed up on load;
 *   - guest memory page by page: a page that holds what its file mapping would read
 *     (unchanged code and data of the .so, .oat, .art, fonts...) is recorded as such
 *     and mapped from the file again on load, all-zero pages are left out, the rest
 *     is stored;
 *   - epoll interest lists, file offsets; host pipes and socket pairs are rebuilt
 *     from their pair ids (data in flight is lost: the app is idle);
 *   - binder, SurfaceFlinger and gralloc state (their own *_snap functions);
 *   - the apps' files under /data/data, put back on load (see save_data).
 * The guest's monotonic clock continues from where it was if the host's restarted.
 * A snapshot is only valid for the build that wrote it (AOI_SNAP_BUILD; on the host
 * AOI_SNAP_ANY_BUILD=1 skips that, to compare interpreter changes that keep the structs). */
#define _GNU_SOURCE                 /* pread */
#define _DARWIN_C_SOURCE
#include "binder.h"
#include "hle.h"
#include "proc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#define SNAP_MAGIC "AOISNAP1"
#define AOI_SNAP_BUILD __DATE__ " " __TIME__
#define PAGE AOI_VM_PAGE
#define PER_CHUNK (AOI_VM_CHUNK / AOI_VM_PAGE)

enum { REC_END = 0, REC_DATA = 1, REC_FILE = 2 };

uint64_t aoi_proc_syscall(struct aoi_cpu *c);

static int w(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n; }
static int r(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

/* A map name that is a file whose bytes the pages show (not a device or a label). */
static int file_name(const char *path)
{
    return path[0] == '/' && strncmp(path, "/dev/", 5) && strncmp(path, "/proc/", 6);
}

/* ---------- memory ---------- */

/* For each present page: the index+1 of the latest map record covering it (0: none). */
static uint16_t *page_maps(struct aoi_proc *p, int64_t *pos, uint64_t nch)
{
    uint16_t *pm = calloc(nch * PER_CHUNK, sizeof *pm);
    int i;
    if (!pm) return NULL;
    for (i = 0; i < p->nmaps && i < 65535; i++) {
        struct aoi_proc_map *m = &p->maps[i];
        uint64_t a;
        if (!file_name(m->path)) continue;
        for (a = m->start & ~(uint64_t)(PAGE - 1); a < m->start + m->len && a < p->vm.size; a += PAGE) {
            uint64_t ci = a >> AOI_VM_CHUNK_SHIFT;
            if (pos[ci] >= 0) pm[(uint64_t)pos[ci] * PER_CHUNK + (a & (AOI_VM_CHUNK - 1)) / PAGE] = (uint16_t)(i + 1);
        }
    }
    return pm;
}

static int all_zero(const uint8_t *b)
{
    const uint64_t *q = (const uint64_t *)b;
    int i;
    for (i = 0; i < (int)(PAGE / 8); i++) if (q[i]) return 0;
    return 1;
}

static const char *save_memory(struct aoi_proc *p, FILE *f)
{
    struct aoi_vm *vm = &p->vm;
    uint64_t nci = vm->size >> AOI_VM_CHUNK_SHIFT, ci, np = vm->size / PAGE, i, nch = 0, k;
    int64_t *pos = malloc(nci * sizeof *pos);
    uint16_t *pm;
    int *mfd = NULL;
    static uint8_t cmp[PAGE];
    static unsigned char incore[AOI_VM_CHUNK / 4096];
    const char *e = NULL;
#ifdef __APPLE__
    int snap_mincore = 1;
#else
    int snap_mincore = getenv("AOI_SNAP_MINCORE") != NULL;       /* host test of the Apple path */
#endif

    if (!pos) return "out of memory";
    w(f, &vm->size, 8); w(f, &vm->hint, 8);
    for (i = 0; i < np; ) {                                 /* protections, run-length coded */
        uint8_t v = vm->prot[i];
        uint32_t run = 1;
        while (i + run < np && vm->prot[i + run] == v && run < 0xffffffffu) run++;
        w(f, &v, 1); w(f, &run, 4);
        i += run;
    }
    for (ci = 0; ci < nci; ci++) pos[ci] = vm->chunk[ci] ? (int64_t)nch++ : -1;
    w(f, &nch, 8);
    for (ci = 0; ci < nci; ci++) if (vm->chunk[ci]) w(f, &ci, 8);
    if (!(pm = page_maps(p, pos, nch)) || !(mfd = malloc(sizeof *mfd * (size_t)(p->nmaps + 1)))) { e = "out of memory"; goto out; }
    for (i = 0; i <= (uint64_t)p->nmaps; i++) mfd[i] = -2;   /* not opened yet */

    for (ci = 0; ci < nci; ci++) {
        long hp = sysconf(_SC_PAGESIZE);
        int resident = 0;
        if (!vm->chunk[ci]) continue;
        /* Which host pages of an anonymous chunk exist: on iOS, reading a page that was
         * never written allocates it (no shared zero page), and ART reserves gigabytes
         * it never touches; reading all of it took the footprint from 0.4 to 2.5 GB.
         * A page that is neither resident nor compressed (MINCORE_PAGED_OUT) was never
         * written: it reads as zero. Only on Apple hosts (Linux reports a swapped-out
         * anonymous page the same as an untouched one) and only in chunks no file was
         * mapped into (a file page not yet read is not resident either). */
        if (snap_mincore && !vm->filemap[ci] && hp > 0 && hp <= (long)AOI_VM_CHUNK && hp % PAGE == 0
            && mincore((void *)vm->chunk[ci], AOI_VM_CHUNK, (void *)incore) == 0) resident = 1;
        for (k = 0; k < PER_CHUNK; k++) {
            uint64_t pg = ci * PER_CHUNK + k, a = pg * PAGE;
            uint8_t *b = vm->chunk[ci] + k * PAGE;
            uint16_t mi = pm[(uint64_t)pos[ci] * PER_CHUNK + k];
            uint8_t kind;
            uint32_t one = 1;
            if (!vm->prot[pg]) continue;
            if (resident && !(incore[k * PAGE / (uint64_t)hp] & AOI_MINCORE_EXISTS)) continue;   /* never written: zero */
            if (mi) {                                        /* the same bytes as its file? */
                struct aoi_proc_map *m = &p->maps[mi - 1];
                uint64_t fo = m->off + (a - m->start);
                ssize_t got;
                if (mfd[mi] == -2) mfd[mi] = aoi_proc_open_host(p, m->path, O_RDONLY);
                if (mfd[mi] >= 0 && (got = pread(mfd[mi], cmp, PAGE, (off_t)fo)) >= 0) {
                    if (got < PAGE) memset(cmp + got, 0, PAGE - (size_t)got);
                    if (!memcmp(cmp, b, PAGE)) {
                        uint32_t idx = mi - 1;
                        kind = REC_FILE;
                        w(f, &kind, 1); w(f, &pg, 8); w(f, &one, 4); w(f, &idx, 4); w(f, &fo, 8);
                        continue;
                    }
                }
            }
            if (all_zero(b)) continue;                       /* a new chunk reads as zero */
            kind = REC_DATA;
            w(f, &kind, 1); w(f, &pg, 8); w(f, &one, 4);
            if (!w(f, b, PAGE)) { e = "write failed"; goto out; }
        }
    }
    { uint8_t end = REC_END; w(f, &end, 1); }
out:
    if (mfd) for (i = 0; i <= (uint64_t)p->nmaps; i++) if (mfd[i] >= 0) close(mfd[i]);
    free(mfd); free(pm); free(pos);
    return e;
}

static const char *load_memory(struct aoi_proc *p, FILE *f)
{
    struct aoi_vm *vm = &p->vm;
    uint64_t size, hint, np, i, nch, ci;
    const char *e;
    if (!r(f, &size, 8) || !r(f, &hint, 8)) return "truncated";
    if ((e = aoi_vm_init(vm, size))) return e;
    vm->hint = hint;
    np = vm->size / PAGE;
    for (i = 0; i < np; ) {
        uint8_t v; uint32_t run;
        if (!r(f, &v, 1) || !r(f, &run, 4) || i + run > np) return "bad page table";
        memset(vm->prot + i, v, run);
        i += run;
    }
    if (!r(f, &nch, 8)) return "truncated";
    for (i = 0; i < nch; i++)
        if (!r(f, &ci, 8) || !aoi_vm_chunk_alloc(vm, ci)) return "no host memory for a chunk";
    for (;;) {
        uint8_t kind; uint64_t pg; uint32_t n;
        if (!r(f, &kind, 1)) return "truncated";
        if (kind == REC_END) break;
        if (!r(f, &pg, 8) || !r(f, &n, 4) || n != 1 || pg >= np || !vm->chunk[(pg * PAGE) >> AOI_VM_CHUNK_SHIFT])
            return "bad page record";
        if (kind == REC_DATA) {
            if (!r(f, vm->chunk[(pg * PAGE) >> AOI_VM_CHUNK_SHIFT] + (pg * PAGE & (AOI_VM_CHUNK - 1)), PAGE)) return "truncated";
        } else if (kind == REC_FILE) {
            /* a run of consecutive file pages of one map record becomes one file mapping */
            uint32_t idx; uint64_t fo, end = pg + 1, fend;
            long back;
            int fd;
            struct stat st;
            if (!r(f, &idx, 4) || !r(f, &fo, 8) || idx >= (uint32_t)p->nmaps) return "bad file record";
            fend = fo + PAGE;
            for (;;) {                                       /* look ahead: more of the same */
                uint8_t k2; uint64_t pg2, fo2; uint32_t n2, idx2;
                back = ftell(f);
                if (!r(f, &k2, 1) || k2 != REC_FILE || !r(f, &pg2, 8) || !r(f, &n2, 4) || !r(f, &idx2, 4) || !r(f, &fo2, 8)
                    || pg2 != end || idx2 != idx || fo2 != fend || ((pg2 * PAGE) >> AOI_VM_CHUNK_SHIFT) != ((pg * PAGE) >> AOI_VM_CHUNK_SHIFT)) {
                    fseek(f, back, SEEK_SET);
                    break;
                }
                end++; fend += PAGE;
            }
            if ((fd = aoi_proc_open_host(p, p->maps[idx].path, O_RDONLY)) < 0) return "a mapped file is gone";
            if (fstat(fd, &st) || aoi_vm_map_file(vm, pg * PAGE, (end - pg) * PAGE, fd, fo, (uint64_t)st.st_size) < 0) {
                close(fd);
                return "cannot map a file again";
            }
            close(fd);
        } else return "bad record";
    }
    return NULL;
}

/* ---------- app data ---------- */

/* The apps' private files (guest /data/data) as they were: saved with the process and
 * put back on load, so what the process holds in memory (SQLite's page cache and WAL
 * index, open file offsets) matches the files. Changes made after the snapshot are
 * dropped with it. */
#define DATA_DIR "/data/data"

static int save_tree(FILE *f, const char *host, const char *rel)
{
    char hp[AOI_PATH * 2], rp[AOI_PATH];
    DIR *d;
    struct dirent *de;
    struct stat st;
    snprintf(hp, sizeof hp, "%s%s", host, rel);
    if (!(d = opendir(hp))) return 0;
    while ((de = readdir(d))) {
        uint8_t kind;
        uint16_t n;
        uint32_t mode;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(rp, sizeof rp, "%s/%s", rel, de->d_name);
        snprintf(hp, sizeof hp, "%s%s", host, rp);
        if (lstat(hp, &st)) continue;
        n = (uint16_t)strlen(rp);
        mode = (uint32_t)st.st_mode & 07777;
        if (S_ISDIR(st.st_mode)) {
            kind = 1;
            w(f, &kind, 1); w(f, &n, 2); w(f, rp, n); w(f, &mode, 4);
            if (save_tree(f, host, rp)) { closedir(d); return -1; }
        } else if (S_ISREG(st.st_mode)) {
            uint64_t size = (uint64_t)st.st_size, done = 0;
            char buf[65536];
            int fd = open(hp, O_RDONLY | O_CLOEXEC);
            ssize_t got;
            if (fd < 0) continue;
            kind = 2;
            w(f, &kind, 1); w(f, &n, 2); w(f, rp, n); w(f, &mode, 4); w(f, &size, 8);
            while (done < size && (got = read(fd, buf, size - done < sizeof buf ? (size_t)(size - done) : sizeof buf)) > 0) {
                w(f, buf, (size_t)got);
                done += (uint64_t)got;
            }
            close(fd);
            if (done < size) { closedir(d); return -1; }   /* it shrank under us: the record is broken */
        }
    }
    closedir(d);
    return 0;
}

static const char *save_data(struct aoi_proc *p, FILE *f)
{
    char host[AOI_PATH];
    uint8_t end = 0;
    aoi_proc_host_path(p, DATA_DIR, host);
    if (host[0] && save_tree(f, host, "")) return "app data changed while saving";
    w(f, &end, 1);
    return NULL;
}

/* Regular files under host+rel that are not in the snapshot are removed. */
static void prune(const char *host, const char *rel, char **keep, int nkeep)
{
    char hp[AOI_PATH * 2], rp[AOI_PATH];
    DIR *d;
    struct dirent *de;
    struct stat st;
    int i;
    snprintf(hp, sizeof hp, "%s%s", host, rel);
    if (!(d = opendir(hp))) return;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(rp, sizeof rp, "%s/%s", rel, de->d_name);
        snprintf(hp, sizeof hp, "%s%s", host, rp);
        if (lstat(hp, &st)) continue;
        if (S_ISDIR(st.st_mode)) { prune(host, rp, keep, nkeep); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        for (i = 0; i < nkeep && strcmp(keep[i], rp); i++) {}
        if (i == nkeep) unlink(hp);
    }
    closedir(d);
}

static const char *load_data(struct aoi_proc *p, FILE *f)
{
    char host[AOI_PATH], hp[AOI_PATH * 2], rp[AOI_PATH], buf[65536];
    char **keep = NULL;
    int nkeep = 0, i;
    const char *e = NULL;
    aoi_proc_host_path(p, DATA_DIR, host);
    for (;;) {
        uint8_t kind; uint16_t n; uint32_t mode; uint64_t size;
        if (!r(f, &kind, 1)) { e = "truncated"; break; }
        if (!kind) break;
        if (!r(f, &n, 2) || n >= sizeof rp || !r(f, rp, n) || !r(f, &mode, 4)) { e = "bad data record"; break; }
        rp[n] = 0;
        snprintf(hp, sizeof hp, "%s%s", host, rp);
        if (kind == 1) { if (host[0]) mkdir(hp, (mode_t)mode); continue; }
        if (kind != 2 || !r(f, &size, 8)) { e = "bad data record"; break; }
        {
            int fd = host[0] ? open(hp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (mode_t)mode) : -1;
            char **k2 = realloc(keep, sizeof *keep * (size_t)(nkeep + 1));
            if (k2) { keep = k2; keep[nkeep] = strdup(rp); if (keep[nkeep]) nkeep++; }
            while (size) {
                size_t c = size < sizeof buf ? (size_t)size : sizeof buf;
                if (!r(f, buf, c)) { e = "truncated"; break; }
                if (fd >= 0 && write(fd, buf, c) != (ssize_t)c) { close(fd); fd = -1; e = "cannot restore app data"; }
                size -= c;
            }
            if (fd >= 0) close(fd);
            if (e) break;
        }
    }
    if (!e && host[0]) prune(host, "", keep, nkeep);
    for (i = 0; i < nkeep; i++) free(keep[i]);
    free(keep);
    return e;
}

/* ---------- fds ---------- */

/* Host fds for the guest fds, from their saved state: files reopened at their offset,
 * pairs rebuilt, the rest (binder, eventfd, epoll, sockets...) on /dev/null. */
static void reopen_fds(struct aoi_proc *p, const int64_t *offset)
{
    int i, j;
    for (i = 0; i < AOI_PROC_FDS; i++) {
        struct aoi_proc_fd *d = &p->fd[i];
        d->dir = NULL;
        if (!d->used) continue;
        d->host = -1;
        if (i <= 2) { d->host = i; continue; }               /* the caller's stdio */
        if (d->kind == AOI_FD_FILE && file_name(d->path)) {
            if ((d->host = aoi_proc_open_host(p, d->path, O_RDWR)) < 0)
                d->host = aoi_proc_open_host(p, d->path, O_RDONLY);
            if (d->host >= 0 && offset[i] > 0) lseek(d->host, (off_t)offset[i], SEEK_SET);
        }
    }
    for (i = 0; i < AOI_PROC_FDS; i++) {                     /* pairs: both ends, then every dup */
        struct aoi_proc_fd *d = &p->fd[i];
        int hv[2], ok;
        if (!d->used || d->kind != AOI_FD_PIPE || !d->pair || d->host >= 0) continue;
        ok = d->ptype == 0 ? !pipe(hv) : d->ptype == 5 ? !aoi_host_msgpair(hv)
           : !socketpair(AF_UNIX, d->ptype == 1 ? SOCK_STREAM : SOCK_DGRAM, 0, hv);
        if (!ok) continue;
        for (j = 0; j < 2; j++) { fcntl(hv[j], F_SETFL, O_NONBLOCK); fcntl(hv[j], F_SETFD, FD_CLOEXEC); }
        for (j = i; j < AOI_PROC_FDS; j++)
            if (p->fd[j].used && p->fd[j].kind == AOI_FD_PIPE && p->fd[j].pair == d->pair)
                p->fd[j].host = dup(hv[p->fd[j].end & 1]);
        if (p->input_pair == d->pair) p->input_w = dup(hv[1]);   /* /dev/aoi_input's host end */
        close(hv[0]); close(hv[1]);
    }
    for (i = 3; i < AOI_PROC_FDS; i++)                       /* anything left: a placeholder */
        if (p->fd[i].used && p->fd[i].host < 0) p->fd[i].host = open("/dev/null", O_RDWR | O_CLOEXEC);
}

static void fix_cpu(struct aoi_proc *p, struct aoi_cpu *c)
{
    c->mem = &p->mem;
    c->syscall = aoi_proc_syscall;
    c->host_ctx = p;
    c->host_call = NULL;
    c->trace = NULL; c->trace_ctx = NULL;
    c->stop_name = NULL;
    aoi_hle_attach(c, c->hle_base);                          /* this build's stand-ins (core/hle.c) */
}

/* ---------- the file ---------- */

const char *aoi_snap_save(struct aoi_proc *p, const char *path)
{
    char tmp[AOI_PATH + 8];
    FILE *f;
    const char *e = NULL;
    struct aoi_epoll *eps[AOI_PROC_FDS];
    int32_t epi[AOI_PROC_FDS];
    int64_t off[AOI_PROC_FDS], mono = aoi_mono_ns();
    uint32_t sz = (uint32_t)sizeof *p, nep = 0;
    int i, j;

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(f = fopen(tmp, "wb"))) return "cannot write the snapshot";
    w(f, SNAP_MAGIC, 8); w(f, AOI_SNAP_BUILD, sizeof AOI_SNAP_BUILD); w(f, &sz, 4);
    w(f, p, sizeof *p);
    w(f, &mono, 8);
    for (i = 0; i < AOI_PROC_FDS; i++) {                     /* epoll lists, shared by dups */
        epi[i] = -1; off[i] = 0;
        if (!p->fd[i].used) continue;
        if (p->fd[i].kind == AOI_FD_EPOLL && p->fd[i].ep) {
            for (j = 0; j < (int)nep && eps[j] != p->fd[i].ep; j++) {}
            if (j == (int)nep) eps[nep++] = p->fd[i].ep;
            epi[i] = j;
        }
        if (p->fd[i].kind == AOI_FD_FILE && i > 2) off[i] = (int64_t)lseek(p->fd[i].host, 0, SEEK_CUR);
    }
    w(f, &nep, 4);
    for (j = 0; j < (int)nep; j++) {
        w(f, &eps[j]->n, sizeof eps[j]->n);
        w(f, eps[j]->e, sizeof *eps[j]->e * (size_t)eps[j]->n);
    }
    w(f, epi, sizeof epi); w(f, off, sizeof off);
    e = save_data(p, f);
    if (!e) e = save_memory(p, f);
    if (!e && (aoi_gralloc_snap(p, f, 1) || aoi_sf_snap(p, f, 1) || aoi_binder_snap(p, f, 1))) e = "service state";
    w(f, SNAP_MAGIC, 8);
    if (fclose(f) && !e) e = "write failed";
    if (e) { unlink(tmp); return e; }
    if (rename(tmp, path)) { unlink(tmp); return "cannot write the snapshot"; }
    return NULL;
}

const char *aoi_snap_load(struct aoi_proc *p, const char *path, const char *root, const char *data)
{
    FILE *f = fopen(path, "rb");
    char magic[8], build[sizeof AOI_SNAP_BUILD];
    uint32_t sz, nep, j;
    int32_t epi[AOI_PROC_FDS];
    int64_t off[AOI_PROC_FDS], mono, now;
    struct aoi_epoll **eps = NULL;
    const char *e = NULL;
    int i, fds_ok = 0;

    memset(p, 0, sizeof *p);
    if (!f) return "no snapshot";
    if (!r(f, magic, 8) || memcmp(magic, SNAP_MAGIC, 8) || !r(f, build, sizeof build) ||
        (memcmp(build, AOI_SNAP_BUILD, sizeof build) && !getenv("AOI_SNAP_ANY_BUILD")) || !r(f, &sz, 4) || sz != sizeof *p) {
        fclose(f);
        return "a snapshot of another build";
    }
    if (!r(f, p, sizeof *p) || !r(f, &mono, 8)) { e = "truncated"; goto fail; }
    /* host things that do not survive: rebuilt below or set again by the caller */
    snprintf(p->root, sizeof p->root, "%s", root);
    snprintf(p->data, sizeof p->data, "%s", data ? data : "");
    p->trace = NULL; p->log = NULL; p->frame = NULL; p->frame_ctx = NULL; p->home = NULL;
    p->gpu = NULL; p->gpu_ctx = NULL; p->gpu_live = 0; p->clip = NULL; p->ime = NULL;
    p->samples = NULL; p->nsamples = p->maxsamples = 0;
    p->stop_request = 0; p->snap_path[0] = 0; p->snap_request = 0;
    p->binder = NULL; p->sf = NULL; p->gralloc = NULL;
    p->input_w = 0; p->nshm = 0;
    memset(&p->vm, 0, sizeof p->vm);
    p->mem.vm = &p->vm;
    fix_cpu(p, &p->cpu);
    for (i = 0; i < AOI_PROC_THREADS; i++) fix_cpu(p, &p->th[i].cpu);
    for (i = 0; i < AOI_PROC_FDS; i++) p->fd[i].ep = NULL;

    if (!r(f, &nep, 4) || nep > AOI_PROC_FDS || !(eps = calloc(nep + 1, sizeof *eps))) { e = "bad epoll lists"; goto fail; }
    for (j = 0; j < nep; j++) {
        if (!(eps[j] = calloc(1, sizeof *eps[j])) || !r(f, &eps[j]->n, sizeof eps[j]->n) || eps[j]->n < 0) { e = "bad epoll list"; goto fail; }
        eps[j]->cap = eps[j]->n ? eps[j]->n : 1;
        if (!(eps[j]->e = calloc((size_t)eps[j]->cap, sizeof *eps[j]->e)) ||
            !r(f, eps[j]->e, sizeof *eps[j]->e * (size_t)eps[j]->n)) { e = "bad epoll list"; goto fail; }
    }
    if (!r(f, epi, sizeof epi) || !r(f, off, sizeof off)) { e = "truncated"; goto fail; }
    for (i = 0; i < AOI_PROC_FDS; i++)
        if (p->fd[i].used && p->fd[i].kind == AOI_FD_EPOLL && epi[i] >= 0 && (uint32_t)epi[i] < nep) {
            p->fd[i].ep = eps[epi[i]];
            eps[epi[i]]->refs++;
        }
    if ((e = load_data(p, f)) || (e = load_memory(p, f))) goto fail;
    reopen_fds(p, off);
    fds_ok = 1;
    if (aoi_gralloc_snap(p, f, 0) || aoi_sf_snap(p, f, 0) || aoi_binder_snap(p, f, 0)) { e = "service state"; goto fail; }
    if (!r(f, magic, 8) || memcmp(magic, SNAP_MAGIC, 8)) { e = "truncated"; goto fail; }
    fclose(f);
    for (j = 0; j < nep; j++) if (!eps[j]->refs) { free(eps[j]->e); free(eps[j]); }
    free(eps);
    aoi_mono_offset = 0;                                     /* guest time never goes back */
    now = aoi_mono_ns();
    if (now < mono) aoi_mono_offset = mono - now;
    return NULL;
fail:
    fclose(f);
    if (!fds_ok) for (i = 0; i < AOI_PROC_FDS; i++) p->fd[i].used = 0;   /* not ours: aoi_proc_free must not close them */
    if (eps) for (j = 0; j < nep; j++) if (eps[j] && !eps[j]->refs) { free(eps[j]->e); free(eps[j]); }
    free(eps);
    return e;
}
