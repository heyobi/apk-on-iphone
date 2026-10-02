package android.opengl;

public class EGL14 {
    public static final int EGL_DEFAULT_DISPLAY = 0, EGL_NONE = 0x3038, EGL_RED_SIZE = 0x3024, EGL_GREEN_SIZE = 0x3023,
            EGL_BLUE_SIZE = 0x3022, EGL_ALPHA_SIZE = 0x3021, EGL_RENDERABLE_TYPE = 0x3040, EGL_OPENGL_ES2_BIT = 4,
            EGL_SURFACE_TYPE = 0x3033, EGL_PBUFFER_BIT = 1, EGL_CONTEXT_CLIENT_VERSION = 0x3098, EGL_WIDTH = 0x3057,
            EGL_HEIGHT = 0x3056;
    public static final EGLContext EGL_NO_CONTEXT = null;
    public static EGLDisplay eglGetDisplay(int id) { return null; }
    public static boolean eglInitialize(EGLDisplay d, int[] major, int majorOffset, int[] minor, int minorOffset) { return false; }
    public static boolean eglChooseConfig(EGLDisplay d, int[] attribs, int attribsOffset, EGLConfig[] configs,
            int configsOffset, int size, int[] num, int numOffset) { return false; }
    public static EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, int[] attribs, int offset) { return null; }
    public static EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, int[] attribs, int offset) { return null; }
    public static boolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c) { return false; }
    public static int eglGetError() { return 0; }
}
