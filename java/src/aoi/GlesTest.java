package aoi;

import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

/** OpenGL ES through Android's own libEGL and our driver (guest/gles.c) to the host's
 *  GPU (gpu/host.c): a 64x64 pbuffer, a red triangle over a blue clear, drawn twice
 *  (vertices in a client-side array, then in a buffer object), read back. */
public final class GlesTest {
    private static final String VS =
            "attribute vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
    private static final String FS =
            "precision mediump float;\nvoid main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";

    private static int shader(int type, String src) {
        int s = GLES20.glCreateShader(type);
        GLES20.glShaderSource(s, src);
        GLES20.glCompileShader(s);
        int[] ok = new int[1];
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
        if (ok[0] == 0) System.out.println("gles: shader: " + GLES20.glGetShaderInfoLog(s));
        return s;
    }

    private static String pixel(ByteBuffer px, int x, int y) {
        int o = (y * 64 + x) * 4;
        return (px.get(o) & 0xff) + "," + (px.get(o + 1) & 0xff) + "," + (px.get(o + 2) & 0xff) + "," + (px.get(o + 3) & 0xff);
    }

    private static boolean frame(int prog, boolean vbo, FloatBuffer tri) {
        GLES20.glViewport(0, 0, 64, 64);
        GLES20.glClearColor(0f, 0f, 1f, 1f);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        GLES20.glUseProgram(prog);
        if (vbo) {
            int[] b = new int[1];
            GLES20.glGenBuffers(1, b, 0);
            GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, b[0]);
            GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER, 6 * 4, tri, GLES20.GL_STATIC_DRAW);
            GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 0, 0);
        } else {
            GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, 0);
            GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 0, tri);
        }
        GLES20.glEnableVertexAttribArray(0);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
        ByteBuffer px = ByteBuffer.allocateDirect(64 * 64 * 4);
        GLES20.glReadPixels(0, 0, 64, 64, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, px);
        String center = pixel(px, 32, 32), corner = pixel(px, 1, 62);
        System.out.println("gles: " + (vbo ? "buffer object" : "client array") + ": center " + center + ", corner " + corner
                + ", error " + GLES20.glGetError());
        return center.equals("255,0,0,255") && corner.equals("0,0,255,255");
    }

    public static void main(String[] args) {
        EGLDisplay d = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] v = new int[2];
        if (!EGL14.eglInitialize(d, v, 0, v, 1)) { System.out.println("gles: eglInitialize " + EGL14.eglGetError()); return; }
        int[] attrs = { EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_NONE };
        EGLConfig[] cfg = new EGLConfig[1];
        int[] n = new int[1];
        if (!EGL14.eglChooseConfig(d, attrs, 0, cfg, 0, 1, n, 0) || n[0] < 1) {
            System.out.println("gles: no config " + EGL14.eglGetError());
            return;
        }
        EGLContext ctx = EGL14.eglCreateContext(d, cfg[0], EGL14.EGL_NO_CONTEXT,
                new int[] { EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE }, 0);
        EGLSurface s = EGL14.eglCreatePbufferSurface(d, cfg[0],
                new int[] { EGL14.EGL_WIDTH, 64, EGL14.EGL_HEIGHT, 64, EGL14.EGL_NONE }, 0);
        if (!EGL14.eglMakeCurrent(d, s, s, ctx)) { System.out.println("gles: eglMakeCurrent " + EGL14.eglGetError()); return; }
        System.out.println("gles: EGL " + v[0] + "." + v[1] + ", " + GLES20.glGetString(GLES20.GL_VERSION)
                + " on " + GLES20.glGetString(GLES20.GL_RENDERER));
        int prog = GLES20.glCreateProgram();
        GLES20.glAttachShader(prog, shader(GLES20.GL_VERTEX_SHADER, VS));
        GLES20.glAttachShader(prog, shader(GLES20.GL_FRAGMENT_SHADER, FS));
        GLES20.glBindAttribLocation(prog, 0, "pos");
        GLES20.glLinkProgram(prog);
        int[] ok = new int[1];
        GLES20.glGetProgramiv(prog, GLES20.GL_LINK_STATUS, ok, 0);
        FloatBuffer tri = ByteBuffer.allocateDirect(6 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        tri.put(new float[] { -0.8f, -0.8f, 0.8f, -0.8f, 0f, 0.8f }).position(0);
        boolean a = frame(prog, false, tri), b = frame(prog, true, tri);
        System.out.println(ok[0] != 0 && a && b ? "gles: red triangle on blue, both ways" : "gles: FAILED (link " + ok[0] + ")");
    }
}
