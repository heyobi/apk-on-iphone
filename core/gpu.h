/* OpenGL ES for the guest: the private syscall between the guest's driver
 * (guest/gles.c, /vendor/lib64/egl/libGLES_aoi.so, which Android's libEGL loads for
 * ro.hardware.egl=aoi) and the host's GPU (gpu/host.c: the host's EGL and GLES,
 * Mesa on Linux, ANGLE on Metal on iOS).
 *
 * x8 = AOI_SYS_GL, x0 = an operation, x1 = the guest address of its arguments, one
 * 64-bit slot each (floats as their bits in the low half). Operations below
 * AOI_GL_COUNT (guest/gl_gen.h, gpu/gl_gen.h) are GLES functions by number; the
 * AOI_EGL_* ones are what the guest's EGL needs from the host. Handles the guest sees
 * (configs, contexts, surfaces, sync objects) are small host-side numbers. */
#ifndef AOI_GPU_H
#define AOI_GPU_H

#define AOI_SYS_GL 0x4f4a

enum {
    AOI_EGL_INIT = 0x1000,        /* () -> 1 if the host has a GPU */
    AOI_EGL_CHOOSE_CONFIG,        /* (attribs, configs[], size, *num) -> EGLBoolean */
    AOI_EGL_CONFIG_ATTRIB,        /* (config, attribute, *value) -> EGLBoolean */
    AOI_EGL_CREATE_CONTEXT,       /* (config, share, attribs) -> context or 0 */
    AOI_EGL_DESTROY_CONTEXT,      /* (context) */
    AOI_EGL_CREATE_SURFACE,       /* (config, width, height) -> surface or 0 */
    AOI_EGL_DESTROY_SURFACE,      /* (surface) */
    AOI_EGL_RESIZE_SURFACE,       /* (surface, width, height) -> EGLBoolean */
    AOI_EGL_MAKE_CURRENT,         /* (draw, read, context) -> EGLBoolean */
    AOI_EGL_QUERY_CONTEXT,        /* (context, attribute, *value) -> EGLBoolean */
    AOI_EGL_GET_ERROR,            /* () -> the host's last EGL error */
    AOI_EGL_READBACK,             /* (surface, pixels, stride in pixels, format) -> EGLBoolean:
                                   * the surface's frame into a window buffer, top row first */
};

#endif
