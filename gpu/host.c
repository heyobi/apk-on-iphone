/* The guest's OpenGL ES on the host's GPU (core/gpu.h).
 *
 * The guest driver (guest/gles.c) turns each GLES call into the syscall AOI_SYS_GL;
 * here it becomes the same call on the host's GLES (Mesa's llvmpipe on Linux, ANGLE
 * on Metal on iOS), made current with the guest thread's context. Pointer
 * arguments are guest memory, contiguous only within 2 MiB chunks (core/vm.h): one
 * inside a chunk is used in place, one across chunks is copied to a temporary (and
 * back, for outputs) around the call; gpu/gl_gen.h (tools/glgen.py) knows each
 * pointer's length. Calls whose pointers outlive the call are written by hand below:
 * client-side vertex arrays (copied at draw time), mapped buffers (a guest copy of
 * the range, written back on flush/unmap), strings the GL returns (copied into guest
 * memory once), sync objects (numbered), string arrays.
 *
 * Every guest surface is a host pbuffer: a window surface's frame goes to the window
 * at eglSwapBuffers, when the guest's EGL asks for AOI_EGL_READBACK into the buffer
 * it dequeued (core/gralloc.c buffers are guest memory). One host thread runs every
 * guest thread (green threads), so the host context follows the calling thread. */
#define _POSIX_C_SOURCE 200809L        /* clock_gettime */
#include "host.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/gpu.h"
#include "../core/proc.h"
#include "../core/vm.h"

#define MAXCFG 512
#define MAXCTX 64
#define MAXSURF 128
#define MAXSYNC 1024
#define MAXATTR 32
#define MAXTEMP 64
#define MAXMAP 32
#define MAXTHR 128

struct attr {                       /* a vertex attribute array of the default VAO */
    int enabled, client, integer, size, normalized;
    GLenum type;
    GLsizei stride;
    uint64_t ga;                    /* the guest pointer, when client */
};

struct mapping {                    /* a mapped buffer range: the guest's copy */
    GLuint buffer;
    void *hp;                       /* the host mapping */
    uint64_t ga, glen;              /* the guest copy (whole pages) */
    GLsizeiptr length;
    GLbitfield access;
};

struct ctx {
    EGLContext h;
    GLuint vao;
    struct attr attr[MAXATTR];
    struct mapping map[MAXMAP];
};

struct surf {
    EGLSurface h;
    EGLConfig cfg;
    int w, h_;
};

struct thr { int tid, draw, read, ctx; };

static EGLDisplay dpy = EGL_NO_DISPLAY;
static int inited, ncfg;
static EGLConfig cfg[MAXCFG];
static struct ctx *ctxs[MAXCTX];
static struct surf surfs[MAXSURF];
static GLsync syncs[MAXSYNC];
static struct thr thr[MAXTHR];
static int hdraw = -1, hread = -1, hctx = -1;   /* what the host has current */
static int trace = -1;                           /* AOI_GL_TRACE: every GL call on stderr */
struct cached { const char *host; uint64_t ga; };
static struct cached strs[512];                  /* strings the GL returned, in guest memory */
static int nstrs;

/* Lines for the process's log (on iOS the only output anyone sees). */
static void say(struct aoi_proc *p, const char *fmt, ...)
{
    va_list ap;
    FILE *f = p && p->log ? p->log : stderr;
    va_start(ap, fmt);
    fputs("I/aoi-gpu: ", f);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
}

struct temp { void *host; uint64_t ga, len; int out; };
struct gl_call {
    struct aoi_proc *p;
    struct thr *t;
    struct ctx *ctx;
    int ntemp;
    struct temp temp[MAXTEMP];
};

/* ---------- guest memory ---------- */

/* Copies what is accessible of [ga, ga+len) (in chunk-sized spans); the rest of dst
 * stays as it is. Returns the bytes copied. */
static uint64_t copy_in(struct aoi_vm *vm, uint64_t ga, void *dst, uint64_t len)
{
    uint64_t done = 0, n;
    while (done < len) {
        uint8_t *h = aoi_vm_span(vm, ga + done, len - done, 0, &n);
        if (!h) break;
        memcpy((uint8_t *)dst + done, h, n);
        done += n;
    }
    return done;
}

static uint64_t copy_out(struct aoi_vm *vm, uint64_t ga, const void *src, uint64_t len)
{
    uint64_t done = 0, n;
    while (done < len) {
        uint8_t *h = aoi_vm_span(vm, ga + done, len - done, AOI_PROT_W, &n);
        if (!h) break;
        memcpy(h, (const uint8_t *)src + done, n);
        done += n;
    }
    return done;
}

static void *temp(struct gl_call *c, uint64_t ga, uint64_t len, int out)
{
    void *h;
    if (c->ntemp == MAXTEMP) return NULL;
    h = calloc(1, len ? len : 1);
    if (!h) return NULL;
    c->temp[c->ntemp++] = (struct temp){ h, ga, len, out };
    return h;
}

/* A guest pointer for a call: in place when it lies in one chunk, else a copy. */
static void *gl_ptr(struct gl_call *c, uint64_t ga, GLsizeiptr len, int out, const char *warn)
{
    void *h;
    if (!ga) return NULL;
    if (warn) {
        static char seen[64][64];
        int k;
        for (k = 0; k < 64 && seen[k][0] && strcmp(seen[k], warn); k++) {}
        if (k < 64 && !seen[k][0]) {
            snprintf(seen[k], sizeof seen[k], "%s", warn);
            say(c->p, "%s: pointer of unknown length (4 KiB assumed)", warn);
        }
    }
    if (len <= 0) len = 1;
    h = aoi_vm_ptr(&c->p->vm, ga, (uint64_t)len, out ? AOI_PROT_W : 0);
    if (h) return h;
    h = temp(c, ga, (uint64_t)len, out);
    if (h) copy_in(&c->p->vm, ga, h, (uint64_t)len);
    return h;
}

/* A string argument: NUL-terminated (n < 0) or n bytes; always a NUL-terminated copy. */
static const char *gl_str(struct gl_call *c, uint64_t ga, GLsizei n)
{
    char *h;
    uint64_t len = 0;
    if (!ga) return NULL;
    if (n >= 0) len = (uint64_t)n;
    else {
        uint64_t sn;
        for (;;) {
            const char *s = (const char *)aoi_vm_span(&c->p->vm, ga + len, 1 << 20, 0, &sn);
            const char *z;
            if (!s) break;
            z = memchr(s, 0, sn);
            if (z) { len += (uint64_t)(z - s); break; }
            len += sn;
            if (len > (64u << 20)) break;
        }
    }
    h = temp(c, ga, len + 1, 0);
    if (!h) return "";
    copy_in(&c->p->vm, ga, h, len);
    h[len] = 0;
    return h;
}

#define gl_strn(c, ga, n) gl_str(c, ga, n)

static void finish(struct gl_call *c)
{
    int i;
    for (i = 0; i < c->ntemp; i++) {
        if (c->temp[i].out) copy_out(&c->p->vm, c->temp[i].ga, c->temp[i].host, c->temp[i].len);
        free(c->temp[i].host);
    }
    c->ntemp = 0;
}

/* Guest memory the host hands out (returned strings, mapped buffer copies). */
static uint64_t guest_alloc(struct aoi_proc *p, uint64_t len)
{
    uint64_t a = aoi_vm_map(&p->vm, 0, (len + AOI_VM_PAGE - 1) & ~(uint64_t)(AOI_VM_PAGE - 1),
                            AOI_PROT_R | AOI_PROT_W, 0);
    return (int64_t)a < 0 ? 0 : a;
}

/* ---------- state the generated code asks for ---------- */

static int gl_bound(GLenum binding)
{
    GLint b = 0;
    glGetIntegerv(binding, &b);
    return b != 0;
}

static float F(uint64_t s) { float f; uint32_t u = (uint32_t)s; memcpy(&f, &u, 4); return f; }

static int components(GLenum format)
{
    switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT:
    case GL_STENCIL_INDEX: return 1;
    case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA: case GL_DEPTH_STENCIL: return 2;
    case GL_RGB: case GL_RGB_INTEGER: return 3;
    default: return 4;                                             /* RGBA, RGBA_INTEGER, BGRA_EXT */
    }
}

static int pixel_bytes(GLenum format, GLenum type)
{
    int n = components(format);
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return n;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: case 0x8D61 /* HALF_FLOAT_OES */: return 2 * n;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4 * n;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
    default: return 4;                                             /* 2_10_10_10, 24_8, 10F_11F_11F, 5_9_9_9 */
    }
}

static GLint geti(GLenum pname) { GLint v = 0; glGetIntegerv(pname, &v); return v; }

static GLsizeiptr img_size(GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, int pack)
{
    GLint align = geti(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT);
    GLint rowlen = geti(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH);
    GLint skipp = geti(pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS);
    GLint skipr = geti(pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS);
    GLint imgh = pack ? 0 : geti(GL_UNPACK_IMAGE_HEIGHT), skipi = pack ? 0 : geti(GL_UNPACK_SKIP_IMAGES);
    GLsizeiptr bpp = pixel_bytes(format, type), row, rows;
    if (w <= 0 || h <= 0 || d <= 0) return 0;
    if (align < 1) align = 1;
    row = (GLsizeiptr)(rowlen > 0 ? rowlen : w) * bpp;
    row = (row + align - 1) / align * align;
    rows = (GLsizeiptr)(imgh > 0 ? imgh : h);
    return ((GLsizeiptr)(skipi + d - 1) * rows + skipr + h - 1) * row + (GLsizeiptr)(skipp + w) * bpp;
}

#define img_unpack(w, h, d, f, t) img_size(w, h, d, f, t, 0)
#define img_pack(w, h, f, t) img_size(w, h, 1, f, t, 1)
#define strn(n) n

/* ---------- calls written by hand ---------- */

static uint64_t guest_string(struct gl_call *c, const char *s)
{
    size_t len;
    uint64_t ga;
    int i;
    if (!s) return 0;
    for (i = 0; i < nstrs; i++) if (strs[i].host == s) return strs[i].ga;
    len = strlen(s) + 1;
    ga = guest_alloc(c->p, len);
    if (!ga) return 0;
    copy_out(&c->p->vm, ga, s, len);
    if (nstrs < 512) strs[nstrs++] = (struct cached){ s, ga };
    return ga;
}

/* Extensions the guest is told about: those that add only enums or shader features,
 * or whose functions are core GLES 3.2 ones under another name (guest/gles.c's
 * eglGetProcAddress maps glFooEXT/OES/KHR to glFoo). The rest (EGLImage, memory
 * objects, timer queries, multi-draw...) would send apps to functions we lack. */
static const char *const ext_ok[] = {
    "GL_APPLE_sync", "GL_APPLE_texture_max_level", "GL_ANGLE_pack_reverse_row_order",
    "GL_ANGLE_texture_compression_dxt3", "GL_ANGLE_texture_compression_dxt5",
    "GL_EXT_blend_minmax", "GL_EXT_color_buffer_float", "GL_EXT_color_buffer_half_float",
    "GL_EXT_compressed_ETC1_RGB8_sub_texture", "GL_EXT_conservative_depth", "GL_EXT_copy_image",
    "GL_EXT_depth_clamp", "GL_EXT_discard_framebuffer", "GL_EXT_draw_buffers", "GL_EXT_draw_buffers_indexed",
    "GL_EXT_draw_instanced", "GL_EXT_float_blend", "GL_EXT_frag_depth", "GL_EXT_geometry_point_size",
    "GL_EXT_geometry_shader", "GL_EXT_gpu_shader5", "GL_EXT_instanced_arrays", "GL_EXT_map_buffer_range",
    "GL_EXT_occlusion_query_boolean", "GL_EXT_primitive_bounding_box", "GL_EXT_read_format_bgra",
    "GL_EXT_render_snorm", "GL_EXT_robustness", "GL_EXT_sRGB_write_control", "GL_EXT_separate_shader_objects",
    "GL_EXT_shader_framebuffer_fetch", "GL_EXT_shader_group_vote", "GL_EXT_shader_implicit_conversions",
    "GL_EXT_shader_integer_mix", "GL_EXT_shader_io_blocks", "GL_EXT_shadow_samplers",
    "GL_EXT_tessellation_point_size", "GL_EXT_tessellation_shader", "GL_EXT_texture_border_clamp",
    "GL_EXT_texture_buffer", "GL_EXT_texture_compression_bptc", "GL_EXT_texture_compression_dxt1",
    "GL_EXT_texture_compression_rgtc", "GL_EXT_texture_compression_s3tc", "GL_EXT_texture_compression_s3tc_srgb",
    "GL_EXT_texture_cube_map_array", "GL_EXT_texture_filter_anisotropic", "GL_EXT_texture_filter_minmax",
    "GL_EXT_texture_format_BGRA8888", "GL_EXT_texture_mirror_clamp_to_edge", "GL_EXT_texture_norm16",
    "GL_EXT_texture_query_lod", "GL_EXT_texture_rg", "GL_EXT_texture_sRGB_R8", "GL_EXT_texture_sRGB_RG8",
    "GL_EXT_texture_sRGB_decode", "GL_EXT_texture_shadow_lod", "GL_EXT_texture_storage",
    "GL_EXT_texture_type_2_10_10_10_REV", "GL_EXT_unpack_subimage", "GL_KHR_blend_equation_advanced",
    "GL_KHR_blend_equation_advanced_coherent", "GL_KHR_context_flush_control", "GL_KHR_debug",
    "GL_KHR_robust_buffer_access_behavior", "GL_KHR_robustness", "GL_KHR_texture_compression_astc_ldr",
    "GL_NV_draw_buffers", "GL_NV_fbo_color_attachments", "GL_NV_generate_mipmap_sRGB", "GL_NV_image_formats",
    "GL_NV_pack_subimage", "GL_NV_pixel_buffer_object", "GL_NV_read_buffer", "GL_NV_read_depth",
    "GL_NV_read_depth_stencil", "GL_NV_read_stencil", "GL_NV_shader_noperspective_interpolation",
    "GL_OES_compressed_ETC1_RGB8_texture", "GL_OES_copy_image", "GL_OES_depth24", "GL_OES_depth_texture",
    "GL_OES_depth_texture_cube_map", "GL_OES_draw_buffers_indexed", "GL_OES_element_index_uint",
    "GL_OES_fbo_render_mipmap", "GL_OES_geometry_point_size", "GL_OES_geometry_shader", "GL_OES_get_program_binary",
    "GL_OES_gpu_shader5", "GL_OES_packed_depth_stencil", "GL_OES_primitive_bounding_box",
    "GL_OES_required_internalformat", "GL_OES_rgb8_rgba8", "GL_OES_sample_shading", "GL_OES_sample_variables",
    "GL_OES_shader_image_atomic", "GL_OES_shader_io_blocks", "GL_OES_shader_multisample_interpolation",
    "GL_OES_standard_derivatives", "GL_OES_stencil8", "GL_OES_surfaceless_context", "GL_OES_tessellation_point_size",
    "GL_OES_tessellation_shader", "GL_OES_texture_border_clamp", "GL_OES_texture_buffer",
    "GL_OES_texture_cube_map_array", "GL_OES_texture_float", "GL_OES_texture_float_linear",
    "GL_OES_texture_half_float", "GL_OES_texture_half_float_linear", "GL_OES_texture_npot",
    "GL_OES_texture_stencil8", "GL_OES_texture_storage_multisample_2d_array", "GL_OES_vertex_array_object",
    "GL_OES_vertex_half_float",
};

static const char *exts[256];       /* the host's extensions the guest sees */
static int nexts = -1;
static char *ext_string;

static void load_extensions(void)
{
    GLint n = 0, i;
    size_t len = 1, k;
    if (nexts >= 0) return;
    nexts = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (i = 0; i < n && nexts < 256; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        for (k = 0; e && k < sizeof ext_ok / sizeof ext_ok[0]; k++)
            if (!strcmp(e, ext_ok[k])) { exts[nexts++] = ext_ok[k]; len += strlen(e) + 1; break; }
    }
    ext_string = calloc(1, len);
    for (i = 0; ext_string && i < nexts; i++) {
        if (i) strcat(ext_string, " ");
        strcat(ext_string, exts[i]);
    }
}

static uint64_t special_glGetString(struct gl_call *c, const uint64_t *s)
{
    if ((GLenum)s[0] == GL_EXTENSIONS) { load_extensions(); return guest_string(c, ext_string); }
    return guest_string(c, (const char *)glGetString((GLenum)s[0]));
}

static uint64_t special_glGetStringi(struct gl_call *c, const uint64_t *s)
{
    if ((GLenum)s[0] == GL_EXTENSIONS) {
        load_extensions();
        return (GLuint)s[1] < (GLuint)nexts ? guest_string(c, exts[s[1]]) : 0;
    }
    return guest_string(c, (const char *)glGetStringi((GLenum)s[0], (GLuint)s[1]));
}

static GLenum binding_of(GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
    case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
    case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
    case GL_UNIFORM_BUFFER: return GL_UNIFORM_BUFFER_BINDING;
    case GL_TRANSFORM_FEEDBACK_BUFFER: return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING;
    case GL_COPY_READ_BUFFER: return GL_COPY_READ_BUFFER_BINDING;
    case GL_COPY_WRITE_BUFFER: return GL_COPY_WRITE_BUFFER_BINDING;
    case GL_SHADER_STORAGE_BUFFER: return GL_SHADER_STORAGE_BUFFER_BINDING;
    case GL_DRAW_INDIRECT_BUFFER: return GL_DRAW_INDIRECT_BUFFER_BINDING;
    case GL_DISPATCH_INDIRECT_BUFFER: return GL_DISPATCH_INDIRECT_BUFFER_BINDING;
    case GL_ATOMIC_COUNTER_BUFFER: return GL_ATOMIC_COUNTER_BUFFER_BINDING;
    case GL_TEXTURE_BUFFER: return GL_TEXTURE_BUFFER_BINDING;
    default: return 0;
    }
}

static struct mapping *mapping_of(struct gl_call *c, GLenum target)
{
    GLuint b = (GLuint)geti(binding_of(target));
    int i;
    for (i = 0; i < MAXMAP; i++) if (c->ctx->map[i].hp && c->ctx->map[i].buffer == b) return &c->ctx->map[i];
    return NULL;
}

static uint64_t special_glMapBufferRange(struct gl_call *c, const uint64_t *s)
{
    GLenum target = (GLenum)s[0];
    GLintptr offset = (GLintptr)s[1];
    GLsizeiptr length = (GLsizeiptr)s[2];
    GLbitfield access = (GLbitfield)s[3];
    struct mapping *m = NULL;
    void *hp;
    uint64_t ga;
    int i;
    for (i = 0; i < MAXMAP; i++) if (!c->ctx->map[i].hp) { m = &c->ctx->map[i]; break; }
    if (!m || length <= 0) return 0;
    hp = glMapBufferRange(target, offset, length, access);
    if (!hp) return 0;
    ga = guest_alloc(c->p, (uint64_t)length);
    if (!ga) { glUnmapBuffer(target); return 0; }
    if (!(access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT)))
        copy_out(&c->p->vm, ga, hp, (uint64_t)length);
    *m = (struct mapping){ (GLuint)geti(binding_of(target)), hp, ga,
                           ((uint64_t)length + AOI_VM_PAGE - 1) & ~(uint64_t)(AOI_VM_PAGE - 1), length, access };
    return ga;
}

static uint64_t special_glFlushMappedBufferRange(struct gl_call *c, const uint64_t *s)
{
    GLenum target = (GLenum)s[0];
    GLintptr offset = (GLintptr)s[1];
    GLsizeiptr length = (GLsizeiptr)s[2];
    struct mapping *m = mapping_of(c, target);
    if (m && offset >= 0 && offset + length <= m->length)
        copy_in(&c->p->vm, m->ga + (uint64_t)offset, (uint8_t *)m->hp + offset, (uint64_t)length);
    glFlushMappedBufferRange(target, offset, length);
    return 0;
}

static uint64_t special_glUnmapBuffer(struct gl_call *c, const uint64_t *s)
{
    GLenum target = (GLenum)s[0];
    struct mapping *m = mapping_of(c, target);
    GLboolean r;
    if (m) {
        if ((m->access & GL_MAP_WRITE_BIT) && !(m->access & GL_MAP_FLUSH_EXPLICIT_BIT))
            copy_in(&c->p->vm, m->ga, m->hp, (uint64_t)m->length);
        aoi_vm_unmap(&c->p->vm, m->ga, m->glen);
        memset(m, 0, sizeof *m);
    }
    r = glUnmapBuffer(target);
    return r;
}

static void put_ptr(struct gl_call *c, uint64_t ga, uint64_t v) { if (ga) copy_out(&c->p->vm, ga, &v, 8); }

static uint64_t special_glGetBufferPointerv(struct gl_call *c, const uint64_t *s)
{
    struct mapping *m = mapping_of(c, (GLenum)s[0]);
    put_ptr(c, s[2], m && (GLenum)s[1] == GL_BUFFER_MAP_POINTER ? m->ga : 0);
    return 0;
}

static uint64_t special_glGetPointerv(struct gl_call *c, const uint64_t *s)
{
    put_ptr(c, s[1], 0);
    return 0;
}

static uint64_t special_glGetVertexAttribPointerv(struct gl_call *c, const uint64_t *s)
{
    GLuint i = (GLuint)s[0];
    void *v = NULL;
    if (i < MAXATTR && c->ctx->vao == 0 && c->ctx->attr[i].client) { put_ptr(c, s[2], c->ctx->attr[i].ga); return 0; }
    glGetVertexAttribPointerv(i, (GLenum)s[1], &v);
    put_ptr(c, s[2], (uint64_t)(uintptr_t)v);
    return 0;
}

/* An array of count guest strings (with optional lengths): host copies. */
static const GLchar **strings(struct gl_call *c, uint64_t ga, GLsizei count, uint64_t lens)
{
    const GLchar **v;
    GLsizei i;
    if (count <= 0 || count > 65536) return NULL;
    v = temp(c, 0, sizeof *v * (uint64_t)count, 0);
    if (!v) return NULL;
    for (i = 0; i < count; i++) {
        uint64_t sp = 0;
        int32_t n = -1;
        copy_in(&c->p->vm, ga + 8 * (uint64_t)i, &sp, 8);
        if (lens) copy_in(&c->p->vm, lens + 4 * (uint64_t)i, &n, 4);
        v[i] = gl_str(c, sp, n);
    }
    return v;
}

static uint64_t special_glShaderSource(struct gl_call *c, const uint64_t *s)
{
    GLsizei count = (GLsizei)s[1];
    const GLchar **v = strings(c, s[2], count, s[3]);
    if (v) glShaderSource((GLuint)s[0], count, v, NULL);
    return 0;
}

static uint64_t special_glTransformFeedbackVaryings(struct gl_call *c, const uint64_t *s)
{
    GLsizei count = (GLsizei)s[1];
    const GLchar **v = strings(c, s[2], count, 0);
    if (v || !count) glTransformFeedbackVaryings((GLuint)s[0], count, v, (GLenum)s[3]);
    return 0;
}

static uint64_t special_glGetUniformIndices(struct gl_call *c, const uint64_t *s)
{
    GLsizei count = (GLsizei)s[1];
    const GLchar **v = strings(c, s[2], count, 0);
    GLuint *out = gl_ptr(c, s[3], 4 * (GLsizeiptr)count, 1, NULL);
    if (v && out) glGetUniformIndices((GLuint)s[0], count, v, out);
    return 0;
}

static uint64_t special_glCreateShaderProgramv(struct gl_call *c, const uint64_t *s)
{
    GLsizei count = (GLsizei)s[1];
    const GLchar **v = strings(c, s[2], count, 0);
    return v ? glCreateShaderProgramv((GLenum)s[0], count, v) : 0;
}

/* Vertex attributes: the default VAO's arrays may point into guest memory; they are
 * recorded and copied at each draw (what the draw reads). */
static uint64_t vertex_pointer(struct gl_call *c, const uint64_t *s, int integer)
{
    GLuint i = (GLuint)s[0];
    GLint size = (GLint)s[1];
    GLenum type = (GLenum)s[2];
    GLboolean norm = integer ? GL_FALSE : (GLboolean)s[3];
    GLsizei stride = (GLsizei)s[integer ? 3 : 4];
    uint64_t ptr = s[integer ? 4 : 5];
    int client = c->ctx->vao == 0 && !gl_bound(GL_ARRAY_BUFFER_BINDING) && ptr;
    if (i < MAXATTR && c->ctx->vao == 0)
        c->ctx->attr[i] = (struct attr){ c->ctx->attr[i].enabled, client, integer, size, norm, type, stride, ptr };
    if (client) return 0;                                          /* set at the draw */
    if (integer) glVertexAttribIPointer(i, size, type, stride, (const void *)(uintptr_t)ptr);
    else glVertexAttribPointer(i, size, type, norm, stride, (const void *)(uintptr_t)ptr);
    return 0;
}

static uint64_t special_glVertexAttribPointer(struct gl_call *c, const uint64_t *s) { return vertex_pointer(c, s, 0); }
static uint64_t special_glVertexAttribIPointer(struct gl_call *c, const uint64_t *s) { return vertex_pointer(c, s, 1); }

static uint64_t special_glEnableVertexAttribArray(struct gl_call *c, const uint64_t *s)
{
    if ((GLuint)s[0] < MAXATTR && c->ctx->vao == 0) c->ctx->attr[s[0]].enabled = 1;
    glEnableVertexAttribArray((GLuint)s[0]);
    return 0;
}

static uint64_t special_glDisableVertexAttribArray(struct gl_call *c, const uint64_t *s)
{
    if ((GLuint)s[0] < MAXATTR && c->ctx->vao == 0) c->ctx->attr[s[0]].enabled = 0;
    glDisableVertexAttribArray((GLuint)s[0]);
    return 0;
}

static uint64_t special_glBindVertexArray(struct gl_call *c, const uint64_t *s)
{
    c->ctx->vao = (GLuint)s[0];
    glBindVertexArray((GLuint)s[0]);
    return 0;
}

static int type_bytes(GLenum type)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: case 0x8D61: return 2;
    default: return 4;
    }
}

static int any_client(struct gl_call *c)
{
    int i;
    if (c->ctx->vao) return 0;
    for (i = 0; i < MAXATTR; i++) if (c->ctx->attr[i].enabled && c->ctx->attr[i].client) return 1;
    return 0;
}

/* Points the client arrays at host copies of the vertices a draw reads: indices up
 * to `vertices` (exclusive), instances up to `instances` for divided attributes. */
static void client_arrays(struct gl_call *c, uint64_t vertices, GLsizei instances)
{
    GLint buf = geti(GL_ARRAY_BUFFER_BINDING);
    int i;
    if (buf) glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (i = 0; i < MAXATTR; i++) {
        struct attr *a = &c->ctx->attr[i];
        GLint div = 0;
        uint64_t n, esz, stride, len;
        void *h;
        if (!a->enabled || !a->client) continue;
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &div);
        n = div ? ((uint64_t)instances + (uint64_t)div - 1) / (uint64_t)div : vertices;
        if (!n) n = 1;
        esz = (uint64_t)type_bytes(a->type) * (uint64_t)(a->size > 0 ? a->size : 4);
        stride = a->stride ? (uint64_t)a->stride : esz;
        len = (n - 1) * stride + esz;
        h = gl_ptr(c, a->ga, (GLsizeiptr)len, 0, NULL);
        if (a->integer) glVertexAttribIPointer((GLuint)i, a->size, a->type, a->stride, h);
        else glVertexAttribPointer((GLuint)i, a->size, a->type, (GLboolean)a->normalized, a->stride, h);
    }
    if (buf) glBindBuffer(GL_ARRAY_BUFFER, (GLuint)buf);
}

/* Indices: the host copy of a client index array, or the offset into the bound
 * element buffer; *max gets the largest index (+ basevertex) when client arrays need it. */
static const void *indices(struct gl_call *c, GLsizei count, GLenum type, uint64_t ga, GLint basevertex,
                           uint64_t *max)
{
    int isz = type_bytes(type);
    const void *h;
    uint64_t m = 0;
    GLsizei k;
    int bound = gl_bound(GL_ELEMENT_ARRAY_BUFFER_BINDING);
    const uint8_t *data;
    if (bound) h = (const void *)(uintptr_t)ga;
    else h = gl_ptr(c, ga, (GLsizeiptr)count * isz, 0, NULL);
    if (!max) return h;
    data = bound ? glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)ga, (GLsizeiptr)count * isz, GL_MAP_READ_BIT)
                 : h;
    if (data) {
        for (k = 0; k < count; k++) {
            uint64_t v = isz == 1 ? data[k] : isz == 2 ? ((const uint16_t *)data)[k] : ((const uint32_t *)data)[k];
            if (v > m) m = v;
        }
        if (bound) glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    }
    *max = m + (uint64_t)(basevertex > 0 ? basevertex : 0) + 1;
    return h;
}

static uint64_t special_glDrawArrays(struct gl_call *c, const uint64_t *s)
{
    if (any_client(c)) client_arrays(c, (uint64_t)(GLint)s[1] + (uint64_t)(GLsizei)s[2], 1);
    glDrawArrays((GLenum)s[0], (GLint)s[1], (GLsizei)s[2]);
    return 0;
}

static uint64_t special_glDrawArraysInstanced(struct gl_call *c, const uint64_t *s)
{
    if (any_client(c)) client_arrays(c, (uint64_t)(GLint)s[1] + (uint64_t)(GLsizei)s[2], (GLsizei)s[3]);
    glDrawArraysInstanced((GLenum)s[0], (GLint)s[1], (GLsizei)s[2], (GLsizei)s[3]);
    return 0;
}

static uint64_t draw_elements(struct gl_call *c, GLenum mode, GLsizei count, GLenum type, uint64_t ga,
                              GLsizei instances, GLint basevertex, int which, GLuint start, GLuint end)
{
    uint64_t max = 0;
    int client = any_client(c);
    const void *idx = indices(c, count, type, ga, basevertex, client ? &max : NULL);
    if (client) client_arrays(c, max, instances);
    switch (which) {
    case 0: glDrawElements(mode, count, type, idx); break;
    case 1: glDrawRangeElements(mode, start, end, count, type, idx); break;
    case 2: glDrawElementsInstanced(mode, count, type, idx, instances); break;
    case 3: glDrawElementsBaseVertex(mode, count, type, idx, basevertex); break;
    case 4: glDrawRangeElementsBaseVertex(mode, start, end, count, type, idx, basevertex); break;
    case 5: glDrawElementsInstancedBaseVertex(mode, count, type, idx, instances, basevertex); break;
    }
    return 0;
}

static uint64_t special_glDrawElements(struct gl_call *c, const uint64_t *s)
{
    return draw_elements(c, (GLenum)s[0], (GLsizei)s[1], (GLenum)s[2], s[3], 1, 0, 0, 0, 0);
}

static uint64_t special_glDrawRangeElements(struct gl_call *c, const uint64_t *s)
{
    return draw_elements(c, (GLenum)s[0], (GLsizei)s[3], (GLenum)s[4], s[5], 1, 0, 1, (GLuint)s[1], (GLuint)s[2]);
}

static uint64_t special_glDrawElementsInstanced(struct gl_call *c, const uint64_t *s)
{
    return draw_elements(c, (GLenum)s[0], (GLsizei)s[1], (GLenum)s[2], s[3], (GLsizei)s[4], 0, 2, 0, 0);
}

static uint64_t special_glDrawElementsBaseVertex(struct gl_call *c, const uint64_t *s)
{
    return draw_elements(c, (GLenum)s[0], (GLsizei)s[1], (GLenum)s[2], s[3], 1, (GLint)s[4], 3, 0, 0);
}

static uint64_t special_glDrawRangeElementsBaseVertex(struct gl_call *c, const uint64_t *s)
{
    return draw_elements(c, (GLenum)s[0], (GLsizei)s[3], (GLenum)s[4], s[5], 1, (GLint)s[6], 4,
                         (GLuint)s[1], (GLuint)s[2]);
}

static uint64_t special_glDrawElementsInstancedBaseVertex(struct gl_call *c, const uint64_t *s)
{
    return draw_elements(c, (GLenum)s[0], (GLsizei)s[1], (GLenum)s[2], s[3], (GLsizei)s[4], (GLint)s[5], 5, 0, 0);
}

/* Sync objects: numbered 1.. for the guest. */
static GLsync sync_of(uint64_t id) { return id && id <= MAXSYNC ? syncs[id - 1] : NULL; }

static uint64_t special_glFenceSync(struct gl_call *c, const uint64_t *s)
{
    int i;
    (void)c;
    for (i = 0; i < MAXSYNC; i++)
        if (!syncs[i]) {
            syncs[i] = glFenceSync((GLenum)s[0], (GLbitfield)s[1]);
            return syncs[i] ? (uint64_t)i + 1 : 0;
        }
    return 0;
}

static uint64_t special_glDeleteSync(struct gl_call *c, const uint64_t *s)
{
    (void)c;
    if (sync_of(s[0])) { glDeleteSync(sync_of(s[0])); syncs[s[0] - 1] = NULL; }
    return 0;
}

static uint64_t special_glIsSync(struct gl_call *c, const uint64_t *s) { (void)c; return sync_of(s[0]) ? glIsSync(sync_of(s[0])) : 0; }

static uint64_t special_glClientWaitSync(struct gl_call *c, const uint64_t *s)
{
    (void)c;
    return sync_of(s[0]) ? glClientWaitSync(sync_of(s[0]), (GLbitfield)s[1], (GLuint64)s[2]) : GL_WAIT_FAILED;
}

static uint64_t special_glWaitSync(struct gl_call *c, const uint64_t *s)
{
    (void)c;
    if (sync_of(s[0])) glWaitSync(sync_of(s[0]), (GLbitfield)s[1], (GLuint64)s[2]);
    return 0;
}

static uint64_t special_glGetSynciv(struct gl_call *c, const uint64_t *s)
{
    GLsizei count = (GLsizei)s[2];
    GLsizei *len = gl_ptr(c, s[3], 4, 1, NULL);
    GLint *val = gl_ptr(c, s[4], 4 * (GLsizeiptr)count, 1, NULL);
    if (sync_of(s[0])) glGetSynciv(sync_of(s[0]), (GLenum)s[1], count, len, val);
    return 0;
}

static uint64_t special_glGetIntegerv(struct gl_call *c, const uint64_t *s)
{
    GLint *v = gl_ptr(c, s[1], 512, 1, NULL);
    if (!v) return 0;
    if ((GLenum)s[0] == GL_NUM_EXTENSIONS) { load_extensions(); *v = nexts; return 0; }
    glGetIntegerv((GLenum)s[0], v);
    return 0;
}

static uint64_t special_glDebugMessageCallback(struct gl_call *c, const uint64_t *s) { (void)c; (void)s; return 0; }

#include "gl_gen.h"

/* ---------- EGL ---------- */

static int read_attribs(struct aoi_proc *p, uint64_t ga, EGLint *out, int max)
{
    int n = 0;
    if (!ga) { out[0] = EGL_NONE; return 0; }
    while (n + 2 < max) {
        EGLint kv[2] = { EGL_NONE, 0 };
        copy_in(&p->vm, ga + 4 * (uint64_t)n, kv, 8);
        if (kv[0] == EGL_NONE) break;
        out[n] = kv[0]; out[n + 1] = kv[1];
        n += 2;
    }
    out[n] = EGL_NONE;
    return n;
}

static int put_i32(struct aoi_proc *p, uint64_t ga, EGLint v) { return ga && copy_out(&p->vm, ga, &v, 4) == 4; }

/* ANGLE (iOS): its Metal back end, asked for by name. */
#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#endif

static int egl_init(struct aoi_proc *p)
{
    EGLint maj, min, n = 0;
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (inited) return inited > 0;
    inited = -1;
#ifdef __APPLE__
    if (gpd) {
        static const EGLint metal[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
        dpy = gpd(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, metal);
    }
#endif
#ifdef EGL_PLATFORM_SURFACELESS_MESA
    if (gpd && dpy == EGL_NO_DISPLAY) dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
#endif
    (void)gpd;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
        say(p, "no EGL display (%#x)", eglGetError());
        return 0;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    eglGetConfigs(dpy, cfg, MAXCFG, &n);
    ncfg = n;
    say(p, "EGL %d.%d, %d configs: %s", maj, min, ncfg, eglQueryString(dpy, EGL_VENDOR));
    inited = 1;
    return 1;
}

int aoi_gpu_available(void) { return egl_init(NULL); }

/* The process is gone: everything it had on the GPU goes too. */
void aoi_gpu_end(void)
{
    int i;
    if (inited <= 0) return;
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    hdraw = hread = hctx = -1;
    memset(syncs, 0, sizeof syncs);                              /* they go with their contexts */
    for (i = 0; i < MAXCTX; i++) if (ctxs[i]) { eglDestroyContext(dpy, ctxs[i]->h); free(ctxs[i]); ctxs[i] = NULL; }
    for (i = 0; i < MAXSURF; i++) if (surfs[i].h) eglDestroySurface(dpy, surfs[i].h);
    memset(surfs, 0, sizeof surfs);
    memset(thr, 0, sizeof thr);
    nstrs = 0;
}

static int config_id(EGLConfig c)
{
    int i;
    for (i = 0; i < ncfg; i++) if (cfg[i] == c) return i + 1;
    return 0;
}

static EGLConfig config_of(uint64_t id) { return id && id <= (uint64_t)ncfg ? cfg[id - 1] : NULL; }
static struct ctx *ctx_of(uint64_t id) { return id && id <= MAXCTX ? ctxs[id - 1] : NULL; }
static struct surf *surf_of(uint64_t id) { return id && id <= MAXSURF && surfs[id - 1].h ? &surfs[id - 1] : NULL; }

static struct thr *thread_of(struct aoi_proc *p)
{
    int tid = p->th[p->cur].tid, i, free_ = -1;
    for (i = 0; i < MAXTHR; i++) {
        if (thr[i].tid == tid) return &thr[i];
        if (!thr[i].tid && free_ < 0) free_ = i;
    }
    if (free_ < 0) return NULL;
    thr[free_] = (struct thr){ tid, 0, 0, 0 };
    return &thr[free_];
}

/* The host's current context and surfaces follow the calling guest thread. */
static int follow(struct thr *t)
{
    struct surf *d, *r;
    struct ctx *x;
    if (!t) return 0;
    if (t->draw == hdraw && t->read == hread && t->ctx == hctx) return 1;
    d = surf_of((uint64_t)t->draw); r = surf_of((uint64_t)t->read); x = ctx_of((uint64_t)t->ctx);
    if (!eglMakeCurrent(dpy, d ? d->h : EGL_NO_SURFACE, r ? r->h : EGL_NO_SURFACE, x ? x->h : EGL_NO_CONTEXT))
        return 0;
    hdraw = t->draw; hread = t->read; hctx = t->ctx;
    return 1;
}

static EGLSurface pbuffer(EGLConfig c, int w, int h)
{
    EGLint a[] = { EGL_WIDTH, w > 0 ? w : 1, EGL_HEIGHT, h > 0 ? h : 1, EGL_NONE };
    return eglCreatePbufferSurface(dpy, c, a);
}

/* A window surface's frame into the guest buffer at ga (stride in pixels, Android
 * format 1 RGBA_8888, 2 RGBX_8888, 5 BGRA_8888, 4 RGB_565), top row first. */
static int readback(struct aoi_proc *p, struct surf *sf, uint64_t ga, int stride, int format)
{
    GLint rfb = geti(GL_READ_FRAMEBUFFER_BINDING), pbo = geti(GL_PIXEL_PACK_BUFFER_BINDING);
    GLint al = geti(GL_PACK_ALIGNMENT), rl = geti(GL_PACK_ROW_LENGTH), sp = geti(GL_PACK_SKIP_PIXELS),
          sr = geti(GL_PACK_SKIP_ROWS);
    int w = sf->w, h = sf->h_, y, x, bpp = format == 4 ? 2 : 4;
    uint8_t *px = malloc((size_t)w * (size_t)h * 4), *row = malloc((size_t)stride * 4);
    if (!px || !row) { free(px); free(row); return 0; }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (pbo) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0); glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    if (pbo) glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pbo);
    glPixelStorei(GL_PACK_ALIGNMENT, al); glPixelStorei(GL_PACK_ROW_LENGTH, rl);
    glPixelStorei(GL_PACK_SKIP_PIXELS, sp); glPixelStorei(GL_PACK_SKIP_ROWS, sr);
    for (y = 0; y < h; y++) {
        const uint8_t *s = px + (size_t)(h - 1 - y) * (size_t)w * 4;
        if (format == 4) {
            uint16_t *d = (uint16_t *)row;
            for (x = 0; x < w; x++)
                d[x] = (uint16_t)((s[4 * x] >> 3) << 11 | (s[4 * x + 1] >> 2) << 5 | s[4 * x + 2] >> 3);
        } else if (format == 5) {
            for (x = 0; x < w; x++) {
                row[4 * x] = s[4 * x + 2]; row[4 * x + 1] = s[4 * x + 1];
                row[4 * x + 2] = s[4 * x]; row[4 * x + 3] = s[4 * x + 3];
            }
        } else memcpy(row, s, (size_t)w * 4);
        copy_out(&p->vm, ga + (uint64_t)y * (uint64_t)stride * (uint64_t)bpp, row, (uint64_t)w * (uint64_t)bpp);
    }
    free(px); free(row);
    return 1;
}

static uint64_t egl_op(struct aoi_proc *p, uint64_t op, const uint64_t *s)
{
    EGLint at[130];
    struct thr *t = thread_of(p);
    int i;
    if (op == AOI_EGL_INIT || !inited) {             /* also after a snapshot: a new host */
        if (!egl_init(p) || op == AOI_EGL_INIT) return inited > 0;
    }
    if (inited <= 0) return 0;
    switch (op) {
    case AOI_EGL_CHOOSE_CONFIG: {
        EGLConfig got[MAXCFG];
        EGLint n = 0, size = (EGLint)s[2], k;
        read_attribs(p, s[0], at, 128);
        if (!eglChooseConfig(dpy, at, got, MAXCFG, &n)) return EGL_FALSE;
        if (s[1]) {
            if (n > size) n = size;
            for (k = 0; k < n; k++) {
                uint64_t id = (uint64_t)config_id(got[k]);
                copy_out(&p->vm, s[1] + 8 * (uint64_t)k, &id, 8);
            }
        }
        put_i32(p, s[3], n);
        return EGL_TRUE;
    }
    case AOI_EGL_CONFIG_ATTRIB: {
        EGLint v = 0;
        if (!config_of(s[0]) || !eglGetConfigAttrib(dpy, config_of(s[0]), (EGLint)s[1], &v)) return EGL_FALSE;
        put_i32(p, s[2], v);
        return EGL_TRUE;
    }
    case AOI_EGL_CREATE_CONTEXT: {
        struct ctx *share = ctx_of(s[1]), *x;
        read_attribs(p, s[2], at, 128);
        for (i = 0; i < MAXCTX && ctxs[i]; i++) {}
        if (i == MAXCTX || !(x = calloc(1, sizeof *x))) return 0;
        x->h = eglCreateContext(dpy, config_of(s[0]) ? config_of(s[0]) : EGL_NO_CONFIG_KHR,
                                share ? share->h : EGL_NO_CONTEXT, at);
        if (x->h == EGL_NO_CONTEXT) { free(x); return 0; }
        ctxs[i] = x;
        p->gpu_live++;
        return (uint64_t)i + 1;
    }
    case AOI_EGL_DESTROY_CONTEXT: {
        struct ctx *x = ctx_of(s[0]);
        if (!x) return EGL_FALSE;
        if (hctx == (int)s[0]) { eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); hdraw = hread = hctx = 0; }
        eglDestroyContext(dpy, x->h);
        free(x);
        ctxs[s[0] - 1] = NULL;
        if (p->gpu_live > 0) p->gpu_live--;
        return EGL_TRUE;
    }
    case AOI_EGL_CREATE_SURFACE: {
        EGLConfig c = config_of(s[0]);
        for (i = 0; i < MAXSURF && surfs[i].h; i++) {}
        if (i == MAXSURF || !c) return 0;
        surfs[i].h = pbuffer(c, (int)s[1], (int)s[2]);
        if (surfs[i].h == EGL_NO_SURFACE) { surfs[i].h = NULL; return 0; }
        surfs[i].cfg = c; surfs[i].w = (int)s[1]; surfs[i].h_ = (int)s[2];
        p->gpu_live++;
        return (uint64_t)i + 1;
    }
    case AOI_EGL_DESTROY_SURFACE: {
        struct surf *sf = surf_of(s[0]);
        if (!sf) return EGL_FALSE;
        if (hdraw == (int)s[0] || hread == (int)s[0]) {
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            hdraw = hread = hctx = 0;
        }
        eglDestroySurface(dpy, sf->h);
        memset(sf, 0, sizeof *sf);
        if (p->gpu_live > 0) p->gpu_live--;
        return EGL_TRUE;
    }
    case AOI_EGL_RESIZE_SURFACE: {
        struct surf *sf = surf_of(s[0]);
        EGLSurface n;
        if (!sf) return EGL_FALSE;
        if ((int)s[1] == sf->w && (int)s[2] == sf->h_) return EGL_TRUE;
        n = pbuffer(sf->cfg, (int)s[1], (int)s[2]);
        if (n == EGL_NO_SURFACE) return EGL_FALSE;
        if (hdraw == (int)s[0] || hread == (int)s[0]) {
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            hdraw = hread = hctx = -1;                             /* follow() binds it again */
        }
        eglDestroySurface(dpy, sf->h);
        sf->h = n; sf->w = (int)s[1]; sf->h_ = (int)s[2];
        return EGL_TRUE;
    }
    case AOI_EGL_MAKE_CURRENT:
        if (!t) return EGL_FALSE;
        t->draw = (int)s[0]; t->read = (int)s[1]; t->ctx = (int)s[2];
        return follow(t) ? EGL_TRUE : EGL_FALSE;
    case AOI_EGL_QUERY_CONTEXT: {
        EGLint v = 0;
        struct ctx *x = ctx_of(s[0]);
        if (!x || !eglQueryContext(dpy, x->h, (EGLint)s[1], &v)) return EGL_FALSE;
        put_i32(p, s[2], v);
        return EGL_TRUE;
    }
    case AOI_EGL_GET_ERROR:
        return (uint64_t)eglGetError();
    case AOI_EGL_READBACK: {
        struct surf *sf = surf_of(s[0]);
        static int frames;
        static double spent;
        struct timespec t0, t1;
        int ok;
        if (!sf || !follow(t) || t->draw != (int)s[0]) return EGL_FALSE;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ok = readback(p, sf, s[1], (int)s[2], (int)s[3]);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        spent += (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (++frames % 100 == 1)                                   /* what a frame's trip back costs */
            say(p, "frame %d: %dx%d, readback %.1f ms on average", frames, sf->w, sf->h_, spent / frames);
        return ok ? EGL_TRUE : EGL_FALSE;
    }
    }
    return 0;
}

/* ---------- the syscall ---------- */

uint64_t aoi_gpu_call(void *unused, struct aoi_proc *p, uint64_t op, uint64_t args)
{
    uint64_t s[16];
    (void)unused;
    memset(s, 0, sizeof s);
    copy_in(&p->vm, args, s, sizeof s);
    if (op < sizeof gl_names / sizeof gl_names[0]) {
        static struct gl_call call;                                /* one host thread runs the guest */
        struct gl_call *c = &call;
        struct thr *t = thread_of(p);
        uint64_t r;
        if (inited <= 0 || !t || !t->ctx || !follow(t)) return 0;
        c->p = p; c->t = t; c->ctx = ctx_of((uint64_t)t->ctx); c->ntemp = 0;
        if (trace < 0) trace = getenv("AOI_GL_TRACE") != NULL;
        if (trace) fprintf(stderr, "[gl] %s\n", gl_names[op]);
        r = gl_dispatch(c, (unsigned)op, s);
        finish(c);
        return r;
    }
    return egl_op(p, op, s);
}
