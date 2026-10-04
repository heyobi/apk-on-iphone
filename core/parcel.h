/* Binder parcels as libbinder lays them out (little-endian, 4-byte aligned), for
 * the native services behind in-process binder handles (core/binder.c, core/sf.c).
 * Internal. */
#ifndef AOI_PARCEL_H
#define AOI_PARCEL_H

#include <stdint.h>
#include <string.h>

#define AOI_BINDER_TYPE_BINDER 0x73622a85u      /* 's','b','*': a local object */
#define AOI_BINDER_TYPE_HANDLE 0x73682a85u      /* 's','h','*': a reference (handle) */
#define AOI_BINDER_TYPE_FD     0x66642a85u      /* 'f','d','*': a file descriptor */
#define AOI_STABILITY_SYSTEM   0x0c             /* libbinder Stability::Level::SYSTEM */

/* A reply being built: data, the offsets of the objects in it, or just a status. */
struct aoi_parcel {
    uint8_t d[4096];
    uint32_t n;
    uint64_t obj[16];
    int nobj;
    int32_t status;                 /* != 0: no data, the transaction failed with this (TF_STATUS_CODE) */
    uint64_t hold[2];               /* a local object the driver now references (BR_INCREFS/ACQUIRE) */
    /* HIDL (hwbinder): buffers that travel with the data (binder_buffer_object): the
     * driver copies each next to the reply and points its object at the copy, and a
     * child's parent at it (aoi_pbuffer). */
    struct { uint8_t d[512]; uint32_t len, obj, obj_index, parent_obj, parent_off; int has_parent; } buf[12];
    int nbuf;
};

#define AOI_BINDER_TYPE_PTR    0x70742a85u      /* 'p','t','*': a buffer (HIDL) */

static inline void aoi_p32(struct aoi_parcel *pc, uint32_t v)
{
    if (pc->n + 4 <= sizeof pc->d) { memcpy(pc->d + pc->n, &v, 4); pc->n += 4; }
}

static inline void aoi_p64(struct aoi_parcel *pc, uint64_t v) { aoi_p32(pc, (uint32_t)v); aoi_p32(pc, (uint32_t)(v >> 32)); }

static inline void aoi_pf32(struct aoi_parcel *pc, float f) { uint32_t v; memcpy(&v, &f, 4); aoi_p32(pc, v); }

/* String16: length, UTF-16 units, NUL, padded to 4 bytes (ASCII only here). */
static inline void aoi_pstr16(struct aoi_parcel *pc, const char *s)
{
    uint32_t i, len = (uint32_t)strlen(s);
    aoi_p32(pc, len);
    for (i = 0; i <= len; i += 2)
        aoi_p32(pc, (uint32_t)(uint8_t)s[i] | (i + 1 <= len ? (uint32_t)(uint8_t)s[i + 1] << 16 : 0));
}

/* writeStrongBinder of a reference to native object `handle` (0: null). */
static inline void aoi_phandle(struct aoi_parcel *pc, uint32_t handle)
{
    if (!handle) { aoi_p32(pc, AOI_BINDER_TYPE_BINDER); aoi_p32(pc, 0); aoi_p64(pc, 0); aoi_p64(pc, 0); aoi_p32(pc, 0); return; }
    if (pc->nobj < 16) pc->obj[pc->nobj++] = pc->n;
    aoi_p32(pc, AOI_BINDER_TYPE_HANDLE); aoi_p32(pc, 0); aoi_p64(pc, handle); aoi_p64(pc, 0);
    aoi_p32(pc, AOI_STABILITY_SYSTEM);
}

/* writeFileDescriptor / a ParcelFileDescriptor's fd: the guest fd number itself
 * (one process: the receiver's fd is the sender's). */
static inline void aoi_pfd(struct aoi_parcel *pc, int fd)
{
    if (pc->nobj < 16) pc->obj[pc->nobj++] = pc->n;
    aoi_p32(pc, AOI_BINDER_TYPE_FD); aoi_p32(pc, 0x17f); aoi_p64(pc, (uint32_t)fd); aoi_p64(pc, 0);
}

/* A HIDL buffer (binder_buffer_object) of len bytes; parent: the index (as returned)
 * of the buffer holding the pointer to this one at parent_off, or -1. Its index. */
static inline int aoi_pbuffer(struct aoi_parcel *pc, const void *d, uint32_t len, int parent, uint32_t parent_off)
{
    int k = pc->nbuf;
    if (k >= 12 || len > sizeof pc->buf[0].d || pc->nobj >= 16) return -1;
    memcpy(pc->buf[k].d, d, len);
    pc->buf[k].len = len;
    pc->buf[k].has_parent = parent >= 0;
    pc->buf[k].parent_obj = parent >= 0 ? (uint32_t)parent : 0;
    pc->buf[k].parent_off = parent_off;
    pc->buf[k].obj = pc->n;
    pc->obj[pc->nobj++] = pc->n;
    aoi_p32(pc, AOI_BINDER_TYPE_PTR); aoi_p32(pc, parent >= 0 ? 1 : 0);      /* BINDER_BUFFER_FLAG_HAS_PARENT */
    aoi_p64(pc, 0); aoi_p64(pc, len);                  /* buffer: set by the driver */
    aoi_p64(pc, parent >= 0 ? (uint64_t)pc->buf[parent].obj_index : 0);
    aoi_p64(pc, parent_off);
    pc->buf[k].obj_index = (uint32_t)(pc->nobj - 1);
    pc->nbuf++;
    return k;
}

/* HIDL hidl_vec<hidl_string> (a vec, its array, each string's bytes). */
static inline void aoi_phidl_strings(struct aoi_parcel *pc, const char *const *s, uint32_t n)
{
    uint8_t vec[16], arr[16 * 8];
    uint32_t i, len;
    int v, a;
    memset(vec, 0, sizeof vec); memset(arr, 0, sizeof arr);
    if (n > 8) n = 8;
    memcpy(vec + 8, &n, 4);
    v = aoi_pbuffer(pc, vec, 16, -1, 0);
    for (i = 0; i < n; i++) { len = (uint32_t)strlen(s[i]); memcpy(arr + 16 * i + 8, &len, 4); }
    a = aoi_pbuffer(pc, arr, 16 * n, v, 0);
    for (i = 0; i < n; i++) aoi_pbuffer(pc, s[i], (uint32_t)strlen(s[i]) + 1, a, 16 * i);
}

/* Reading a request. */
struct aoi_reader { const uint8_t *d; uint32_t n, pos; };

static inline uint32_t aoi_r32(struct aoi_reader *r)
{
    uint32_t v = 0;
    if (r->pos + 4 <= r->n) memcpy(&v, r->d + r->pos, 4);
    r->pos += 4;
    return v;
}

static inline uint64_t aoi_r64(struct aoi_reader *r) { uint64_t lo = aoi_r32(r); return lo | (uint64_t)aoi_r32(r) << 32; }

/* A String16 as ASCII into out (cap bytes); skips it in the stream. */
static inline void aoi_rstr16(struct aoi_reader *r, char *out, size_t cap)
{
    uint32_t len = aoi_r32(r), i;
    if (cap) out[0] = 0;
    if (len == 0xffffffffu) return;
    for (i = 0; i < len && r->pos + 2 * i + 2 <= r->n; i++) if (i + 1 < cap) { out[i] = (char)r->d[r->pos + 2 * i]; out[i + 1] = 0; }
    r->pos += (2 * (len + 1) + 3) & ~3u;
}

/* The interface token every AIDL call starts with: strict-mode policy, work source,
 * 'SYST', the descriptor. */
static inline void aoi_rtoken(struct aoi_reader *r, char *iface, size_t cap)
{
    aoi_r32(r); aoi_r32(r); aoi_r32(r);
    aoi_rstr16(r, iface, cap);
}

#endif
