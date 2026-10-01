/* An in-process stand-in for the binder driver (/dev/binder, hwbinder, vndbinder).
 *
 * libbinder talks to the kernel through BINDER_WRITE_READ: a write buffer of BC_*
 * commands, a read buffer it wants filled with BR_* returns. Here there is one
 * process and no other: the only peer is handle 0, the context manager
 * (servicemanager), answered right here. It replies to PING and INTERFACE, and to
 * the IServiceManager calls (Android 14 AIDL) with "no such service". Replies are
 * parcels written into the receive buffer the guest mmapped on its binder fd.
 * Native services (AIM ADR 0013) will later plug in behind real handles.
 *
 * As the kernel does, a call resumes from write_consumed / read_consumed: a looper
 * thread with nothing to read is put to sleep by core/proc.c and runs the ioctl
 * again, without repeating the commands it already wrote. */
#include "binder.h"
#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BC_SIZE(cmd)        (((cmd) >> 16) & 0x3fff)        /* _IOW payload size */
#define BC_TRANSACTION      0x40406300u
#define BC_REPLY            0x40406301u
#define BC_TRANSACTION_SG   0x40486311u
#define BC_REPLY_SG         0x40486312u
#define BC_ENTER_LOOPER     0x0000630cu
#define BC_REGISTER_LOOPER  0x0000630bu
#define BC_EXIT_LOOPER      0x0000630du
#define BR_REPLY            0x80407203u
#define BR_DEAD_REPLY       0x00007205u
#define BR_TRANSACTION_COMPLETE 0x00007206u
#define BR_NOOP             0x0000720cu
#define TF_ONE_WAY          0x01u

#define BINDER_WRITE_READ   0xc0306201u
#define BINDER_SET_MAX_THREADS 0x40046205u
#define BINDER_SET_CONTEXT_MGR 0x40046207u
#define BINDER_THREAD_EXIT  0x40046208u
#define BINDER_VERSION      0xc0046209u
#define BINDER_ENABLE_ONEWAY_SPAM_DETECTION 0x40046210u
#define BINDER_GET_EXTENDED_ERROR 0xc00c6211u

#define BINDER_TYPE_BINDER  0x73622a85u                        /* B_PACK_CHARS('s','b','*',0x85) */
#define PING_TRANSACTION    0x5f504e47u                        /* '_PNG' */
#define INTERFACE_TRANSACTION 0x5f4e5446u                      /* '_NTF' */

#define EINVAL_ 22
#define EFAULT_ 14

/* Per guest thread: BR_* words waiting to be read, and whether it is a looper. */
struct bthread { int tid, looper; uint32_t n; uint8_t q[512]; };

struct aoi_binder {
    uint64_t buf, buflen, next;     /* the guest's receive mapping, bump allocator in it */
    struct bthread th[AOI_PROC_THREADS];
};

static struct aoi_binder *state(struct aoi_proc *p)
{
    if (!p->binder) p->binder = calloc(1, sizeof *p->binder);
    return p->binder;
}

static struct bthread *bthread(struct aoi_binder *b, int tid)
{
    int i, fr = -1;
    for (i = 0; i < AOI_PROC_THREADS; i++) {
        if (b->th[i].tid == tid) return &b->th[i];
        if (!b->th[i].tid && fr < 0) fr = i;
    }
    if (fr < 0) return NULL;
    memset(&b->th[fr], 0, sizeof b->th[fr]);
    b->th[fr].tid = tid;
    return &b->th[fr];
}

static int gread(struct aoi_proc *p, uint64_t a, void *d, uint64_t n)
{
    return !n || aoi_vm_read(&p->vm, a & 0x00ffffffffffffffULL, d, n, AOI_PROT_R);
}

static int gwrite(struct aoi_proc *p, uint64_t a, const void *s, uint64_t n)
{
    return !n || aoi_vm_write(&p->vm, a & 0x00ffffffffffffffULL, s, n, AOI_PROT_W);
}

static void push(struct bthread *t, const void *v, uint32_t n)
{
    if (t->n + n <= sizeof t->q) { memcpy(t->q + t->n, v, n); t->n += n; }
}

static void push32(struct bthread *t, uint32_t v) { push(t, &v, 4); }

/* ---------- parcels ---------- */

struct parcel { uint8_t d[1024]; uint32_t n; };

static void p32(struct parcel *pc, uint32_t v)
{
    if (pc->n + 4 <= sizeof pc->d) { memcpy(pc->d + pc->n, &v, 4); pc->n += 4; }
}

static void p64(struct parcel *pc, uint64_t v) { p32(pc, (uint32_t)v); p32(pc, (uint32_t)(v >> 32)); }

static void pstr16(struct parcel *pc, const char *s)          /* String16: length, UTF-16, NUL, pad to 4 */
{
    uint32_t i, len = (uint32_t)strlen(s);
    p32(pc, len);
    for (i = 0; i <= len; i += 2) p32(pc, (uint32_t)(uint8_t)s[i] | (i + 1 <= len ? (uint32_t)(uint8_t)s[i + 1] << 16 : 0));
}

static void pnullbinder(struct parcel *pc)                    /* writeStrongBinder(nullptr) */
{
    p32(pc, BINDER_TYPE_BINDER); p32(pc, 0); p64(pc, 0); p64(pc, 0);
    p32(pc, 0);                                                /* stability */
}

/* The first String16 at or after `off` words in, as ASCII (for the trace). */
static void str16_at(const uint8_t *d, uint32_t n, uint32_t off, char *out, size_t cap)
{
    uint32_t len, i;
    out[0] = 0;
    if (off + 4 > n) return;
    memcpy(&len, d + off, 4);
    if (len == 0xffffffffu || off + 4 + 2 * len > n) return;
    for (i = 0; i < len && i + 1 < cap; i++) out[i] = (char)d[off + 4 + 2 * i];
    out[i] = 0;
}

/* servicemanager (handle 0): the reply to `code` with request `req`. */
static void servicemanager(struct aoi_proc *p, uint32_t code, const uint8_t *req, uint32_t n, struct parcel *rep)
{
    char iface[96], name[128];
    uint32_t ilen = 0;
    rep->n = 0;
    if (code == PING_TRANSACTION) return;                      /* empty reply: alive */
    if (code == INTERFACE_TRANSACTION) { pstr16(rep, "android.os.IServiceManager"); return; }
    if (code >> 24 == '_') return;                             /* other meta transactions: nothing */
    /* request: strict-mode policy, work source, 'SYST', interface token, then the arguments */
    str16_at(req, n, 12, iface, sizeof iface);
    if (n >= 16) { memcpy(&ilen, req + 12, 4); ilen = 16 + ((2 * (ilen + 1) + 3) & ~3u); }
    str16_at(req, n, ilen, name, sizeof name);
    if (p->trace) fprintf(p->trace, "[binder] servicemanager call %u (%s) \"%s\": no such service\n", code, iface, name);
    p32(rep, 0);                                               /* Status: no exception */
    switch (code) {
    case 1: case 2: pnullbinder(rep); break;                   /* getService / checkService: null */
    case 3: case 5: case 6: break;                             /* addService, (un)registerForNotifications */
    case 7: p32(rep, 0); break;                                /* isDeclared: false */
    case 4: case 8: case 10: p32(rep, 0); break;               /* listServices, getDeclaredInstances, getUpdatableNames: [] */
    case 9: p32(rep, 0xffffffffu); break;                      /* updatableViaApex: null string */
    case 11: p32(rep, 0); break;                               /* getConnectionInfo: null parcelable */
    default: break;
    }
}

/* ---------- the driver ---------- */

static uint64_t write_read(struct aoi_proc *p, struct aoi_binder *b, uint64_t arg, int *block)
{
    uint64_t bwr[6];                    /* write_size, write_consumed, write_buffer, read_size, read_consumed, read_buffer */
    struct bthread *t = bthread(b, p->th[p->cur].tid);
    if (!t) return (uint64_t)-EINVAL_;
    if (!gread(p, arg, bwr, sizeof bwr)) return (uint64_t)-EFAULT_;

    while (bwr[1] + 4 <= bwr[0]) {
        uint32_t cmd, sz;
        uint8_t pay[128];
        if (!gread(p, bwr[2] + bwr[1], &cmd, 4)) return (uint64_t)-EFAULT_;
        sz = BC_SIZE(cmd);
        if (sz > sizeof pay || bwr[1] + 4 + sz > bwr[0] || !gread(p, bwr[2] + bwr[1] + 4, pay, sz))
            return (uint64_t)-EINVAL_;
        bwr[1] += 4 + sz;
        if (cmd == BC_ENTER_LOOPER || cmd == BC_REGISTER_LOOPER) t->looper = 1;
        else if (cmd == BC_EXIT_LOOPER) t->looper = 0;
        else if (cmd == BC_TRANSACTION || cmd == BC_TRANSACTION_SG) {
            uint32_t handle, code, flags;
            uint64_t dsize, dptr;
            memcpy(&handle, pay, 4); memcpy(&code, pay + 16, 4); memcpy(&flags, pay + 20, 4);
            memcpy(&dsize, pay + 32, 8); memcpy(&dptr, pay + 48, 8);
            push32(t, BR_TRANSACTION_COMPLETE);
            if (flags & TF_ONE_WAY) continue;
            if (handle != 0 || !b->buf) {                      /* nobody else exists */
                if (p->trace) fprintf(p->trace, "[binder] call %#x to handle %u: dead\n", code, handle);
                push32(t, BR_DEAD_REPLY);
                continue;
            }
            {
                static uint8_t req[4096];
                struct parcel rep;
                uint64_t at, tr[8];
                uint32_t rn = dsize < sizeof req ? (uint32_t)dsize : sizeof req;
                if (!gread(p, dptr, req, rn)) rn = 0;
                servicemanager(p, code, req, rn, &rep);
                if (b->next + rep.n + 8 > b->buflen) b->next = 0;
                at = b->buf + b->next;
                b->next += (rep.n + 15) & ~7u;
                if (!gwrite(p, at, rep.d, rep.n)) { push32(t, BR_DEAD_REPLY); continue; }
                memset(tr, 0, sizeof tr);                      /* binder_transaction_data */
                tr[4] = rep.n;                                 /* data_size; offsets_size 0 */
                tr[6] = at; tr[7] = at + ((rep.n + 7) & ~7u);  /* buffer, offsets */
                push32(t, BR_REPLY);
                push(t, tr, sizeof tr);
            }
        }
        /* BC_REPLY, BC_FREE_BUFFER, reference counts, death notifications: nothing to do */
    }

    if (bwr[3] > bwr[4]) {
        if (!t->n) {
            if (!gwrite(p, arg, bwr, sizeof bwr)) return (uint64_t)-EFAULT_;
            *block = 1;                                        /* nothing to read: wait */
            return 0;
        }
        if (bwr[4] == 0 && bwr[3] >= 4 + t->n) {               /* the kernel starts a read with BR_NOOP */
            uint32_t noop = BR_NOOP;
            if (!gwrite(p, bwr[5], &noop, 4)) return (uint64_t)-EFAULT_;
            bwr[4] = 4;
        }
        {
            uint32_t k = bwr[3] - bwr[4] < t->n ? (uint32_t)(bwr[3] - bwr[4]) & ~3u : t->n;
            if (!gwrite(p, bwr[5] + bwr[4], t->q, k)) return (uint64_t)-EFAULT_;
            memmove(t->q, t->q + k, t->n - k);
            t->n -= k;
            bwr[4] += k;
        }
    }
    return gwrite(p, arg, bwr, sizeof bwr) ? 0 : (uint64_t)-EFAULT_;
}

uint64_t aoi_binder_ioctl(struct aoi_proc *p, uint64_t cmd, uint64_t arg, int *block)
{
    struct aoi_binder *b = state(p);
    *block = 0;
    if (!b) return (uint64_t)-EINVAL_;
    switch ((uint32_t)cmd) {
    case BINDER_WRITE_READ: return write_read(p, b, arg, block);
    case BINDER_VERSION: {
        int32_t v = 8;                                         /* BINDER_CURRENT_PROTOCOL_VERSION, 64-bit */
        return gwrite(p, arg, &v, 4) ? 0 : (uint64_t)-EFAULT_;
    }
    case BINDER_GET_EXTENDED_ERROR: {
        uint32_t z[3] = { 0, 0, 0 };
        return gwrite(p, arg, z, sizeof z) ? 0 : (uint64_t)-EFAULT_;
    }
    case BINDER_SET_MAX_THREADS: case BINDER_SET_CONTEXT_MGR: case BINDER_ENABLE_ONEWAY_SPAM_DETECTION:
        return 0;
    case BINDER_THREAD_EXIT: {
        struct bthread *t = bthread(b, p->th[p->cur].tid);
        if (t) t->tid = 0;
        return 0;
    }
    default:
        if (p->trace) fprintf(p->trace, "[binder] ioctl %#llx not implemented\n", (unsigned long long)cmd);
        return (uint64_t)-EINVAL_;
    }
}

void aoi_binder_mapped(struct aoi_proc *p, uint64_t addr, uint64_t len)
{
    struct aoi_binder *b = state(p);
    if (b) { b->buf = addr; b->buflen = len; b->next = 0; }
}

void aoi_binder_free(struct aoi_proc *p)
{
    free(p->binder);
    p->binder = NULL;
}
