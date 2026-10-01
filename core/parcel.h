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
};

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
