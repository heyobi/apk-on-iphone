/* libGLES_aoi.so: the guest's OpenGL ES driver. Android's libEGL loads it from
 * /vendor/lib64/egl for ro.hardware.egl=aoi (its Loader takes EGL and every GLES
 * function from this one library). Guest code, freestanding: no libc, raw syscalls.
 *
 * GLES: each function (guest/gl_gen.h, from tools/glgen.py) is the private syscall
 * AOI_SYS_GL with its number and arguments; the host (gpu/host.c) runs it on its GPU.
 * EGL: configs, contexts and surfaces are host objects by number; a window surface is
 * a host pbuffer of the window's size, and eglSwapBuffers dequeues the window's next
 * buffer (core/gralloc.c: guest memory), has the host write the frame into it
 * (AOI_EGL_READBACK) and queues it, as a producer of the window's BufferQueue. */
#include "../core/gpu.h"
#include "../core/gralloc.h"

typedef unsigned long size_t;
typedef unsigned long uintptr_t;
typedef long int64_t;
typedef unsigned long uint64_t;
typedef int int32_t;
typedef unsigned int uint32_t;

#define EXPORT __attribute__((visibility("default")))

/* ---------- the syscall ---------- */

static uint64_t aoi_gl(uint64_t op, const uint64_t *args)
{
    register uint64_t x8 __asm__("x8") = AOI_SYS_GL, x0 __asm__("x0") = op, x1 __asm__("x1") = (uint64_t)args;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}

static uint64_t op0(uint64_t op) { uint64_t s[1] = { 0 }; return aoi_gl(op, s); }

static long sys1(long nr, long a0)
{
    register long x8 __asm__("x8") = nr, x0 __asm__("x0") = a0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

#define NR_CLOSE 57

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---------- GLES ---------- */

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef int GLfixed;
typedef long GLintptr;
typedef long GLsizeiptr;
typedef char GLchar;
typedef long GLint64;
typedef unsigned long GLuint64;
typedef struct __GLsync *GLsync;
typedef void (*GLDEBUGPROC)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);

static uint64_t fbits(float f) { union { float f; uint32_t u; } v = { f }; return v.u; }

#include "gl_gen.h"

/* ---------- EGL ---------- */

typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext, *EGLClientBuffer;
typedef void (*EGLproc)(void);

#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_NONE 0x3038
#define EGL_SUCCESS 0x3000
#define EGL_NOT_INITIALIZED 0x3001
#define EGL_BAD_ALLOC 0x3003
#define EGL_BAD_CONFIG 0x3005
#define EGL_BAD_DISPLAY 0x3008
#define EGL_BAD_MATCH 0x3009
#define EGL_BAD_NATIVE_WINDOW 0x300B
#define EGL_BAD_PARAMETER 0x300C
#define EGL_BAD_SURFACE 0x300D
#define EGL_BAD_ATTRIBUTE 0x3004
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_SURFACE_TYPE 0x3033
#define EGL_DONT_CARE (-1)
#define EGL_PBUFFER_BIT 0x0001
#define EGL_WINDOW_BIT 0x0004
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_CLIENT_APIS 0x308D
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_RENDER_BUFFER 0x3086
#define EGL_BACK_BUFFER 0x3084
#define EGL_SWAP_BEHAVIOR 0x3093
#define EGL_BUFFER_DESTROYED 0x3095
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONFIG_ID 0x3028
#define EGL_RECORDABLE_ANDROID 0x3142
#define EGL_FRAMEBUFFER_TARGET_ANDROID 0x3147

static EGLint error = EGL_SUCCESS;
static int initialized;
#define DPY ((EGLDisplay)1)

static EGLint fail(EGLint e) { error = e; return 0; }

EXPORT EGLint eglGetError(void)
{
    EGLint e = error;
    error = EGL_SUCCESS;
    return e;
}

EXPORT EGLDisplay eglGetDisplay(void *native) { return native == 0 ? DPY : 0; }
EXPORT EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native, const long *attribs)
{
    (void)platform; (void)attribs;
    return eglGetDisplay(native);
}

EXPORT EGLBoolean eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    if (d != DPY) return fail(EGL_BAD_DISPLAY);
    if (!initialized && !op0(AOI_EGL_INIT)) return fail(EGL_NOT_INITIALIZED);
    initialized = 1;
    if (major) *major = 1;
    if (minor) *minor = 4;
    return EGL_TRUE;
}

EXPORT EGLBoolean eglTerminate(EGLDisplay d) { return d == DPY ? EGL_TRUE : fail(EGL_BAD_DISPLAY); }

EXPORT const char *eglQueryString(EGLDisplay d, EGLint name)
{
    (void)d;
    switch (name) {
    case EGL_VENDOR: return "aoi";
    case EGL_VERSION: return "1.4 aoi";
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    case EGL_EXTENSIONS:
        return "EGL_KHR_create_context EGL_KHR_no_config_context EGL_KHR_surfaceless_context "
               "EGL_KHR_swap_buffers_with_damage EGL_ANDROID_presentation_time EGL_ANDROID_recordable "
               "EGL_ANDROID_framebuffer_target";
    }
    fail(EGL_BAD_PARAMETER);
    return 0;
}

EXPORT EGLBoolean eglBindAPI(EGLenum api) { return api == EGL_OPENGL_ES_API ? EGL_TRUE : fail(EGL_BAD_PARAMETER); }
EXPORT EGLenum eglQueryAPI(void) { return EGL_OPENGL_ES_API; }

/* Configs: the host's (pbuffer configs, as every surface is a pbuffer there), shown
 * as window configs too, with the Android attributes libEGL and HWUI ask for. */
static int filter(const EGLint *in, EGLint *out, int max)
{
    int n = 0, surface = 0;
    for (; in && in[0] != EGL_NONE && n + 4 < max; in += 2) {
        EGLint k = in[0], v = in[1];
        if (k == EGL_RECORDABLE_ANDROID || k == EGL_FRAMEBUFFER_TARGET_ANDROID || k == EGL_NATIVE_VISUAL_ID) continue;
        if (k == EGL_SURFACE_TYPE) {
            surface = 1;
            if (v != EGL_DONT_CARE) v = (v & ~EGL_WINDOW_BIT) | ((v & EGL_WINDOW_BIT) ? EGL_PBUFFER_BIT : 0);
        }
        out[n++] = k; out[n++] = v;
    }
    if (!surface) { out[n++] = EGL_SURFACE_TYPE; out[n++] = EGL_PBUFFER_BIT; }
    out[n] = EGL_NONE;
    return n;
}

EXPORT EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *attribs, EGLConfig *configs, EGLint size, EGLint *num)
{
    EGLint a[130];
    uint64_t s[4];
    if (d != DPY || !initialized) return fail(EGL_NOT_INITIALIZED);
    if (!num) return fail(EGL_BAD_PARAMETER);
    filter(attribs, a, 128);
    s[0] = (uint64_t)a; s[1] = (uint64_t)configs; s[2] = (uint64_t)(int64_t)size; s[3] = (uint64_t)num;
    return aoi_gl(AOI_EGL_CHOOSE_CONFIG, s) ? EGL_TRUE : fail(EGL_BAD_ATTRIBUTE);
}

EXPORT EGLBoolean eglGetConfigs(EGLDisplay d, EGLConfig *configs, EGLint size, EGLint *num)
{
    static const EGLint any[] = { EGL_SURFACE_TYPE, EGL_DONT_CARE, EGL_NONE };
    return eglChooseConfig(d, any, configs, size, num);
}

static EGLBoolean host_attrib(EGLConfig c, EGLint attr, EGLint *v)
{
    uint64_t s[3] = { (uint64_t)c, (uint64_t)(int64_t)attr, (uint64_t)v };
    return aoi_gl(AOI_EGL_CONFIG_ATTRIB, s) ? EGL_TRUE : EGL_FALSE;
}

EXPORT EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attr, EGLint *value)
{
    EGLint r = 0, g = 0, b = 0, a = 0;
    if (d != DPY) return fail(EGL_BAD_DISPLAY);
    if (!value) return fail(EGL_BAD_PARAMETER);
    switch (attr) {
    case EGL_RECORDABLE_ANDROID: case EGL_FRAMEBUFFER_TARGET_ANDROID:
        *value = EGL_TRUE;
        return EGL_TRUE;
    case EGL_NATIVE_VISUAL_ID:                                 /* the window format libEGL sets */
        host_attrib(c, EGL_RED_SIZE, &r); host_attrib(c, EGL_GREEN_SIZE, &g);
        host_attrib(c, EGL_BLUE_SIZE, &b); host_attrib(c, EGL_ALPHA_SIZE, &a);
        *value = r == 5 && g == 6 && b == 5 ? 4 /* RGB_565 */ : a ? 1 /* RGBA_8888 */ : 2 /* RGBX_8888 */;
        return EGL_TRUE;
    case EGL_SURFACE_TYPE:
        if (!host_attrib(c, attr, value)) return fail(EGL_BAD_CONFIG);
        if (*value & EGL_PBUFFER_BIT) *value |= EGL_WINDOW_BIT;
        return EGL_TRUE;
    }
    return host_attrib(c, attr, value) ? EGL_TRUE : fail(EGL_BAD_ATTRIBUTE);
}

/* ---------- surfaces ---------- */

/* system/window.h, nativebase.h (arm64 layout) */
struct native_base { int magic, version; void *reserved[4]; void (*incRef)(struct native_base *); void (*decRef)(struct native_base *); };
typedef struct { int version, numFds, numInts; int data[]; } native_handle_t;
struct ANativeWindowBuffer {
    struct native_base common;
    int width, height, stride, format, usage_deprecated;
    uintptr_t layerCount;
    void *reserved[1];
    const native_handle_t *handle;
    uint64_t usage;
    void *reserved_proc[7];
};
struct ANativeWindow {
    struct native_base common;
    const uint32_t flags;
    const int minSwapInterval, maxSwapInterval;
    const float xdpi, ydpi;
    long oem[4];
    int (*setSwapInterval)(struct ANativeWindow *, int);
    int (*dequeueBuffer_DEPRECATED)(struct ANativeWindow *, struct ANativeWindowBuffer **);
    int (*lockBuffer_DEPRECATED)(struct ANativeWindow *, struct ANativeWindowBuffer *);
    int (*queueBuffer_DEPRECATED)(struct ANativeWindow *, struct ANativeWindowBuffer *);
    int (*query)(const struct ANativeWindow *, int what, int *value);
    int (*perform)(struct ANativeWindow *, int operation, ...);
    int (*cancelBuffer_DEPRECATED)(struct ANativeWindow *, struct ANativeWindowBuffer *);
    int (*dequeueBuffer)(struct ANativeWindow *, struct ANativeWindowBuffer **, int *fenceFd);
    int (*queueBuffer)(struct ANativeWindow *, struct ANativeWindowBuffer *, int fenceFd);
    int (*cancelBuffer)(struct ANativeWindow *, struct ANativeWindowBuffer *, int fenceFd);
};
#define NATIVE_WINDOW_WIDTH 0
#define NATIVE_WINDOW_HEIGHT 1

#define MAXSURF 64
struct surface {
    uint64_t id;                    /* the host's surface, 0 if free */
    struct ANativeWindow *win;      /* a window surface's window */
    int w, h;
};
static struct surface surfaces[MAXSURF];

static struct surface *surface_new(void)
{
    int i;
    for (i = 0; i < MAXSURF; i++) if (!surfaces[i].id) return &surfaces[i];
    return 0;
}

static uint64_t sid(EGLSurface s) { return s ? ((struct surface *)s)->id : 0; }

static uint64_t host_surface(EGLConfig c, int w, int h)
{
    uint64_t s[3] = { (uint64_t)c, (uint64_t)(int64_t)w, (uint64_t)(int64_t)h };
    return aoi_gl(AOI_EGL_CREATE_SURFACE, s);
}

EXPORT EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, struct ANativeWindow *win, const EGLint *attribs)
{
    struct surface *sf = surface_new();
    int w = 0, h = 0;
    (void)attribs;
    if (d != DPY) { fail(EGL_BAD_DISPLAY); return 0; }
    if (!win) { fail(EGL_BAD_NATIVE_WINDOW); return 0; }
    if (!sf) { fail(EGL_BAD_ALLOC); return 0; }
    win->query(win, NATIVE_WINDOW_WIDTH, &w);
    win->query(win, NATIVE_WINDOW_HEIGHT, &h);
    sf->id = host_surface(c, w, h);
    if (!sf->id) { fail(EGL_BAD_MATCH); return 0; }
    sf->win = win; sf->w = w; sf->h = h;
    win->common.incRef(&win->common);
    return sf;
}

EXPORT EGLSurface eglCreatePlatformWindowSurface(EGLDisplay d, EGLConfig c, void *win, const long *attribs)
{
    (void)attribs;
    return eglCreateWindowSurface(d, c, win, 0);
}

EXPORT EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *attribs)
{
    struct surface *sf = surface_new();
    int w = 0, h = 0;
    if (d != DPY) { fail(EGL_BAD_DISPLAY); return 0; }
    if (!sf) { fail(EGL_BAD_ALLOC); return 0; }
    for (; attribs && attribs[0] != EGL_NONE; attribs += 2) {
        if (attribs[0] == EGL_WIDTH) w = attribs[1];
        if (attribs[0] == EGL_HEIGHT) h = attribs[1];
    }
    sf->id = host_surface(c, w, h);
    if (!sf->id) { fail(EGL_BAD_MATCH); return 0; }
    sf->win = 0; sf->w = w; sf->h = h;
    return sf;
}

EXPORT EGLSurface eglCreatePixmapSurface(EGLDisplay d, EGLConfig c, void *pixmap, const EGLint *attribs)
{
    (void)d; (void)c; (void)pixmap; (void)attribs;
    fail(EGL_BAD_MATCH);
    return 0;
}

EXPORT EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s)
{
    struct surface *sf = s;
    uint64_t a[1];
    (void)d;
    if (!sf || !sf->id) return fail(EGL_BAD_SURFACE);
    a[0] = sf->id;
    aoi_gl(AOI_EGL_DESTROY_SURFACE, a);
    if (sf->win) sf->win->common.decRef(&sf->win->common);
    sf->id = 0; sf->win = 0;
    return EGL_TRUE;
}

EXPORT EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *value)
{
    struct surface *sf = s;
    (void)d;
    if (!sf || !sf->id) return fail(EGL_BAD_SURFACE);
    switch (attr) {
    case EGL_WIDTH: *value = sf->w; return EGL_TRUE;
    case EGL_HEIGHT: *value = sf->h; return EGL_TRUE;
    case EGL_RENDER_BUFFER: *value = EGL_BACK_BUFFER; return EGL_TRUE;
    case EGL_SWAP_BEHAVIOR: *value = EGL_BUFFER_DESTROYED; return EGL_TRUE;
    case EGL_CONFIG_ID: *value = 0; return EGL_TRUE;
    }
    *value = 0;
    return EGL_TRUE;
}

EXPORT EGLBoolean eglSurfaceAttrib(EGLDisplay d, EGLSurface s, EGLint attr, EGLint value)
{
    (void)d; (void)s; (void)attr; (void)value;
    return EGL_TRUE;
}

EXPORT EGLBoolean eglSwapInterval(EGLDisplay d, EGLint interval) { (void)d; (void)interval; return EGL_TRUE; }

/* A window's next buffer gets the frame: dequeue, the host writes it, queue. */
EXPORT EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    struct surface *sf = s;
    struct ANativeWindowBuffer *buf = 0;
    int fence = -1, w = 0, h = 0;
    const native_handle_t *nh;
    uint64_t a[4], addr;
    (void)d;
    if (!sf || !sf->id) return fail(EGL_BAD_SURFACE);
    if (!sf->win) return EGL_TRUE;                             /* a pbuffer: nothing to show */
    if (sf->win->dequeueBuffer(sf->win, &buf, &fence) != 0 || !buf) return fail(EGL_BAD_NATIVE_WINDOW);
    if (fence >= 0) sys1(NR_CLOSE, fence);                     /* host buffers are never in flight */
    nh = buf->handle;
    if (nh && nh->numInts >= AOI_GB_INTS && nh->data[nh->numFds + AOI_GB_I_MAGIC] == (int)AOI_GB_MAGIC &&
        buf->width == sf->w && buf->height == sf->h) {
        addr = (uint64_t)(uint32_t)nh->data[nh->numFds + AOI_GB_I_ADDR_LO] |
               (uint64_t)(uint32_t)nh->data[nh->numFds + AOI_GB_I_ADDR_HI] << 32;
        a[0] = sf->id; a[1] = addr; a[2] = (uint64_t)(int64_t)buf->stride; a[3] = (uint64_t)(int64_t)buf->format;
        aoi_gl(AOI_EGL_READBACK, a);
    }
    sf->win->queueBuffer(sf->win, buf, -1);
    sf->win->query(sf->win, NATIVE_WINDOW_WIDTH, &w);       /* the window resized: so does the surface */
    sf->win->query(sf->win, NATIVE_WINDOW_HEIGHT, &h);
    if (w > 0 && h > 0 && (w != sf->w || h != sf->h)) {
        uint64_t r[3] = { sf->id, (uint64_t)(int64_t)w, (uint64_t)(int64_t)h };
        if (aoi_gl(AOI_EGL_RESIZE_SURFACE, r)) { sf->w = w; sf->h = h; }
    }
    return EGL_TRUE;
}

EXPORT EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay d, EGLSurface s, const EGLint *rects, EGLint n)
{
    (void)rects; (void)n;
    return eglSwapBuffers(d, s);
}

EXPORT EGLBoolean eglPresentationTimeANDROID(EGLDisplay d, EGLSurface s, long long t)
{
    (void)d; (void)s; (void)t;
    return EGL_TRUE;
}

EXPORT EGLBoolean eglCopyBuffers(EGLDisplay d, EGLSurface s, void *target)
{
    (void)d; (void)s; (void)target;
    return fail(EGL_BAD_MATCH);
}

EXPORT EGLBoolean eglBindTexImage(EGLDisplay d, EGLSurface s, EGLint buffer) { (void)d; (void)s; (void)buffer; return fail(EGL_BAD_MATCH); }
EXPORT EGLBoolean eglReleaseTexImage(EGLDisplay d, EGLSurface s, EGLint buffer) { (void)d; (void)s; (void)buffer; return fail(EGL_BAD_MATCH); }
EXPORT EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay d, EGLenum t, EGLClientBuffer b, EGLConfig c, const EGLint *a)
{
    (void)d; (void)t; (void)b; (void)c; (void)a;
    fail(EGL_BAD_PARAMETER);
    return 0;
}

/* ---------- contexts ---------- */

EXPORT EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attribs)
{
    uint64_t s[3] = { (uint64_t)c, (uint64_t)share, (uint64_t)attribs }, id;
    if (d != DPY || !initialized) { fail(EGL_NOT_INITIALIZED); return 0; }
    id = aoi_gl(AOI_EGL_CREATE_CONTEXT, s);
    if (!id) fail(EGL_BAD_MATCH);
    return (EGLContext)id;
}

EXPORT EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext ctx)
{
    uint64_t s[1] = { (uint64_t)ctx };
    (void)d;
    return aoi_gl(AOI_EGL_DESTROY_CONTEXT, s) ? EGL_TRUE : EGL_FALSE;
}

EXPORT EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    uint64_t s[3] = { sid(draw), sid(read), (uint64_t)ctx };
    (void)d;
    return aoi_gl(AOI_EGL_MAKE_CURRENT, s) ? EGL_TRUE : fail(EGL_BAD_MATCH);
}

EXPORT EGLBoolean eglQueryContext(EGLDisplay d, EGLContext ctx, EGLint attr, EGLint *value)
{
    uint64_t s[3] = { (uint64_t)ctx, (uint64_t)(int64_t)attr, (uint64_t)value };
    (void)d;
    return aoi_gl(AOI_EGL_QUERY_CONTEXT, s) ? EGL_TRUE : fail(EGL_BAD_ATTRIBUTE);
}

/* libEGL keeps the current context and surfaces itself; these are its fallbacks. */
EXPORT EGLContext eglGetCurrentContext(void) { return 0; }
EXPORT EGLSurface eglGetCurrentSurface(EGLint which) { (void)which; return 0; }
EXPORT EGLDisplay eglGetCurrentDisplay(void) { return DPY; }

EXPORT EGLBoolean eglWaitClient(void) { glFinish(); return EGL_TRUE; }
EXPORT EGLBoolean eglWaitGL(void) { glFinish(); return EGL_TRUE; }
EXPORT EGLBoolean eglWaitNative(EGLint engine) { (void)engine; return EGL_TRUE; }
EXPORT EGLBoolean eglReleaseThread(void)
{
    eglMakeCurrent(DPY, 0, 0, 0);
    return EGL_TRUE;
}

EXPORT EGLproc eglGetProcAddress(const char *name)
{
    static const struct { const char *name; void *fn; } egl[] = {
        { "eglSwapBuffersWithDamageKHR", (void *)eglSwapBuffersWithDamageKHR },
        { "eglPresentationTimeANDROID", (void *)eglPresentationTimeANDROID },
    };
    unsigned i;
    if (!name) return 0;
    for (i = 0; i < sizeof aoi_gl_procs / sizeof aoi_gl_procs[0]; i++)
        if (streq(aoi_gl_procs[i].name, name)) return (EGLproc)aoi_gl_procs[i].fn;
    for (i = 0; i < sizeof egl / sizeof egl[0]; i++)
        if (streq(egl[i].name, name)) return (EGLproc)egl[i].fn;
    return 0;
}
