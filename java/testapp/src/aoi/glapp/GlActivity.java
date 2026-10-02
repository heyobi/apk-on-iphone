package aoi.glapp;

import android.app.Activity;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/** The test app of tools/mktestapk.py: what a GL game does, without the game. A
 *  GLSurfaceView (a SurfaceView: its own layer below the window, its own EGL
 *  window surface, a render thread) clears to a color that steps every frame and
 *  draws a white square in the middle with the scissor test. */
public final class GlActivity extends Activity {
    private GLSurfaceView view;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        view = new GLSurfaceView(this);
        view.setEGLContextClientVersion(2);
        view.setRenderer(new GLSurfaceView.Renderer() {
            private int w, h, frame;

            @Override public void onSurfaceCreated(GL10 gl, EGLConfig c) {
                System.out.println("glapp: surface created, " + GLES20.glGetString(0x1F01));
            }
            @Override public void onSurfaceChanged(GL10 gl, int width, int height) {
                w = width; h = height;
                GLES20.glViewport(0, 0, w, h);
                System.out.println("glapp: surface " + w + "x" + h);
            }
            @Override public void onDrawFrame(GL10 gl) {
                frame++;
                GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
                GLES20.glClearColor((frame % 60) / 60f, 0.2f, 0.6f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
                GLES20.glScissor(w / 4, h / 4, w / 2, h / 2);
                GLES20.glClearColor(1f, 1f, 1f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                if (frame % 30 == 0) System.out.println("glapp: frame " + frame);
            }
        });
        setContentView(view);
    }

    @Override protected void onPause() { super.onPause(); view.onPause(); }
    @Override protected void onResume() { super.onResume(); view.onResume(); }
}
