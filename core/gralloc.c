/* The gralloc allocator, natively: the AIDL service
 * android.hardware.graphics.allocator.IAllocator/default (V2) behind an in-process
 * binder handle (core/binder.c). libui's Gralloc5 asks it for buffers (allocate2) and
 * for the name of the mapper library to load (mapper.aoi.so, guest/mapper.c).
 *
 * A buffer is guest memory mapped here: the pixels, then a metadata page (gralloc.h).
 * Its native_handle is a placeholder fd and ints describing it; the mapper reads
 * those. Imports take references through a private syscall (AOI_SYS_GRALLOC), and the
 * memory is unmapped with the last one. SurfaceFlinger (core/sf.c) reads the pixels
 * of queued buffers with aoi_gralloc_find.
 *
 * The parcel layout is the NDK backend's: a non-null parcelable is 1, then its size,
 * then its fields; a ParcelFileDescriptor is 1, 0 (no comm channel), the fd object.
 * Transaction codes from the guest's android.hardware.graphics.allocator-V2-ndk.so. */
#define _POSIX_C_SOURCE 200809L
#include "binder.h"
#include "gralloc.h"
#include "proc.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFERS 256
#define PAGE 4096u
#define EX_SERVICE_SPECIFIC (-8)
#define ERR_BAD_DESCRIPTOR 1                    /* AllocationError */
#define ERR_NO_RESOURCES 2
#define ERR_UNSUPPORTED 3
#define INTERFACE_HASH "9499fec09c544e9de5be3c87125721600f8ade66"   /* allocator V2, from the guest's lib */

struct aoi_gralloc {
    struct aoi_gbuf b[BUFFERS];
    uint32_t next_id;
};

struct desc { char name[128]; int32_t width, height, layers, format; uint64_t usage, reserved; };

/* Bytes per pixel of the formats we lay out (one plane); 0: unsupported. */
static int bpp(int32_t format)
{
    switch (format) {
    case 1: case 2: case 5: case 0x2b: return 4;  /* RGBA/RGBX/BGRA_8888, RGBA_1010102 */
    case 0x22: return 4;                            /* IMPLEMENTATION_DEFINED: RGBA_8888 */
    case 3: return 3;                               /* RGB_888 */
    case 4: return 2;                               /* RGB_565 */
    case 0x16: return 8;                            /* RGBA_FP16 */
    case 0x21: case 0x38: return 1;                 /* BLOB, R_8 */
    default: return 0;
    }
}

/* BufferDescriptorInfo (non-null parcelable): name byte[128], width, height,
 * layerCount, format, usage, reservedSize, additionalOptions[]. */
static int read_desc(struct aoi_reader *r, struct desc *d)
{
    uint32_t n, i;
    memset(d, 0, sizeof *d);
    if (aoi_r32(r) != 1) return 0;
    aoi_r32(r);                                     /* size */
    n = aoi_r32(r);
    for (i = 0; i < n && r->pos + i < r->n; i++) if (i < sizeof d->name - 1) d->name[i] = (char)r->d[r->pos + i];
    r->pos += (n + 3) & ~3u;
    d->width = (int32_t)aoi_r32(r); d->height = (int32_t)aoi_r32(r);
    d->layers = (int32_t)aoi_r32(r); d->format = (int32_t)aoi_r32(r);
    d->usage = aoi_r64(r); d->reserved = aoi_r64(r);
    return r->pos <= r->n;
}

static int supported(const struct desc *d)
{
    return d->width > 0 && d->height > 0 && d->layers == 1 && bpp(d->format) && d->width <= 8192 && d->height <= 8192;
}

static void service_error(struct aoi_parcel *rep, int32_t code)
{
    rep->status = 0;
    aoi_p32(rep, (uint32_t)EX_SERVICE_SPECIFIC);
    aoi_pstr16(rep, "");                            /* message */
    aoi_p32(rep, 0);                                /* no remote stack trace */
    aoi_p32(rep, (uint32_t)code);
}

static struct aoi_gbuf *new_buffer(struct aoi_proc *p, struct aoi_gralloc *g, const struct desc *d)
{
    int k, px = bpp(d->format);
    struct aoi_gbuf *b;
    struct aoi_gb_meta m;
    uint32_t stride = (uint32_t)d->width;
    uint64_t size, len, a;
    while ((stride * (uint32_t)px) % 64) stride++; /* 64-byte rows */
    size = (uint64_t)stride * (uint32_t)d->height * (uint32_t)px;
    len = ((size + PAGE - 1) & ~(uint64_t)(PAGE - 1)) + PAGE;
    for (k = 0; k < BUFFERS && g->b[k].used; k++) {}
    if (k == BUFFERS || size > 0xffffffffu) return NULL;
    a = aoi_proc_map_anon(p, len, "/dev/gralloc");
    if (a >= (uint64_t)-4096) return NULL;
    b = &g->b[k];
    memset(b, 0, sizeof *b);
    b->used = 1; b->id = ++g->next_id;
    b->addr = a; b->len = len; b->size = (uint32_t)size;
    b->width = (uint32_t)d->width; b->height = (uint32_t)d->height; b->stride = stride;
    b->format = d->format; b->bpp = px; b->usage = d->usage;
    memset(&m, 0, sizeof m);
    m.magic = AOI_GB_MAGIC;
    m.name_len = (unsigned)strlen(d->name);
    memcpy(m.name, d->name, m.name_len);
    aoi_vm_write(&p->vm, a + len - PAGE, &m, sizeof m, 0);
    return b;
}

/* A NativeHandle parcelable: fds [the placeholder], ints. The fd is a new guest fd
 * each time: the receiving Parcel closes the ones it got. */
static int put_handle(struct aoi_proc *p, struct aoi_parcel *rep, const struct aoi_gbuf *b)
{
    uint32_t start, size, v[AOI_GB_INTS];
    int i, host = open("/dev/null", O_RDWR | O_CLOEXEC), fd;
    if (host < 0) return 0;
    if ((fd = aoi_proc_fd_install(p, host, "/dev/gralloc", AOI_FD_FILE)) < 0) { close(host); return 0; }
    v[AOI_GB_I_MAGIC] = AOI_GB_MAGIC; v[AOI_GB_I_ID] = b->id;
    v[AOI_GB_I_WIDTH] = b->width; v[AOI_GB_I_HEIGHT] = b->height; v[AOI_GB_I_STRIDE] = b->stride;
    v[AOI_GB_I_FORMAT] = (uint32_t)b->format; v[AOI_GB_I_LAYERS] = 1;
    v[AOI_GB_I_USAGE_LO] = (uint32_t)b->usage; v[AOI_GB_I_USAGE_HI] = (uint32_t)(b->usage >> 32);
    v[AOI_GB_I_ADDR_LO] = (uint32_t)b->addr; v[AOI_GB_I_ADDR_HI] = (uint32_t)(b->addr >> 32);
    v[AOI_GB_I_SIZE] = b->size; v[AOI_GB_I_BPP] = (uint32_t)b->bpp;
    aoi_p32(rep, 1);
    start = rep->n;
    aoi_p32(rep, 0);
    aoi_p32(rep, 1);                                /* fds: one ParcelFileDescriptor */
    aoi_p32(rep, 1); aoi_p32(rep, 0); aoi_pfd(rep, fd);
    aoi_p32(rep, AOI_GB_INTS);
    for (i = 0; i < AOI_GB_INTS; i++) aoi_p32(rep, v[i]);
    size = rep->n - start; memcpy(rep->d + start, &size, 4);
    return 1;
}

static void allocator(struct aoi_proc *p, void *self, uint32_t code, struct aoi_reader *req, struct aoi_parcel *rep)
{
    struct aoi_gralloc *g = self;
    struct desc d;
    switch (code) {
    case 1:                                         /* allocate(byte[] descriptor, count): the IMapper 4 path */
        service_error(rep, ERR_UNSUPPORTED);
        break;
    case 2: {                                       /* allocate2(BufferDescriptorInfo, count) -> AllocationResult */
        int32_t count, i;
        struct aoi_gbuf *bs[8];
        uint32_t start, size;
        if (!read_desc(req, &d) || (count = (int32_t)aoi_r32(req)) <= 0 || count > 8) {
            if (p->trace) {
                uint32_t o;
                fprintf(p->trace, "[gralloc] allocate2: bad request (%u bytes):", req->n);
                for (o = 0; o + 4 <= req->n && o < 400; o += 4) fprintf(p->trace, " %x", *(const uint32_t *)(req->d + o));
                fprintf(p->trace, "\n");
            }
            service_error(rep, ERR_BAD_DESCRIPTOR);
            break;
        }
        if (!supported(&d)) {
            if (p->trace) fprintf(p->trace, "[gralloc] %dx%d format %#x layers %d: unsupported\n", d.width, d.height, d.format, d.layers);
            service_error(rep, ERR_UNSUPPORTED);
            break;
        }
        for (i = 0; i < count; i++)
            if (!(bs[i] = new_buffer(p, g, &d))) {
                while (i-- > 0) aoi_gralloc_release(p, bs[i]->id);
                service_error(rep, ERR_NO_RESOURCES);
                return;
            }
        rep->status = 0;
        aoi_p32(rep, 0);                            /* Status: ok */
        aoi_p32(rep, 1);                            /* non-null AllocationResult: */
        start = rep->n;
        aoi_p32(rep, 0);
        aoi_p32(rep, bs[0]->stride);                /*   stride */
        aoi_p32(rep, (uint32_t)count);              /*   buffers */
        for (i = 0; i < count; i++)
            if (!put_handle(p, rep, bs[i])) { rep->status = -12; return; }
        size = rep->n - start; memcpy(rep->d + start, &size, 4);
        if (p->trace)
            fprintf(p->trace, "[gralloc] \"%s\" %dx%d format %#x usage %#llx: %d buffer(s) from id %u, stride %u\n",
                    d.name, d.width, d.height, d.format, (unsigned long long)d.usage, count, bs[0]->id, bs[0]->stride);
        break;
    }
    case 3:                                         /* isSupported(BufferDescriptorInfo) -> boolean */
        read_desc(req, &d);
        rep->status = 0; aoi_p32(rep, 0); aoi_p32(rep, (uint32_t)supported(&d));
        break;
    case 4:                                         /* getIMapperLibrarySuffix() -> String */
        rep->status = 0; aoi_p32(rep, 0); aoi_pstr16(rep, AOI_GB_SUFFIX);
        break;
    case 0xffffff:                                  /* getInterfaceVersion */
        rep->status = 0; aoi_p32(rep, 0); aoi_p32(rep, 2);
        break;
    case 0xfffffe:                                  /* getInterfaceHash */
        rep->status = 0; aoi_p32(rep, 0); aoi_pstr16(rep, INTERFACE_HASH);
        break;
    default:
        if (p->trace) fprintf(p->trace, "[gralloc] IAllocator call %#x not implemented\n", code);
        break;
    }
}

struct aoi_gbuf *aoi_gralloc_find(struct aoi_proc *p, uint32_t id)
{
    struct aoi_gralloc *g = p->gralloc;
    int k;
    if (!g || !id) return NULL;
    for (k = 0; k < BUFFERS; k++) if (g->b[k].used && g->b[k].id == id) return &g->b[k];
    return NULL;
}

void aoi_gralloc_retain(struct aoi_proc *p, uint32_t id)
{
    struct aoi_gbuf *b = aoi_gralloc_find(p, id);
    if (b) b->refs++;
}

void aoi_gralloc_release(struct aoi_proc *p, uint32_t id)
{
    struct aoi_gbuf *b = aoi_gralloc_find(p, id);
    if (!b || --b->refs > 0) return;
    aoi_proc_unmap_anon(p, b->addr, b->len);
    if (p->trace) fprintf(p->trace, "[gralloc] buffer %u freed\n", b->id);
    b->used = 0;
}

uint64_t aoi_gralloc_syscall(struct aoi_proc *p, uint64_t op, uint64_t id)
{
    if (!aoi_gralloc_find(p, (uint32_t)id)) return (uint64_t)-22;
    if (op == AOI_GB_RETAIN) aoi_gralloc_retain(p, (uint32_t)id);
    else if (op == AOI_GB_RELEASE) aoi_gralloc_release(p, (uint32_t)id);
    else return (uint64_t)-22;
    return 0;
}

void aoi_gralloc_init(struct aoi_proc *p)
{
    struct aoi_gralloc *g = calloc(1, sizeof *g);
    if (!g) return;
    p->gralloc = g;
    aoi_binder_native(p, "android.hardware.graphics.allocator.IAllocator/default",
                      "android.hardware.graphics.allocator.IAllocator", allocator, g);
}

void aoi_gralloc_free(struct aoi_proc *p)
{
    free(p->gralloc);
    p->gralloc = NULL;
}
