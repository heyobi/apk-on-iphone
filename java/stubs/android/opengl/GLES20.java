package android.opengl;

public class GLES20 {
    public static final int GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_COLOR_BUFFER_BIT = 0x4000,
            GL_TRIANGLES = 4, GL_FLOAT = 0x1406, GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401, GL_RENDERER = 0x1F01,
            GL_VERSION = 0x1F02, GL_LINK_STATUS = 0x8B82, GL_COMPILE_STATUS = 0x8B81, GL_ARRAY_BUFFER = 0x8892,
            GL_STATIC_DRAW = 0x88E4;
    public static String glGetString(int name) { return null; }
    public static int glCreateShader(int type) { return 0; }
    public static void glShaderSource(int shader, String source) {}
    public static void glCompileShader(int shader) {}
    public static void glGetShaderiv(int shader, int pname, int[] params, int offset) {}
    public static String glGetShaderInfoLog(int shader) { return null; }
    public static int glCreateProgram() { return 0; }
    public static void glAttachShader(int program, int shader) {}
    public static void glBindAttribLocation(int program, int index, String name) {}
    public static void glLinkProgram(int program) {}
    public static void glGetProgramiv(int program, int pname, int[] params, int offset) {}
    public static void glUseProgram(int program) {}
    public static void glViewport(int x, int y, int w, int h) {}
    public static void glClearColor(float r, float g, float b, float a) {}
    public static void glClear(int mask) {}
    public static void glVertexAttribPointer(int index, int size, int type, boolean normalized, int stride, java.nio.Buffer ptr) {}
    public static void glVertexAttribPointer(int index, int size, int type, boolean normalized, int stride, int offset) {}
    public static void glEnableVertexAttribArray(int index) {}
    public static void glGenBuffers(int n, int[] buffers, int offset) {}
    public static void glBindBuffer(int target, int buffer) {}
    public static void glBufferData(int target, int size, java.nio.Buffer data, int usage) {}
    public static void glDrawArrays(int mode, int first, int count) {}
    public static void glReadPixels(int x, int y, int w, int h, int format, int type, java.nio.Buffer pixels) {}
    public static void glFinish() {}
    public static int glGetError() { return 0; }
}
