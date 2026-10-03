/* An in-process stand-in for the binder driver (/dev/binder, hwbinder, vndbinder).
 *
 * libbinder talks to the kernel through BINDER_WRITE_READ: a write buffer of BC_*
 * commands, a read buffer it wants filled with BR_* returns. Here there is one
 * process and no other. Its peers:
 *   - handle 0, the context manager: a built-in servicemanager. A service the app
 *     process registers (our Java services, java/src/aoi) is handed back as the very
 *     same local object, so those calls never come through here.
 *   - native objects behind handles 1, 2, ... (aoi_binder_native): services written
 *     in C on the host side, such as SurfaceFlinger (core/sf.c). A transaction to one
 *     calls its function with the request parcel and sends back the reply it builds.
 * Replies are parcels written into the receive buffer the guest mmapped on its
 * binder fd.
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
#define BR_TRANSACTION      0x80407202u
#define BR_REPLY            0x80407203u
#define BR_DEAD_REPLY       0x00007205u
#define BR_TRANSACTION_COMPLETE 0x00007206u
#define BR_NOOP             0x0000720cu
#define BR_INCREFS          0x80107207u                        /* {ptr, cookie}: the owner takes a weak ref */
#define BR_ACQUIRE          0x80107208u                        /* {ptr, cookie}: ... and a strong one */
#define TF_ONE_WAY          0x01u
#define TF_STATUS_CODE      0x08u

#define BINDER_WRITE_READ   0xc0306201u
#define BINDER_SET_MAX_THREADS 0x40046205u
#define BINDER_SET_CONTEXT_MGR 0x40046207u
#define BINDER_THREAD_EXIT  0x40046208u
#define BINDER_VERSION      0xc0046209u
#define BINDER_ENABLE_ONEWAY_SPAM_DETECTION 0x40046210u
#define BINDER_GET_EXTENDED_ERROR 0xc00c6211u

#define PING_TRANSACTION    0x5f504e47u                        /* '_PNG' */
#define INTERFACE_TRANSACTION 0x5f4e5446u                      /* '_NTF' */
#define UNKNOWN_TRANSACTION (-74)                              /* -EBADMSG, libbinder's status */

#define EINVAL_ 22
#define EFAULT_ 14
#define NATIVES 256

/* Per guest thread: BR_* words waiting to be read, and whether it is a looper. */
struct bthread { int tid, looper; uint32_t n; uint8_t q[1024]; };

/* A registered service: a local object of the guest (type BINDER: its pointer and
 * cookie, handed back as is) or one of our native handles (type HANDLE). */
struct service { char name[96]; uint32_t type, flags; uint64_t binder, cookie; int32_t stability; };

struct native { aoi_native_fn fn; void *self; const char *iface; };

struct aoi_binder {
    uint64_t buf, buflen, next;     /* the guest's receive mapping, bump allocator in it */
    struct bthread th[AOI_PROC_THREADS];
    struct service svc[512];        /* ~210 are aoi.Services stand-ins: 128 silently dropped the last */
    int nsvc;
    struct native nat[NATIVES];     /* handle h is nat[h]; 0 is servicemanager */
    int nnat;
    uint32_t pn;                    /* the process's work: host calls (BR_TRANSACTION + its data) */
    uint8_t pq[64 * 68];            /* waiting for a looper that waits for work */
};

static struct aoi_binder *state(struct aoi_proc *p)
{
    if (!p->binder && (p->binder = calloc(1, sizeof *p->binder))) {
        p->binder->nnat = 1;
        aoi_sf_init(p);                                        /* native services exist from the start */
        aoi_gralloc_init(p);
    }
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

/* ---------- native objects ---------- */

uint32_t aoi_binder_native(struct aoi_proc *p, const char *name, const char *iface, aoi_native_fn fn, void *self)
{
    struct aoi_binder *b = p->binder;
    uint32_t h;
    if (!b || b->nnat == NATIVES) return 0;
    h = (uint32_t)b->nnat++;
    b->nat[h].fn = fn; b->nat[h].self = self; b->nat[h].iface = iface;
    if (name && b->nsvc < (int)(sizeof b->svc / sizeof b->svc[0])) {
        struct service *sv = &b->svc[b->nsvc++];
        memset(sv, 0, sizeof *sv);
        snprintf(sv->name, sizeof sv->name, "%s", name);
        sv->type = AOI_BINDER_TYPE_HANDLE;
        sv->binder = h;
        sv->stability = AOI_STABILITY_SYSTEM;
    }
    return h;
}

/* ---------- servicemanager (handle 0) ---------- */

static struct service *find(struct aoi_binder *b, const char *name)
{
    int i;
    for (i = 0; i < b->nsvc; i++) if (!strcmp(b->svc[i].name, name)) return &b->svc[i];
    return NULL;
}

/* The reply to `code`; the request's first object (if any) sits at byte offset obj0. */
static void servicemanager(struct aoi_proc *p, struct aoi_binder *b, uint32_t code, struct aoi_reader *r,
                           int64_t obj0, struct aoi_parcel *rep)
{
    char iface[96], name[128];
    struct service *sv;
    if (code == PING_TRANSACTION) return;                      /* empty reply: alive */
    if (code == INTERFACE_TRANSACTION) { aoi_pstr16(rep, "android.os.IServiceManager"); return; }
    if (code >> 24 == '_') return;                             /* other meta transactions: nothing */
    aoi_rtoken(r, iface, sizeof iface);
    aoi_rstr16(r, name, sizeof name);
    sv = find(b, name);
    if (code == 3 && obj0 >= 0 && obj0 + 28 <= (int64_t)r->n) { /* addService(name, binder, ...) */
        const uint8_t *o = r->d + obj0;
        if (!sv && b->nsvc < (int)(sizeof b->svc / sizeof b->svc[0])) sv = &b->svc[b->nsvc++];
        if (!sv && p->log) fprintf(p->log, "I/aoi: servicemanager full: \"%s\" not registered\n", name);
        if (sv) {
            snprintf(sv->name, sizeof sv->name, "%s", name);
            memcpy(&sv->type, o, 4); memcpy(&sv->flags, o + 4, 4);
            memcpy(&sv->binder, o + 8, 8); memcpy(&sv->cookie, o + 16, 8);
            memcpy(&sv->stability, o + 24, 4);
            /* the kernel holds a node it gives out: weak + strong refs on the owner's
             * object, which keep e.g. a Java service's native JavaBBinder alive */
            if (sv->type == AOI_BINDER_TYPE_BINDER) { rep->hold[0] = sv->binder; rep->hold[1] = sv->cookie; }
        }
    }
    if (p->trace)
        fprintf(p->trace, "[binder] servicemanager call %u (%s) \"%s\"%s\n", code, iface, name,
                code == 3 ? ": registered" : (code == 1 || code == 2) ? (sv ? ": found" : ": no such service") : "");
    aoi_p32(rep, 0);                                           /* Status: no exception */
    switch (code) {
    case 1: case 2:                                            /* getService / checkService */
        if (!sv) { aoi_phandle(rep, 0); break; }
        if (sv->type == AOI_BINDER_TYPE_HANDLE) { aoi_phandle(rep, (uint32_t)sv->binder); break; }
        rep->obj[rep->nobj++] = rep->n;                        /* the guest's own object, as it gave it */
        aoi_p32(rep, sv->type); aoi_p32(rep, sv->flags); aoi_p64(rep, sv->binder); aoi_p64(rep, sv->cookie);
        aoi_p32(rep, (uint32_t)sv->stability);
        break;
    case 3: case 5: case 6: break;                             /* addService, (un)registerForNotifications */
    case 7: aoi_p32(rep, sv != NULL); break;                   /* isDeclared: the services we have */
    case 4: case 8: case 10: aoi_p32(rep, 0); break;           /* listServices, getDeclaredInstances, getUpdatableNames: [] */
    case 9: aoi_p32(rep, 0xffffffffu); break;                  /* updatableViaApex: null string */
    case 11: aoi_p32(rep, 0); break;                           /* getConnectionInfo: null parcelable */
    default: break;
    }
}

/* ---------- the driver ---------- */

/* Puts the reply parcel into the receive buffer and queues BR_REPLY for thread t. */
static void reply(struct aoi_proc *p, struct aoi_binder *b, struct bthread *t, struct aoi_parcel *rep)
{
    uint64_t at, tr[8];
    uint32_t dn, on, flags = 0;
    if (rep->status) {                                         /* TF_STATUS_CODE: the data is the status */
        rep->n = 0; rep->nobj = 0;
        aoi_p32(rep, (uint32_t)rep->status);
        flags = TF_STATUS_CODE;
    }
    dn = (rep->n + 7) & ~7u; on = 8u * (uint32_t)rep->nobj;
    if (b->next + dn + on + 8 > b->buflen) b->next = 0;
    at = b->buf + b->next;
    b->next += dn + on + 8;
    /* the receive buffer is read-only to the guest; the driver writes it anyway */
    if (!aoi_vm_write(&p->vm, at, rep->d, rep->n, 0) || (on && !aoi_vm_write(&p->vm, at + dn, rep->obj, on, 0))) {
        push32(t, BR_DEAD_REPLY);
        return;
    }
    if (rep->hold[1]) {
        push32(t, BR_INCREFS); push(t, rep->hold, 16);
        push32(t, BR_ACQUIRE); push(t, rep->hold, 16);
    }
    memset(tr, 0, sizeof tr);                                  /* binder_transaction_data */
    tr[2] = (uint64_t)flags << 32;                             /* code 0, flags */
    tr[4] = rep->n;                                            /* data_size */
    tr[5] = on;                                                /* offsets_size */
    tr[6] = at; tr[7] = at + dn;                               /* buffer, offsets */
    push32(t, BR_REPLY);
    push(t, tr, sizeof tr);
}

/* A one-way call from the host to a local object of the guest (ptr, cookie as its
 * flat_binder_object gave them): queued as BR_TRANSACTION in the process's work,
 * which a looper thread takes when it reads with nothing of its own (write_read).
 * As in the kernel, never a thread inside a call of its own: one waiting for
 * SurfaceFlinger's reply while holding libgui's BufferCache lock would run
 * onReleaseBuffer nested, wanting BLASTBufferQueue's lock, held by RenderThread
 * waiting for the BufferCache lock (a deadlock). 0, or -1 if no looper or no room. */
int aoi_binder_send(struct aoi_proc *p, uint64_t ptr, uint64_t cookie, uint32_t code, const struct aoi_parcel *data)
{
    struct aoi_binder *b = p->binder;
    struct bthread *t = NULL;
    uint64_t at, tr[8];
    uint32_t dn, on;
    int i;
    if (!b || !b->buf || !ptr) return -1;
    for (i = 0; i < AOI_PROC_THREADS; i++)
        if (b->th[i].tid && b->th[i].looper) { t = &b->th[i]; break; }
    if (!t || b->pn + 4 + 64 > sizeof b->pq) return -1;
    dn = (data->n + 7) & ~7u; on = 8u * (uint32_t)data->nobj;
    if (b->next + dn + on + 8 > b->buflen) b->next = 0;
    at = b->buf + b->next;
    b->next += dn + on + 8;
    if (!aoi_vm_write(&p->vm, at, data->d, data->n, 0) || (on && !aoi_vm_write(&p->vm, at + dn, data->obj, on, 0)))
        return -1;
    memset(tr, 0, sizeof tr);
    tr[0] = ptr; tr[1] = cookie;
    tr[2] = code | (uint64_t)TF_ONE_WAY << 32;                 /* code, flags */
    tr[4] = data->n; tr[5] = on;
    tr[6] = at; tr[7] = at + dn;
    dn = BR_TRANSACTION;
    memcpy(b->pq + b->pn, &dn, 4);
    memcpy(b->pq + b->pn + 4, tr, sizeof tr);
    b->pn += 4 + sizeof tr;
    if (p->trace) fprintf(p->trace, "[binder] host call %u to %#llx queued\n", code, (unsigned long long)ptr);
    return 0;
}

static void transaction(struct aoi_proc *p, struct aoi_binder *b, struct bthread *t, const uint8_t *pay)
{
    static _Thread_local uint8_t req[8192];
    static _Thread_local struct aoi_parcel rep;
    uint32_t handle, code, flags, rn;
    uint64_t dsize, dptr, osize, optr, o0 = ~0ULL;
    struct aoi_reader r;
    memcpy(&handle, pay, 4); memcpy(&code, pay + 16, 4); memcpy(&flags, pay + 20, 4);
    memcpy(&dsize, pay + 32, 8); memcpy(&osize, pay + 40, 8); memcpy(&dptr, pay + 48, 8); memcpy(&optr, pay + 56, 8);
    if (osize >= 8 && !gread(p, optr, &o0, 8)) o0 = ~0ULL;
    push32(t, BR_TRANSACTION_COMPLETE);
    rn = dsize < sizeof req ? (uint32_t)dsize : sizeof req;
    if (!gread(p, dptr, req, rn)) rn = 0;
    r.d = req; r.n = rn; r.pos = 0;
    memset(&rep, 0, sizeof rep);
    if (handle == 0) servicemanager(p, b, code, &r, o0 == ~0ULL ? -1 : (int64_t)o0, &rep);
    else if (handle < (uint32_t)b->nnat && b->nat[handle].fn) {
        if (code == PING_TRANSACTION) {}                       /* alive: empty reply */
        else if (code == INTERFACE_TRANSACTION) aoi_pstr16(&rep, b->nat[handle].iface);
        else if (code >> 24 == '_') {}
        else {
            char iface[128];
            aoi_rtoken(&r, iface, sizeof iface);
            rep.status = UNKNOWN_TRANSACTION;                  /* unless the service answers */
            b->nat[handle].fn(p, b->nat[handle].self, code, &r, &rep);
        }
    } else {
        if (p->trace) fprintf(p->trace, "[binder] call %#x to handle %u: dead\n", code, handle);
        if (!(flags & TF_ONE_WAY)) push32(t, BR_DEAD_REPLY);
        return;
    }
    if (flags & TF_ONE_WAY) return;
    if (!b->buf) { push32(t, BR_DEAD_REPLY); return; }
    reply(p, b, t, &rep);
}

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
        else if (cmd == BC_TRANSACTION || cmd == BC_TRANSACTION_SG) transaction(p, b, t, pay);
        /* BC_REPLY, BC_FREE_BUFFER, reference counts, death notifications: nothing to do */
    }

    if (bwr[3] > bwr[4]) {
        if (!t->n && t->looper && b->pn) {                     /* free for the process's work */
            push(t, b->pq, 4 + 64);
            memmove(b->pq, b->pq + 4 + 64, b->pn - 4 - 64);
            b->pn -= 4 + 64;
        }
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
            if (p->trace) {
                uint32_t o, w;
                fprintf(p->trace, "[binder] tid %d reads %u bytes:", t->tid, k);
                for (o = 0; o + 4 <= k && o < 80; o += 4) { memcpy(&w, t->q + o, 4); fprintf(p->trace, " %x", w); }
                fprintf(p->trace, "\n");
            }
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
    aoi_sf_free(p);
    aoi_gralloc_free(p);
    free(p->binder);
    p->binder = NULL;
}

/* ---------- snapshots (core/snap.c) ---------- */

/* The driver state as is, its native objects as (kind, index) of the service that
 * owns them: after gralloc and SurfaceFlinger are loaded. */
int aoi_binder_snap(struct aoi_proc *p, FILE *f, int save)
{
    struct aoi_binder *b;
    int32_t ki[2];
    int h;
    uint8_t has;
    if (save) {
        has = p->binder != NULL;
        if (fwrite(&has, 1, 1, f) != 1) return -1;
        if (!has) return 0;
        b = p->binder;
        if (fwrite(b, sizeof *b, 1, f) != 1) return -1;
        for (h = 1; h < b->nnat; h++) {
            if (aoi_sf_native_id(p, b->nat[h].self, b->nat[h].fn, &ki[0], &ki[1]) &&
                aoi_gralloc_native_id(p, b->nat[h].self, b->nat[h].fn, &ki[0], &ki[1])) return -1;
            if (fwrite(ki, sizeof ki, 1, f) != 1) return -1;
        }
        return 0;
    }
    if (fread(&has, 1, 1, f) != 1) return -1;
    if (!has) return 0;
    if (!(b = calloc(1, sizeof *b))) return -1;
    p->binder = b;
    if (fread(b, sizeof *b, 1, f) != 1 || b->nnat < 1 || b->nnat > NATIVES) return -1;
    memset(&b->nat[0], 0, sizeof b->nat[0]);
    for (h = 1; h < b->nnat; h++) {
        struct native *n = &b->nat[h];
        if (fread(ki, sizeof ki, 1, f) != 1) return -1;
        if (aoi_sf_native_ref(p, ki[0], ki[1], &n->fn, &n->self, &n->iface) &&
            aoi_gralloc_native_ref(p, ki[0], ki[1], &n->fn, &n->self, &n->iface)) return -1;
    }
    return 0;
}
