package android.opengl;

public class GLES20 {
    public static void glGetIntegerv(int pname, java.nio.IntBuffer params) {}
    public static final int GL_COLOR_BUFFER_BIT = 0x4000, GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401, GL_SCISSOR_TEST = 0xC11;
    public static void glViewport(int x, int y, int w, int h) {}
    public static void glClearColor(float r, float g, float b, float a) {}
    public static void glClear(int mask) {}
    public static void glEnable(int cap) {}
    public static void glDisable(int cap) {}
    public static void glScissor(int x, int y, int w, int h) {}
    public static String glGetString(int name) { return null; }
}
