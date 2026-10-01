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
    int rate;                                   /* setVsyncRate: every rate-th vsync (0: off) */
    int oneshot;                                /* requestNextVsync pending */
    int64_t next;                               /* when the next vsync is due */
    uint32_t count;
};

struct aoi_sf {
    uint32_t handle;
    struct conn c[CONNS];
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

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

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
    default:
        if (p->trace) fprintf(p->trace, "[sf] IDisplayEventConnection call %u not implemented\n", code);
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
        if (k == CONNS || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv)) { rep->status = -12; return; }
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
    case 6:                                                    /* getPhysicalDisplayIds() -> long[] */
        ok(rep);
        aoi_p32(rep, 1);
        aoi_p64(rep, DISPLAY_ID);
        break;
    default:
        if (p->trace)
            fprintf(p->trace, "[sf] ISurfaceComposer.%s (%u) not implemented\n",
                    code < sizeof composer_names / sizeof *composer_names && composer_names[code] ? composer_names[code] : "?",
                    code);
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
