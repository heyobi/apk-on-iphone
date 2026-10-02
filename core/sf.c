/* SurfaceFlinger, in the emulator: the native service "SurfaceFlingerAIDL"
 * (android.gui.ISurfaceComposer) behind an in-process binder handle (core/binder.c).
 * framework.jar has no Java binding of it, so it is written here, against the C++
 * AIDL parcel format. Transaction codes come from the guest's own libgui.so
 * (tools/aidlcodes.py): they follow the .aidl declaration order of this build.
 *
 * So far: the display event connection that drives Choreographer. An app gets a
 * SEQPACKET socket (stealReceiveChannel) and asks for vsync (requestNextVsync, or
 * setVsyncRate for a steady stream); aoi_sf_tick, run by the scheduler, writes
 * DisplayEventReceiver::Event records (216 bytes in this build) at 60 Hz. */
#define _POSIX_C_SOURCE 200809L
#include "binder.h"
#include "gralloc.h"
#include "proc.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PERIOD_NS 16666667LL                    /* 60 Hz */
#define DISPLAY_ID 1ULL                         /* the one physical display */
#define EVENT_SIZE 216                          /* sizeof(DisplayEventReceiver::Event), libgui getEvents */
#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define CONNS 32

struct conn {
    int used;
    int send, recv;                             /* host socketpair: we write send, the app reads recv */
    int pair;                                   /* its pair id (core/proc.h): the guest holds both ends */
    int rate;                                   /* setVsyncRate: every rate-th vsync (0: off) */
    int oneshot;                                /* requestNextVsync pending */
    int64_t next;                               /* when the next vsync is due */
    uint32_t count;
};

#define LAYERS 256

/* A layer: a name, an id, its handle (the IBinder apps pass in transactions); the
 * buffer it shows (we hold a gralloc reference) with its GraphicBuffer id, frame
 * number and release listener; where aoi.WindowSession placed it (/dev/aoi_layer):
 * position, z order, how much it dims what is below. */
struct layer {
    int used;
    uint32_t handle;
    int32_t id;
    char name[96];
    uint32_t buf;
    struct { uint64_t gb, number, ptr, cookie; } shown;
    int32_t x, y, z;
    int32_t dim;                                /* 0..1000 */
    int hidden;
};

#define CACHED 64

struct aoi_sf {
    uint32_t handle, client;                    /* ISurfaceComposer, the one ISurfaceComposerClient */
    uint32_t legacy;                            /* "SurfaceFlinger": android.ui.ISurfaceComposer */
    uint64_t frames;                            /* how many were queued */
    struct { uint64_t gb; uint32_t id; } cache[CACHED];   /* GraphicBuffer id -> gralloc id (client buffer cache) */
    int ncache;
    struct conn c[CONNS];
    struct layer l[LAYERS];
    int32_t next_layer_id;
    int64_t vsync_id;
};

/* ISurfaceComposer method names by transaction code, for the trace (libgui order). */
static const char *const composer_names[] = {
    0, "bootFinished", "createDisplayEventConnection", "createConnection", "createDisplay", "destroyDisplay",
    "getPhysicalDisplayIds", "getPhysicalDisplayToken", "getSupportedFrameTimestamps", "setPowerMode",
    "getDisplayStats", "getDisplayState", "getStaticDisplayInfo", "getDynamicDisplayInfoFromId",
    "getDynamicDisplayInfoFromToken", "getDisplayNativePrimaries", "setActiveColorMode", "setBootDisplayMode",
    "clearBootDisplayMode", "getBootDisplayModeSupport", "getHdrConversionCapabilities", "setHdrConversionStrategy",
    "getHdrOutputConversionSupport", "setAutoLowLatencyMode", "setGameContentType", "captureDisplay",
    "captureDisplayById", "captureLayersSync", "captureLayers", "clearAnimationFrameStats", "getAnimationFrameStats",
    "overrideHdrTypes", "onPullAtom", "getLayerDebugInfo", "getCompositionPreference",
    "getDisplayedContentSamplingAttributes", "setDisplayContentSamplingEnabled", "getDisplayedContentSample",
    "getProtectedContentSupport", "isWideColorDisplay", "addRegionSamplingListener", "removeRegionSamplingListener",
    "addFpsListener", "removeFpsListener", "addTunnelModeEnabledListener", "removeTunnelModeEnabledListener",
    "setDesiredDisplayModeSpecs", "getDesiredDisplayModeSpecs", "getDisplayBrightnessSupport", "setDisplayBrightness",
    "addHdrLayerInfoListener", "removeHdrLayerInfoListener", "notifyPowerBoost", "setGlobalShadowSettings",
    "getDisplayDecorationSupport", "setGameModeFrameRateOverride", "setGameDefaultFrameRateOverride",
    "updateSmallAreaDetection", "setSmallAreaDetectionThreshold", "enableRefreshRateOverlay", "setDebugFlash",
    "scheduleComposite", "scheduleCommit", "forceClientComposition", "getGpuContextPriority",
    "getMaxAcquiredBufferCount", "addWindowInfosListener", "removeWindowInfosListener", "getOverlaySupport",
    "getStalledTransactionInfo", "getSchedulingPolicy",
};

static int64_t now_ns(void) { return aoi_mono_ns(); }     /* the guest's clock */

static void ok(struct aoi_parcel *rep) { rep->status = 0; aoi_p32(rep, 0); }   /* binder::Status OK */

/* ---------- IDisplayEventConnection ---------- */

static void connection(struct aoi_proc *p, void *self, uint32_t code, struct aoi_reader *req, struct aoi_parcel *rep)
{
    struct conn *c = self;
    int64_t t = now_ns();
    switch (code) {
    case 1: {                                                  /* stealReceiveChannel(out BitTube) */
        /* BitTube travels as two fds, receive then send end; the guest owns both
         * now, we keep a dup of the send end to write the events */
        int hs = dup(c->send), fr = -1, fs = -1;
        if (c->recv >= 0 && hs >= 0) {
            fr = aoi_proc_fd_install(p, c->recv, "socket:[bittube]", AOI_FD_PIPE);
            fs = fr >= 0 ? aoi_proc_fd_install(p, hs, "socket:[bittube]", AOI_FD_PIPE) : -1;
        }
        if (fr < 0 || fs < 0) { if (hs >= 0 && fs < 0) close(hs); rep->status = -12; return; }   /* NO_MEMORY */
        c->recv = -1;
        c->pair = aoi_proc_pair(p, fr, fs, 5);                 /* recv end 0, send end 1 */
        ok(rep);
        aoi_p32(rep, 1);                                       /* a non-null parcelable */
        aoi_pfd(rep, fr);
        aoi_pfd(rep, fs);
        break;
    }
    case 2:                                                    /* setVsyncRate(int count) */
        c->rate = (int)aoi_r32(req);
        c->next = t + PERIOD_NS - t % PERIOD_NS;
        ok(rep);
        break;
    case 3:                                                    /* requestNextVsync() */
        if (!c->oneshot) { c->oneshot = 1; c->next = t + PERIOD_NS - t % PERIOD_NS; }
        ok(rep);
        break;
    case 4: {                                                  /* getLatestVsyncEventData() -> ParcelableVsyncEventData */
        int64_t base = t - t % PERIOD_NS, i;
        ok(rep);
        aoi_p32(rep, 1);                                       /* non-null */
        aoi_p64(rep, PERIOD_NS);                               /* frameInterval */
        aoi_p32(rep, 0); aoi_p32(rep, 1);                      /* preferredFrameTimelineIndex, frameTimelinesLength */
        for (i = 0; i < 7; i++) {                              /* kFrameTimelinesCapacity entries */
            aoi_p64(rep, (uint64_t)(((struct aoi_sf *)p->sf)->vsync_id + i));
            aoi_p64(rep, (uint64_t)(base + (i + 1) * PERIOD_NS));
            aoi_p64(rep, (uint64_t)(base + (i + 2) * PERIOD_NS));
        }
        break;
    }
    default:
        if (p->trace) fprintf(p->trace, "[sf] IDisplayEventConnection call %u not implemented\n", code);
        break;
    }
}

/* ---------- layers: ISurfaceComposerClient ---------- */

/* A layer handle has no interface of its own (SurfaceFlinger's LayerHandle is a bare
 * BBinder); transactions name layers by it. */
static void layer_handle(struct aoi_proc *p, void *self, uint32_t code, struct aoi_reader *req, struct aoi_parcel *rep)
{
    (void)p; (void)self; (void)code; (void)req; (void)rep;
}

static void client(struct aoi_proc *p, void *self, uint32_t code, struct aoi_reader *req, struct aoi_parcel *rep)
{
    struct aoi_sf *sf = self;
    switch (code) {
    case 1: {                                                  /* createSurface(name, flags, parent, metadata) */
        char name[96];
        uint32_t flags, start;
        int k;
        struct layer *ly;
        aoi_rstr16(req, name, sizeof name);
        flags = aoi_r32(req);
        for (k = 0; k < LAYERS && sf->l[k].used; k++) {}
        if (k == LAYERS) { rep->status = -12; return; }
        ly = &sf->l[k];
        memset(ly, 0, sizeof *ly);
        ly->used = 1;
        ly->id = ++sf->next_layer_id;
        snprintf(ly->name, sizeof ly->name, "%s#%d", name, ly->id);
        ly->handle = aoi_binder_native(p, NULL, "", layer_handle, ly);
        if (!ly->handle) { ly->used = 0; rep->status = -12; return; }
        if (p->trace) fprintf(p->trace, "[sf] layer %d \"%s\" flags %#x, handle %u\n", ly->id, ly->name, flags, ly->handle);
        ok(rep);
        aoi_p32(rep, 1);                                       /* non-null CreateSurfaceResult */
        start = rep->n;
        aoi_p32(rep, 0);                                       /* structured parcelable: its size, then fields */
        aoi_phandle(rep, ly->handle);                          /*   handle */
        aoi_p32(rep, (uint32_t)ly->id);                        /*   layerId */
        aoi_pstr16(rep, ly->name);                             /*   layerName */
        aoi_p32(rep, 0);                                       /*   transformHint */
        { uint32_t size = rep->n - start; memcpy(rep->d + start, &size, 4); }
        break;
    }
    default:
        if (p->trace) fprintf(p->trace, "[sf] ISurfaceComposerClient call %u not implemented\n", code);
        break;
    }
}

/* ---------- ISurfaceComposer ---------- */

static void composer(struct aoi_proc *p, void *self, uint32_t code, struct aoi_reader *req, struct aoi_parcel *rep)
{
    struct aoi_sf *sf = self;
    (void)req;
    switch (code) {
    case 1: ok(rep); break;                                    /* bootFinished */
    case 2: {                                                  /* createDisplayEventConnection(source, registration, layer) */
        int k, sv[2];
        uint32_t h;
        for (k = 0; k < CONNS && sf->c[k].used; k++) {}
        if (k == CONNS || aoi_host_msgpair(sv)) { rep->status = -12; return; }
        fcntl(sv[0], F_SETFL, O_NONBLOCK); fcntl(sv[1], F_SETFL, O_NONBLOCK);
        fcntl(sv[0], F_SETFD, FD_CLOEXEC); fcntl(sv[1], F_SETFD, FD_CLOEXEC);
        memset(&sf->c[k], 0, sizeof sf->c[k]);
        sf->c[k].used = 1; sf->c[k].send = sv[0]; sf->c[k].recv = sv[1];
        h = aoi_binder_native(p, NULL, "android.gui.IDisplayEventConnection", connection, &sf->c[k]);
        if (!h) { close(sv[0]); close(sv[1]); sf->c[k].used = 0; rep->status = -12; return; }
        ok(rep);
        aoi_phandle(rep, h);
        break;
    }
    case 3:                                                    /* createConnection() -> ISurfaceComposerClient */
        if (!sf->client) sf->client = aoi_binder_native(p, NULL, "android.gui.ISurfaceComposerClient", client, sf);
        if (!sf->client) { rep->status = -12; return; }
        ok(rep);
        aoi_phandle(rep, sf->client);
        break;
    case 65:                                                   /* getMaxAcquiredBufferCount() -> int */
        ok(rep);
        aoi_p32(rep, 1);                                       /* the app may hold 1 + this many */
        break;
    case 6:                                                    /* getPhysicalDisplayIds() -> long[] */
        ok(rep);
        aoi_p32(rep, 1);
        aoi_p64(rep, DISPLAY_ID);
        break;
    case 12: {                                                 /* getStaticDisplayInfo(long id) -> StaticDisplayInfo */
        float density = 2.0f;                                  /* (HWUI's GPU pipeline asks, for wide color) */
        uint32_t bits;
        memcpy(&bits, &density, 4);
        ok(rep);
        aoi_p32(rep, 1);                                       /* non-null */
        aoi_p32(rep, 24);                                      /* the parcelable's size, this int included */
        aoi_p32(rep, 0);                                       /* connectionType: Internal */
        aoi_p32(rep, (int32_t)bits);                           /* density */
        aoi_p32(rep, 1);                                       /* secure */
        aoi_p32(rep, 0);                                       /* deviceProductInfo: null */
        aoi_p32(rep, 0);                                       /* installOrientation: Rotation0 */
        break;
    }
    case 13:                                                   /* getDynamicDisplayInfoFromId(long id) -> DynamicDisplayInfo */
        /* Android 14's layout (libgui's readFromParcel): HWUI's GPU pipeline reads the
         * color modes (sRGB only: no wide color) and the mode; Java gets the display
         * from aoi.DisplayManager, so the resolution here is nominal. */
        ok(rep);
        aoi_p32(rep, 1);                                       /* non-null */
        aoi_p32(rep, 140);                                     /* DynamicDisplayInfo, size included */
        aoi_p32(rep, 1);                                       /* supportedDisplayModes: one */
        aoi_p32(rep, 1);                                       /*   non-null */
        aoi_p32(rep, 72);                                      /*   DisplayMode, size included */
        aoi_p32(rep, 0);                                       /*   id */
        aoi_p32(rep, 1); aoi_p32(rep, 12); aoi_p32(rep, 804); aoi_p32(rep, 1556);   /* resolution: Size */
        aoi_pf32(rep, 460.0f); aoi_pf32(rep, 460.0f);          /*   xDpi, yDpi */
        aoi_p32(rep, 0);                                       /*   supportedHdrTypes: none */
        aoi_pf32(rep, 60.0f); aoi_pf32(rep, 60.0f);            /*   refresh rates */
        aoi_p64(rep, 0); aoi_p64(rep, 0);                      /*   app/sf vsync offsets */
        aoi_p64(rep, 16666666);                                /*   presentationDeadline */
        aoi_p32(rep, 0);                                       /*   group */
        aoi_p32(rep, 0);                                       /* activeDisplayModeId */
        aoi_pf32(rep, 60.0f);                                  /* renderFrameRate */
        aoi_p32(rep, 1); aoi_p32(rep, 0);                      /* supportedColorModes: NATIVE */
        aoi_p32(rep, 0);                                       /* activeColorMode */
        aoi_p32(rep, 1); aoi_p32(rep, 20);                     /* hdrCapabilities: non-null, size */
        aoi_p32(rep, 0);                                       /*   supportedHdrTypes: none */
        aoi_pf32(rep, 500.0f); aoi_pf32(rep, 500.0f); aoi_pf32(rep, 0.05f);
        aoi_p32(rep, 0); aoi_p32(rep, 0);                      /* autoLowLatencyMode, gameContentType */
        aoi_p32(rep, 0);                                       /* preferredBootDisplayMode */
        break;
    case 34:                                                   /* getCompositionPreference() -> CompositionPreference */
        ok(rep);
        aoi_p32(rep, 1);                                       /* non-null */
        aoi_p32(rep, 20);                                      /* size included */
        aoi_p32(rep, 142671872);                               /* defaultDataspace: V0_SRGB */
        aoi_p32(rep, 1);                                       /* defaultPixelFormat: RGBA_8888 */
        aoi_p32(rep, 142671872);                               /* wideColorGamutDataspace: none wider */
        aoi_p32(rep, 1);                                       /* wideColorGamutPixelFormat */
        break;
    default:
        if (p->trace)
            fprintf(p->trace, "[sf] ISurfaceComposer.%s (%u) not implemented\n",
                    code < sizeof composer_names / sizeof *composer_names && composer_names[code] ? composer_names[code] : "?",
                    code);
        break;
    }
}

/* ---------- frames: the legacy ISurfaceComposer's setTransactionState ---------- */

/* The screen as a PPM file at $AOI_SF_DUMP (host debugging); a "%d" in it keeps
 * every frame, numbered. */
static void dump(struct aoi_proc *p, const uint8_t *px, uint32_t w, uint32_t h, uint32_t format, uint64_t n)
{
    const char *path = getenv("AOI_SF_DUMP");
    char numbered[1024];
    FILE *f;
    size_t i;
    (void)p;
    if (path && strstr(path, "%d")) { snprintf(numbered, sizeof numbered, path, (int)n); path = numbered; }
    if (!path || !*path || !(f = fopen(path, "wb"))) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (i = 0; i < (size_t)w * h; i++) {
        const uint8_t *q = px + 4 * i;
        uint8_t rgb[3];
        if (format == 5) { rgb[0] = q[2]; rgb[1] = q[1]; rgb[2] = q[0]; }   /* BGRA */
        else { rgb[0] = q[0]; rgb[1] = q[1]; rgb[2] = q[2]; }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* ITransactionCompletedListener.onReleaseBuffer(ReleaseCallbackId{buffer id, frame
 * number}, no fence, max acquired 1), one-way: the app's BLASTBufferQueue may reuse
 * the buffer. As SurfaceFlinger does when the next buffer replaces it. */
static void release(struct aoi_proc *p, struct layer *ly)
{
    struct aoi_parcel pc;
    if (ly->shown.ptr) {
        memset(&pc, 0, sizeof pc);
        aoi_p32(&pc, 0); aoi_p32(&pc, 0xffffffffu); aoi_p32(&pc, 0x53595354u);   /* interface token */
        aoi_pstr16(&pc, "android.gui.ITransactionComposerListener");
        aoi_p32(&pc, 1);                                                           /* non-null ReleaseCallbackId: */
        aoi_p64(&pc, ly->shown.gb); aoi_p64(&pc, ly->shown.number);              /*   buffer id, frame number */
        aoi_p32(&pc, 4); aoi_p32(&pc, 0); aoi_p32(&pc, 0);                         /* Fence: no fd */
        aoi_p32(&pc, 1);                                                           /* currentMaxAcquiredBufferCount */
        aoi_binder_send(p, ly->shown.ptr, ly->shown.cookie, 2, &pc);              /* ON_RELEASE_BUFFER */
        ly->shown.ptr = 0;
    }
    if (ly->buf) aoi_gralloc_release(p, ly->buf);
    ly->buf = 0;
}

/* The layers that show a buffer, bottom to top (z, then age). */
static int visible(struct aoi_proc *p, struct aoi_sf *sf, struct layer **v)
{
    int k, n = 0, i;
    for (k = 0; k < LAYERS; k++) {
        struct layer *ly = &sf->l[k];
        struct aoi_gbuf *b;
        if (!ly->used || ly->hidden || !ly->buf || !(b = aoi_gralloc_find(p, ly->buf)) || b->bpp != 4) continue;
        for (i = n; i > 0 && (v[i - 1]->z > ly->z || (v[i - 1]->z == ly->z && v[i - 1]->id > ly->id)); i--) v[i] = v[i - 1];
        v[i] = ly;
        n++;
    }
    return n;
}

/* The screen: the bottom layer's buffer, then each one above it, its dim first, drawn
 * over it (premultiplied alpha, source over), clipped to the bottom one. As packed rows
 * of RGBA (or BGRA, the bottom layer's format) to the host and to $AOI_SF_DUMP. */
static void compose(struct aoi_proc *p, struct aoi_sf *sf)
{
    struct layer *v[LAYERS];
    struct aoi_gbuf *base, *b;
    uint8_t *px, *row = NULL;
    uint32_t w, h, y, x;
    int n = visible(p, sf, v), k;
    if (!n || (!p->frame && !getenv("AOI_SF_DUMP"))) return;
    base = aoi_gralloc_find(p, v[0]->buf);
    w = base->width; h = base->height;
    if (!(px = malloc((size_t)w * h * 4))) return;
    for (y = 0; y < h; y++)
        if (!aoi_vm_read(&p->vm, base->addr + (uint64_t)y * base->stride * 4, px + (size_t)y * w * 4, (uint64_t)w * 4, 0))
            break;
    if (y < h) { free(px); return; }
    for (k = 1; k < n; k++) {
        struct layer *ly = v[k];
        int32_t x0, y0, x1, y1;
        b = aoi_gralloc_find(p, ly->buf);
        if (ly->dim > 0) {                                     /* FLAG_DIM_BEHIND: black at dimAmount */
            uint32_t keep = (uint32_t)(1000 - (ly->dim > 1000 ? 1000 : ly->dim)) * 256 / 1000, i;
            for (i = 0; i < w * h * 4; i++) if ((i & 3) != 3) px[i] = (uint8_t)(px[i] * keep >> 8);
        }
        x0 = ly->x < 0 ? -ly->x : 0; y0 = ly->y < 0 ? -ly->y : 0;
        x1 = (int32_t)b->width; y1 = (int32_t)b->height;
        if (ly->x + x1 > (int32_t)w) x1 = (int32_t)w - ly->x;
        if (ly->y + y1 > (int32_t)h) y1 = (int32_t)h - ly->y;
        if (x0 >= x1 || y0 >= y1 || !(row = realloc(row, (size_t)b->width * 4))) continue;
        for (y = (uint32_t)y0; y < (uint32_t)y1; y++) {
            uint8_t *d = px + ((size_t)(ly->y + (int32_t)y) * w + (size_t)(ly->x + x0)) * 4, *s = row + (size_t)x0 * 4;
            if (!aoi_vm_read(&p->vm, b->addr + (uint64_t)y * b->stride * 4, row, (uint64_t)b->width * 4, 0)) break;
            for (x = (uint32_t)x0; x < (uint32_t)x1; x++, d += 4, s += 4) {
                uint32_t a = s[3], c;
                if (a == 255) { memcpy(d, s, 4); continue; }
                if (!a) continue;
                for (c = 0; c < 4; c++) d[c] = (uint8_t)(s[c] + (d[c] * (255 - a) + 127) / 255);
            }
        }
    }
    free(row);
    dump(p, px, w, h, base->format, sf->frames);
    if (p->frame) p->frame(p->frame_ctx, px, w, h);
    free(px);
}

static void show(struct aoi_proc *p, struct aoi_sf *sf, struct layer *ly, uint32_t id, uint64_t gb, uint64_t number,
                 uint64_t ptr, uint64_t cookie)
{
    struct aoi_gbuf *b = aoi_gralloc_find(p, id);
    if (!b) return;
    aoi_gralloc_retain(p, id);                                 /* on screen: ours until the next one */
    release(p, ly);
    ly->buf = id;
    ly->shown.gb = gb; ly->shown.number = number; ly->shown.ptr = ptr; ly->shown.cookie = cookie;
    sf->frames++;
    if (p->trace) fprintf(p->trace, "[sf] frame %llu: layer %d, buffer %u (%ux%u), frame number %llu, after %llu instructions, at %.3f s\n",
                          (unsigned long long)sf->frames, ly->id, id, b->width, b->height, (unsigned long long)number,
                          (unsigned long long)p->cpu.steps, (double)now_ns() / 1e9);
}

/* setTransactionState carries layer_state_t records whose layout changes with every
 * release; we look for the buffers. Each record starts with its layer: the handle
 * (a flat binder object, 28 bytes with its stability) and the layer id. A new buffer
 * travels flattened: 'GB01', 12 words (width, height, stride, format, layers, usage,
 * id hi/lo, generation, fds, ints, usage hi), then its native_handle ints, ours
 * (gralloc.h). Then BufferData goes on (LayerState.cpp): fence, frameNumber,
 * releaseBufferListener, releaseBufferEndpoint, cachedBuffer.token, cachedBuffer.id.
 * The client caches buffers, so a known one travels as that cache token and id only:
 * the token's id names the buffer, and frameNumber and the listener sit at fixed
 * distances before it. A buffer belongs to the record it is in. */
static void transaction_state(struct aoi_proc *p, struct aoi_sf *sf, struct aoi_reader *r)
{
    struct { uint32_t at; struct layer *ly; } st[32];
    struct { struct layer *ly; uint32_t id; uint64_t gb, number, ptr, cookie; } nb[32];
    uint32_t o, w;
    int ns = 0, nn = 0, k, i;
    for (o = r->pos & ~3u; o + 32 <= r->n && ns < 32; o += 4) {   /* the records: our layer handles */
        uint32_t hd;
        int32_t lid;
        memcpy(&w, r->d + o, 4);
        if (w != AOI_BINDER_TYPE_HANDLE) continue;
        memcpy(&hd, r->d + o + 8, 4); memcpy(&lid, r->d + o + 28, 4);
        for (k = 0; k < LAYERS && !(sf->l[k].used && sf->l[k].handle == hd && sf->l[k].id == lid); k++) {}
        if (k < LAYERS) { st[ns].at = o; st[ns].ly = &sf->l[k]; ns++; }
    }
    for (o = r->pos & ~3u; o + 4 <= r->n; o += 4) {           /* new buffers: learn their ids */
        memcpy(&w, r->d + o, 4);
        if (w == 0x47423031u && o + 4 * (13 + AOI_GB_INTS) <= r->n) {           /* 'GB01' */
            uint32_t v[13 + AOI_GB_INTS];
            uint64_t gb;
            memcpy(v, r->d + o, sizeof v);
            if (v[11] != AOI_GB_INTS || v[13 + AOI_GB_I_MAGIC] != AOI_GB_MAGIC) continue;
            gb = (uint64_t)v[7] << 32 | v[8];
            for (k = 0; k < sf->ncache && sf->cache[k].gb != gb; k++) {}
            if (k == sf->ncache) { if (sf->ncache < CACHED) sf->ncache++; else k = (int)(gb % CACHED); }
            sf->cache[k].gb = gb; sf->cache[k].id = v[13 + AOI_GB_I_ID];
        }
    }
    for (o = r->pos & ~3u; o + 36 <= r->n; o += 4) {          /* the cache tokens and what precedes them */
        uint64_t t;
        struct layer *ly = NULL;
        memcpy(&w, r->d + o, 4);
        if (w != AOI_BINDER_TYPE_BINDER || o < 64) continue;
        memcpy(&t, r->d + o + 28, 8);
        for (k = 0; k < sf->ncache && !(t && sf->cache[k].gb == t); k++) {}
        if (k == sf->ncache) continue;
        for (i = 0; i < ns && st[i].at < o; i++) ly = st[i].ly;
        if (!ly) {                                             /* no record found: the newest layer */
            for (i = 0; i < LAYERS; i++) if (sf->l[i].used && (!ly || sf->l[i].id > ly->id)) ly = &sf->l[i];
            if (!ly) continue;
        }
        for (i = 0; i < nn && nb[i].ly != ly; i++) {}
        if (i == nn) { if (nn == 32) continue; nn++; }
        nb[i].ly = ly; nb[i].gb = t; nb[i].id = sf->cache[k].id; nb[i].ptr = nb[i].cookie = 0;
        memcpy(&nb[i].number, r->d + o - 64, 8);
        memcpy(&w, r->d + o - 56, 4);
        if (w == AOI_BINDER_TYPE_BINDER) { memcpy(&nb[i].ptr, r->d + o - 48, 8); memcpy(&nb[i].cookie, r->d + o - 40, 8); }
        o += 32;
    }
    for (i = 0; i < nn; i++) show(p, sf, nb[i].ly, nb[i].id, nb[i].gb, nb[i].number, nb[i].ptr, nb[i].cookie);
    if (nn) compose(p, sf);
}

/* /dev/aoi_layer/ID/X/Y/Z/DIM from aoi.WindowSession: where a window's layer goes
 * (DIM in thousandths); /dev/aoi_layer/ID/hide: the window is gone. */
void aoi_sf_place(struct aoi_proc *p, const char *cmd)
{
    struct aoi_sf *sf = p->sf;
    int32_t id, x, y, z, dim;
    int k;
    char what[8];
    if (!sf) return;
    if (sscanf(cmd, "%d/%d/%d/%d/%d", &id, &x, &y, &z, &dim) == 5) {
        for (k = 0; k < LAYERS && !(sf->l[k].used && sf->l[k].id == id); k++) {}
        if (k == LAYERS) return;
        sf->l[k].x = x; sf->l[k].y = y; sf->l[k].z = z; sf->l[k].dim = dim; sf->l[k].hidden = 0;
    } else if (sscanf(cmd, "%d/%7s", &id, what) == 2 && !strcmp(what, "hide")) {
        for (k = 0; k < LAYERS && !(sf->l[k].used && sf->l[k].id == id); k++) {}
        if (k == LAYERS) return;
        sf->l[k].hidden = 1;
        release(p, &sf->l[k]);
    } else return;
    if (p->trace) fprintf(p->trace, "[sf] place %s\n", cmd);
    compose(p, sf);
}

static void legacy(struct aoi_proc *p, void *self, uint32_t code, struct aoi_reader *req, struct aoi_parcel *rep)
{
    struct aoi_sf *sf = self;
    switch (code) {
    case 8:                                                    /* SET_TRANSACTION_STATE (one-way) */
        transaction_state(p, sf, req);
        rep->status = 0;
        break;
    default:
        if (p->trace) fprintf(p->trace, "[sf] android.ui.ISurfaceComposer call %u not implemented\n", code);
        break;
    }
}

/* ---------- vsync ---------- */

static void vsync(struct aoi_sf *sf, struct conn *c, int64_t t)
{
    uint8_t ev[EVENT_SIZE];
    uint32_t type = FOURCC('v', 's', 'y', 'n'), one = 1, zero = 0;
    uint64_t id = DISPLAY_ID;
    int64_t interval = PERIOD_NS, vid = ++sf->vsync_id, deadline = t + PERIOD_NS, present = t + 2 * PERIOD_NS;
    memset(ev, 0, sizeof ev);
    memcpy(ev, &type, 4);                                      /* Header: type, displayId, timestamp */
    memcpy(ev + 8, &id, 8);
    memcpy(ev + 16, &t, 8);
    c->count++;
    memcpy(ev + 24, &c->count, 4);                             /* VSync: count, then VsyncEventData */
    memcpy(ev + 32, &interval, 8);                             /*   frameInterval */
    memcpy(ev + 40, &zero, 4);                                 /*   preferredFrameTimelineIndex */
    memcpy(ev + 44, &one, 4);                                  /*   frameTimelinesLength */
    memcpy(ev + 48, &vid, 8);                                  /*   frameTimelines[0]: vsyncId, */
    memcpy(ev + 56, &deadline, 8);                             /*     deadlineTimestamp, */
    memcpy(ev + 64, &present, 8);                              /*     expectedPresentationTime */
    if (send(c->send, ev, sizeof ev, 0) < 0) {}                /* a full socket drops the frame, as SF does */
}

void aoi_sf_tick(struct aoi_proc *p)
{
    struct aoi_sf *sf = p->sf;
    int64_t t;
    int k;
    if (!sf) return;
    t = now_ns();
    for (k = 0; k < CONNS; k++) {
        struct conn *c = &sf->c[k];
        if (!c->used || (!c->oneshot && c->rate <= 0) || t < c->next) continue;
        vsync(sf, c, t - (t - c->next) % PERIOD_NS);           /* stamped on the period boundary */
        c->oneshot = 0;
        c->next = t + PERIOD_NS * (c->rate > 1 ? c->rate : 1) - (t - c->next) % PERIOD_NS;
    }
}

void aoi_sf_init(struct aoi_proc *p)
{
    struct aoi_sf *sf = calloc(1, sizeof *sf);
    if (!sf) return;
    p->sf = sf;
    sf->handle = aoi_binder_native(p, "SurfaceFlingerAIDL", "android.gui.ISurfaceComposer", composer, sf);
    sf->legacy = aoi_binder_native(p, "SurfaceFlinger", "android.ui.ISurfaceComposer", legacy, sf);
}

void aoi_sf_free(struct aoi_proc *p)
{
    struct aoi_sf *sf = p->sf;
    int k;
    if (!sf) return;
    for (k = 0; k < CONNS; k++)
        if (sf->c[k].used) {
            close(sf->c[k].send);
            if (sf->c[k].recv >= 0) close(sf->c[k].recv);
        }
    free(sf);
    p->sf = NULL;
}

/* ---------- snapshots (core/snap.c) ---------- */

int aoi_sf_snap(struct aoi_proc *p, FILE *f, int save)
{
    struct aoi_sf *sf;
    int k, i, sv[2];
    if (save) {
        uint8_t has = p->sf != NULL;
        if (fwrite(&has, 1, 1, f) != 1) return -1;
        return has && fwrite(p->sf, sizeof *p->sf, 1, f) != 1 ? -1 : 0;
    }
    {
        uint8_t has;
        if (fread(&has, 1, 1, f) != 1) return -1;
        if (!has) return 0;
    }
    if (!(sf = calloc(1, sizeof *sf)) || fread(sf, sizeof *sf, 1, f) != 1) { free(sf); return -1; }
    p->sf = sf;
    for (k = 0; k < CONNS; k++) sf->c[k].send = sf->c[k].recv = -1;   /* the saved ones are gone */
    for (k = 0; k < CONNS; k++) {                              /* host sockets: rebuilt */
        struct conn *c = &sf->c[k];
        if (!c->used) continue;
        if (c->pair)                                           /* the guest's send end: we write a dup of it */
            for (i = 0; i < AOI_PROC_FDS; i++)
                if (p->fd[i].used && p->fd[i].kind == AOI_FD_PIPE && p->fd[i].pair == c->pair && p->fd[i].end == 1) {
                    c->send = dup(p->fd[i].host);
                    break;
                }
        if (c->send < 0) {                                     /* not handed out yet, or closed by the guest */
            if (aoi_host_msgpair(sv)) return -1;
            fcntl(sv[0], F_SETFL, O_NONBLOCK); fcntl(sv[1], F_SETFL, O_NONBLOCK);
            fcntl(sv[0], F_SETFD, FD_CLOEXEC); fcntl(sv[1], F_SETFD, FD_CLOEXEC);
            c->send = sv[0]; c->recv = sv[1];
            c->pair = 0;
        }
        c->oneshot = 1;                                        /* an event in flight was lost: Choreographer */
        c->next = 0;                                           /* ignores a vsync it did not ask for */
    }
    return 0;
}

/* Native objects by kind and index, so binder handles survive a snapshot. */
enum { N_COMPOSER = 1, N_LEGACY, N_CLIENT, N_CONN, N_LAYER };

int aoi_sf_native_id(struct aoi_proc *p, void *self, aoi_native_fn fn, int32_t *kind, int32_t *idx)
{
    struct aoi_sf *sf = p->sf;
    if (!sf) return -1;
    *idx = 0;
    if (self == sf) { *kind = fn == composer ? N_COMPOSER : fn == legacy ? N_LEGACY : N_CLIENT; return 0; }
    if ((char *)self >= (char *)sf->c && (char *)self < (char *)(sf->c + CONNS)) {
        *kind = N_CONN; *idx = (int32_t)((struct conn *)self - sf->c); return 0;
    }
    if ((char *)self >= (char *)sf->l && (char *)self < (char *)(sf->l + LAYERS)) {
        *kind = N_LAYER; *idx = (int32_t)((struct layer *)self - sf->l); return 0;
    }
    return -1;
}

int aoi_sf_native_ref(struct aoi_proc *p, int32_t kind, int32_t idx, aoi_native_fn *fn, void **self, const char **iface)
{
    struct aoi_sf *sf = p->sf;
    if (!sf) return -1;
    switch (kind) {
    case N_COMPOSER: *fn = composer; *self = sf; *iface = "android.gui.ISurfaceComposer"; return 0;
    case N_LEGACY: *fn = legacy; *self = sf; *iface = "android.ui.ISurfaceComposer"; return 0;
    case N_CLIENT: *fn = client; *self = sf; *iface = "android.gui.ISurfaceComposerClient"; return 0;
    case N_CONN:
        if (idx < 0 || idx >= CONNS) return -1;
        *fn = connection; *self = &sf->c[idx]; *iface = "android.gui.IDisplayEventConnection"; return 0;
    case N_LAYER:
        if (idx < 0 || idx >= LAYERS) return -1;
        *fn = layer_handle; *self = &sf->l[idx]; *iface = ""; return 0;
    }
    return -1;
}

/* The screen, to the host again (after a snapshot is loaded). */
void aoi_sf_redraw(struct aoi_proc *p)
{
    if (p->sf) compose(p, p->sf);
}
