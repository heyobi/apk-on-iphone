/* Linux/arm64 process: execve-style loader and the syscall layer (see proc.h).
 *
 * Numbers, flags and structure layouts below are Linux arm64's (asm-generic).
 * The host may be Linux or Darwin, so every value crossing the boundary is
 * translated: open flags, errno, struct stat, dirents. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "proc.h"
#include "binder.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
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
#define SIGTRAMP    STACK_TOP                   /* one page: rt_sigreturn, the vDSO's role */
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
       L_ENODATA = 61, L_EOVERFLOW = 75, L_ENOTSOCK = 88, L_ENOTSUP = 95, L_EAFNOSUPPORT = 97,
       L_ENOTCONN = 107, L_ETIMEDOUT = 110, L_ECONNREFUSED = 111 };

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
/* ---------- userfaultfd (SIGBUS mode) ----------
 * A page becomes missing when MREMAP_DONTUNMAP moves its bytes away or madvise
 * zaps it inside a registered range. Touching a missing page in a registered
 * range is the guest's SIGBUS (ART's CMC GC then compacts into it with
 * UFFDIO_COPY); anywhere else it is simply zero-filled, as the kernel would. */
static int uffd_watched(struct aoi_proc *p, uint64_t a)
{
    int i;
    for (i = 0; i < p->nuffd_reg; i++)
        if (a - p->uffd_reg[i].start < p->uffd_reg[i].len) return 1;
    return 0;
}

/* Zero-fills the unwatched missing pages of a range. 1 if there were any. */
static int uffd_fill(struct aoi_proc *p, uint64_t a, uint64_t n)
{
    uint64_t q;
    int any = 0;
    if (a + n < a || a + n > p->vm.size) return 0;
    for (q = a & ~(uint64_t)(AOI_VM_PAGE - 1); q < a + n; q += AOI_VM_PAGE)
        if ((p->vm.prot[q / AOI_VM_PAGE] & AOI_PROT_MISSING) && !uffd_watched(p, q)) {
            aoi_vm_set_missing(&p->vm, q, AOI_VM_PAGE, 0);
            any = 1;
        }
    return any;
}

static int put(struct aoi_proc *p, uint64_t a, const void *src, uint64_t n)
{
    a &= 0x00ffffffffffffffULL;
    return !n || aoi_vm_write(&p->vm, a, src, n, AOI_PROT_W)
        || (uffd_fill(p, a, n) && aoi_vm_write(&p->vm, a, src, n, AOI_PROT_W));
}

static int get(struct aoi_proc *p, uint64_t a, void *dst, uint64_t n)
{
    a &= 0x00ffffffffffffffULL;
    return !n || aoi_vm_read(&p->vm, a, dst, n, AOI_PROT_R)
        || (uffd_fill(p, a, n) && aoi_vm_read(&p->vm, a, dst, n, AOI_PROT_R));
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
            p->fd[i].used = 1; p->fd[i].host = host; p->fd[i].dir = NULL; p->fd[i].kind = AOI_FD_FILE;
            p->fd[i].nonblock = 0; p->fd[i].count = 0; p->fd[i].sem = 0; p->fd[i].ep = NULL;
            if (p->fd[i].path != path) join(p->fd[i].path, AOI_PATH, "", path);
            return i;
        }
    return -1;
}

int aoi_proc_fd_install(struct aoi_proc *p, int host, const char *path, int kind)
{
    int n = fd_new(p, host, path, 0);
    if (n >= 0) p->fd[n].kind = kind;
    return n;
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
    note_map(p, STACK_TOP - STACK_SIZE, STACK_SIZE, 0, "[stack]");
    {   /* The kernel returns from a handler without SA_RESTORER (bionic never sets it on
         * arm64) through the vDSO's __kernel_rt_sigreturn; this page stands in for it. */
        static const uint8_t tramp[8] = { 0x68, 0x11, 0x80, 0xd2, 0x01, 0x00, 0x00, 0xd4 };   /* mov x8, #139; svc #0 */
        if (IS_ERR(aoi_vm_map(&p->vm, SIGTRAMP, PAGE, AOI_PROT_R | AOI_PROT_X, 1)) ||
            !aoi_vm_write(&p->vm, SIGTRAMP, tramp, sizeof tramp, 0))
            return "cannot map the signal trampoline";
        note_map(p, SIGTRAMP, PAGE, 0, "[vdso]");
    }
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
    for (i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]) + 1;
        if (p->cmdline_len + l > sizeof p->cmdline) break;
        memcpy(p->cmdline + p->cmdline_len, argv[i], l);
        p->cmdline_len += l;
    }
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
    p->stack_start = sp;
    p->th[0].state = AOI_T_RUN;
    p->th[0].tid = GUEST_PID;
    p->next_tid = GUEST_PID + 1;
    p->cpu.syscall = aoi_proc_syscall;
    p->cpu.host_ctx = p;
    return NULL;
}

void aoi_proc_free(struct aoi_proc *p)
{
    int i;
    aoi_binder_free(p);
    for (i = 3; i < AOI_PROC_FDS; i++)
        if (p->fd[i].used) {
            if (p->fd[i].dir) closedir(p->fd[i].dir); else close(p->fd[i].host);
        }
    aoi_vm_free(&p->vm);
}

/* ---------- signals ---------- */

/* Linux arm64 rt_sigframe: siginfo (128) then ucontext { flags, link, stack_t,
 * sigmask, padding, mcontext @176 { fault_address, x0-x30, sp, pc, pstate,
 * __reserved @288 [4096]: fpsimd_context record, then a null record } }, and a
 * frame record {x29, x30} after it so unwinders can walk through the handler. */
#define SF_UC        128
#define SF_MC        (SF_UC + 176)
#define SF_FP        (SF_MC + 288)
#define SF_RECORD    (SF_MC + 288 + 4096)
#define SF_SIZE      (SF_RECORD + 16)
#define FPSIMD_MAGIC 0x46508001u
#define SA_RESTORER_ 0x04000000u
#define SA_ONSTACK_  0x08000000u
#define SA_NODEFER_  0x40000000u
#define SA_RESETHAND_ 0x80000000u
#define SIGBIT(s)    ((uint64_t)1 << ((s) - 1))
#define UNBLOCKABLE  (SIGBIT(9) | SIGBIT(19))       /* SIGKILL, SIGSTOP */

static void put64(uint8_t *b, size_t off, uint64_t v) { memcpy(b + off, &v, 8); }
static void put32(uint8_t *b, size_t off, uint32_t v) { memcpy(b + off, &v, 4); }

/* Default action of a signal nobody handles: 1 = ignore, 0 = terminate. */
static int default_ignored(int sig) { return sig == 17 || sig == 18 || sig == 23 || sig == 28; }

/* Builds a signal frame on the running thread's stack (or its altstack) and
 * points it at the handler. info: 128 bytes of siginfo prepared by the caller. */
static int sig_deliver(struct aoi_proc *p, int sig, const uint8_t *info, uint64_t fault_addr)
{
    struct aoi_thread *t = &p->th[p->cur];
    struct aoi_cpu *c = &p->cpu;
    uint64_t *act = p->sigact[sig], sp = c->sp, frame;
    static uint8_t f[SF_SIZE];
    int on_alt = t->altstack[2] && sp >= t->altstack[0] && sp < t->altstack[0] + t->altstack[2], i;

    if ((act[1] & SA_ONSTACK_) && !(t->altstack[1] & 2) && t->altstack[2] && !on_alt) {
        sp = t->altstack[0] + t->altstack[2];
        on_alt = 1;
    }
    frame = (sp - SF_SIZE) & ~(uint64_t)15;
    memset(f, 0, sizeof f);
    memcpy(f, info, 128);
    put64(f, SF_UC + 16, t->altstack[0]);
    put32(f, SF_UC + 24, (uint32_t)(on_alt ? 1 : (t->altstack[1] & 2 ? 2 : 0)));
    put64(f, SF_UC + 32, t->altstack[2]);
    put64(f, SF_UC + 40, t->sigmask);
    put64(f, SF_MC, fault_addr);
    for (i = 0; i < 31; i++) put64(f, SF_MC + 8 + 8 * (size_t)i, c->x[i]);
    put64(f, SF_MC + 256, c->sp);
    put64(f, SF_MC + 264, c->pc);
    put64(f, SF_MC + 272, (uint64_t)c->n << 31 | (uint64_t)c->z << 30 | (uint64_t)c->c << 29 | (uint64_t)c->v << 28);
    put32(f, SF_FP, FPSIMD_MAGIC);
    put32(f, SF_FP + 4, 528);
    put32(f, SF_FP + 8, c->fpsr);
    put32(f, SF_FP + 12, c->fpcr);
    memcpy(f + SF_FP + 16, c->vreg, 512);
    put64(f, SF_RECORD, c->x[29]);
    put64(f, SF_RECORD + 8, c->x[30]);
    if (!put(p, frame, f, SF_SIZE)) return 0;                      /* no stack: the thread is lost */

    c->x[0] = (uint64_t)sig;
    c->x[1] = frame;
    c->x[2] = frame + SF_UC;
    c->x[29] = frame + SF_RECORD;
    c->x[30] = act[1] & SA_RESTORER_ ? act[2] : SIGTRAMP;
    c->sp = frame;
    c->pc = act[0];
    c->excl_valid = 0;
    t->sigmask |= act[3] & ~UNBLOCKABLE;
    if (!(act[1] & SA_NODEFER_)) t->sigmask |= SIGBIT(sig);
    if (act[1] & SA_RESETHAND_) act[0] = 0;
    if (p->trace) fprintf(p->trace, "[sig] %d delivered to %d, handler %#llx\n", sig, t->tid, (unsigned long long)act[0]);
    return 1;
}

/* rt_sigreturn: restore the context saved by sig_deliver (as possibly edited by the handler). */
static int sig_return(struct aoi_proc *p)
{
    struct aoi_cpu *c = &p->cpu;
    static uint8_t f[SF_SIZE];
    uint64_t pstate, mask;
    uint32_t magic, size;
    int i;
    if (!get(p, c->sp, f, SF_RECORD)) return 0;
    for (i = 0; i < 31; i++) c->x[i] = u64(f + SF_MC + 8 + 8 * (size_t)i);
    c->sp = u64(f + SF_MC + 256);
    c->pc = u64(f + SF_MC + 264);
    pstate = u64(f + SF_MC + 272);
    c->n = (int)(pstate >> 31 & 1); c->z = (int)(pstate >> 30 & 1); c->c = (int)(pstate >> 29 & 1); c->v = (int)(pstate >> 28 & 1);
    magic = u32(f + SF_FP); size = u32(f + SF_FP + 4);
    if (magic == FPSIMD_MAGIC && size >= 528) {
        c->fpsr = u32(f + SF_FP + 8); c->fpcr = u32(f + SF_FP + 12);
        memcpy(c->vreg, f + SF_FP + 16, 512);
    }
    mask = u64(f + SF_UC + 40);
    p->th[p->cur].sigmask = mask & ~UNBLOCKABLE;
    c->excl_valid = 0;
    return 1;
}

/* Delivers the first pending, unblocked signal of the running thread, if any.
 * Returns 0 if a signal's default action ends the process (cpu.stop set). */
static int sig_pending(struct aoi_proc *p)
{
    struct aoi_thread *t = &p->th[p->cur];
    uint64_t ready = t->pending & ~t->sigmask;
    int sig;
    uint8_t info[128];
    if (!ready) return 1;
    sig = __builtin_ctzll(ready) + 1;
    t->pending &= ~SIGBIT(sig);
    if (p->sigact[sig][0] == 1 || (p->sigact[sig][0] == 0 && default_ignored(sig))) return 1;
    if (p->sigact[sig][0] == 0) {
        fprintf(stderr, "[aoiproc] signal %d (default action: terminate)\n", sig);
        p->cpu.exit_code = 128 + sig;
        p->cpu.stop = AOI_STOP_EXIT;
        return 0;
    }
    memset(info, 0, sizeof info);
    put32(info, 0, (uint32_t)sig);
    put32(info, 8, (uint32_t)-6);                                  /* SI_TKILL */
    put32(info, 16, GUEST_PID);
    if (!sig_deliver(p, sig, info, 0)) { p->cpu.stop = AOI_STOP_FAULT; return 0; }
    return 1;
}

/* Sends sig to thread index i: wakes it from a futex wait or sleep (-EINTR) if
 * it will run a handler. Returns 0 if the default action ends the process. */
static int sig_send(struct aoi_proc *p, int i, int sig)
{
    struct aoi_thread *t = &p->th[i];
    uint64_t h = p->sigact[sig][0];
    if (h == 1 || (h == 0 && default_ignored(sig))) return 1;
    if (h == 0) {
        fprintf(stderr, "[aoiproc] signal %d to thread %d (default action: terminate)\n", sig, t->tid);
        p->cpu.exit_code = 128 + sig;
        p->cpu.stop = AOI_STOP_EXIT;
        p->thread_exit = 0;
        return 0;
    }
    t->pending |= SIGBIT(sig);
    if ((t->state == AOI_T_FUTEX || t->state == AOI_T_SLEEP) && !(t->sigmask & SIGBIT(sig))) {
        uint64_t eintr = (uint64_t)-(int64_t)L_EINTR;
        t->state = AOI_T_RUN;
        if (t->restart) t->restart = 0;                        /* the syscall reruns after the handler */
        else if (i == p->cur) p->cpu.x[0] = eintr; else t->cpu.x[0] = eintr;
    }
    return 1;
}

/* A CPU fault becomes SIGSEGV (or, on a userfaultfd-watched missing page, SIGBUS)
 * if the guest handles it; an unwatched missing page is zero-filled and the
 * instruction retried. Returns 1 if the guest continues. */
static int sig_fault(struct aoi_proc *p)
{
    uint8_t info[128];
    uint64_t a = p->cpu.fault_addr & 0x00ffffffffffffffULL;
    int mapped_page = a < p->vm.size && p->vm.prot[a / AOI_VM_PAGE];
    uint64_t h = p->sigact[11][0];
    if (a < p->vm.size) {
        uint64_t pg = a & ~(uint64_t)(AOI_VM_PAGE - 1);
        if (!(p->vm.prot[pg / AOI_VM_PAGE] & AOI_PROT_MISSING) && pg + AOI_VM_PAGE < p->vm.size
            && (p->vm.prot[pg / AOI_VM_PAGE + 1] & AOI_PROT_MISSING) && a + 64 > pg + AOI_VM_PAGE)
            pg += AOI_VM_PAGE;                                     /* an access straddling into it */
        if (p->vm.prot[pg / AOI_VM_PAGE] & AOI_PROT_MISSING) {
            uint64_t sa = a > pg ? a : pg;
            if (!uffd_watched(p, pg)) { uffd_fill(p, pg, AOI_VM_PAGE); p->cpu.stop = AOI_RUN; return 1; }
            h = p->sigact[7][0];
            if (h == 0 || h == 1) return 0;
            memset(info, 0, sizeof info);
            put32(info, 0, 7);
            put32(info, 8, 2);                                     /* BUS_ADRERR, as userfaultfd's SIGBUS mode */
            put64(info, 16, sa);
            p->th[p->cur].sigmask &= ~SIGBIT(7);
            if (p->trace) fprintf(p->trace, "[uffd] SIGBUS at %#llx\n", (unsigned long long)sa);
            if (!sig_deliver(p, 7, info, sa)) return 0;
            p->cpu.stop = AOI_RUN;
            return 1;
        }
    }
    if (h == 0 || h == 1) return 0;                                /* no handler: stop and report */
    memset(info, 0, sizeof info);
    put32(info, 0, 11);
    put32(info, 8, mapped_page ? 2 : 1);                           /* SEGV_ACCERR / SEGV_MAPERR */
    put64(info, 16, p->cpu.fault_addr);
    p->th[p->cur].sigmask &= ~SIGBIT(11);                          /* a synchronous fault is never blocked */
    if (p->trace) {
        char w[256], w2[256];
        int k;
        uint8_t obj[64];
        fprintf(p->trace, "[sig] SIGSEGV at %#llx: pc %s, lr %s\n", (unsigned long long)p->cpu.fault_addr,
                aoi_proc_where(p, p->cpu.pc, w, sizeof w), aoi_proc_where(p, p->cpu.x[30], w2, sizeof w2));
        for (k = 0; k < 31; k++)
            fprintf(p->trace, "[sig]   x%-2d %#018llx%s", k, (unsigned long long)p->cpu.x[k], k % 4 == 3 ? "\n" : "");
        fprintf(p->trace, "\n[sig]   sp  %#llx\n", (unsigned long long)p->cpu.sp);
        if (aoi_vm_read(&p->vm, p->cpu.x[0] & 0x00ffffffffffffffULL, obj, 64, 0)) {
            fprintf(p->trace, "[sig]   [x0]:");
            for (k = 0; k < 64; k += 4) fprintf(p->trace, " %08x", (unsigned)u32(obj + k));
            fprintf(p->trace, "\n");
        }
#ifdef AOI_DEBUG
        {
            extern uint64_t *aoi_pcring;
            uint64_t q;
            if (aoi_pcring)                                        /* the last 40 pcs */
                for (q = p->cpu.steps > 40 ? p->cpu.steps - 40 : 0; q < p->cpu.steps; q++)
                    fprintf(p->trace, "[pc] %s\n", aoi_proc_where(p, aoi_pcring[q & 1023], w, sizeof w));
        }
#endif
        {                                                          /* frame-pointer backtrace */
            uint64_t fp = p->cpu.x[29], fr[2];
            for (k = 0; k < 24 && fp && aoi_vm_read(&p->vm, fp, fr, 16, AOI_PROT_R); k++, fp = fr[0])
                fprintf(p->trace, "[sig]   #%-2d %s\n", k, aoi_proc_where(p, fr[1], w, sizeof w));
        }
    }
    if (!sig_deliver(p, 11, info, p->cpu.fault_addr)) return 0;
    p->cpu.stop = AOI_RUN;
    return 1;
}

/* ---------- green threads ---------- */

static int64_t now_ns(void);

/* A blocking call that would wait (an empty pipe, a binder looper with no work):
 * the thread sleeps `ns` and the syscall runs again, so the other guest threads
 * (the writer) get to run. */
static uint64_t block_and_retry(struct aoi_proc *p, int64_t ns)
{
    p->th[p->cur].state = AOI_T_SLEEP;
    p->th[p->cur].deadline = now_ns() + ns;
    p->th[p->cur].restart = 1;
    p->cpu.stop = AOI_STOP_RESTART;
    return 0;
}

#define SLICE 100000                /* guest instructions per turn */

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Wakes up to n threads waiting on the futex word at addr (bitset-filtered);
 * returns how many. Their futex call returns 0. */
static int futex_wake(struct aoi_proc *p, uint64_t addr, int n, uint32_t bitset)
{
    int i, woken = 0;
    for (i = 0; i < AOI_PROC_THREADS && woken < n; i++) {
        struct aoi_thread *t = &p->th[i];
        if (t->state == AOI_T_FUTEX && t->futex_addr == addr && (t->futex_bitset & bitset)) {
            t->state = AOI_T_RUN;
            t->cpu.x[0] = 0;
            woken++;
        }
    }
    return woken;
}

/* Saves the running thread and loads the next runnable one (round robin).
 * Expired waits are woken first; if nobody can run but someone has a deadline,
 * the host sleeps until it. Returns 0, or -1 if every thread waits forever. */
static int schedule(struct aoi_proc *p)
{
    uint64_t steps = p->cpu.steps;
    int i, k, next = -1;
    for (;;) {
        int64_t now = now_ns(), soonest = 0;
        for (i = 0; i < AOI_PROC_THREADS; i++) {
            struct aoi_thread *t = &p->th[i];
            if ((t->state == AOI_T_FUTEX || t->state == AOI_T_SLEEP) && t->deadline && t->deadline <= now) {
                if (t->restart) t->restart = 0;
                else if (i == p->cur) p->cpu.x[0] = t->state == AOI_T_FUTEX ? err(L_ETIMEDOUT) : 0;
                else t->cpu.x[0] = t->state == AOI_T_FUTEX ? err(L_ETIMEDOUT) : 0;
                t->state = AOI_T_RUN;
            }
            if ((t->state == AOI_T_FUTEX || t->state == AOI_T_SLEEP) && t->deadline &&
                (!soonest || t->deadline < soonest)) soonest = t->deadline;
        }
        for (k = 1; k <= AOI_PROC_THREADS; k++) {
            i = (p->cur + k) % AOI_PROC_THREADS;
            if (p->th[i].state == AOI_T_RUN) { next = i; break; }
        }
        if (next >= 0) break;
        if (!soonest) return -1;
        {
            int64_t d = soonest - now;
            struct timespec ts;
            if (p->trace) {
                fprintf(p->trace, "[sched] idle %.3f ms:", (double)d / 1e6);
                for (i = 0; i < AOI_PROC_THREADS; i++)
                    if (p->th[i].state != AOI_T_FREE)
                        fprintf(p->trace, " %d:%d%s", p->th[i].tid, p->th[i].state,
                                p->th[i].deadline ? (p->th[i].deadline == soonest ? "*" : "t") : "");
                fprintf(p->trace, "\n");
            }
            ts.tv_sec = d / 1000000000; ts.tv_nsec = d % 1000000000;
            nanosleep(&ts, NULL);
        }
    }
    if (next != p->cur) {
        p->th[p->cur].cpu = p->cpu;
        p->cpu = p->th[next].cpu;
        p->cur = next;
    }
    p->cpu.steps = steps;
    p->cpu.excl_valid = 0;          /* another thread may have written: stxr must fail */
    return 0;
}

static int live_threads(struct aoi_proc *p)
{
    int i, n = 0;
    for (i = 0; i < AOI_PROC_THREADS; i++) n += p->th[i].state != AOI_T_FREE;
    return n;
}

/* The running thread ends (exit, not exit_group): CLONE_CHILD_CLEARTID is honoured. */
static void thread_end(struct aoi_proc *p)
{
    struct aoi_thread *t = &p->th[p->cur];
    if (t->clear_tid) {
        uint32_t zero = 0;
        put(p, t->clear_tid, &zero, 4);
        futex_wake(p, t->clear_tid & 0x00ffffffffffffffULL, 1 << 30, ~0u);
    }
    t->state = AOI_T_FREE;
}

enum aoi_stop aoi_proc_run(struct aoi_proc *p, uint64_t max_steps)
{
    for (;;) {
        uint64_t end = p->cpu.steps + SLICE;
        enum aoi_stop st;
        if (max_steps && end > max_steps) end = max_steps;
        if (!sig_pending(p)) return p->cpu.stop;
        if (p->sf) aoi_sf_tick(p);                                 /* vsync events that are due */
        st = aoi_cpu_run(&p->cpu, end);
        if (st == AOI_STOP_FAULT && sig_fault(p)) continue;
        if (st == AOI_RUN && p->samples && p->nsamples < p->maxsamples)   /* time slices, not waits */
            p->samples[p->nsamples++] = p->cpu.pc;
        if (st == AOI_STOP_EXIT && p->thread_exit) {
            p->thread_exit = 0;
            thread_end(p);
            if (!live_threads(p)) return AOI_STOP_EXIT;
        } else if (st == AOI_RUN) {
            if (max_steps && p->cpu.steps >= max_steps) return AOI_RUN;
        } else if (st != AOI_STOP_YIELD) {
            return st;                                             /* exit_group, fault, undef */
        }
        p->cpu.stop = AOI_RUN;
        if (schedule(p)) {
            fprintf(stderr, "[aoiproc] deadlock: every thread waits on a futex with no timeout\n");
            p->cpu.stop = AOI_STOP_SYSCALL;
            return AOI_STOP_SYSCALL;
        }
    }
}

/* ---------- syscalls ---------- */

enum {
    NR_getcwd = 17, NR_pipe2 = 59, NR_eventfd2 = 19, NR_epoll_create1 = 20, NR_epoll_ctl = 21,
    NR_epoll_pwait = 22, NR_ppoll = 73, NR_mincore = 232, NR_flock = 32, NR_userfaultfd = 282, NR_rt_sigreturn = 139, NR_rt_sigtimedwait = 137, NR_setpriority = 140, NR_getpriority = 141, NR_clone = 220, NR_membarrier = 283, NR_socket = 198, NR_connect = 203, NR_symlinkat = 36, NR_linkat = 37, NR_renameat = 38, NR_ftruncate = 46, NR_fchmod = 52, NR_fchmodat = 53, NR_fchownat = 54, NR_fchown = 55, NR_fsync = 82, NR_fdatasync = 83, NR_utimensat = 88, NR_renameat2 = 276, NR_dup = 23, NR_dup3 = 24, NR_setpgid = 154, NR_getpgid = 155, NR_getsid = 156, NR_statfs = 43, NR_fstatfs = 44, NR_fcntl = 25, NR_ioctl = 29, NR_mkdirat = 34, NR_unlinkat = 35, NR_faccessat = 48,
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
    if (p->nmaps == AOI_PROC_MAPS) {                                /* full: forget what is unmapped now */
        int i, j;
        for (i = j = 0; i < p->nmaps; i++) {
            uint64_t s0 = p->maps[i].start;
            if (s0 < p->vm.size && p->vm.prot[s0 / AOI_VM_PAGE]) p->maps[j++] = p->maps[i];
        }
        p->nmaps = j;
        if (p->nmaps == AOI_PROC_MAPS) return;
    }
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

/* ---------- /proc: generated when opened ---------- */

/* Writes the content of a synthetic /proc file into a fresh host temp file and
 * returns its descriptor (positioned at 0), or -1 if `g` is not one we make. */
static int proc_file(struct aoi_proc *p, const char *g)
{
    const char *f;
    FILE *t;
    int fd;
    if (!strncmp(g, "/proc/self/", 11)) f = g + 11;
    else if (!strncmp(g, "/proc/thread-self/", 18)) f = g + 18;
    else if (!strncmp(g, "/proc/1000/", 11)) f = g + 11;
    else if (!strncmp(g, "/proc/1000/task/1000/", 21)) f = g + 21;
    else f = g;                                                    /* /proc/meminfo etc. */
    if (!(t = tmpfile())) return -1;
    if (!strcmp(f, "cmdline")) {
        fwrite(p->cmdline, 1, p->cmdline_len, t);
    } else if (!strcmp(f, "comm")) {
        const char *b = strrchr(p->exe, '/');
        fprintf(t, "%.15s\n", b ? b + 1 : p->exe);
    } else if (!strcmp(f, "stat")) {
        const char *b = strrchr(p->exe, '/');
        fprintf(t, "%d (%.15s) R 1 %d %d 0 -1 4194304 0 0 0 0 %llu 0 0 0 20 0 1 0 1 %llu %llu "
                   "18446744073709551615 0 0 %llu 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                1000, b ? b + 1 : p->exe, 1000, 1000, (unsigned long long)(p->cpu.steps / 1000000),
                (unsigned long long)(p->vm.nchunks * AOI_VM_CHUNK),
                (unsigned long long)(p->vm.nchunks * AOI_VM_CHUNK / AOI_VM_PAGE),
                (unsigned long long)p->stack_start);
    } else if (!strcmp(f, "status")) {
        fprintf(t, "Name:\tapp\nState:\tR (running)\nTgid:\t1000\nPid:\t1000\nPPid:\t1\nTracerPid:\t0\n"
                   "Uid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nThreads:\t1\nVmSize:\t%llu kB\nVmRSS:\t%llu kB\n"
                   "SigQ:\t0/0\nSigPnd:\t0000000000000000\nSigBlk:\t%016llx\n",
                (unsigned long long)(p->vm.nchunks * AOI_VM_CHUNK >> 10),
                (unsigned long long)(p->vm.nchunks * AOI_VM_CHUNK >> 10), (unsigned long long)p->th[p->cur].sigmask);
    } else if (!strcmp(f, "maps")) {                               /* runs of equal protection */
        uint64_t a = 0, n = p->vm.size / AOI_VM_PAGE;
        while (a < n) {
            uint8_t pr = p->vm.prot[a];
            uint64_t b = a + 1, start = a * AOI_VM_PAGE;
            char w[256];
            if (!pr) { a++; continue; }
            while (b < n && p->vm.prot[b] == pr) b++;
            aoi_proc_where(p, start, w, sizeof w);
            if (!strcmp(w, "?")) w[0] = 0; else *strrchr(w, '+') = 0;
            fprintf(t, "%llx-%llx %c%c%cp 00000000 00:00 0 %s\n", (unsigned long long)start,
                    (unsigned long long)(b * AOI_VM_PAGE), pr & 1 ? 'r' : '-', pr & 2 ? 'w' : '-', pr & 4 ? 'x' : '-', w);
            a = b;
        }
    } else if (!strcmp(g, "/proc/meminfo")) {
        fprintf(t, "MemTotal:        8000000 kB\nMemFree:         4000000 kB\nMemAvailable:    4000000 kB\n"
                   "Buffers:               0 kB\nCached:                0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n");
    } else if (!strcmp(g, "/proc/cpuinfo")) {
        fprintf(t, "processor\t: 0\nBogoMIPS\t: 48.00\nFeatures\t: fp asimd crc32 atomics\nCPU implementer\t: 0x61\n"
                   "CPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0x000\nCPU revision\t: 0\n\n");
    } else if (!strcmp(g, "/proc/sys/kernel/randomize_va_space")) {
        fprintf(t, "2\n");
    } else {
        fclose(t);
        return -1;
    }
    fflush(t);
    fd = dup(fileno(t));
    fclose(t);
    if (fd >= 0) lseek(fd, 0, SEEK_SET);
    return fd;
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
        if (f->kind == AOI_FD_BINDER) {                            /* the binder receive buffer */
            a = aoi_vm_map(&p->vm, fixed ? addr : (addr ? down(addr, PAGE) : 0), len, prot & 7, fixed);
            if (!IS_ERR(a)) { aoi_binder_mapped(p, a, up(len, PAGE)); note_map(p, a, len, 0, f->path); }
            return a;
        }
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
    if (f) {                                                       /* private, copy-on-write view of the file */
        struct stat st;
        int e = fstat(f->host, &st) ? -errno : aoi_vm_map_file(&p->vm, a, len, f->host, off, (uint64_t)st.st_size);
        if (e < 0) { aoi_vm_unmap(&p->vm, a, len); errno = -e; return herr(); }
        note_map(p, a, len, off, f->path);
    }
    aoi_vm_protect(&p->vm, a, len, prot & 7);
    return a;
}

/* ---------- eventfd, epoll, ppoll ----------
 * Emulated, not the host's (Darwin has neither eventfd nor epoll). Readiness is
 * level-triggered: eventfd from its counter, host-backed fds from poll(2) with no
 * wait. A wait with nothing ready sleeps a little and runs again until its own
 * deadline (t->poll_deadline), so a write by another green thread wakes it. */
#define EP_IN 0x001u
#define EP_OUT 0x004u
#define EP_ERR 0x008u
#define EP_HUP 0x010u

static uint32_t fd_ready(struct aoi_proc *p, int fd, uint32_t want)
{
    struct aoi_proc_fd *f = fd_get(p, (uint64_t)fd);
    uint32_t r = 0;
    if (!f) return 0;
    switch (f->kind) {
    case AOI_FD_EVENTFD:
        if (f->count) r |= EP_IN;
        if (f->count < 0xfffffffffffffffeULL) r |= EP_OUT;
        break;
    case AOI_FD_FILE: case AOI_FD_PIPE: {
        struct pollfd pf;
        pf.fd = f->host; pf.events = (short)((want & EP_IN ? POLLIN : 0) | (want & EP_OUT ? POLLOUT : 0)); pf.revents = 0;
        if (poll(&pf, 1, 0) > 0) {
            if (pf.revents & POLLIN) r |= EP_IN;
            if (pf.revents & POLLOUT) r |= EP_OUT;
            if (pf.revents & POLLERR) r |= EP_ERR;
            if (pf.revents & POLLHUP) r |= EP_HUP;
        }
        break;
    }
    case AOI_FD_LOGD: r |= EP_OUT; break;
    default: break;                                                /* sockets, binder, epoll, uffd: never */
    }
    return r & (want | EP_ERR | EP_HUP);
}

/* Start or continue a wait; 1 = timed out now, 0 = the thread was put to sleep. */
static int poll_wait(struct aoi_proc *p, int64_t timeout_ns)
{
    struct aoi_thread *t = &p->th[p->cur];
    int64_t now = now_ns(), left;
    if (!t->poll_deadline) t->poll_deadline = timeout_ns < 0 ? INT64_MAX : now + timeout_ns;
    left = t->poll_deadline - now;
    if (left <= 0) { t->poll_deadline = 0; return 1; }
    block_and_retry(p, left < 2000000 ? left : 2000000);
    return 0;
}

static void epoll_unref(struct aoi_epoll *ep)
{
    if (ep && --ep->refs <= 0) { free(ep->e); free(ep); }
}

/* A closed fd leaves every epoll set (its last reference in the kernel's terms). */
static void epoll_forget(struct aoi_proc *p, int fd)
{
    int i, k;
    for (i = 0; i < AOI_PROC_FDS; i++)
        if (p->fd[i].used && p->fd[i].kind == AOI_FD_EPOLL && p->fd[i].ep) {
            struct aoi_epoll *ep = p->fd[i].ep;
            for (k = 0; k < ep->n; k++)
                if (ep->e[k].fd == fd) { ep->e[k] = ep->e[--ep->n]; k--; }
        }
}

static uint64_t sys_epoll_ctl(struct aoi_proc *p, struct aoi_proc_fd *f, int op, int fd, uint64_t evp)
{
    struct aoi_epoll *ep = f->ep;
    uint8_t ev[16];
    int k;
    if (!fd_get(p, (uint64_t)fd)) return err(L_EBADF);
    for (k = 0; k < ep->n && ep->e[k].fd != fd; k++) {}
    if (op != 2 && !get(p, evp, ev, 16)) return err(L_EFAULT);   /* {u32 events; u64 data}, not packed on arm64 */
    switch (op) {
    case 1:                                                        /* EPOLL_CTL_ADD */
        if (k < ep->n) return err(L_EEXIST);
        if (ep->n == ep->cap) {
            int nc = ep->cap ? 2 * ep->cap : 16;
            void *ne = realloc(ep->e, (size_t)nc * sizeof *ep->e);
            if (!ne) return err(L_ENOMEM);
            ep->e = ne; ep->cap = nc;
        }
        ep->e[ep->n].fd = fd; ep->e[ep->n].events = u32(ev); ep->e[ep->n].data = u64(ev + 8);
        ep->n++;
        return 0;
    case 2:                                                        /* EPOLL_CTL_DEL */
        if (k == ep->n) return err(L_ENOENT);
        ep->e[k] = ep->e[--ep->n];
        return 0;
    case 3:                                                        /* EPOLL_CTL_MOD */
        if (k == ep->n) return err(L_ENOENT);
        ep->e[k].events = u32(ev); ep->e[k].data = u64(ev + 8);
        return 0;
    default: return err(L_EINVAL);
    }
}

static uint64_t sys_epoll_pwait(struct aoi_proc *p, struct aoi_proc_fd *f, uint64_t evp, int max, int timeout_ms)
{
    struct aoi_epoll *ep = f->ep;
    int k, n = 0;
    if (max <= 0) return err(L_EINVAL);
    for (k = 0; k < ep->n && n < max; k++) {
        uint32_t r = fd_ready(p, ep->e[k].fd, ep->e[k].events);
        if (r) {
            uint8_t ev[16];
            memset(ev, 0, sizeof ev);
            memcpy(ev, &r, 4); memcpy(ev + 8, &ep->e[k].data, 8);
            if (!put(p, evp + 16 * (uint64_t)n, ev, 16)) return err(L_EFAULT);
            n++;
            if (ep->e[k].events & 0x40000000u) ep->e[k].events = 0;   /* EPOLLONESHOT: disarmed */
        }
    }
    if (n || timeout_ms == 0) { p->th[p->cur].poll_deadline = 0; return (uint64_t)n; }
    return poll_wait(p, timeout_ms < 0 ? -1 : (int64_t)timeout_ms * 1000000) ? 0 : 0;
}

static uint64_t sys_ppoll(struct aoi_proc *p, uint64_t fds, uint64_t nfds, uint64_t tsp)
{
    uint64_t i, n = 0;
    int64_t timeout = -1;
    if (nfds > 4096) return err(L_EINVAL);
    if (tsp) {
        uint8_t ts[16];
        if (!get(p, tsp, ts, 16)) return err(L_EFAULT);
        timeout = (int64_t)u64(ts) * 1000000000 + (int64_t)u64(ts + 8);
    }
    for (i = 0; i < nfds; i++) {
        uint8_t pf[8];
        int32_t fd;
        uint16_t want, rev;
        if (!get(p, fds + 8 * i, pf, 8)) return err(L_EFAULT);
        memcpy(&fd, pf, 4); want = u16(pf + 4);
        if (fd < 0) rev = 0;
        else if (!fd_get(p, (uint64_t)fd)) rev = 0x20;             /* POLLNVAL */
        else rev = (uint16_t)fd_ready(p, fd, want);
        memcpy(pf + 6, &rev, 2);
        if (!put(p, fds + 8 * i + 6, pf + 6, 2)) return err(L_EFAULT);
        if (rev) n++;
    }
    if (n || timeout == 0) { p->th[p->cur].poll_deadline = 0; return n; }
    poll_wait(p, timeout);
    return 0;
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

/* logd emulation: liblog writes one packet per writev to /dev/socket/logdw:
 * header { u8 log_id; u16 tid; u32 sec; u32 nsec } then, for text logs,
 * u8 priority, tag\0, message. Printed as "P/tag: message". */
static uint64_t logd_write(struct aoi_proc *p, const uint8_t *pkt, uint64_t n)
{
    static const char prio[] = "??VDIWEF";
    const char *tag, *msg;
    if (n < 12 || !p->log) return n;
    if (pkt[0] == 2 || pkt[0] == 3 || pkt[0] == 6 || pkt[0] == 7) return n;   /* binary event buffers */
    tag = (const char *)pkt + 12;
    msg = memchr(tag, 0, (size_t)(n - 12)) ? tag + strlen(tag) + 1 : NULL;
    if (!msg || msg >= (const char *)pkt + n) return n;
    fprintf(p->log, "%c/%s: %.*s\n", pkt[11] < 8 ? prio[pkt[11]] : '?', tag,
            (int)((const char *)pkt + n - msg), msg);
    fflush(p->log);
    return n;
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
        if (f->kind == AOI_FD_EVENTFD) {                           /* the counter (or 1), then less */
            uint64_t v;
            if (a2 < 8) { r = err(L_EINVAL); break; }
            if (!f->count) { r = f->nonblock ? err(L_EAGAIN) : block_and_retry(p, 1000000); break; }
            v = f->sem ? 1 : f->count;
            f->count -= v;
            r = put(p, a1, &v, 8) ? 8 : err(L_EFAULT);
            break;
        }
        r = a2 ? (uint64_t)xfer(p, f->host, a1, a2, 1, nr == NR_read ? -1 : (int64_t)a3) : 0;
        if (r == err(L_EAGAIN) && f->kind == AOI_FD_PIPE && !f->nonblock) r = block_and_retry(p, 1000000);
        break;
    }
    case NR_write: case NR_pwrite64: {
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        if (f->kind == AOI_FD_EVENTFD) {                           /* add to the counter */
            uint64_t v;
            if (a2 < 8 || !get(p, a1, &v, 8)) { r = err(a2 < 8 ? L_EINVAL : L_EFAULT); break; }
            if (v == ~0ULL) { r = err(L_EINVAL); break; }
            if (f->count + v < f->count || f->count + v == ~0ULL) {
                r = f->nonblock ? err(L_EAGAIN) : block_and_retry(p, 1000000);
                break;
            }
            f->count += v;
            r = 8;
            break;
        }
        if (f->kind == AOI_FD_LOGD) {
            uint8_t pkt[4096];
            uint64_t n = a2 < sizeof pkt ? a2 : sizeof pkt;
            r = get(p, a1, pkt, n) ? logd_write(p, pkt, n), a2 : err(L_EFAULT);
            break;
        }
        if (f->host <= 2) fflush(stdout);
        r = a2 ? (uint64_t)xfer(p, f->host, a1, a2, 0, nr == NR_write ? -1 : (int64_t)a3) : 0;
        if (r == err(L_EAGAIN) && f->kind == AOI_FD_PIPE && !f->nonblock) r = block_and_retry(p, 1000000);
        break;
    }
    case NR_readv: case NR_writev: {
        uint64_t i, total = 0;
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        if (f->kind == AOI_FD_LOGD && nr == NR_writev) {          /* gather one log packet */
            uint8_t pkt[4096];
            uint64_t n = 0;
            for (i = 0; i < a2 && i < 16; i++) {
                uint8_t e[16];
                uint64_t len;
                if (!get(p, a1 + i * 16, e, 16)) break;
                len = u64(e + 8);
                if (n + len > sizeof pkt) len = sizeof pkt - n;
                if (len && !get(p, u64(e), pkt + n, len)) break;
                n += len; total += u64(e + 8);
            }
            logd_write(p, pkt, n);
            r = total;
            break;
        }
        if (f->kind != AOI_FD_FILE && f->kind != AOI_FD_PIPE) { r = err(L_ENOTCONN); break; }
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
        if (!strncmp(g, "/proc/", 6)) {                             /* synthetic procfs */
            if ((hfd = proc_file(p, g)) < 0) { r = err(L_ENOENT); break; }
        } else if (!strcmp(g, "/dev/binder") || !strcmp(g, "/dev/hwbinder") || !strcmp(g, "/dev/vndbinder")) {
            if ((hfd = open("/dev/null", O_RDWR | O_CLOEXEC)) < 0) { r = herr(); break; }
            if ((fdn = fd_new(p, hfd, g, 0)) < 0) { close(hfd); r = err(L_EMFILE); break; }
            p->fd[fdn].kind = AOI_FD_BINDER;                        /* core/binder.c */
            r = (uint64_t)fdn;
            if (p->trace) fprintf(p->trace, "[sys] open %s -> %d (binder)\n", g, fdn);
            break;
        } else {
            to_host(p, g, h);
            hfd = open(h, host_oflags(a2) | O_CLOEXEC, (mode_t)a3);
        }
        if (hfd < 0) { r = herr(); break; }
        if ((fdn = fd_new(p, hfd, g, 0)) < 0) { close(hfd); r = err(L_EMFILE); break; }
        r = (uint64_t)fdn;
        if (p->trace) fprintf(p->trace, "[sys] open %s -> %d\n", g, fdn);
        break;
    }
    case NR_close:
        if (!(f = fd_get(p, a0))) { r = err(L_EBADF); break; }
        if (a0 > 2) { if (f->dir) closedir(f->dir); else close(f->host); }
        if (f->kind == AOI_FD_EPOLL) epoll_unref(f->ep);
        f->used = 0; f->dir = NULL; f->ep = NULL;
        epoll_forget(p, (int)a0);
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
            p->fd[n].kind = f->kind; p->fd[n].nonblock = f->nonblock;
            if ((p->fd[n].ep = f->ep)) f->ep->refs++;             /* (a dup'd eventfd copies its counter) */
            r = (uint64_t)n;
            break;
        }
        case 1: case 2: r = 0; break;                              /* F_GETFD / F_SETFD */
        case 3: r = 2 | (f->nonblock ? 04000 : 0); break;          /* F_GETFL: O_RDWR (+ O_NONBLOCK) */
        case 4: f->nonblock = (a2 & 04000) != 0; r = 0; break;     /* F_SETFL: O_NONBLOCK is what counts */
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
            if (t->used && t->kind == AOI_FD_EPOLL) epoll_unref(t->ep);
            t->used = 1; t->host = d; t->dir = NULL; t->kind = f->kind; t->nonblock = f->nonblock;
            if ((t->ep = f->ep)) f->ep->refs++;
            join(t->path, AOI_PATH, "", f->path);
            r = a1;
        } else {
            if ((n = fd_new(p, d, f->path, 0)) < 0) { close(d); r = err(L_EMFILE); break; }
            p->fd[n].kind = f->kind; p->fd[n].nonblock = f->nonblock;
            if ((p->fd[n].ep = f->ep)) f->ep->refs++;             /* (a dup'd eventfd copies its counter) */
            r = (uint64_t)n;
        }
        break;
    }
    case NR_eventfd2: case NR_epoll_create1: {
        int d, n;
        if ((d = open("/dev/null", O_RDWR | O_CLOEXEC)) < 0) { r = herr(); break; }
        if ((n = fd_new(p, d, nr == NR_eventfd2 ? "anon_inode:[eventfd]" : "anon_inode:[eventpoll]", 0)) < 0) {
            close(d); r = err(L_EMFILE); break;
        }
        if (nr == NR_eventfd2) {                                   /* initval, EFD_SEMAPHORE|NONBLOCK|CLOEXEC */
            p->fd[n].kind = AOI_FD_EVENTFD;
            p->fd[n].count = (uint32_t)a0;
            p->fd[n].sem = (a1 & 1) != 0;
            p->fd[n].nonblock = (a1 & 04000) != 0;
        } else {
            if (!(p->fd[n].ep = calloc(1, sizeof *p->fd[n].ep))) { p->fd[n].used = 0; close(d); r = err(L_ENOMEM); break; }
            p->fd[n].ep->refs = 1;
            p->fd[n].kind = AOI_FD_EPOLL;
        }
        r = (uint64_t)n;
        break;
    }
    case NR_epoll_ctl:
        if (!(f = fd_get(p, a0)) || f->kind != AOI_FD_EPOLL) { r = err(f ? L_EINVAL : L_EBADF); break; }
        r = sys_epoll_ctl(p, f, (int)a1, (int)a2, a3);
        break;
    case NR_epoll_pwait:
        if (!(f = fd_get(p, a0)) || f->kind != AOI_FD_EPOLL) { r = err(f ? L_EINVAL : L_EBADF); break; }
        r = sys_epoll_pwait(p, f, a1, (int)a2, (int)a3);
        break;
    case NR_ppoll:
        r = sys_ppoll(p, a0, a1, a2);
        break;
    case NR_pipe2: {                                               /* host pipe, non-blocking underneath */
        int hp[2], n0, n1;
        int32_t gfd[2];
        if (a1 & ~(uint64_t)(04000 | 02000000)) { r = err(L_EINVAL); break; }   /* O_NONBLOCK, O_CLOEXEC */
        if (pipe(hp)) { r = herr(); break; }
        fcntl(hp[0], F_SETFL, O_NONBLOCK); fcntl(hp[1], F_SETFL, O_NONBLOCK);
        fcntl(hp[0], F_SETFD, FD_CLOEXEC); fcntl(hp[1], F_SETFD, FD_CLOEXEC);
        if ((n0 = fd_new(p, hp[0], "pipe:[0]", 0)) < 0) { close(hp[0]); close(hp[1]); r = err(L_EMFILE); break; }
        if ((n1 = fd_new(p, hp[1], "pipe:[0]", 0)) < 0) { p->fd[n0].used = 0; close(hp[0]); close(hp[1]); r = err(L_EMFILE); break; }
        p->fd[n0].kind = p->fd[n1].kind = AOI_FD_PIPE;
        p->fd[n0].nonblock = p->fd[n1].nonblock = (a1 & 04000) != 0;
        gfd[0] = n0; gfd[1] = n1;
        r = put(p, a0, gfd, 8) ? 0 : err(L_EFAULT);
        break;
    }
    case NR_socket: {                                              /* AF_UNIX only: logd, or nothing */
        int n, d;
        if (a0 != 1) { r = err(L_EAFNOSUPPORT); break; }
        if ((d = open("/dev/null", O_RDWR | O_CLOEXEC)) < 0) { r = herr(); break; }
        if ((n = fd_new(p, d, "socket:[unix]", 0)) < 0) { close(d); r = err(L_EMFILE); break; }
        p->fd[n].kind = AOI_FD_SOCKET;
        r = (uint64_t)n;
        break;
    }
    case NR_connect: {
        char path[110];
        uint8_t sa[110];
        if (!(f = fd_get(p, a0)) || f->kind == AOI_FD_FILE) { r = err(L_ENOTSOCK); break; }
        memset(sa, 0, sizeof sa);
        if (a2 < 3 || !get(p, a1, sa, a2 < sizeof sa ? a2 : sizeof sa - 1)) { r = err(L_EFAULT); break; }
        memcpy(path, sa + 2, sizeof path - 2); path[sizeof path - 3] = 0;
        if (!strcmp(path, "/dev/socket/logdw")) { f->kind = AOI_FD_LOGD; r = 0; }
        else r = err(L_ECONNREFUSED);                              /* no other daemons yet */
        break;
    }
    case NR_membarrier:                                            /* one thread: every barrier is trivially done */
        r = a0 == 0 ? (1 << 3 | 1 << 4) : 0;                       /* QUERY: PRIVATE_EXPEDITED (+REGISTER) */
        break;
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
    case NR_userfaultfd: {                                         /* see uffd_ioctl */
        int n, d;
        if (!p->uffd) { r = err(L_ENOSYS); break; }               /* see docs/STATUS.md: boot image under CMC */
        if ((d = open("/dev/null", O_RDWR | O_CLOEXEC)) < 0) { r = herr(); break; }
        if ((n = fd_new(p, d, "anon_inode:[userfaultfd]", 0)) < 0) { close(d); r = err(L_EMFILE); break; }
        p->fd[n].kind = AOI_FD_UFFD;
        r = (uint64_t)n;
        break;
    }
    case NR_ioctl:
        if ((f = fd_get(p, a0)) && f->kind == AOI_FD_BINDER) {
            int block;
            r = aoi_binder_ioctl(p, a1, a2, &block);
            if (block) r = block_and_retry(p, 5000000);             /* a looper waits for work */
            break;
        }
        if ((f = fd_get(p, a0)) && f->kind == AOI_FD_UFFD) {
            if (a1 == 0xc018aa3f) {                                /* UFFDIO_API {api, features, ioctls} */
                uint64_t api[3];
                if (!get(p, a2, api, 24)) { r = err(L_EFAULT); break; }
                if (api[0] != 0xaa) { r = err(L_EINVAL); break; }
                api[1] = 1u << 7;                                  /* SIGBUS mode only: no shmem, no minor faults */
                api[2] = 1ULL << 0x3f | 1ULL << 0 | 1ULL << 1;     /* API, REGISTER, UNREGISTER */
                r = put(p, a2, api, 24) ? 0 : err(L_EFAULT);
            } else if (a1 == 0xc020aa00) {                         /* UFFDIO_REGISTER {start, len, mode, ioctls} */
                uint64_t rg[4];
                if (!get(p, a2, rg, 32)) { r = err(L_EFAULT); break; }
                if (rg[0] % PAGE || !rg[1] || rg[1] % PAGE || rg[2] != 1) { r = err(L_EINVAL); break; }   /* MISSING only */
                if (!mapped(p, rg[0], rg[1])) { r = err(L_ENOMEM); break; }
                if (p->nuffd_reg == (int)(sizeof p->uffd_reg / sizeof p->uffd_reg[0])) { r = err(L_ENOMEM); break; }
                p->uffd_reg[p->nuffd_reg].start = rg[0];
                p->uffd_reg[p->nuffd_reg++].len = rg[1];
                rg[3] = 1ULL << 2 | 1ULL << 3 | 1ULL << 4;          /* WAKE, COPY, ZEROPAGE */
                r = put(p, a2, rg, 32) ? 0 : err(L_EFAULT);
            } else if (a1 == 0x8010aa01) {                         /* UFFDIO_UNREGISTER {start, len} */
                uint64_t rg[2];
                int i, j;
                if (!get(p, a2, rg, 16)) { r = err(L_EFAULT); break; }
                for (i = j = 0; i < p->nuffd_reg; i++) {           /* drop the ranges inside, trim the others */
                    uint64_t s0 = p->uffd_reg[i].start, e0 = s0 + p->uffd_reg[i].len, e1 = rg[0] + rg[1];
                    if (s0 >= rg[0] && e0 <= e1) continue;
                    if (s0 < rg[0] && e0 > rg[0]) e0 = rg[0];
                    else if (s0 < e1 && e0 > e1) s0 = e1;
                    p->uffd_reg[j].start = s0; p->uffd_reg[j++].len = e0 - s0;
                }
                p->nuffd_reg = j;
                uffd_fill(p, rg[0], rg[1]);                        /* unwatched now: zero-fill on demand */
                r = 0;
            } else if (a1 == 0xc028aa03 || a1 == 0xc020aa04) {    /* UFFDIO_COPY {dst, src, len, mode, copy} / ZEROPAGE */
                uint64_t u[5], o, dst, len;
                int zero = a1 == 0xc020aa04;
                uint8_t pg[4096];
                if (!get(p, a2, u, zero ? 32 : 40)) { r = err(L_EFAULT); break; }
                dst = u[0]; len = zero ? u[1] : u[2];
                if (dst % PAGE || len % PAGE || !len) { r = err(L_EINVAL); break; }
                for (o = 0, r = 0; o < len; o += PAGE) {
                    if (!mapped(p, dst + o, PAGE)) { r = err(L_ENOENT); break; }
                    if (!(p->vm.prot[(dst + o) / PAGE] & AOI_PROT_MISSING)) { r = err(L_EEXIST); break; }
                    if (zero) memset(pg, 0, sizeof pg);
                    else if (!get(p, u[1] + o, pg, PAGE)) { r = err(L_EFAULT); break; }
                    aoi_vm_set_missing(&p->vm, dst + o, PAGE, 0);
                    aoi_vm_write(&p->vm, dst + o, pg, PAGE, 0);
                }
                u[zero ? 3 : 4] = o ? o : r;                       /* bytes done, or the error */
                put(p, a2, u, zero ? 32 : 40);
            } else if (a1 == 0x8010aa02) {                         /* UFFDIO_WAKE: nobody sleeps in SIGBUS mode */
                r = 0;
            } else {
                if (p->trace) fprintf(p->trace, "[uffd] ioctl %#llx not implemented\n", (unsigned long long)a1);
                r = err(L_EINVAL);
            }
            break;
        }
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
        if (a3 & 4) {                                              /* MREMAP_DONTUNMAP: move the bytes, keep the */
            if (a1 != a2 || a1 % PAGE) { r = err(L_EINVAL); break; }   /* source mapped but missing (userfaultfd) */
            if (fixed && a4 < a0 + a1 && a0 < a4 + a1) { r = err(L_EINVAL); break; }
            prot0 = p->vm.prot[a0 / PAGE] & 7;
            na = aoi_vm_map(&p->vm, fixed ? a4 : 0, a1, (int)prot0 | AOI_PROT_W, fixed);
            if (IS_ERR(na)) { r = na; break; }
            uffd_fill(p, a0, a1);                                  /* (missing source pages move as zero) */
            aoi_vm_move(&p->vm, na, a0, a1);
            aoi_vm_protect(&p->vm, na, a1, (int)prot0);
            aoi_vm_set_missing(&p->vm, a0, a1, 1);
            r = na;
            break;
        }
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
    case NR_mincore: {                                             /* every mapped page counts as resident */
        uint64_t q, e = a0 + up(a1, PAGE);
        uint8_t v[256];
        if (a0 % PAGE) { r = err(L_EINVAL); break; }
        if (!mapped(p, a0, a1 ? a1 : 1)) { r = err(L_ENOMEM); break; }
        for (q = a0, r = 0; q < e; q += sizeof v * PAGE) {
            uint64_t k, n = (e - q) / PAGE < sizeof v ? (e - q) / PAGE : sizeof v;
            for (k = 0; k < n; k++) v[k] = !(p->vm.prot[q / PAGE + k] & AOI_PROT_MISSING);
            if (!put(p, a2 + (q - a0) / PAGE, v, n)) { r = err(L_EFAULT); break; }
        }
        break;
    }
    case NR_madvise:
        if (a2 == 4 || a2 == 9) {                                  /* MADV_DONTNEED / MADV_REMOVE: reads back as zero */
            uint64_t q, e = a0 + up(a1, PAGE), n;
            if (mapped(p, a0, a1))
                for (q = a0; q < e; q = n) {                       /* in runs: whole host pages are released, */
                    int w = uffd_watched(p, q), k = (p->vm.prot[q / PAGE] & AOI_PROT_W) != 0;   /* not written */
                    for (n = q + PAGE; n < e && uffd_watched(p, n) == w
                         && ((p->vm.prot[n / PAGE] & AOI_PROT_W) != 0) == k; n += PAGE) {}
                    if (w) aoi_vm_set_missing(&p->vm, q, n - q, 1);   /* zapped: missing again */
                    else if (k) aoi_vm_zero(&p->vm, q, n - q);
                }
        }
        r = 0;
        break;
    case NR_brk:
        r = p->brk;                                                /* fixed: bionic does not grow it */
        break;
    case NR_exit: case NR_exit_group:
        c->exit_code = (int)a0;
        c->stop = AOI_STOP_EXIT;
        p->thread_exit = nr == NR_exit;                            /* exit ends only this thread */
        r = 0;
        break;
    case NR_set_tid_address:
        p->th[p->cur].clear_tid = a0;
        r = (uint64_t)p->th[p->cur].tid;
        break;
    case NR_gettid: r = (uint64_t)p->th[p->cur].tid; break;
    case NR_getpid: r = GUEST_PID; break;
    case NR_clone: {                                               /* threads; fork is not done yet */
        int i;
        struct aoi_thread *t = NULL;
        if (!(a0 & 0x100) || !(a0 & 0x10000)) { r = err(L_ENOSYS); break; }  /* need CLONE_VM | CLONE_THREAD */
        for (i = 0; i < AOI_PROC_THREADS; i++) if (p->th[i].state == AOI_T_FREE) { t = &p->th[i]; break; }
        if (!t) { r = err(L_EAGAIN); break; }
        memset(t, 0, sizeof *t);
        t->tid = p->next_tid++;
        t->cpu = *c;
        t->cpu.x[0] = 0;
        t->cpu.pc = c->pc + 4;                                     /* the child returns from this svc */
        if (a1) t->cpu.sp = a1;
        if (a0 & 0x80000) t->cpu.tpidr = a3;                       /* CLONE_SETTLS */
        t->cpu.excl_valid = 0;
        t->cpu.stop = AOI_RUN;
        if (a0 & 0x200000) t->clear_tid = a4;                      /* CLONE_CHILD_CLEARTID */
        {
            uint32_t tid32 = (uint32_t)t->tid;
            if ((a0 & 0x100000) && !put(p, a2, &tid32, 4)) { r = err(L_EFAULT); break; }   /* PARENT_SETTID */
            if ((a0 & 0x1000000) && !put(p, a4, &tid32, 4)) { r = err(L_EFAULT); break; }  /* CHILD_SETTID */
        }
        t->state = AOI_T_RUN;
        r = (uint64_t)t->tid;
        break;
    }
    case NR_getppid: r = 1; break;
    case NR_getuid: case NR_geteuid: case NR_getgid: case NR_getegid: r = 0; break;
    case NR_umask: r = 022; break;
    case NR_set_robust_list: r = 0; break;
    case NR_futex: {
        int op = (int)a1 & 127;                                    /* minus PRIVATE / CLOCK_REALTIME */
        uint64_t addr = a0 & 0x00ffffffffffffffULL;
        uint8_t w[4];
        if (op == 0 || op == 9) {                                  /* WAIT / WAIT_BITSET */
            struct aoi_thread *t = &p->th[p->cur];
            int64_t deadline = 0;
            if (!get(p, a0, w, 4)) { r = err(L_EFAULT); break; }
            if (u32(w) != (uint32_t)a2) { r = err(L_EAGAIN); break; }
            if (op == 9 && !(uint32_t)a5) { r = err(L_EINVAL); break; }
            if (a3) {
                uint8_t ts[16];
                int64_t v;
                if (!get(p, a3, ts, 16)) { r = err(L_EFAULT); break; }
                v = (int64_t)u64(ts) * 1000000000 + (int64_t)u64(ts + 8);
                if (op == 0) deadline = now_ns() + v;              /* relative */
                else if (a1 & 256) {                               /* absolute, CLOCK_REALTIME */
                    struct timespec rt;
                    clock_gettime(CLOCK_REALTIME, &rt);
                    deadline = now_ns() + v - ((int64_t)rt.tv_sec * 1000000000 + rt.tv_nsec);
                } else deadline = v;                               /* absolute, CLOCK_MONOTONIC */
                if (deadline <= 0) deadline = 1;
            }
            t->state = AOI_T_FUTEX;
            t->futex_addr = addr;
            t->futex_bitset = op == 9 ? (uint32_t)a5 : ~0u;
            t->deadline = deadline;
            c->stop = AOI_STOP_YIELD;
            r = 0;                                                 /* or ETIMEDOUT, set by the scheduler */
        } else if (op == 1 || op == 10) {                          /* WAKE / WAKE_BITSET */
            r = (uint64_t)futex_wake(p, addr, (int)(a2 > 0x7fffffff ? 0x7fffffff : a2), op == 10 ? (uint32_t)a5 : ~0u);
        } else if (op == 3 || op == 4) {                           /* REQUEUE / CMP_REQUEUE */
            int i, woken, moved = 0;
            uint64_t addr2 = a4 & 0x00ffffffffffffffULL;
            if (op == 4) {
                if (!get(p, a0, w, 4)) { r = err(L_EFAULT); break; }
                if (u32(w) != (uint32_t)a5) { r = err(L_EAGAIN); break; }
            }
            woken = futex_wake(p, addr, (int)a2, ~0u);
            for (i = 0; i < AOI_PROC_THREADS && (uint64_t)moved < a3; i++)
                if (p->th[i].state == AOI_T_FUTEX && p->th[i].futex_addr == addr) { p->th[i].futex_addr = addr2; moved++; }
            r = (uint64_t)(woken + moved);
        } else r = err(L_ENOSYS);
        break;
    }
    case NR_rt_sigaction:
        if (a0 < 1 || a0 > 64) { r = err(L_EINVAL); break; }
        if (a2 && !put(p, a2, p->sigact[a0], 32)) { r = err(L_EFAULT); break; }
        if (a1 && (a0 == 9 || a0 == 19)) { r = err(L_EINVAL); break; }
        if (a1 && !get(p, a1, p->sigact[a0], 32)) { r = err(L_EFAULT); break; }
        if (a1 && p->trace)
            fprintf(p->trace, "[sig] sigaction(%d): handler %#llx flags %#llx restorer %#llx\n", (int)a0,
                    (unsigned long long)p->sigact[a0][0], (unsigned long long)p->sigact[a0][1], (unsigned long long)p->sigact[a0][2]);
        r = 0;
        break;
    case NR_rt_sigreturn:
        if (!sig_return(p)) { c->stop = AOI_STOP_FAULT; c->fault_addr = c->sp; r = 0; break; }
        c->stop = AOI_STOP_NEWPC;
        r = 0;
        break;
    case NR_rt_sigprocmask: {
        uint64_t old = p->th[p->cur].sigmask, set, *m = &p->th[p->cur].sigmask;
        if (a1) {
            uint8_t s8[8];
            if (!get(p, a1, s8, 8)) { r = err(L_EFAULT); break; }
            set = u64(s8);
            if (a0 == 0) *m |= set; else if (a0 == 1) *m &= ~set; else if (a0 == 2) *m = set;
            else { r = err(L_EINVAL); break; }
            *m &= ~UNBLOCKABLE;
            if (p->th[p->cur].pending & ~*m) c->stop = AOI_STOP_YIELD;  /* now deliverable: on return */
        }
        r = a2 && !put(p, a2, &old, 8) ? err(L_EFAULT) : 0;
        break;
    }
    case NR_sigaltstack:
        {
            struct aoi_thread *t = &p->th[p->cur];
            uint64_t old[3];
            old[0] = t->altstack[0]; old[2] = t->altstack[2];
            old[1] = !t->altstack[2] ? 2 : (p->cpu.sp >= t->altstack[0] && p->cpu.sp < t->altstack[0] + t->altstack[2]);
            if (a1 && !put(p, a1, old, 24)) { r = err(L_EFAULT); break; }
            if (a0 && !get(p, a0, t->altstack, 24)) { r = err(L_EFAULT); break; }
            if (a0 && (t->altstack[1] & 2)) t->altstack[2] = 0;           /* SS_DISABLE */
        }
        r = 0;
        break;
    case NR_kill: case NR_tkill: case NR_tgkill: {
        int sig = (int)(nr == NR_tgkill ? a2 : a1), i;
        uint64_t target = nr == NR_tgkill ? a1 : a0;
        if (sig < 0 || sig > 64) { r = err(L_EINVAL); break; }
        if (nr == NR_kill) {                                       /* the process: any thread takes it */
            if (target != GUEST_PID && target != 0 && (int64_t)target != -1) { r = err(L_ESRCH); break; }
            target = (uint64_t)p->th[p->cur].tid;
        }
        for (i = 0; i < AOI_PROC_THREADS; i++)
            if (p->th[i].state != AOI_T_FREE && p->th[i].tid == (int)target) break;
        if (i == AOI_PROC_THREADS) { r = err(L_ESRCH); break; }
        r = 0;
        if (!sig) break;
        {
            struct aoi_thread *t = &p->th[i];                     /* waiting in sigwait for it? */
            if (t->state == AOI_T_SLEEP && (t->sigwait_mask & SIGBIT(sig))) {
                uint8_t info[128];
                memset(info, 0, sizeof info);
                info[0] = (uint8_t)sig;
                if (t->sigwait_info) put(p, t->sigwait_info, info, sizeof info);
                t->sigwait_mask = 0;
                t->state = AOI_T_RUN;
                if (i == p->cur) p->cpu.x[0] = (uint64_t)sig; else t->cpu.x[0] = (uint64_t)sig;
                break;
            }
        }
        if (!sig_send(p, i, sig)) break;                           /* default action: the process ends */
        if (i == p->cur) c->stop = AOI_STOP_YIELD;                 /* to itself: delivered on return */
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
        {
            struct aoi_thread *t = &p->th[p->cur];
            int64_t v = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
            if (nr == NR_clock_nanosleep && (a1 & 1)) {            /* TIMER_ABSTIME */
                if (a0 == 0) {
                    struct timespec rt;
                    clock_gettime(CLOCK_REALTIME, &rt);
                    v -= (int64_t)rt.tv_sec * 1000000000 + rt.tv_nsec;
                } else v -= now_ns();
            }
            t->state = AOI_T_SLEEP;
            t->deadline = now_ns() + (v > 0 ? v : 0) + 1;
            c->stop = AOI_STOP_YIELD;                              /* others run meanwhile */
        }
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
    case NR_rt_sigtimedwait: {                                     /* signals are not delivered: wait it out */
        struct aoi_thread *t = &p->th[p->cur];
        uint8_t m[8];
        t->deadline = 0;
        if (!get(p, a0, m, 8)) { r = err(L_EFAULT); break; }
        t->sigwait_mask = u64(m);
        t->sigwait_info = a1;
        if (a2) {
            uint8_t ts[16];
            if (!get(p, a2, ts, 16)) { r = err(L_EFAULT); break; }
            t->deadline = now_ns() + (int64_t)u64(ts) * 1000000000 + (int64_t)u64(ts + 8) + 1;
        }
        t->state = AOI_T_SLEEP;                                    /* deadline 0: until the process ends */
        c->stop = AOI_STOP_YIELD;
        r = err(L_EAGAIN);
        break;
    }
    case NR_sched_yield: c->stop = AOI_STOP_YIELD; r = 0; break;
    case NR_flock:                                                  /* one process: locks always succeed */
        r = fd_get(p, a0) ? 0 : err(L_EBADF);
        break;
    case NR_setpriority: r = 0; break;                              /* accepted; one scheduler for all */
    case NR_getpriority: r = 20; break;                             /* nice 0, in the kernel's 20-nice form */
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
    if (c->stop != AOI_STOP_RESTART) trace_sys(p, nr, r);       /* a blocked retry is not a result */
    return r;
}
