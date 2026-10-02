package android.opengl;

public class GLSurfaceView extends android.view.SurfaceView {
    public static final int RENDERMODE_CONTINUOUSLY = 1;
    public interface Renderer {
        void onSurfaceCreated(javax.microedition.khronos.opengles.GL10 gl, javax.microedition.khronos.egl.EGLConfig config);
        void onSurfaceChanged(javax.microedition.khronos.opengles.GL10 gl, int width, int height);
        void onDrawFrame(javax.microedition.khronos.opengles.GL10 gl);
    }
    public GLSurfaceView(android.content.Context c) { super(c); }
    public void setEGLContextClientVersion(int v) {}
    public void setRenderer(Renderer r) {}
    public void setRenderMode(int m) {}
    public void onPause() {}
    public void onResume() {}
}
