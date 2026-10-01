/* mapper.aoi.so: the stable-C AIMapper v5 that libui's Gralloc5 loads (from
 * /vendor/lib64/hw, the sphal namespace) after core/gralloc.c named the suffix "aoi".
 * Guest code, freestanding: no libc, raw syscalls.
 *
 * A buffer's native_handle says everything (core/gralloc.h): its pixels are guest
 * memory already mapped in this one process, so lock returns their address. Import
 * copies the handle and takes a reference on the host (AOI_SYS_GRALLOC); free drops
 * it. Metadata values use the encoding of IMapperMetadataTypes.h: the type
 * (name as int64 length + bytes, then the int64 value), then the value. */
#include "../core/gralloc.h"

typedef unsigned long size_t;
typedef long int64_t;
typedef unsigned long uint64_t;
typedef int int32_t;
typedef unsigned int uint32_t;

#define EXPORT __attribute__((visibility("default")))

enum { NONE = 0, BAD_BUFFER = 2, BAD_VALUE = 3, NO_RESOURCES = 5, UNSUPPORTED = 7 };

typedef struct { int version, numFds, numInts; int data[]; } native_handle_t;
typedef const native_handle_t *buffer_handle_t;
typedef struct { const char *name; int64_t value; } AIMapper_MetadataType;
typedef struct { int32_t left, top, right, bottom; } ARect;
typedef struct {
    AIMapper_MetadataType metadataType;
    const char *description;
    _Bool isGettable, isSettable;
    unsigned char reserved[32];
} AIMapper_MetadataTypeDescription;
typedef void (*dump_fn)(void *, AIMapper_MetadataType, const void *, size_t);
typedef void (*begin_fn)(void *);

typedef struct {
    int (*importBuffer)(const native_handle_t *, buffer_handle_t *);
    int (*freeBuffer)(buffer_handle_t);
    int (*getTransportSize)(buffer_handle_t, uint32_t *, uint32_t *);
    int (*lock)(buffer_handle_t, uint64_t, ARect, int, void **);
    int (*unlock)(buffer_handle_t, int *);
    int (*flushLockedBuffer)(buffer_handle_t);
    int (*rereadLockedBuffer)(buffer_handle_t);
    int32_t (*getMetadata)(buffer_handle_t, AIMapper_MetadataType, void *, size_t);
    int32_t (*getStandardMetadata)(buffer_handle_t, int64_t, void *, size_t);
    int (*setMetadata)(buffer_handle_t, AIMapper_MetadataType, const void *, size_t);
    int (*setStandardMetadata)(buffer_handle_t, int64_t, const void *, size_t);
    int (*listSupportedMetadataTypes)(const AIMapper_MetadataTypeDescription **, size_t *);
    int (*dumpBuffer)(buffer_handle_t, dump_fn, void *);
    int (*dumpAllBuffers)(begin_fn, dump_fn, void *);
    int (*getReservedRegion)(buffer_handle_t, void **, uint64_t *);
} AIMapperV5;

typedef struct { _Alignas(16) uint32_t version; AIMapperV5 v5; } AIMapper;

EXPORT const uint32_t ANDROID_HAL_STABLEC_VERSION = 5;
EXPORT const uint32_t ANDROID_HAL_MAPPER_VERSION = 5;

/* ---------- freestanding bits ---------- */

static long sys(long nr, long a0, long a1)
{
    register long x8 __asm__("x8") = nr, x0 __asm__("x0") = a0, x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}

#define NR_DUP 23
#define NR_CLOSE 57

static int streq(const char *a, const char *b)
{
    if (!a) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* ---------- imported buffers ---------- */

#define SLOTS 512
struct slot { int version, numFds, numInts, fd; int ints[AOI_GB_INTS]; };   /* a native_handle_t */
static struct slot slots[SLOTS];
static unsigned char used[SLOTS];

static int valid(const native_handle_t *h)
{
    return h && h->numFds == 1 && h->numInts == AOI_GB_INTS && (uint32_t)h->data[1 + AOI_GB_I_MAGIC] == AOI_GB_MAGIC;
}

/* An imported handle: one of ours. */
static const int *ints(buffer_handle_t h)
{
    const struct slot *s = (const struct slot *)h;
    if (s < slots || s >= slots + SLOTS || !used[s - slots]) return 0;
    return s->ints;
}

static unsigned char *base(const int *v) { return (unsigned char *)((uint64_t)(uint32_t)v[AOI_GB_I_ADDR_LO] | (uint64_t)(uint32_t)v[AOI_GB_I_ADDR_HI] << 32); }

static struct aoi_gb_meta *meta(const int *v)
{
    uint64_t off = ((uint64_t)(uint32_t)v[AOI_GB_I_SIZE] + 4095) & ~4095UL;
    return (struct aoi_gb_meta *)(base(v) + off);
}

static int importBuffer(const native_handle_t *h, buffer_handle_t *out)
{
    int k, i, fd;
    if (!valid(h) || !out) return BAD_BUFFER;
    for (k = 0; k < SLOTS && used[k]; k++) {}
    if (k == SLOTS) return NO_RESOURCES;
    if (sys(AOI_SYS_GRALLOC, AOI_GB_RETAIN, h->data[1 + AOI_GB_I_ID]) != 0) return BAD_BUFFER;
    fd = (int)sys(NR_DUP, h->data[0], 0);
    if (fd < 0) { sys(AOI_SYS_GRALLOC, AOI_GB_RELEASE, h->data[1 + AOI_GB_I_ID]); return NO_RESOURCES; }
    used[k] = 1;
    slots[k].version = (int)sizeof(native_handle_t);
    slots[k].numFds = 1; slots[k].numInts = AOI_GB_INTS;
    slots[k].fd = fd;
    for (i = 0; i < AOI_GB_INTS; i++) slots[k].ints[i] = h->data[1 + i];
    *out = (buffer_handle_t)&slots[k];
    return NONE;
}

static int freeBuffer(buffer_handle_t h)
{
    const int *v = ints(h);
    struct slot *s = (struct slot *)h;
    if (!v) return BAD_BUFFER;
    sys(NR_CLOSE, s->fd, 0);
    sys(AOI_SYS_GRALLOC, AOI_GB_RELEASE, v[AOI_GB_I_ID]);
    used[s - slots] = 0;
    return NONE;
}

static int getTransportSize(buffer_handle_t h, uint32_t *fds, uint32_t *n)
{
    if (!ints(h)) return BAD_BUFFER;
    *fds = 1; *n = AOI_GB_INTS;
    return NONE;
}

static void close_fence(int fence) { if (fence >= 0) sys(NR_CLOSE, fence, 0); }   /* CPU writes are done */

static int lock(buffer_handle_t h, uint64_t usage, ARect r, int fence, void **out)
{
    const int *v = ints(h);
    (void)usage; (void)r;
    close_fence(fence);
    if (!v) return BAD_BUFFER;
    *out = base(v);
    return NONE;
}

static int unlock(buffer_handle_t h, int *fence)
{
    if (!ints(h)) return BAD_BUFFER;
    *fence = -1;
    return NONE;
}

static int validate(buffer_handle_t h) { return ints(h) ? NONE : BAD_BUFFER; }

/* ---------- metadata ---------- */

static const char STANDARD[] = "android.hardware.graphics.common.StandardMetadataType";

struct writer { unsigned char *d; size_t cap, n; };

static void put(struct writer *w, const void *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++, w->n++) if (w->n < w->cap) w->d[w->n] = ((const unsigned char *)s)[i];
}
static void p32(struct writer *w, int32_t v) { put(w, &v, 4); }
static void p64(struct writer *w, int64_t v) { put(w, &v, 8); }
static void pstr(struct writer *w, const char *s, size_t n) { p64(w, (int64_t)n); put(w, s, n); }
static void header(struct writer *w, int64_t t) { pstr(w, STANDARD, slen(STANDARD)); p64(w, t); }
static void extendable(struct writer *w, const char *name, int64_t v) { pstr(w, name, slen(name)); p64(w, v); }

/* Components of the one plane: type (PlaneLayoutComponentType), offset, size in bits. */
static int components(int format, int64_t c[4][3])
{
    enum { R = 1 << 10, G = 1 << 11, B = 1 << 12, A = 1 << 30, RAW = 1 << 20 };
    int64_t t[4][3];
    int n = 0, i, j;
    switch (format) {
    case 1: case 0x22: n = 4; t[0][0] = R; t[1][0] = G; t[2][0] = B; t[3][0] = A; break;
    case 2: case 3: n = 3; t[0][0] = R; t[1][0] = G; t[2][0] = B; break;
    case 5: n = 4; t[0][0] = B; t[1][0] = G; t[2][0] = R; t[3][0] = A; break;
    case 4: n = 3; t[0][0] = B; t[0][1] = 0; t[0][2] = 5; t[1][0] = G; t[1][1] = 5; t[1][2] = 6;
        t[2][0] = R; t[2][1] = 11; t[2][2] = 5;
        for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) c[i][j] = t[i][j];
        return 3;
    case 0x16: n = 4; t[0][0] = R; t[1][0] = G; t[2][0] = B; t[3][0] = A;
        for (i = 0; i < 4; i++) { t[i][1] = 16 * i; t[i][2] = 16; c[i][0] = t[i][0]; c[i][1] = t[i][1]; c[i][2] = t[i][2]; }
        return 4;
    case 0x2b: n = 4; t[0][0] = R; t[1][0] = G; t[2][0] = B; t[3][0] = A;
        for (i = 0; i < 4; i++) { c[i][0] = t[i][0]; c[i][1] = 10 * i; c[i][2] = i == 3 ? 2 : 10; }
        return 4;
    case 0x38: n = 1; t[0][0] = R; break;
    default: n = 1; t[0][0] = RAW; break;
    }
    for (i = 0; i < n; i++) { c[i][0] = t[i][0]; c[i][1] = 8 * i; c[i][2] = 8; }
    return n;
}

static int32_t getStandardMetadata(buffer_handle_t h, int64_t t, void *dest, size_t cap)
{
    const int *v = ints(h);
    struct writer w;
    struct aoi_gb_meta *m;
    int64_t w64, h64, stride, bpp;
    if (!v) return -BAD_BUFFER;
    m = meta(v);
    w.d = dest; w.cap = dest ? cap : 0; w.n = 0;
    w64 = (uint32_t)v[AOI_GB_I_WIDTH]; h64 = (uint32_t)v[AOI_GB_I_HEIGHT];
    stride = (uint32_t)v[AOI_GB_I_STRIDE]; bpp = (uint32_t)v[AOI_GB_I_BPP];
    switch (t) {
    case 1: header(&w, t); p64(&w, (uint32_t)v[AOI_GB_I_ID]); break;                 /* BUFFER_ID */
    case 2: header(&w, t); pstr(&w, m->name, m->name_len < 128 ? m->name_len : 128); break;   /* NAME */
    case 3: header(&w, t); p64(&w, w64); break;                                        /* WIDTH */
    case 4: header(&w, t); p64(&w, h64); break;                                        /* HEIGHT */
    case 5: header(&w, t); p64(&w, 1); break;                                          /* LAYER_COUNT */
    case 6: header(&w, t); p32(&w, v[AOI_GB_I_FORMAT]); break;                         /* PIXEL_FORMAT_REQUESTED */
    case 7: {                                                                          /* PIXEL_FORMAT_FOURCC (drm) */
        int f = v[AOI_GB_I_FORMAT];
        uint32_t cc = f == 2 ? 0x34324258u : f == 5 ? 0x34325241u : f == 4 ? 0x36314752u : f == 3 ? 0x34324742u
                    : f == 0x16 ? 0x48344241u : f == 0x2b ? 0x30334241u : f == 0x38 ? 0x20203852u : 0x34324241u;
        header(&w, t); p32(&w, (int32_t)cc);                                           /* XB24 AR24 RG16 BG24 AB4H AB30 R8 AB24 */
        break;
    }
    case 8: header(&w, t); p64(&w, 0); break;                                          /* PIXEL_FORMAT_MODIFIER: linear */
    case 9: header(&w, t); p64(&w, (int64_t)((uint64_t)(uint32_t)v[AOI_GB_I_USAGE_LO] | (uint64_t)(uint32_t)v[AOI_GB_I_USAGE_HI] << 32)); break;
    case 10: header(&w, t); p64(&w, (uint32_t)v[AOI_GB_I_SIZE]); break;               /* ALLOCATION_SIZE */
    case 11: header(&w, t); p64(&w, 0); break;                                         /* PROTECTED_CONTENT */
    case 12: header(&w, t); extendable(&w, "android.hardware.graphics.common.Compression", 0); break;
    case 13: header(&w, t); extendable(&w, "android.hardware.graphics.common.Interlaced", 0); break;
    case 14: header(&w, t); extendable(&w, "android.hardware.graphics.common.ChromaSiting", 0); break;
    case 15: {                                                                         /* PLANE_LAYOUTS: one plane */
        int64_t c[4][3];
        int n = components(v[AOI_GB_I_FORMAT], c), i;
        header(&w, t);
        p64(&w, 1);
        p64(&w, n);
        for (i = 0; i < n; i++) {
            extendable(&w, "android.hardware.graphics.common.PlaneLayoutComponentType", c[i][0]);
            p64(&w, c[i][1]); p64(&w, c[i][2]);
        }
        p64(&w, 0);                                                                    /* offsetInBytes */
        p64(&w, bpp * 8);                                                              /* sampleIncrementInBits */
        p64(&w, stride * bpp);                                                         /* strideInBytes */
        p64(&w, w64); p64(&w, h64);                                                    /* width/heightInSamples */
        p64(&w, (uint32_t)v[AOI_GB_I_SIZE]);                                           /* totalSizeInBytes */
        p64(&w, 1); p64(&w, 1);                                                        /* subsampling */
        break;
    }
    case 16: header(&w, t); p64(&w, 1); p32(&w, 0); p32(&w, 0); p32(&w, (int32_t)w64); p32(&w, (int32_t)h64); break;   /* CROP */
    case 17: header(&w, t); p32(&w, m->dataspace); break;                              /* DATASPACE */
    case 18: header(&w, t); p32(&w, m->blend_mode); break;                             /* BLEND_MODE */
    case 19: case 20: case 21: case 22: return 0;                                     /* HDR metadata: absent */
    case 23: header(&w, t); p32(&w, (int32_t)stride); break;                           /* STRIDE */
    default: return -UNSUPPORTED;
    }
    return (int32_t)w.n;
}

static int32_t getMetadata(buffer_handle_t h, AIMapper_MetadataType t, void *dest, size_t cap)
{
    if (!streq(t.name, STANDARD)) return -UNSUPPORTED;
    return getStandardMetadata(h, t.value, dest, cap);
}

static int setStandardMetadata(buffer_handle_t h, int64_t t, const void *src, size_t n)
{
    const int *v = ints(h);
    const unsigned char *s = src;
    size_t hl = 8 + (sizeof STANDARD - 1) + 8;
    int32_t x;
    int i;
    if (!v) return BAD_BUFFER;
    if (t >= 19 && t <= 22) return NONE;                                               /* HDR metadata: ignored */
    if (t != 17 && t != 18) return UNSUPPORTED;
    if (!src || n < hl + 4) return BAD_VALUE;
    for (i = 0; i < 4; i++) ((unsigned char *)&x)[i] = s[hl + i];
    if (t == 17) meta(v)->dataspace = x; else meta(v)->blend_mode = x;
    return NONE;
}

static int setMetadata(buffer_handle_t h, AIMapper_MetadataType t, const void *src, size_t n)
{
    if (!streq(t.name, STANDARD)) return UNSUPPORTED;
    return setStandardMetadata(h, t.value, src, n);
}

static AIMapper_MetadataTypeDescription types[23];

static int listSupportedMetadataTypes(const AIMapper_MetadataTypeDescription **out, size_t *n)
{
    int i;
    for (i = 0; i < 23; i++) {
        types[i].metadataType.name = STANDARD;
        types[i].metadataType.value = i + 1;
        types[i].isGettable = 1;
        types[i].isSettable = i + 1 >= 17 && i + 1 <= 22;
    }
    *out = types; *n = 23;
    return NONE;
}

static int dumpBuffer(buffer_handle_t h, dump_fn f, void *ctx) { (void)f; (void)ctx; return ints(h) ? NONE : BAD_BUFFER; }
static int dumpAllBuffers(begin_fn b, dump_fn f, void *ctx) { (void)b; (void)f; (void)ctx; return NONE; }

static int getReservedRegion(buffer_handle_t h, void **out, uint64_t *size)
{
    if (!ints(h)) return BAD_BUFFER;
    *out = 0; *size = 0;
    return NONE;
}

static const AIMapper mapper = {
    5,
    { importBuffer, freeBuffer, getTransportSize, lock, unlock, validate, validate, getMetadata,
      getStandardMetadata, setMetadata, setStandardMetadata, listSupportedMetadataTypes, dumpBuffer,
      dumpAllBuffers, getReservedRegion },
};

EXPORT int AIMapper_loadIMapper(const AIMapper **out)
{
    *out = &mapper;
    return NONE;
}
