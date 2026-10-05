/* AudioFlinger and AudioPolicy, in the emulator: the native services
 * "media.audio_flinger" (android.media.IAudioFlingerService) and "media.audio_policy"
 * (android.media.IAudioPolicyService) behind in-process binder handles
 * (core/binder.c), as core/sf.c does for SurfaceFlinger. Transaction codes come from
 * the guest's audioflinger-aidl-cpp.so and audiopolicy-aidl-cpp.so
 * (tools/aidlcodes.py); parcel layouts from their readFromParcel code.
 *
 * Playback: AudioTrack (libaudioclient) asks createTrack, gets an IAudioTrack and,
 * from getCblk, a memfd holding audio_track_cblk_t and the track's ring of frames. The
 * app writes frames and moves mRear; we play them: aoi_af_tick, run by the scheduler,
 * takes what a 48 kHz stereo output clock has used since the last tick from every
 * started track (resampled, mixed, with its volume), moves mFront and mServer (the
 * position the app reads) and wakes a writer waiting on mFutex. The mix goes to
 * p->audio (iOS plays it; aoiproc writes it to $AOI_AUDIO_OUT). The cblk is reached
 * through the app's own mapping of the memfd, so it is the memory the app sees.
 *
 * audio_track_cblk_t in this build (ClientProxy code in libaudioclient.so): mServer 0,
 * mFutex 0x08, mVolumeLR 0x10, the ExtendedTimestamp queue's mSequence 0x3c and value
 * 0x40 (mPosition[5], mTimeNs[5], mTimebaseOffset[2], mFlushed: AudioTrack::
 * getTimestamp, which MediaPlayer's clock follows), mBufferSizeInFrames 0xa8, mFlags 0xb0, then the
 * streaming part: mFront 0xb8, mRear 0xbc, mFlush 0xc0, mStop 0xc4, mUnderrunFrames
 * 0xc8, mUnderrunCount 0xcc; the frames start at 0xe8 (AudioTrack::createTrack_l:
 * buffers = cblk + 1).
 *
 * AOI_AF_TRACE=1 traces every call with its request bytes. */
#define _POSIX_C_SOURCE 200809L
#include "binder.h"
#include "proc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OUT_RATE 48000
#define OUT_IO 13                               /* the one output's audio_io_handle_t */
#define TRACKS 32
#define TICK_NS 10000000LL                      /* the mixer runs every 10 ms */

enum {                                          /* audio_track_cblk_t */
    C_SERVER = 0x00, C_FUTEX = 0x08, C_VOLUME = 0x10, C_TS_SEQ = 0x3c, C_TS = 0x40, C_BUFSIZE = 0xa8, C_FLAGS = 0xb0,
    C_FRONT = 0xb8, C_REAR = 0xbc, C_FLUSH = 0xc0, C_STOP = 0xc4, C_UNDER_FRAMES = 0xc8, C_UNDER_COUNT = 0xcc,
    C_SIZE = 0xe8,
    /* a static track's (MODE_STATIC: SoundPool) u.mStatic instead of mFront...: the
     * client's StaticAudioTrackState queue, the server's position/loop queue */
    S_ACK = 0xb8, S_SEQ = 0xbc, S_STATE = 0xc0, S_POS_ACK = 0xd8, S_POS_SEQ = 0xdc, S_POS = 0xe0
};
#define CBLK_INVALID 0x04
#define CBLK_LOOP_CYCLE 0x20
#define CBLK_LOOP_FINAL 0x40
#define CBLK_BUFFER_END 0x80
#define CBLK_FUTEX_WAKE 1

enum { F_U8, F_S16, F_S32, F_FLOAT, F_S24 };     /* sample formats we take */

struct track {
    int used, dead, seen;                       /* seen: the app had it mapped (gone again: released) */
    uint32_t handle;
    char name[48];                              /* the memfd's name: finds the app's mapping */
    int fd;                                     /* the guest fd we made (the app dups it) */
    uint64_t va;                                /* where the app mapped the cblk (0: not yet looked up) */
    uint32_t frames, frame_size, channels, rate, format;
    int started, stopping;
    int32_t flushed;                            /* the last mFlush seen */
    double phase;                               /* resampling position, in track frames */
    int32_t session, port;
    /* MODE_STATIC: the frames are in the app's shared buffer (a memfd: read through a
     * host dup of it), played from a position, maybe looped */
    int is_static, shared_fd;
    uint64_t shared_off;
    int32_t st_seq;                             /* the client's state queue, last taken */
    uint32_t pos, loop_start, loop_end, loop_seq, pos_seq;
    int32_t loop_count;
};

struct aoi_af {
    uint32_t flinger, policy, resources, packages;
    int32_t next_id;
    struct track t[TRACKS];
    int64_t clock0, played;                     /* output frames made since clock0 */
    int64_t next;                               /* when the mixer is due (0: nothing plays) */
    FILE *out;                                  /* $AOI_AUDIO_OUT (host tests) */
};

static int tracing(void) { static int t = -1; if (t < 0) t = getenv("AOI_AF_TRACE") != NULL; return t; }

static void trace(const char *who, uint32_t code, struct aoi_reader *r)
{
    uint32_t i;
    if (!tracing()) return;
    fprintf(stderr, "[af] %s %u (%u bytes):", who, code, r->n - r->pos);
    for (i = r->pos; i < r->n && i < r->pos + 128; i += 4) {
        uint32_t v = 0;
        memcpy(&v, r->d + i, r->n - i >= 4 ? 4 : r->n - i);
        fprintf(stderr, " %08x", v);
    }
    fprintf(stderr, "\n");
}

static void ok(struct aoi_parcel *rep) { rep->status = 0; aoi_p32(rep, 0); }   /* binder::Status OK */

/* ---------- parcels ---------- */

/* A parcelable field: its non-null marker, then size-prefixed fields. The position
 * of its end (where the next field starts), or 0 if null. *start: its first field. */
static uint32_t parcelable(struct aoi_reader *r, uint32_t *start)
{
    uint32_t at, size;
    if (!aoi_r32(r)) return 0;
    at = r->pos;
    size = aoi_r32(r);
    if (start) *start = r->pos;
    return at + size;
}

static void skip_parcelable(struct aoi_reader *r) { uint32_t end = parcelable(r, NULL); if (end) r->pos = end; }

/* AudioChannelLayout (a union: tag, value): how many channels. */
static uint32_t channel_count(uint32_t tag, uint32_t value)
{
    if (tag == 2 || tag == 3 || tag == 4) return value ? (uint32_t)__builtin_popcount(value) : 2;   /* index/layout/voice mask */
    return 2;
}

static void p_layout(struct aoi_parcel *rep, uint32_t channels)   /* non-null AudioChannelLayout */
{
    aoi_p32(rep, 1);
    aoi_p32(rep, 3);                                           /* layoutMask */
    aoi_p32(rep, channels == 1 ? 1 : 3);                       /* MONO = FRONT_LEFT, STEREO = FL|FR */
}

static void p_format(struct aoi_parcel *rep, int pcm)          /* non-null AudioFormatDescription */
{
    aoi_p32(rep, 1);
    aoi_p32(rep, 20);                                          /* its size */
    aoi_p32(rep, 1);                                           /* type PCM */
    aoi_p32(rep, (uint32_t)pcm);
    aoi_p32(rep, 0); aoi_p32(rep, 0);                          /* encoding "" */
}

/* ---------- the app's cblk ---------- */

static struct aoi_proc_map *find_map(struct aoi_proc *p, const char *path)
{
    int i;
    for (i = p->nmaps - 1; i >= 0; i--)
        if (p->maps[i].off == 0 && !strcmp(p->maps[i].path, path)) return &p->maps[i];
    return NULL;
}

/* The app's mapping of the track's cblk, or 0 (not mapped yet, or gone). */
static uint64_t cblk(struct aoi_proc *p, struct track *t)
{
    uint64_t n;
    if (t->va && aoi_vm_span(&p->vm, t->va, C_SIZE, AOI_PROT_R, &n) && n >= C_SIZE) return t->va;
    t->va = 0;
    {
        struct aoi_proc_map *m = find_map(p, t->name);
        if (m && aoi_vm_span(&p->vm, m->start, C_SIZE, AOI_PROT_R, &n) && n >= C_SIZE) t->va = m->start;
    }
    if (t->va) t->seen = 1;
    else if (t->seen) t->dead = 1;                     /* unmapped: the app released it */
    return t->va;
}

static int32_t c32(struct aoi_proc *p, uint64_t va, uint32_t off)
{
    int32_t v = 0;
    aoi_vm_read(&p->vm, va + off, &v, 4, 0);
    return v;
}

static void s32(struct aoi_proc *p, uint64_t va, uint32_t off, int32_t v) { aoi_vm_write(&p->vm, va + off, &v, 4, 0); }

/* gain_minifloat_t (audio_utils): 3-bit exponent, 13-bit mantissa; 0xE000 is unity. */
static float minifloat(uint32_t m)
{
    int mant = (int)(m & 0x1fff), e = (int)((m >> 13) & 7);
    return ldexpf((float)(e ? (0x2000 | mant) : mant << 1) / 8192.0f, e - 7);
}

/* One sample of frame `f`, channel `c` (frames beyond the first two channels are
 * dropped; mono is both sides), as a float in [-1, 1]. */
static float sample(const uint8_t *fr, struct track *t, uint32_t c)
{
    if (c >= t->channels) c = t->channels - 1;
    switch (t->format) {
    case F_U8: return ((float)fr[c] - 128.0f) / 128.0f;
    case F_S16: { int16_t v; memcpy(&v, fr + 2 * c, 2); return (float)v / 32768.0f; }
    case F_S32: { int32_t v; memcpy(&v, fr + 4 * c, 4); return (float)v / 2147483648.0f; }
    case F_S24: { int32_t v = (int32_t)((uint32_t)fr[3 * c] << 8 | (uint32_t)fr[3 * c + 1] << 16 | (uint32_t)fr[3 * c + 2] << 24);
                  return (float)v / 2147483648.0f; }
    default: { float v; memcpy(&v, fr + 4 * c, 4); return v; }
    }
}

/* The timestamp the app's AudioTrack reads (getTimestamp, MediaPlayer's clock): the
 * frames played now, at the server and the "kernel". */
static void write_ts(struct aoi_proc *p, uint64_t va)
{
    int64_t ts[13];                                    /* ExtendedTimestamp */
    int32_t seq = c32(p, va, C_TS_SEQ);
    memset(ts, 0, sizeof ts);
    ts[1] = ts[3] = (uint32_t)c32(p, va, C_SERVER);    /* mPosition[LOCATION_SERVER, LOCATION_KERNEL] */
    ts[5 + 1] = ts[5 + 3] = aoi_mono_ns();             /* mTimeNs[...] */
    s32(p, va, C_TS_SEQ, seq + 1);                     /* SingleStateQueue: odd while written */
    aoi_vm_write(&p->vm, va + C_TS, ts, sizeof ts, 0);
    s32(p, va, C_TS_SEQ, seq + 2);
}

static void wake(struct aoi_proc *p, uint64_t va)      /* (ServerProxy::releaseBuffer) */
{
    int32_t old = c32(p, va, C_FUTEX);
    s32(p, va, C_FUTEX, old | CBLK_FUTEX_WAKE);
    if (!(old & CBLK_FUTEX_WAKE)) aoi_proc_futex_wake(p, va + C_FUTEX, 1 << 30);
}

/* A static track: takes the client's new position or loop (StaticAudioTrackServerProxy::
 * pollPosition), plays from the shared buffer, loops, and reports the position and
 * CBLK_LOOP_* / CBLK_BUFFER_END as releaseBuffer does. */
static void mix_static(struct aoi_proc *p, struct track *t, float *mix, uint32_t want)
{
    uint64_t va = cblk(p, t);
    int32_t seq;
    uint32_t i = 0, total = 0, vol;
    int32_t flags = 0;
    float gl, gr;
    double step;
    if (!va) return;
    if (c32(p, va, C_FLAGS) & CBLK_INVALID) { t->dead = 1; return; }
    seq = c32(p, va, S_SEQ);
    if (seq != t->st_seq && !(seq & 1)) {
        uint32_t st[6];                                /* loopStart, loopEnd, loopCount, loopSeq, position, positionSeq */
        aoi_vm_read(&p->vm, va + S_STATE, st, sizeof st, 0);
        if (c32(p, va, S_SEQ) == seq) {
            int loop_first = (int32_t)(st[3] - st[5]) < 0, pass;
            for (pass = 0; pass < 2; pass++) {
                if ((pass == 0) == loop_first) {       /* the loop */
                    if (st[3] != t->loop_seq) {
                        if ((int32_t)st[2] == 0 || ((int32_t)st[2] >= -1 && st[1] <= t->frames && st[0] < st[1])) {
                            t->loop_start = st[0]; t->loop_end = st[1]; t->loop_count = (int32_t)st[2];
                        }
                        t->loop_seq = st[3];
                    }
                } else if (st[5] != t->pos_seq) {      /* the position */
                    if (st[4] <= t->frames) {
                        if (t->loop_count && st[4] >= t->loop_end) t->loop_count = 0;
                        t->pos = st[4];
                    }
                    t->pos_seq = st[5];
                }
            }
            t->st_seq = seq;
            s32(p, va, S_ACK, seq + 1);                /* (Observer::poll, done) */
            t->phase = 0;
        }
    }
    if (!t->started) return;
    vol = (uint32_t)c32(p, va, C_VOLUME);
    gl = minifloat(vol & 0xffff);
    gr = minifloat(vol >> 16);
    step = (double)t->rate / OUT_RATE;
    while (i < want) {
        uint8_t buf[1024 * 32];
        uint32_t end = t->loop_count && t->loop_end > t->pos ? t->loop_end : t->frames, n, used, newpos;
        if (t->pos >= end) break;
        n = end - t->pos;
        if (n > sizeof buf / t->frame_size) n = (uint32_t)(sizeof buf / t->frame_size);
        if (pread(t->shared_fd, buf, (size_t)n * t->frame_size, (off_t)(t->shared_off + (uint64_t)t->pos * t->frame_size))
            != (ssize_t)((size_t)n * t->frame_size)) { t->dead = 1; return; }
        for (; i < want; i++) {
            uint32_t k = (uint32_t)t->phase;
            const uint8_t *f0, *f1;
            double fx;
            if (k >= n) break;
            fx = t->phase - k;
            f0 = buf + (size_t)k * t->frame_size;
            f1 = k + 1 < n ? f0 + t->frame_size : f0;
            mix[2 * i] += gl * (float)(sample(f0, t, 0) + (sample(f1, t, 0) - sample(f0, t, 0)) * fx);
            mix[2 * i + 1] += gr * (float)(sample(f0, t, 1) + (sample(f1, t, 1) - sample(f0, t, 1)) * fx);
            t->phase += step;
        }
        used = (uint32_t)t->phase < n ? (uint32_t)t->phase : n;
        t->phase -= used;
        newpos = t->pos + used;
        if (t->loop_count && newpos == t->loop_end) {
            newpos = t->loop_start;
            flags |= t->loop_count == -1 || --t->loop_count ? CBLK_LOOP_CYCLE : CBLK_LOOP_FINAL;
        }
        if (newpos == t->frames) flags |= CBLK_BUFFER_END;
        t->pos = newpos;
        total += used;
        if (!used) break;
    }
    if (!total) {
        if (t->pos >= t->frames && !t->loop_count) t->started = 0;   /* played out */
        return;
    }
    s32(p, va, C_SERVER, c32(p, va, C_SERVER) + (int32_t)total);
    {
        int32_t ps = c32(p, va, S_POS_SEQ);
        uint32_t pl[2] = { t->pos, (uint32_t)t->loop_count };
        s32(p, va, S_POS_SEQ, ps + 1);
        aoi_vm_write(&p->vm, va + S_POS, pl, sizeof pl, 0);
        s32(p, va, S_POS_SEQ, ps + 2);
    }
    if (flags) s32(p, va, C_FLAGS, c32(p, va, C_FLAGS) | flags);
    write_ts(p, va);
    wake(p, va);
}

/* Mixes up to `want` output frames of track t into mix (stereo floats); moves its front. */
static void mix_track(struct aoi_proc *p, struct track *t, float *mix, uint32_t want)
{
    uint64_t va = cblk(p, t);
    int32_t front, rear, flush, stop, avail;
    uint32_t mask, i, used, need;
    uint32_t vol;
    float gl, gr;
    double step;
    uint8_t fr[2][32];
    if (!va) return;
    if (c32(p, va, C_FLAGS) & CBLK_INVALID) { t->dead = 1; return; }
    front = c32(p, va, C_FRONT);
    rear = c32(p, va, C_REAR);
    flush = c32(p, va, C_FLUSH);
    mask = t->frames - 1;
    if (flush != t->flushed) {                         /* ServerProxy::flushBufferIfNeeded */
        uint32_t ov = t->frames << 1, m2 = ov - 1;
        int32_t nf = (int32_t)(((uint32_t)front & ~m2) | ((uint32_t)flush & m2)), filled = rear - nf;
        if (filled < 0 || filled > (int32_t)t->frames) nf = rear;
        front = nf;
        t->flushed = flush;
        s32(p, va, C_FRONT, front);
    }
    if (!t->started) return;
    avail = rear - front;
    if (t->stopping) {                                 /* to mStop, then the track is done */
        stop = c32(p, va, C_STOP);
        if (stop - front < avail && stop - front >= 0) avail = stop - front;
    }
    if (avail <= 0) {
        if (t->stopping) { t->started = t->stopping = 0; }
        else s32(p, va, C_UNDER_FRAMES, c32(p, va, C_UNDER_FRAMES) + (int32_t)want);
        return;
    }
    vol = (uint32_t)c32(p, va, C_VOLUME);
    gl = minifloat(vol & 0xffff);
    gr = minifloat(vol >> 16);
    step = (double)t->rate / OUT_RATE;
    used = 0;
    for (i = 0; i < want; i++) {
        uint32_t k = (uint32_t)t->phase;               /* frames past front */
        double fx = t->phase - k;
        float l0, r0, l1, r1;
        if (k + 1 >= (uint32_t)avail) break;
        aoi_vm_read(&p->vm, va + C_SIZE + (uint64_t)(((uint32_t)front + k) & mask) * t->frame_size, fr[0], t->frame_size, 0);
        aoi_vm_read(&p->vm, va + C_SIZE + (uint64_t)(((uint32_t)front + k + 1) & mask) * t->frame_size, fr[1], t->frame_size, 0);
        l0 = sample(fr[0], t, 0); r0 = sample(fr[0], t, 1);
        l1 = sample(fr[1], t, 0); r1 = sample(fr[1], t, 1);
        mix[2 * i] += gl * (float)(l0 + (l1 - l0) * fx);
        mix[2 * i + 1] += gr * (float)(r0 + (r1 - r0) * fx);
        t->phase += step;
    }
    used = (uint32_t)t->phase;
    if ((int32_t)used > avail) used = (uint32_t)avail;
    t->phase -= used;
    need = want - i;
    if (need) {                                        /* ran dry: an underrun */
        s32(p, va, C_UNDER_FRAMES, c32(p, va, C_UNDER_FRAMES) + (int32_t)need);
        if (avail - (int32_t)used <= 1) { used = (uint32_t)avail; t->phase = 0; }
    }
    if (!used) return;
    s32(p, va, C_FRONT, front + (int32_t)used);
    s32(p, va, C_SERVER, c32(p, va, C_SERVER) + (int32_t)used);
    write_ts(p, va);
    wake(p, va);                                       /* room again: wake a writer */
}

static int any_started(struct aoi_af *af)
{
    int k;
    for (k = 0; k < TRACKS; k++) if (af->t[k].used && af->t[k].started && !af->t[k].dead) return 1;
    return 0;
}

void aoi_af_tick(struct aoi_proc *p)
{
    struct aoi_af *af = p->af;
    int64_t now, due;
    float mix[2 * 4800];
    int16_t pcm[2 * 4800];
    uint32_t n, i;
    int k;
    if (!af || !af->next || (now = aoi_mono_ns()) < af->next) return;
    due = (now - af->clock0) * OUT_RATE / 1000000000LL - af->played;
    if (due > 4800) { af->played += due - 4800; due = 4800; }   /* fell behind (a stall): skip ahead */
    n = (uint32_t)(due > 0 ? due : 0);
    if (n) {
        memset(mix, 0, sizeof(float) * 2 * n);
        for (k = 0; k < TRACKS; k++)
            if (af->t[k].used && !af->t[k].dead) (af->t[k].is_static ? mix_static : mix_track)(p, &af->t[k], mix, n);
        for (i = 0; i < 2 * n; i++) {
            float v = mix[i] * 32767.0f;
            pcm[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
        if (p->audio) p->audio(p->audio_ctx, pcm, n);
        if (af->out) fwrite(pcm, 4, n, af->out);
        af->played += n;
    }
    af->next = any_started(af) ? now + TICK_NS : 0;
}

int64_t aoi_af_next(struct aoi_proc *p) { return p->af ? p->af->next : 0; }

static void clock_start(struct aoi_af *af)
{
    if (af->next) return;
    af->clock0 = aoi_mono_ns();
    af->played = 0;
    af->next = af->clock0 + TICK_NS;
}

/* ---------- IAudioTrack ---------- */

static void track(struct aoi_proc *p, void *self, uint32_t code, struct aoi_reader *req, struct aoi_parcel *rep)
{
    struct track *t = self;
    trace("track", code, req);
    if (t->dead) { rep->status = -32; return; }        /* DEAD_OBJECT: the app makes a new track */
    switch (code) {
    case 1:                                            /* getCblk() -> @nullable SharedFileRegion */
        ok(rep);
        aoi_p32(rep, 1);
        aoi_p32(rep, 4 + 12 + 24 + 8 + 8 + 4);          /* its size */
        aoi_p32(rep, 1); aoi_p32(rep, 0);               /* ParcelFileDescriptor: non-null, no comm channel */
        aoi_pfd(rep, t->fd);
        aoi_p64(rep, 0);                               /* offset */
        aoi_p64(rep, C_SIZE + (t->is_static ? 0 : (uint64_t)t->frames * t->frame_size));
        aoi_p32(rep, 1);                               /* writeable */
        break;
    case 2:                                            /* start() -> int status */
        t->started = 1; t->stopping = 0;
        clock_start(p->af);
        ok(rep); aoi_p32(rep, 0);
        break;
    case 3:                                            /* stop(): plays what was written, to mStop */
        if (t->is_static) t->started = 0;              /* (static: at once; the app resets the position) */
        else t->stopping = 1;
        ok(rep); break;
    case 4: {                                          /* flush() */
        uint64_t va = cblk(p, t);
        if (va && !t->is_static) s32(p, va, C_FRONT, c32(p, va, C_REAR));
        t->phase = 0;
        ok(rep);
        break;
    }
    case 5: t->started = 0; ok(rep); break;            /* pause() */
    case 6: case 7: ok(rep); aoi_p32(rep, 0); break;   /* attachAuxEffect, setParameters -> int */
    case 9: rep->status = -38; break;                  /* getTimestamp: INVALID_OPERATION (the app uses positions) */
    case 11: ok(rep); aoi_p32(rep, -38); break;        /* applyVolumeShaper -> int: not supported */
    default: ok(rep); break;
    }
}

/* ---------- IAudioFlingerService ---------- */

static void create_track(struct aoi_proc *p, struct aoi_af *af, struct aoi_reader *r, struct aoi_parcel *rep)
{
    uint32_t end, cfg, fmt_end, rate, tag, chv, ch, pcm, size;
    int32_t flags, session;
    int64_t frames;
    int k, fmt, shared = -1;
    uint64_t shared_off = 0, shared_size = 0;
    struct track *t;
    if (!(end = parcelable(r, NULL))) { rep->status = -22; return; }   /* BAD_VALUE */
    skip_parcelable(r);                                                  /* attributes */
    if (!(cfg = parcelable(r, NULL)) || !parcelable(r, NULL)) { rep->status = -22; return; }   /* config, its base */
    rate = aoi_r32(r);
    aoi_r32(r); tag = aoi_r32(r); chv = aoi_r32(r);                     /* channel layout */
    if (!(fmt_end = parcelable(r, NULL))) { rep->status = -22; return; }
    aoi_r32(r); pcm = aoi_r32(r);                                        /* format: type, pcm */
    r->pos = cfg;
    skip_parcelable(r);                                                  /* client */
    {                                                                    /* @nullable sharedBuffer: MODE_STATIC */
        uint32_t sb_end = parcelable(r, NULL);
        if (sb_end) {                                                    /* SharedFileRegion */
            if (aoi_r32(r)) {                                            /* ParcelFileDescriptor: non-null, */
                aoi_r32(r); aoi_r32(r); aoi_r32(r);                      /* no comm channel; flat_binder_object */
                shared = (int)aoi_r32(r);                                /* (type, flags), the fd */
                aoi_r32(r); aoi_r64(r);
            }
            shared_off = aoi_r64(r);
            shared_size = aoi_r64(r);
            r->pos = sb_end;
        }
    }
    aoi_r32(r); aoi_r32(r);                                              /* notificationsPerBuffer, speed */
    r->pos += 28;                                                        /* the callback binder */
    size = aoi_r32(r);                                                   /* opPackageName (String16) */
    if (size != 0xffffffffu) r->pos += (2 * (size + 1) + 3) & ~3u;
    flags = (int32_t)aoi_r32(r);
    frames = (int64_t)aoi_r64(r);
    aoi_r64(r); aoi_r32(r);                                              /* notificationFrameCount, selectedDevice */
    session = (int32_t)aoi_r32(r);
    (void)end; (void)fmt_end; (void)flags;
    ch = channel_count(tag, chv);
    switch (pcm) {
    case 0: fmt = F_U8; size = 1; break;
    case 1: fmt = F_S16; size = 2; break;
    case 2: fmt = F_S32; size = 4; break;
    case 4: fmt = F_FLOAT; size = 4; break;
    case 5: fmt = F_S24; size = 3; break;
    default: rep->status = -22; return;                                  /* (fixed point, compressed: not here) */
    }
    if (!rate) rate = OUT_RATE;
    if (shared >= 0) {                                                   /* static: the buffer's frames */
        int hfd = aoi_proc_host_fd(p, shared);
        if (hfd < 0 || shared_size < size * ch || (shared = dup(hfd)) < 0) { rep->status = -22; return; }
        frames = (int64_t)(shared_size / (size * ch));
    } else if (frames < 256) frames = (int64_t)rate / 10;                       /* the app left it to us: 100 ms */
    {
        uint32_t f = 256;
        while (f < frames && f < (1u << 20)) f <<= 1;                   /* a power of two: the ring's mask */
        frames = f;
    }
    for (k = 0; k < TRACKS && af->t[k].used && !af->t[k].dead; k++) {}
    if (k == TRACKS)                                                     /* full: any the app has let go of? */
        for (k = 0; k < TRACKS && (!af->t[k].seen || cblk(p, &af->t[k])); k++) {}
    if (k == TRACKS) { if (shared >= 0) close(shared); rep->status = -12; return; }   /* NO_MEMORY */
    t = &af->t[k];
    if (t->is_static && t->shared_fd >= 0) close(t->shared_fd);
    memset(t, 0, sizeof *t);
    t->is_static = shared >= 0;
    t->shared_fd = shared;
    t->shared_off = shared_off;
    t->used = 1;
    t->channels = ch > 8 ? 8 : ch;
    t->format = (uint32_t)fmt;
    t->frame_size = size * ch;
    t->frames = (uint32_t)frames;
    t->rate = rate;
    t->session = session ? session : (af->next_id += 8);
    t->port = (af->next_id += 8);
    snprintf(t->name, sizeof t->name, "memfd:AudioTrack-%d", t->port);
    if ((t->fd = aoi_proc_memfd(p, t->name + 6, C_SIZE + (t->is_static ? 0 : (uint64_t)t->frames * t->frame_size))) < 0) {
        t->used = 0; rep->status = -12; return;
    }
    {                                                                    /* the server side of the cblk */
        int32_t init[C_SIZE / 4];
        memset(init, 0, sizeof init);
        init[C_BUFSIZE / 4] = (int32_t)t->frames;
        init[C_BUFSIZE / 4 + 1] = (int32_t)t->frames;                    /* mStartThresholdInFrames */
        init[C_VOLUME / 4] = (int32_t)0xE000E000u;                       /* unity */
        if (pwrite(aoi_proc_host_fd(p, t->fd), init, sizeof init, 0) != (ssize_t)sizeof init) { t->used = 0; rep->status = -12; return; }
    }
    t->handle = aoi_binder_native(p, NULL, "android.media.IAudioTrack", track, t);
    if (!t->handle) { t->used = 0; rep->status = -12; return; }
    if (tracing()) fprintf(stderr, "[af] track %d: %u Hz, %u ch, format %d, %u frames\n", k, rate, ch, fmt, t->frames);
    ok(rep);
    aoi_p32(rep, 1);                                                     /* CreateTrackResponse */
    {
        uint32_t at = rep->n;
        aoi_p32(rep, 0);                                                 /* its size, below */
        aoi_p32(rep, 0);                                                 /* flags */
        aoi_p64(rep, t->frames);                                         /* frameCount */
        aoi_p64(rep, t->frames / 2);                                     /* notificationFrameCount */
        aoi_p32(rep, 0);                                                 /* selectedDeviceId */
        aoi_p32(rep, (uint32_t)t->session);
        aoi_p32(rep, t->rate);                                           /* sampleRate */
        aoi_p32(rep, 3);                                                 /* streamType: MUSIC */
        aoi_p64(rep, 960);                                               /* afFrameCount */
        aoi_p32(rep, OUT_RATE);                                          /* afSampleRate */
        p_layout(rep, 2);                                                /* afChannelMask */
        p_format(rep, 1);                                                /* afFormat: PCM 16 */
        aoi_p32(rep, 40);                                                /* afLatencyMs */
        aoi_p32(rep, 0);                                                 /* afTrackFlags: mixed, not direct */
        aoi_p32(rep, OUT_IO);                                            /* outputId */
        aoi_p32(rep, (uint32_t)t->port);                                 /* portId */
        aoi_phandle(rep, t->handle);                                     /* audioTrack */
        size = rep->n - at;
        memcpy(rep->d + at, &size, 4);
    }
}

static void flinger(struct aoi_proc *p, void *self, uint32_t code, struct aoi_reader *req, struct aoi_parcel *rep)
{
    struct aoi_af *af = self;
    trace("flinger", code, req);
    switch (code) {
    case 1: create_track(p, af, req, rep); break;
    case 3: ok(rep); aoi_p32(rep, OUT_RATE); break;            /* sampleRate(io) */
    case 4:                                                    /* format(io) -> AudioFormatDescription */
        ok(rep);
        p_format(rep, 1);
        break;
    case 5: ok(rep); aoi_p64(rep, 960); break;                 /* frameCount(io) */
    case 6: ok(rep); aoi_p32(rep, 40); break;                  /* latency(io), ms */
    case 9: ok(rep); aoi_pf32(rep, 1.0f); break;               /* masterVolume */
    case 10: case 16: case 19: ok(rep); aoi_p32(rep, 0); break;   /* masterMute, streamMute, getMicMute */
    case 15: ok(rep); aoi_pf32(rep, 1.0f); break;              /* streamVolume */
    case 23: ok(rep); break;                                   /* registerClient */
    case 35: case 36: ok(rep); aoi_p32(rep, (uint32_t)(af->next_id += 8)); break;   /* newAudioUniqueId, acquireAudioSessionId */
    case 45: ok(rep); aoi_p32(rep, OUT_RATE); break;           /* getPrimaryOutputSamplingRate */
    case 46: case 56: ok(rep); aoi_p64(rep, 960); break;       /* getPrimaryOutputFrameCount, frameCountHAL */
    case 2: rep->status = -38; break;                          /* createRecord: no microphone yet */
    default: ok(rep); break;
    }
}

/* ---------- IAudioPolicyService ---------- */

static void policy(struct aoi_proc *p, void *self, uint32_t code, struct aoi_reader *req, struct aoi_parcel *rep)
{
    (void)p; (void)self;
    trace("policy", code, req);
    switch (code) {
    case 8: ok(rep); aoi_p32(rep, OUT_IO); break;              /* getOutput(stream) -> io handle */
    case 31: case 32: case 33: ok(rep); aoi_p32(rep, 0); break;   /* isStreamActive, ...Remotely, isSourceActive */
    case 79: case 81: ok(rep); aoi_p32(rep, 0); break;         /* listAudioProductStrategies, listAudioVolumeGroups: [] */
    default: ok(rep); break;
    }
}

/* ---------- IResourceManagerService ----------
 * "media.resource_manager": MediaCodec registers and reclaims codec resources with
 * it, and waits for it to exist. One app, no competition: every call succeeds
 * (void calls, and false/0 for the few that return something). */

static void resources(struct aoi_proc *p, void *self, uint32_t code, struct aoi_reader *req, struct aoi_parcel *rep)
{
    (void)p; (void)self;
    trace("resources", code, req);
    ok(rep);
    aoi_p32(rep, 0);
}

static void permissions(struct aoi_proc *p, void *self, uint32_t code, struct aoi_reader *req, struct aoi_parcel *rep)
{
    (void)p; (void)self;
    trace("permission", code, req);
    ok(rep);
    aoi_p32(rep, code == 1 || code == 4);              /* checkPermission, isRuntimePermission: true; else 0 */
}

/* ---------- life ---------- */

void aoi_af_init(struct aoi_proc *p)
{
    struct aoi_af *af = calloc(1, sizeof *af);
    const char *out = getenv("AOI_AUDIO_OUT");
    if (!af) return;
    p->af = af;
    af->next_id = 16;
    if (out && *out) af->out = fopen(out, "wb");
    af->flinger = aoi_binder_native(p, "media.audio_flinger", "android.media.IAudioFlingerService", flinger, af);
    af->policy = aoi_binder_native(p, "media.audio_policy", "android.media.IAudioPolicyService", policy, af);
    af->resources = aoi_binder_native(p, "media.resource_manager", "android.media.IResourceManagerService", resources, af);
    /* the same answers for IPackageManagerNative, which MediaCodec's metrics wait for */
    af->packages = aoi_binder_native(p, "package_native", "android.content.pm.IPackageManagerNative", resources, af);
    /* media.extractor and media.codec: MediaPlayerService links to their deaths only (the
     * extractors run in the app, media.stagefright.extractremote=false) */
    aoi_binder_native(p, "media.extractor", "android.media.IMediaExtractorService", resources, af);
    aoi_binder_native(p, "media.codec", "android.hardware.IOMX", resources, af);
    aoi_binder_native(p, "media.metrics", "android.media.IMediaMetricsService", resources, af);   /* (waited for) */
    /* ISensorServer: no sensors. Native SensorManager aborts without the service
     * ("getService(SensorService) NULL"); with it an app finds no sensors. Its calls
     * answer a 0 (getSensorList: none; isDataInjectionEnabled: false). */
    aoi_binder_native(p, "sensorservice", "android.gui.SensorServer", resources, af);
    /* IPermissionController: native services check permissions with it (MediaCodec's
     * resource manager, AudioFlinger's clients): one app, its permissions granted */
    aoi_binder_native(p, "permission", "android.os.IPermissionController", permissions, af);
}

void aoi_af_free(struct aoi_proc *p)
{
    int k;
    if (!p->af) return;
    for (k = 0; k < TRACKS; k++)
        if (p->af->t[k].is_static && p->af->t[k].shared_fd >= 0) close(p->af->t[k].shared_fd);
    if (p->af->out) fclose(p->af->out);
    free(p->af);
    p->af = NULL;
}

/* ---------- snapshots (core/snap.c) ----------
 * The tracks' memfds come back as copies (the app's pages), so a restored track
 * cannot go on: each is marked invalid in the app's cblk (CBLK_INVALID) and answers
 * DEAD_OBJECT, and AudioTrack makes itself a new one (restoreTrack_l), as after an
 * audioserver restart. */

enum { N_FLINGER = 32, N_POLICY = 33, N_TRACK = 34, N_RESOURCES = 35, N_PERMISSIONS = 36 };

int aoi_af_snap(struct aoi_proc *p, FILE *f, int save)
{
    struct aoi_af *af;
    uint8_t has;
    int k;
    if (save) {
        has = p->af != NULL;
        if (fwrite(&has, 1, 1, f) != 1) return -1;
        return has && fwrite(p->af, sizeof *p->af, 1, f) != 1 ? -1 : 0;
    }
    if (fread(&has, 1, 1, f) != 1) return -1;
    if (!has) return 0;
    if (!(af = calloc(1, sizeof *af)) || fread(af, sizeof *af, 1, f) != 1) { free(af); return -1; }
    af->out = NULL;
    af->next = 0;
    p->af = af;
    for (k = 0; k < TRACKS; k++) {
        struct track *t = &af->t[k];
        uint64_t va;
        t->shared_fd = -1;                             /* (a host fd of the saved process) */
        if (!t->used) continue;
        t->started = 0;
        if (!t->dead && (va = cblk(p, t))) s32(p, va, C_FLAGS, c32(p, va, C_FLAGS) | CBLK_INVALID);
        t->dead = 1;
    }
    return 0;
}

int aoi_af_native_id(struct aoi_proc *p, void *self, aoi_native_fn fn, int32_t *kind, int32_t *idx)
{
    struct aoi_af *af = p->af;
    if (!af) return -1;
    *idx = 0;
    if (self == af) { *kind = fn == flinger ? N_FLINGER : fn == policy ? N_POLICY : fn == permissions ? N_PERMISSIONS : N_RESOURCES; return 0; }
    if ((char *)self >= (char *)af->t && (char *)self < (char *)(af->t + TRACKS)) {
        *kind = N_TRACK; *idx = (int32_t)((struct track *)self - af->t); return 0;
    }
    return -1;
}

int aoi_af_native_ref(struct aoi_proc *p, int32_t kind, int32_t idx, aoi_native_fn *fn, void **self, const char **iface)
{
    struct aoi_af *af = p->af;
    if (!af) return -1;
    switch (kind) {
    case N_FLINGER: *fn = flinger; *self = af; *iface = "android.media.IAudioFlingerService"; return 0;
    case N_POLICY: *fn = policy; *self = af; *iface = "android.media.IAudioPolicyService"; return 0;
    case N_RESOURCES: *fn = resources; *self = af; *iface = "android.media.IResourceManagerService"; return 0;
    case N_PERMISSIONS: *fn = permissions; *self = af; *iface = "android.os.IPermissionController"; return 0;
    case N_TRACK:
        if (idx < 0 || idx >= TRACKS) return -1;
        *fn = track; *self = &af->t[idx]; *iface = "android.media.IAudioTrack"; return 0;
    }
    return -1;
}
