/* Linux/arm64 process: execve-style loader and the syscall layer (see proc.h).
 *
 * Numbers, flags and structure layouts below are Linux arm64's (asm-generic).
 * The host may be Linux or Darwin, so every value crossing the boundary is
 * translated: open flags, errno, struct stat, dirents. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "proc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* 64 GiB of guest addresses (reserved, not committed): scudo alone reserves
 * 8+ GiB up front. Mappings without an address hint go above 4 GiB, as on a
 * real kernel, which leaves the low 4 GiB to requests that ask for it (ART's
 * heap, whose 32-bit references must point below 4 GiB). */
#define GUEST_SPACE (64ULL << 30)
#define HIGH_START  (4ULL << 30)
#define IS_ERR(v)   ((v) >= (uint64_t)-4096)
#define STACK_TOP   (GUEST_SPACE - (1ULL << 30))
#define STACK_SIZE  (8ULL << 20)
#define PAGE        AOI_VM_PAGE
#define GUEST_PID   1000

static uint64_t up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
static uint64_t down(uint64_t v, uint64_t a) { return v & ~(a - 1); }

/* ---------- errno, Linux values ---------- */

enum { L_EPERM = 1, L_ENOENT = 2, L_ESRCH = 3, L_EINTR = 4, L_EIO = 5, L_EBADF = 9, L_ECHILD = 10,
       L_EAGAIN = 11, L_ENOMEM = 12, L_EACCES = 13, L_EFAULT = 14, L_EBUSY = 16, L_EEXIST = 17,
       L_EXDEV = 18, L_ENOTDIR = 20, L_EISDIR = 21, L_EINVAL = 22, L_ENFILE = 23, L_EMFILE = 24,
       L_ENOTTY = 25, L_EFBIG = 27, L_ENOSPC = 28, L_ESPIPE = 29, L_EROFS = 30, L_EPIPE = 32,
       L_ERANGE = 34, L_ENAMETOOLONG = 36, L_ENOSYS = 38, L_ENOTEMPTY = 39, L_ELOOP = 40,
       L_ENODATA = 61, L_EOVERFLOW = 75, L_ENOTSUP = 95, L_ETIMEDOUT = 110 };

static int lx_errno(int e)
{
    switch (e) {
    case EPERM: return L_EPERM;       case ENOENT: return L_ENOENT;   case ESRCH: return L_ESRCH;
    case EINTR: return L_EINTR;       case EIO: return L_EIO;         case EBADF: return L_EBADF;
    case ECHILD: return L_ECHILD;     case EAGAIN: return L_EAGAIN;   case ENOMEM: return L_ENOMEM;
    case EACCES: return L_EACCES;     case EFAULT: return L_EFAULT;   case EBUSY: return L_EBUSY;
    case EEXIST: return L_EEXIST;     case EXDEV: return L_EXDEV;     case ENOTDIR: return L_ENOTDIR;
    case EISDIR: return L_EISDIR;     case EINVAL: return L_EINVAL;   case ENFILE: return L_ENFILE;
    case EMFILE: return L_EMFILE;     case ENOTTY: return L_ENOTTY;   case EFBIG: return L_EFBIG;
    case ENOSPC: return L_ENOSPC;     case ESPIPE: return L_ESPIPE;   case EROFS: return L_EROFS;
    case EPIPE: return L_EPIPE;       case ERANGE: return L_ERANGE;   case ENAMETOOLONG: return L_ENAMETOOLONG;
    case ENOSYS: return L_ENOSYS;     case ENOTEMPTY: return L_ENOTEMPTY; case ELOOP: return L_ELOOP;
    case EOVERFLOW: return L_EOVERFLOW; case ETIMEDOUT: return L_ETIMEDOUT;
    default: return L_EIO;
    }
}

static uint64_t err(int linux_errno) { return (uint64_t)-(int64_t)linux_errno; }
static uint64_t herr(void) { return err(lx_errno(errno)); }

/* ---------- guest memory ---------- */

static uint8_t *gp(struct aoi_proc *p, uint64_t a, uint64_t len, int need)
{
    a &= 0x00ffffffffffffffULL;                                    /* tagged pointers, as the kernel untags */
    return aoi_vm_ptr(&p->vm, a, len ? len : 1, need);
}

/* Copies a NUL-terminated guest string; NULL if unmapped or too long. */
static const char *gstr(struct aoi_proc *p, uint64_t a, char *buf, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t *c = gp(p, a + i, 1, AOI_PROT_R);
        if (!c) return NULL;
        buf[i] = (char)*c;
        if (!*c) return buf;
    }
    return NULL;
}

/* Copies to / from guest memory; the space is sparse, so ranges may span chunks. */
static int put(struct aoi_proc *p, uint64_t a, const void *src, uint64_t n)
{
    return !n || aoi_vm_write(&p->vm, a & 0x00ffffffffffffffULL, src, n, AOI_PROT_W);
}

static int get(struct aoi_proc *p, uint64_t a, void *dst, uint64_t n)
{
    return !n || aoi_vm_read(&p->vm, a & 0x00ffffffffffffffULL, dst, n, AOI_PROT_R);
}

/* Host read()/write() (pread/pwrite when off >= 0) straight into / out of guest
 * memory, one chunk-contiguous span at a time. Bytes moved, or a Linux -errno. */
static int64_t xfer(struct aoi_proc *p, int host, uint64_t a, uint64_t len, int to_guest, int64_t off)
{
    uint64_t done = 0, n;
    a &= 0x00ffffffffffffffULL;
    while (done < len) {
        uint8_t *b = aoi_vm_span(&p->vm, a + done, len - done, to_guest ? AOI_PROT_W : AOI_PROT_R, &n);
        ssize_t k;
        if (!b) return done ? (int64_t)done : -L_EFAULT;
        if (to_guest) k = off < 0 ? read(host, b, (size_t)n) : pread(host, b, (size_t)n, (off_t)(off + (int64_t)done));
        else k = off < 0 ? write(host, b, (size_t)n) : pwrite(host, b, (size_t)n, (off_t)(off + (int64_t)done));
        if (k < 0) return done ? (int64_t)done : -lx_errno(errno);
        done += (uint64_t)k;
        if ((uint64_t)k < n) break;
    }
    return (int64_t)done;
}

/* Every page of the range is mapped (any protection). */
static int mapped(struct aoi_proc *p, uint64_t a, uint64_t len)
{
    uint64_t q;
    if (a + len < a || a + len > p->vm.size) return 0;
    for (q = a & ~(uint64_t)(PAGE - 1); q < a + len; q += PAGE)
        if (!p->vm.prot[q / PAGE]) return 0;
    return 1;
}

/* ---------- paths: guest path -> host path inside root ---------- */

/* dst = a + b, at most n bytes with the NUL; 0 if it does not fit. */
static int join(char *dst, size_t n, const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la + lb + 1 > n) return 0;
    memmove(dst + la, b, lb + 1);
    memmove(dst, a, la);
    return 1;
}

static void host_path(struct aoi_proc *p, const char *guest, char *out)
{
    if (!join(out, AOI_PATH, p->root, guest)) out[0] = 0;
}

/* Host devices passed straight through. */
static int is_passthrough(const char *g)
{
    return !strcmp(g, "/dev/null") || !strcmp(g, "/dev/zero") || !strcmp(g, "/dev/urandom") ||
           !strcmp(g, "/dev/random") || !strcmp(g, "/dev/tty");
}

/* Normalises `path` (relative to the guest directory `base`) into an absolute
 * guest path, following symlinks inside the root; the last component is
 * followed only if `follow`. Returns 0 or a Linux errno. */
static int resolve(struct aoi_proc *p, const char *base, const char *path, int follow, char *out)
{
    char work[AOI_PATH * 2], cur[AOI_PATH], hp[AOI_PATH], link[AOI_PATH];
    int links = 0;
    size_t cl;

    if (!*path) return L_ENOENT;
    if (path[0] == '/') { if (!join(work, sizeof work, "", path)) return L_ENAMETOOLONG; }
    else {
        char b2[AOI_PATH + 1];
        if (!join(b2, sizeof b2, base, "/") || !join(work, sizeof work, b2, path)) return L_ENAMETOOLONG;
    }
    cur[0] = 0; cl = 0;
    for (;;) {
        char *s = work, *comp, *rest;
        while (*s == '/') s++;
        if (!*s) break;
        comp = s;
        while (*s && *s != '/') s++;
        rest = s;
        {
            size_t n = (size_t)(rest - comp);
            int last;
            char *r = rest;
            while (*r == '/') r++;
            last = !*r;
            if (n == 1 && comp[0] == '.') { memmove(work, rest, strlen(rest) + 1); continue; }
            if (n == 2 && comp[0] == '.' && comp[1] == '.') {
                while (cl && cur[cl - 1] != '/') cl--;
                if (cl) cl--;
                cur[cl] = 0;
                memmove(work, rest, strlen(rest) + 1);
                continue;
            }
            if (cl + 1 + n >= AOI_PATH) return L_ENAMETOOLONG;
            cur[cl++] = '/';
            memcpy(cur + cl, comp, n);
            cl += n;
            cur[cl] = 0;
            memmove(work, rest, strlen(rest) + 1);
            if (last && !follow) break;
            if (!strcmp(cur, "/proc/self/exe") || !strcmp(cur, "/proc/self/fd")) continue;
            host_path(p, cur, hp);
            {
                struct stat st;
                ssize_t k;
                if (lstat(hp, &st) != 0 || !S_ISLNK(st.st_mode)) continue;
                if (++links > 40) return L_ELOOP;
                if ((k = readlink(hp, link, sizeof link - 1)) < 0) return lx_errno(errno);
                link[k] = 0;
                {
                    char tmp[AOI_PATH * 2];
                    if (!join(tmp, sizeof tmp, link, "/") || !join(work, sizeof work, tmp, work))
                        return L_ENAMETOOLONG;
                }
                if (link[0] == '/') { cl = 0; cur[0] = 0; }
                else { while (cl && cur[cl - 1] != '/') cl--; if (cl) cl--; cur[cl] = 0; }
            }
        }
    }
    if (!cl) { cur[0] = '/'; cur[1] = 0; }
    memcpy(out, cur, cl + 2 > AOI_PATH ? AOI_PATH : cl + 2);
    return 0;
}

/* dirfd + path -> resolved guest path. */
static int at_path(struct aoi_proc *p, int64_t dirfd, uint64_t upath, int follow, char *out)
{
    char path[AOI_PATH];
    const char *base = p->cwd;
    if (!gstr(p, upath, path, sizeof path)) return L_EFAULT;
    if (path[0] != '/' && dirfd != -100) {                 /* not AT_FDCWD */
        if (dirfd < 0 || dirfd >= AOI_PROC_FDS || !p->fd[dirfd].used) return L_EBADF;
        base = p->fd[dirfd].path;
    }
    return resolve(p, base, path, follow, out);
}

/* The host path for a resolved guest path (devices pass through). */
static void to_host(struct aoi_proc *p, const char *g, char *out)
{
    if (is_passthrough(g)) join(out, AOI_PATH, "", g);
    else host_path(p, g, out);
}

/* ---------- file descriptors ---------- */

static int fd_new(struct aoi_proc *p, int host, const char *path, int min)
{
    int i;
    for (i = min; i < AOI_PROC_FDS; i++)
        if (!p->fd[i].used) {
            p->fd[i].used = 1; p->fd[i].host = host; p->fd[i].dir = NULL;
            if (p->fd[i].path != path) join(p->fd[i].path, AOI_PATH, "", path);
            return i;
        }
    return -1;
}

static struct aoi_proc_fd *fd_get(struct aoi_proc *p, uint64_t fd)
{
    return fd < AOI_PROC_FDS && p->fd[fd].used ? &p->fd[fd] : NULL;
}

static int host_oflags(uint64_t f)
{
    int h = (int)(f & 3);
    if (f & 0100) h |= O_CREAT;
    if (f & 0200) h |= O_EXCL;
    if (f & 0400) h |= O_NOCTTY;
    if (f & 01000) h |= O_TRUNC;
    if (f & 02000) h |= O_APPEND;
    if (f & 04000) h |= O_NONBLOCK;
    if (f & 040000) h |= O_DIRECTORY;
    if (f & 0100000) h |= O_NOFOLLOW;
    if (f & 02000000) h |= O_CLOEXEC;
    return h;
}

/* struct stat, Linux arm64 layout (128 bytes). */
static void lx_stat(const struct stat *st, uint8_t *o)
{
    uint64_t v;
    uint32_t w;
    int64_t sv;
    memset(o, 0, 128);
    v = (uint64_t)st->st_dev; memcpy(o + 0, &v, 8);
    v = (uint64_t)st->st_ino; memcpy(o + 8, &v, 8);
    w = (uint32_t)st->st_mode; memcpy(o + 16, &w, 4);
    w = (uint32_t)st->st_nlink; memcpy(o + 20, &w, 4);
    w = 0; memcpy(o + 24, &w, 4); memcpy(o + 28, &w, 4);       /* uid, gid: root */
    v = (uint64_t)st->st_rdev; memcpy(o + 32, &v, 8);
    sv = (int64_t)st->st_size; memcpy(o + 48, &sv, 8);
    w = (uint32_t)st->st_blksize; memcpy(o + 56, &w, 4);
    sv = (int64_t)st->st_blocks; memcpy(o + 64, &sv, 8);
#ifdef __APPLE__
    sv = st->st_atimespec.tv_sec; memcpy(o + 72, &sv, 8); v = (uint64_t)st->st_atimespec.tv_nsec; memcpy(o + 80, &v, 8);
    sv = st->st_mtimespec.tv_sec; memcpy(o + 88, &sv, 8); v = (uint64_t)st->st_mtimespec.tv_nsec; memcpy(o + 96, &v, 8);
    sv = st->st_ctimespec.tv_sec; memcpy(o + 104, &sv, 8); v = (uint64_t)st->st_ctimespec.tv_nsec; memcpy(o + 112, &v, 8);
#else
    sv = st->st_atim.tv_sec; memcpy(o + 72, &sv, 8); v = (uint64_t)st->st_atim.tv_nsec; memcpy(o + 80, &v, 8);
    sv = st->st_mtim.tv_sec; memcpy(o + 88, &sv, 8); v = (uint64_t)st->st_mtim.tv_nsec; memcpy(o + 96, &v, 8);
    sv = st->st_ctim.tv_sec; memcpy(o + 104, &sv, 8); v = (uint64_t)st->st_ctim.tv_nsec; memcpy(o + 112, &v, 8);
#endif
}

static void note_map(struct aoi_proc *p, uint64_t start, uint64_t len, uint64_t off, const char *path);

/* ---------- ELF loading (execve) ---------- */

struct loaded { uint64_t bias, entry, phdr, phnum, lo, hi; char interp[AOI_PATH]; };

static uint16_t u16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t u32(const uint8_t *b) { return (uint32_t)u16(b) | (uint32_t)u16(b + 2) << 16; }
static uint64_t u64(const uint8_t *b) { return (uint64_t)u32(b) | (uint64_t)u32(b + 4) << 32; }

static void *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    void *buf = NULL;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (buf = malloc((size_t)n)) && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) *size = (size_t)n;
    fclose(f);
    return buf;
}

static const char *load_elf(struct aoi_proc *p, const char *guest, struct loaded *L)
{
    char hp[AOI_PATH], g[AOI_PATH];
    size_t size = 0;
    uint8_t *f;
    uint64_t phoff, i, lo = ~0ULL, hi = 0, base;
    unsigned phnum, phentsize;
    int type, rc;

    memset(L, 0, sizeof *L);
    if ((rc = resolve(p, "/", guest, 1, g))) return "cannot resolve program path";
    host_path(p, g, hp);
    if (!(f = slurp(hp, &size))) return "cannot read program";
    if (size < 64 || memcmp(f, "\177ELF\2\1", 6) || u16(f + 18) != 183) { free(f); return "not an ELF64 AArch64 file"; }
    type = u16(f + 16);
    phoff = u64(f + 32); phentsize = u16(f + 54); phnum = u16(f + 56);
    if (phentsize != 56 || phoff + (uint64_t)phnum * 56 > size) { free(f); return "bad program headers"; }
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = f + phoff + i * 56;
        if (u32(ph) == 1) {                                        /* PT_LOAD */
            uint64_t va = u64(ph + 16), msz = u64(ph + 40);
            if (down(va, PAGE) < lo) lo = down(va, PAGE);
            if (up(va + msz, PAGE) > hi) hi = up(va + msz, PAGE);
        } else if (u32(ph) == 3) {                                 /* PT_INTERP */
            uint64_t off = u64(ph + 8), fsz = u64(ph + 32);
            if (off + fsz > size || fsz >= AOI_PATH) { free(f); return "bad PT_INTERP"; }
            memcpy(L->interp, f + off, fsz); L->interp[fsz] = 0;
        }
    }
    if (hi <= lo) { free(f); return "no PT_LOAD"; }
    if (type == 3) {                                               /* ET_DYN: anywhere free */
        base = aoi_vm_map(&p->vm, 0, hi - lo, 0, 0);
        if (IS_ERR(base)) { free(f); return "no address space for program"; }
        L->bias = base - lo;
    } else {
        L->bias = 0;
        if (IS_ERR(aoi_vm_map(&p->vm, lo, hi - lo, 0, 1))) { free(f); return "ET_EXEC outside the guest space"; }
    }
    /* map the whole span writable, copy every segment, then apply page protections */
    aoi_vm_map(&p->vm, lo + L->bias, hi - lo, AOI_PROT_R | AOI_PROT_W, 1);
    {
        uint8_t *prot = calloc((size_t)((hi - lo) / PAGE), 1);
        if (!prot) { free(f); return "out of memory"; }
        for (i = 0; i < phnum; i++) {
            const uint8_t *ph = f + phoff + i * 56;
            uint64_t off, va, fsz, msz, a;
            uint32_t fl;
            int pr;
            if (u32(ph) != 1) continue;
            fl = u32(ph + 4); off = u64(ph + 8); va = u64(ph + 16); fsz = u64(ph + 32); msz = u64(ph + 40);
            if (off + fsz > size || fsz > msz) { free(prot); free(f); return "bad PT_LOAD"; }
            put(p, va + L->bias, f + off, fsz);
            pr = (fl & 4 ? AOI_PROT_R : 0) | (fl & 2 ? AOI_PROT_W : 0) | (fl & 1 ? AOI_PROT_X : 0);
            for (a = down(va, PAGE); a < up(va + msz, PAGE); a += PAGE) prot[(a - lo) / PAGE] |= (uint8_t)pr;
            if (off == 0 || (phoff >= off && phoff < off + fsz)) L->phdr = va + (phoff - off) + L->bias;
        }
        for (i = 0; i < (hi - lo) / PAGE; i++) aoi_vm_protect(&p->vm, lo + L->bias + i * PAGE, PAGE, prot[i]);
        free(prot);
    }
    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = f + phoff + i * 56;
        if (u32(ph) == 6) L->phdr = u64(ph + 16) + L->bias;       /* PT_PHDR */
    }
    L->entry = u64(f + 24) + L->bias;
    note_map(p, lo + L->bias, hi - lo, lo, g);
    L->phnum = phnum;
    L->lo = lo + L->bias; L->hi = hi + L->bias;
    free(f);
    return NULL;
}

/* Pushes a string onto the guest stack (growing down) and returns its address. */
static uint64_t push_str(struct aoi_proc *p, uint64_t *sp, const char *s)
{
    size_t n = strlen(s) + 1;
    *sp -= n;
    put(p, *sp, s, n);
    return *sp;
}

enum { AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6, AT_BASE = 7, AT_FLAGS = 8,
       AT_ENTRY = 9, AT_UID = 11, AT_EUID = 12, AT_GID = 13, AT_EGID = 14, AT_PLATFORM = 15,
       AT_HWCAP = 16, AT_CLKTCK = 17, AT_SECURE = 23, AT_RANDOM = 25, AT_HWCAP2 = 26, AT_EXECFN = 31 };

/* What the interpreter implements: FP, ASIMD, CRC32, LSE atomics. No crypto, no FP16. */
#define GUEST_HWCAP ((1u << 0) | (1u << 1) | (1u << 7) | (1u << 8))

uint64_t aoi_proc_syscall(struct aoi_cpu *c);

const char *aoi_proc_exec(struct aoi_proc *p, const char *root, const char *path,
                          int argc, const char *const *argv, const char *const *envp)
{
    struct loaded prog, interp;
    const char *e;
    uint64_t sp, argp[256], envp_a[256], execfn, platform, rnd, *v, words[600];
    int envc = 0, i, n = 0;
    uint8_t random16[16];

    memset(p, 0, sizeof *p);
    snprintf(p->root, sizeof p->root, "%s", root);
    snprintf(p->cwd, sizeof p->cwd, "/");
    if ((e = aoi_vm_init(&p->vm, GUEST_SPACE))) return e;
    p->vm.hint = HIGH_START;
    p->mem.vm = &p->vm;
    for (i = 0; i < 3; i++) { p->fd[i].used = 1; p->fd[i].host = i; snprintf(p->fd[i].path, AOI_PATH, "/dev/tty"); }

    if (argc > 255) return "too many arguments";
    while (envp && envp[envc]) envc++;
    if (envc > 255) return "too many environment variables";

    if ((e = load_elf(p, path, &prog))) return e;
    resolve(p, "/", path, 1, p->exe);
    if (prog.interp[0]) { if ((e = load_elf(p, prog.interp, &interp))) return e; }
    else interp = prog;

    if (IS_ERR(aoi_vm_map(&p->vm, STACK_TOP - STACK_SIZE, STACK_SIZE, AOI_PROT_R | AOI_PROT_W, 1)))
        return "cannot map the stack";
    sp = STACK_TOP;
    execfn = push_str(p, &sp, path);
    platform = push_str(p, &sp, "aarch64");
    {
        FILE *u = fopen("/dev/urandom", "rb");
        if (!u || fread(random16, 1, 16, u) != 16) memset(random16, 0x5a, 16);
        if (u) fclose(u);
    }
    sp -= 16; rnd = sp; put(p, rnd, random16, 16);
    for (i = argc - 1; i >= 0; i--) argp[i] = push_str(p, &sp, argv[i]);
    for (i = envc - 1; i >= 0; i--) envp_a[i] = push_str(p, &sp, envp[i]);

    v = words;
    v[n++] = (uint64_t)argc;
    for (i = 0; i < argc; i++) v[n++] = argp[i];
    v[n++] = 0;
    for (i = 0; i < envc; i++) v[n++] = envp_a[i];
    v[n++] = 0;
#define AUX(k, val) do { v[n++] = (k); v[n++] = (val); } while (0)
    AUX(AT_PHDR, prog.phdr); AUX(AT_PHENT, 56); AUX(AT_PHNUM, prog.phnum); AUX(AT_PAGESZ, PAGE);
    AUX(AT_BASE, prog.interp[0] ? interp.bias : 0); AUX(AT_FLAGS, 0); AUX(AT_ENTRY, prog.entry);
    AUX(AT_UID, 0); AUX(AT_EUID, 0); AUX(AT_GID, 0); AUX(AT_EGID, 0); AUX(AT_PLATFORM, platform);
    AUX(AT_HWCAP, GUEST_HWCAP); AUX(AT_CLKTCK, 100); AUX(AT_SECURE, 0); AUX(AT_RANDOM, rnd);
    AUX(AT_HWCAP2, 0); AUX(AT_EXECFN, execfn); AUX(AT_NULL, 0);
#undef AUX
    sp = down(sp - (uint64_t)n * 8, 16);
    put(p, sp, words, (uint64_t)n * 8);

    p->brk = up(prog.hi, PAGE);
    p->cpu.mem = &p->mem;
    p->cpu.pc = interp.entry;
    p->cpu.sp = sp;
    p->cpu.syscall = aoi_proc_syscall;
    p->cpu.host_ctx = p;
    return NULL;
}

void aoi_proc_free(struct aoi_proc *p)
{
    int i;
    for (i = 3; i < AOI_PROC_FDS; i++)
        if (p->fd[i].used) {
            if (p->fd[i].dir) closedir(p->fd[i].dir); else close(p->fd[i].host);
        }
    aoi_vm_free(&p->vm);
}

enum aoi_stop aoi_proc_run(struct aoi_proc *p, uint64_t max_steps)
{
    return aoi_cpu_run(&p->cpu, max_steps);
}

/* ---------- syscalls ---------- */

enum {
    NR_getcwd = 17, NR_symlinkat = 36, NR_linkat = 37, NR_renameat = 38, NR_ftruncate = 46, NR_fchmod = 52, NR_fchmodat = 53, NR_fchownat = 54, NR_fchown = 55, NR_fsync = 82, NR_fdatasync = 83, NR_utimensat = 88, NR_renameat2 = 276, NR_dup = 23, NR_dup3 = 24, NR_setpgid = 154, NR_getpgid = 155, NR_getsid = 156, NR_statfs = 43, NR_fstatfs = 44, NR_fcntl = 25, NR_ioctl = 29, NR_mkdirat = 34, NR_unlinkat = 35, NR_faccessat = 48,
    NR_chdir = 49, NR_openat = 56, NR_close = 57, NR_getdents64 = 61, NR_lseek = 62, NR_read = 63,
    NR_write = 64, NR_readv = 65, NR_writev = 66, NR_pread64 = 67, NR_pwrite64 = 68,
    NR_readlinkat = 78, NR_newfstatat = 79, NR_fstat = 80, NR_exit = 93, NR_exit_group = 94,
    NR_set_tid_address = 96, NR_futex = 98, NR_set_robust_list = 99, NR_nanosleep = 101,
    NR_clock_gettime = 113, NR_clock_getres = 114, NR_clock_nanosleep = 115,
    NR_sched_getparam = 121, NR_sched_getscheduler = 120, NR_sched_getaffinity = 123, NR_sched_yield = 124, NR_kill = 129, NR_tkill = 130, NR_tgkill = 131,
    NR_sigaltstack = 132, NR_rt_sigaction = 134, NR_rt_sigprocmask = 135, NR_uname = 160,
    NR_getrlimit = 163, NR_umask = 166, NR_prctl = 167, NR_gettimeofday = 169, NR_getpid = 172,
    NR_getppid = 173, NR_getuid = 174, NR_geteuid = 175, NR_getgid = 176, NR_getegid = 177,
    NR_gettid = 178, NR_sysinfo = 179, NR_brk = 214, NR_munmap = 215, NR_mremap = 216, NR_mmap = 222,
    NR_mprotect = 226, NR_madvise = 233, NR_prlimit64 = 261, NR_getrandom = 278, NR_faccessat2 = 439
};

static int64_t sx32(uint64_t v) { return (int64_t)(int32_t)v; }

static void trace_sys(struct aoi_proc *p, uint64_t nr, uint64_t r)
{
    if (!p->trace) return;
    fprintf(p->trace, "[sys] %3llu(%#llx, %#llx, %#llx, %#llx) = %lld\n", (unsigned long long)nr,
            (unsigned long long)p->cpu.x[0], (unsigned long long)p->cpu.x[1],
            (unsigned long long)p->cpu.x[2], (unsigned long long)p->cpu.x[3], (long long)r);
}

static void note_map(struct aoi_proc *p, uint64_t start, uint64_t len, uint64_t off, const char *path)
{
    struct aoi_proc_map *m;
    if (p->nmaps == AOI_PROC_MAPS) return;
    m = &p->maps[p->nmaps++];
    m->start = start; m->len = len; m->off = off;
    {
        size_t n = strlen(path);
        if (n >= sizeof m->path) { path += n - (sizeof m->path - 1); n = sizeof m->path - 1; }  /* keep the tail */
        memcpy(m->path, path, n);
        m->path[n] = 0;
    }
}

const char *aoi_proc_where(struct aoi_proc *p, uint64_t addr, char *buf, size_t n)
{
    int i;
    for (i = p->nmaps - 1; i >= 0; i--) {
        struct aoi_proc_map *m = &p->maps[i];
        if (addr >= m->start && addr < m->start + m->len) {
            snprintf(buf, n, "%s+%#llx", m->path, (unsigned long long)(addr - m->start + m->off));
            return buf;
        }
    }
    snprintf(buf, n, "?");
    return buf;
}

static uint64_t sys_mmap(struct aoi_proc *p, uint64_t addr, uint64_t len, int prot, int flags,
                         int64_t fd, uint64_t off)
{
    int fixed = (flags & 0x10) != 0, noreplace = (flags & 0x100000) != 0;
    uint64_t a, i;
    struct aoi_proc_fd *f = NULL;
    if (!len || off % PAGE) return err(L_EINVAL);
    if (!(flags & 0x20)) {                                         /* file mapping */
        if (!(f = fd_get(p, (uint64_t)fd))) return err(L_EBADF);
    }
    if (noreplace) {
        for (i = 0; i < up(len, PAGE); i += PAGE)
            if (addr + i >= p->vm.size || p->vm.prot[(addr + i) / PAGE]) return err(L_EEXIST);
        fixed = 1;
    }
    /* anonymous memory gets its final protection at once (a PROT_NONE reservation
     * must not back chunks); a file mapping is written first, then protected */
    a = aoi_vm_map(&p->vm, fixed ? addr : (addr ? down(addr, PAGE) : 0), len,
                   f ? AOI_PROT_R | AOI_PROT_W : prot & 7, fixed);
    if (IS_ERR(a)) return a;
    if (!f) return a;
    if (f) {                                                       /* private copy of the file range */
        int64_t got = xfer(p, f->host, a, len, 1, (int64_t)off);
        if (got < 0) { aoi_vm_unmap(&p->vm, a, len); return (uint64_t)got; }
        note_map(p, a, len, off, f->path);
    }
    aoi_vm_protect(&p->vm, a, len, prot & 7);
    return a;
}

static uint64_t sys_getdents64(struct aoi_proc *p, struct aoi_proc_fd *f, uint64_t buf, uint64_t n)
{
    uint64_t used = 0;
    struct dirent *de;
    if (!f->dir) {
        int d = dup(f->host);
        if (d < 0 || !(f->dir = fdopendir(d))) return herr();
    }
    for (;;) {
        long pos = telldir(f->dir);
        size_t nl, rec;
        uint8_t ent[AOI_PATH + 32];
        uint64_t ino, offv;
        uint16_t reclen;
        if (!(de = readdir(f->dir))) break;
        nl = strlen(de->d_name);
        rec = up(19 + nl + 1, 8);
        if (used + rec > n) { seekdir(f->dir, pos); if (!used) return err(L_EINVAL); break; }
        memset(ent, 0, rec);
        ino = (uint64_t)de->d_ino; offv = (uint64_t)telldir(f->dir); reclen = (uint16_t)rec;
        memcpy(ent, &ino, 8); memcpy(ent + 8, &offv, 8); memcpy(ent + 16, &reclen, 2);
        ent[18] = de->d_type;                                    /* DT_* values match Linux */
        memcpy(ent + 19, de->d_name, nl);
        if (!put(p, buf + used, ent, rec)) return err(L_EFAULT);
        used += rec;
    }
    return used;
}

uint64_t aoi_proc_syscall(struct aoi_cpu *c)
{
    struct aoi_proc *p = c->host_ctx;
    uint64_t nr = c->x[8], a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3], a4 = c->x[4],
             a5 = c->x[5], r;
    char g[AOI_PATH], h[AOI_PATH];
    struct aoi_proc_fd *f;
    struct stat st;
    uint8_t sbuf[128];
    int rc;

    switch (nr) {
    case NR_read: case NR_pread64: {
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        r = a2 ? (uint64_t)xfer(p, f->host, a1, a2, 1, nr == NR_read ? -1 : (int64_t)a3) : 0;
        break;
    }
    case NR_write: case NR_pwrite64: {
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        if (f->host <= 2) fflush(stdout);
        r = a2 ? (uint64_t)xfer(p, f->host, a1, a2, 0, nr == NR_write ? -1 : (int64_t)a3) : 0;
        break;
    }
    case NR_readv: case NR_writev: {
        uint64_t i, total = 0;
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        r = 0;
        for (i = 0; i < a2; i++) {
            uint8_t e[16];
            uint64_t base, len;
            int64_t k;
            if (!get(p, a1 + i * 16, e, 16)) { r = err(L_EFAULT); break; }
            base = u64(e); len = u64(e + 8);
            if (!len) continue;
            k = xfer(p, f->host, base, len, nr == NR_readv, -1);
            if (k < 0) { r = total ? total : (uint64_t)k; break; }
            total += (uint64_t)k;
            r = total;
            if ((uint64_t)k < len) break;
        }
        break;
    }
    case NR_openat: {
        int hfd, fdn;
        if ((rc = at_path(p, sx32(a0), a1, !(a2 & 0100000), g))) { r = err(rc); break; }
        to_host(p, g, h);
        hfd = open(h, host_oflags(a2) | O_CLOEXEC, (mode_t)a3);
        if (hfd < 0) { r = herr(); break; }
        if ((fdn = fd_new(p, hfd, g, 0)) < 0) { close(hfd); r = err(L_EMFILE); break; }
        r = (uint64_t)fdn;
        if (p->trace) fprintf(p->trace, "[sys] open %s -> %d\n", g, fdn);
        break;
    }
    case NR_close:
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        if (a0 > 2) { if (f->dir) closedir(f->dir); else close(f->host); }
        f->used = 0; f->dir = NULL;
        r = 0;
        break;
    case NR_lseek: {
        off_t o;
        int wh = (int)a2;
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        if (wh > 2) { r = err(L_EINVAL); break; }
        o = lseek(f->host, (off_t)a1, wh == 0 ? SEEK_SET : wh == 1 ? SEEK_CUR : SEEK_END);
        r = o < 0 ? herr() : (uint64_t)o;
        break;
    }
    case NR_fstat:
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        if (fstat(f->host, &st)) { r = herr(); break; }
        lx_stat(&st, sbuf);
        r = put(p, a1, sbuf, 128) ? 0 : err(L_EFAULT);
        break;
    case NR_newfstatat: {
        int empty = (a3 & 0x1000) != 0;                           /* AT_EMPTY_PATH */
        char path0[2];
        if (empty && gstr(p, a1, path0, 2) && !path0[0]) {
            if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
            if (fstat(f->host, &st)) { r = herr(); break; }
        } else {
            if ((rc = at_path(p, sx32(a0), a1, !(a3 & 0x100), g))) { r = err(rc); break; }
            to_host(p, g, h);
            if ((a3 & 0x100) ? lstat(h, &st) : stat(h, &st)) { r = herr(); break; }
        }
        lx_stat(&st, sbuf);
        r = put(p, a2, sbuf, 128) ? 0 : err(L_EFAULT);
        break;
    }
    case NR_faccessat: case NR_faccessat2:
        if ((rc = at_path(p, sx32(a0), a1, 1, g))) { r = err(rc); break; }
        to_host(p, g, h);
        r = access(h, (int)a2 & 7) ? herr() : 0;
        break;
    case NR_readlinkat: {
        char path[AOI_PATH], target[AOI_PATH];
        ssize_t k;
        if (!gstr(p, a1, path, sizeof path)) { r = err(L_EFAULT); break; }
        if (!strcmp(path, "/proc/self/exe")) {
            k = (ssize_t)strlen(p->exe);
            memcpy(target, p->exe, (size_t)k);
        } else if (!strncmp(path, "/proc/self/fd/", 14)) {
            uint64_t n = strtoull(path + 14, NULL, 10);
            if (!(f = fd_get(p, n))) { r = err(L_ENOENT); break; }
            k = (ssize_t)strlen(f->path);
            memcpy(target, f->path, (size_t)k);
        } else {
            if ((rc = at_path(p, sx32(a0), a1, 0, g))) { r = err(rc); break; }
            to_host(p, g, h);
            if ((k = readlink(h, target, sizeof target)) < 0) { r = herr(); break; }
        }
        if ((uint64_t)k > a3) k = (ssize_t)a3;
        r = put(p, a2, target, (uint64_t)k) ? (uint64_t)k : err(L_EFAULT);
        break;
    }
    case NR_getdents64:
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        r = sys_getdents64(p, f, a1, a2);
        break;
    case NR_getcwd: {
        size_t n = strlen(p->cwd) + 1;
        if (n > a1) { r = err(L_ERANGE); break; }
        r = put(p, a0, p->cwd, n) ? (uint64_t)n : err(L_EFAULT);
        break;
    }
    case NR_chdir:
        if ((rc = at_path(p, -100, a0, 1, g))) { r = err(rc); break; }
        to_host(p, g, h);
        if (stat(h, &st)) { r = herr(); break; }
        if (!S_ISDIR(st.st_mode)) { r = err(L_ENOTDIR); break; }
        snprintf(p->cwd, sizeof p->cwd, "%s", g);
        r = 0;
        break;
    case NR_fcntl:
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        switch (a1) {
        case 0: case 1030: {                                       /* F_DUPFD(_CLOEXEC) */
            int d = dup(f->host), n;
            if (d < 0) { r = herr(); break; }
            if ((n = fd_new(p, d, f->path, (int)a2)) < 0) { close(d); r = err(L_EMFILE); break; }
            r = (uint64_t)n;
            break;
        }
        case 1: case 2: r = 0; break;                              /* F_GETFD / F_SETFD */
        case 3: r = 2; break;                                      /* F_GETFL: O_RDWR */
        case 4: r = 0; break;                                      /* F_SETFL */
        default: r = err(L_EINVAL); break;
        }
        break;
    case NR_dup: case NR_dup3: {
        int d, n;
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        if (nr == NR_dup3) {
            if (a1 >= AOI_PROC_FDS) { r = err(L_EBADF); break; }
            if (a1 == a0) { r = err(L_EINVAL); break; }
        }
        if ((d = dup(f->host)) < 0) { r = herr(); break; }
        if (nr == NR_dup3) {
            struct aoi_proc_fd *t = &p->fd[a1];
            if (t->used && a1 > 2) { if (t->dir) closedir(t->dir); else close(t->host); }
            if (a1 <= 2) { dup2(d, (int)a1); close(d); d = (int)a1; }
            t->used = 1; t->host = d; t->dir = NULL;
            join(t->path, AOI_PATH, "", f->path);
            r = a1;
        } else {
            if ((n = fd_new(p, d, f->path, 0)) < 0) { close(d); r = err(L_EMFILE); break; }
            r = (uint64_t)n;
        }
        break;
    }
    case NR_getpgid: case NR_getsid: r = GUEST_PID; break;
    case NR_setpgid: r = 0; break;
    case NR_fchmod:
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        r = fchmod(f->host, (mode_t)a1 & 07777) ? herr() : 0;
        break;
    case NR_fchmodat:
        if ((rc = at_path(p, sx32(a0), a1, 1, g))) { r = err(rc); break; }
        to_host(p, g, h);
        r = chmod(h, (mode_t)a2 & 07777) ? herr() : 0;
        break;
    case NR_fchown: case NR_fchownat:                              /* one user: accepted, not applied */
        r = nr == NR_fchown && !fd_get(p, a0) ? err(L_EBADF) : 0;
        break;
    case NR_mkdirat:
        if ((rc = at_path(p, sx32(a0), a1, 0, g))) { r = err(rc); break; }
        to_host(p, g, h);
        r = mkdir(h, (mode_t)a2 & 07777) ? herr() : 0;
        break;
    case NR_unlinkat:
        if ((rc = at_path(p, sx32(a0), a1, 0, g))) { r = err(rc); break; }
        to_host(p, g, h);
        r = ((a2 & 0x200) ? rmdir(h) : unlink(h)) ? herr() : 0;   /* AT_REMOVEDIR */
        break;
    case NR_renameat: case NR_renameat2: {
        char g2[AOI_PATH], h2[AOI_PATH];
        if (nr == NR_renameat2 && a4) { r = err(L_EINVAL); break; }
        if ((rc = at_path(p, sx32(a0), a1, 0, g)) || (rc = at_path(p, sx32(a2), a3, 0, g2))) { r = err(rc); break; }
        to_host(p, g, h); to_host(p, g2, h2);
        r = rename(h, h2) ? herr() : 0;
        break;
    }
    case NR_symlinkat: {
        char target[AOI_PATH];
        if (!gstr(p, a0, target, sizeof target)) { r = err(L_EFAULT); break; }
        if ((rc = at_path(p, sx32(a1), a2, 0, g))) { r = err(rc); break; }
        to_host(p, g, h);
        r = symlink(target, h) ? herr() : 0;                       /* target stays a guest path */
        break;
    }
    case NR_linkat: {
        char g2[AOI_PATH], h2[AOI_PATH];
        if ((rc = at_path(p, sx32(a0), a1, 0, g)) || (rc = at_path(p, sx32(a2), a3, 0, g2))) { r = err(rc); break; }
        to_host(p, g, h); to_host(p, g2, h2);
        r = link(h, h2) ? herr() : 0;
        break;
    }
    case NR_ftruncate:
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        r = ftruncate(f->host, (off_t)a1) ? herr() : 0;
        break;
    case NR_fsync: case NR_fdatasync:
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        r = fsync(f->host) ? herr() : 0;
        break;
    case NR_utimensat:                                             /* timestamps: accepted, not applied */
        r = 0;
        break;
    case NR_ioctl:
        r = err(L_ENOTTY);
        break;
    case NR_mmap:
        r = sys_mmap(p, a0, a1, (int)a2, (int)a3, sx32(a4), a5);
        break;
    case NR_munmap:
        r = (uint64_t)(int64_t)aoi_vm_unmap(&p->vm, a0, a1);
        break;
    case NR_mprotect:
        r = (uint64_t)(int64_t)aoi_vm_protect(&p->vm, a0, a1, (int)a2 & 7);
        break;
    case NR_mremap: {                                              /* move-and-copy */
        uint64_t na, keep, prot0;
        int fixed = (a3 & 2) != 0;                                 /* MREMAP_FIXED: new address in x4 */
        if (a0 % PAGE || (fixed && (a4 % PAGE || !(a3 & 1)))) { r = err(L_EINVAL); break; }
        if (!mapped(p, a0, a1 ? a1 : 1)) { r = err(L_EFAULT); break; }
        if (!fixed && a2 <= a1) {                                  /* shrink in place */
            if (up(a2, PAGE) < up(a1, PAGE)) aoi_vm_unmap(&p->vm, a0 + up(a2, PAGE), up(a1, PAGE) - up(a2, PAGE));
            r = a0;
            break;
        }
        if (!(a3 & 1)) { r = err(L_ENOMEM); break; }               /* growing needs MREMAP_MAYMOVE */
        if (fixed && a4 < a0 + up(a1, PAGE) && a0 < a4 + up(a2, PAGE)) { r = err(L_EINVAL); break; }
        prot0 = p->vm.prot[a0 / PAGE] & 7;
        na = aoi_vm_map(&p->vm, fixed ? a4 : 0, a2, AOI_PROT_R | AOI_PROT_W, fixed);
        if (IS_ERR(na)) { r = na; break; }
        keep = a1 < a2 ? a1 : a2;
        {                                                          /* page by page: the source may be sparse */
            uint64_t o;
            for (o = 0; o < keep; o += PAGE) {
                uint64_t n = keep - o < PAGE ? keep - o : PAGE;
                uint8_t *src = aoi_vm_ptr(&p->vm, a0 + o, n, 0), *dst = aoi_vm_ptr(&p->vm, na + o, n, 0);
                if (src && dst) memcpy(dst, src, (size_t)n);
            }
        }
        aoi_vm_protect(&p->vm, na, a2, (int)prot0);
        aoi_vm_unmap(&p->vm, a0, a1);
        r = na;
        break;
    }
    case NR_madvise:
        if (a2 == 4) {                                             /* MADV_DONTNEED: reads back as zero */
            uint64_t i;
            if (mapped(p, a0, a1))
                for (i = 0; i < up(a1, PAGE); i += PAGE)
                    if (p->vm.prot[(a0 + i) / PAGE] & AOI_PROT_W) aoi_vm_zero(&p->vm, a0 + i, PAGE);
        }
        r = 0;
        break;
    case NR_brk:
        r = p->brk;                                                /* fixed: bionic does not grow it */
        break;
    case NR_exit: case NR_exit_group:
        c->exit_code = (int)a0;
        c->stop = AOI_STOP_EXIT;
        r = 0;
        break;
    case NR_set_tid_address: case NR_gettid: case NR_getpid:
        r = GUEST_PID;
        break;
    case NR_getppid: r = 1; break;
    case NR_getuid: case NR_geteuid: case NR_getgid: case NR_getegid: r = 0; break;
    case NR_umask: r = 022; break;
    case NR_set_robust_list: r = 0; break;
    case NR_futex: {                                               /* one thread: never blocks */
        int op = (int)a1 & 127;
        if (op == 0 || op == 9) {                                  /* WAIT / WAIT_BITSET */
            uint8_t w[4];
            if (!get(p, a0, w, 4)) { r = err(L_EFAULT); break; }
            r = u32(w) != (uint32_t)a2 ? err(L_EAGAIN) : err(L_ETIMEDOUT);
        } else r = 0;                                              /* WAKE: nobody waits */
        break;
    }
    case NR_rt_sigaction:
        if (a0 < 1 || a0 > 64) { r = err(L_EINVAL); break; }
        if (a2 && !put(p, a2, p->sigact[a0], 32)) { r = err(L_EFAULT); break; }
        if (a1 && !get(p, a1, p->sigact[a0], 32)) { r = err(L_EFAULT); break; }
        r = 0;
        break;
    case NR_rt_sigprocmask: {
        uint64_t old = p->sigmask, set;
        if (a1) {
            uint8_t s8[8];
            if (!get(p, a1, s8, 8)) { r = err(L_EFAULT); break; }
            set = u64(s8);
            if (a0 == 0) p->sigmask |= set; else if (a0 == 1) p->sigmask &= ~set; else if (a0 == 2) p->sigmask = set;
            else { r = err(L_EINVAL); break; }
        }
        r = a2 && !put(p, a2, &old, 8) ? err(L_EFAULT) : 0;
        break;
    }
    case NR_sigaltstack:
        if (a1 && !put(p, a1, p->altstack, 24)) { r = err(L_EFAULT); break; }
        if (a0 && !get(p, a0, p->altstack, 24)) { r = err(L_EFAULT); break; }
        r = 0;
        break;
    case NR_kill: case NR_tkill: case NR_tgkill: {
        uint64_t sig = nr == NR_tgkill ? a2 : a1;
        if (!sig) { r = 0; break; }
        fprintf(stderr, "[aoiproc] guest raised signal %llu\n", (unsigned long long)sig);
        c->exit_code = 128 + (int)sig;
        c->stop = AOI_STOP_EXIT;
        r = 0;
        break;
    }
    case NR_prctl:
        r = (a0 == 0x53564d41 || a0 == 15 || a0 == 38) ? 0 : a0 == 3 ? 1 : err(L_EINVAL); /* SET_VMA, SET_NAME, NO_NEW_PRIVS */
        break;
    case NR_uname: {
        uint8_t u[390];
        memset(u, 0, sizeof u);
        strcpy((char *)u, "Linux"); strcpy((char *)u + 65, "localhost"); strcpy((char *)u + 130, "6.1.0-aoi");
        strcpy((char *)u + 195, "#1 SMP"); strcpy((char *)u + 260, "aarch64");
        r = put(p, a0, u, sizeof u) ? 0 : err(L_EFAULT);
        break;
    }
    case NR_clock_gettime: case NR_clock_getres: {
        struct timespec ts;
        int64_t t[2];
        clockid_t id = (a0 == 0 || a0 == 5 || a0 == 8) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
        if (nr == NR_clock_getres) { ts.tv_sec = 0; ts.tv_nsec = 1; }
        else clock_gettime(id, &ts);
        t[0] = ts.tv_sec; t[1] = ts.tv_nsec;
        r = !a1 || put(p, a1, t, 16) ? 0 : err(L_EFAULT);
        break;
    }
    case NR_gettimeofday: {
        struct timeval tv;
        int64_t t[2];
        gettimeofday(&tv, NULL);
        t[0] = tv.tv_sec; t[1] = tv.tv_usec;
        r = !a0 || put(p, a0, t, 16) ? 0 : err(L_EFAULT);
        break;
    }
    case NR_nanosleep: case NR_clock_nanosleep: {
        uint8_t s[16];
        struct timespec ts;
        if (!get(p, nr == NR_nanosleep ? a0 : a2, s, 16)) { r = err(L_EFAULT); break; }
        ts.tv_sec = (time_t)u64(s); ts.tv_nsec = (long)u64(s + 8);
        if (nr == NR_clock_nanosleep && (a1 & 1)) { r = 0; break; } /* TIMER_ABSTIME: not slept */
        nanosleep(&ts, NULL);
        r = 0;
        break;
    }
    case NR_sched_getaffinity: {
        uint64_t one = 1;
        if (a1 < 8) { r = err(L_EINVAL); break; }
        r = put(p, a2, &one, 8) ? 8 : err(L_EFAULT);
        break;
    }
    case NR_statfs: case NR_fstatfs: {                              /* every guest file system looks like ext4 */
        uint64_t sf[15];
        if (nr == NR_fstatfs && !fd_get(p, a0)) { r = err(L_EBADF); break; }
        if (nr == NR_statfs) {
            if ((rc = at_path(p, -100, a0, 1, g))) { r = err(rc); break; }
            to_host(p, g, h);
            if (stat(h, &st)) { r = herr(); break; }
        }
        memset(sf, 0, sizeof sf);
        sf[0] = 0xef53; sf[1] = PAGE; sf[2] = sf[3] = sf[4] = 1 << 20; sf[5] = sf[6] = 1 << 20;
        sf[8] = 255; sf[9] = PAGE;                                 /* f_namelen, f_frsize */
        r = put(p, a1, sf, 120) ? 0 : err(L_EFAULT);
        break;
    }
    case NR_sched_yield: r = 0; break;
    case NR_sched_getscheduler: r = 0; break;                       /* SCHED_OTHER */
    case NR_sched_getparam: { uint32_t prio = 0; r = put(p, a1, &prio, 4) ? 0 : err(L_EFAULT); break; }
    case NR_getrlimit: case NR_prlimit64: {
        uint64_t res = nr == NR_getrlimit ? a0 : a1, out = nr == NR_getrlimit ? a1 : a3, lim[2];
        lim[0] = lim[1] = ~0ULL;
        if (res == 3) lim[0] = STACK_SIZE;                         /* RLIMIT_STACK */
        if (res == 7) lim[0] = lim[1] = AOI_PROC_FDS;              /* RLIMIT_NOFILE */
        r = !out || put(p, out, lim, 16) ? 0 : err(L_EFAULT);
        break;
    }
    case NR_sysinfo: {
        uint8_t si[112];
        uint64_t total = 8ULL << 30;                             /* report an 8 GB phone */
        memset(si, 0, sizeof si);
        memcpy(si + 32, &total, 8); memcpy(si + 40, &total, 8);
        si[104] = 1;                                               /* mem_unit */
        r = put(p, a0, si, sizeof si) ? 0 : err(L_EFAULT);
        break;
    }
    case NR_getrandom: {
        int u = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        int64_t k = u < 0 ? -L_EIO : xfer(p, u, a0, a1, 1, -1);
        if (u >= 0) close(u);
        r = (uint64_t)k;
        break;
    }
    default:
        if (nr < sizeof p->unknown * 8 && !(p->unknown[nr / 8] & (1 << (nr % 8)))) {
            p->unknown[nr / 8] |= (unsigned char)(1 << (nr % 8));
            fprintf(stderr, "[aoiproc] syscall %llu not implemented (ENOSYS), pc=%#llx\n",
                    (unsigned long long)nr, (unsigned long long)c->pc);
        }
        r = err(L_ENOSYS);
        break;
    }
    trace_sys(p, nr, r);
    return r;
}
