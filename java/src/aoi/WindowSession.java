package aoi;

import android.content.res.Configuration;
import android.graphics.Rect;
import android.graphics.Region;
import android.os.Bundle;
import android.os.IBinder;
import android.util.MergedConfiguration;
import android.view.IWindow;
import android.view.IWindowSession;
import android.view.InputChannel;
import android.view.InsetsSourceControl;
import android.view.InsetsState;
import android.view.SurfaceControl;
import android.view.SurfaceSession;
import android.view.WindowManager;
import android.window.ClientWindowFrames;
import android.window.OnBackInvokedCallbackInfo;
import java.util.List;

/** The app's windows: each is added with an input channel (we keep the server end,
 *  touches will be written to it) and laid out full screen; relayout gives it a BLAST
 *  layer created through our SurfaceFlinger (core/sf.c). */
final class WindowSession extends IWindowSession.Stub {
    private final SurfaceSession surfaces = new SurfaceSession();
    InputChannel input;                                            /* the last window's server end */

    private static Rect screen() { return new Rect(0, 0, DisplayManager.WIDTH, DisplayManager.HEIGHT); }

    /** The configuration a window sees: the phone's, with window bounds = the screen. */
    static Configuration windowConfig() {
        Configuration c = ActivityManager.phone();
        c.windowConfiguration.setBounds(screen());
        c.windowConfiguration.setAppBounds(screen());
        c.windowConfiguration.setMaxBounds(screen());
        c.windowConfiguration.setWindowingMode(1);                 /* FULLSCREEN */
        c.windowConfiguration.setRotation(0);
        c.windowConfiguration.setDisplayRotation(0);
        return c;
    }

    @Override
    public int addToDisplayAsUser(IWindow window, WindowManager.LayoutParams attrs, int visibility, int layerStack,
            int userId, int requestedVisibleTypes, InputChannel outInputChannel, InsetsState insets,
            InsetsSourceControl.Array controls, Rect attachedFrame, float[] sizeCompatScale) {
        System.out.println("aoi: window added: " + attrs.getTitle());
        if (outInputChannel != null) {
            InputChannel[] pair = InputChannel.openInputChannelPair(String.valueOf(attrs.getTitle()));
            input = pair[0];
            pair[1].copyTo(outInputChannel);
        }
        if (insets != null) insets.setDisplayFrame(screen());
        if (sizeCompatScale != null && sizeCompatScale.length > 0) sizeCompatScale[0] = 1f;
        return 0x3;                                                /* ADD_OKAY | IN_TOUCH_MODE | APP_VISIBLE */
    }

    @Override
    public int addToDisplay(IWindow window, WindowManager.LayoutParams attrs, int visibility, int layerStack,
            int requestedVisibleTypes, InputChannel outInputChannel, InsetsState insets,
            InsetsSourceControl.Array controls, Rect attachedFrame, float[] sizeCompatScale) {
        return addToDisplayAsUser(window, attrs, visibility, layerStack, 0, requestedVisibleTypes, outInputChannel,
                insets, controls, attachedFrame, sizeCompatScale);
    }

    @Override
    public int relayout(IWindow window, WindowManager.LayoutParams attrs, int w, int h, int visibility, int flags,
            int seq, int lastSyncSeqId, ClientWindowFrames frames, MergedConfiguration merged,
            SurfaceControl outSurface, InsetsState insets, InsetsSourceControl.Array controls, Bundle bundle) {
        System.out.println("aoi: relayout " + (attrs != null ? attrs.getTitle() : "") + " " + w + "x" + h
                + " visibility " + visibility);
        if (frames != null) {
            frames.frame.set(0, 0, DisplayManager.WIDTH, DisplayManager.HEIGHT);
            frames.displayFrame.set(0, 0, DisplayManager.WIDTH, DisplayManager.HEIGHT);
            frames.parentFrame.set(0, 0, DisplayManager.WIDTH, DisplayManager.HEIGHT);
            frames.compatScale = 1f;
        }
        if (merged != null) merged.setConfiguration(windowConfig(), new Configuration());
        if (insets != null) insets.setDisplayFrame(screen());
        if (outSurface != null && visibility == 0) {               /* VISIBLE: it gets a layer to draw into */
            SurfaceControl sc = new SurfaceControl.Builder(surfaces)
                    .setName(String.valueOf(attrs != null ? attrs.getTitle() : "window"))
                    .setBufferSize(DisplayManager.WIDTH, DisplayManager.HEIGHT)
                    .setFormat(-3)                                 /* PixelFormat.TRANSLUCENT */
                    .setBLASTLayer()
                    .setCallsite("aoi.WindowSession.relayout")
                    .build();
            outSurface.copyFrom(sc, "aoi.WindowSession.relayout");
        }
        return 0;
    }

    @Override public void relayoutAsync(IWindow window, WindowManager.LayoutParams attrs, int w, int h, int v, int f, int s, int l) {}

    @Override
    public void finishDrawing(IWindow window, SurfaceControl.Transaction postDraw, int seqId) {
        System.out.println("aoi: finishDrawing");
    }

    @Override public void remove(IBinder token) {}
    @Override public boolean outOfMemory(IWindow window) { return false; }
    @Override public void setInsets(IWindow window, int touchable, Rect content, Rect visible, Region area) {}
    @Override public void clearTouchableRegion(IWindow window) {}
    @Override public boolean cancelDraw(IWindow window) { return false; }
    @Override public void pokeDrawLock(IBinder window) {}
    @Override public void updateRequestedVisibleTypes(IWindow window, int types) {}
    @Override public void setOnBackInvokedCallbackInfo(IWindow window, OnBackInvokedCallbackInfo info) {}
    @Override public void reportSystemGestureExclusionChanged(IWindow window, List rects) {}
    @Override public void reportKeepClearAreasChanged(IWindow window, List restricted, List unrestricted) {}
    @Override public void reportDecorViewGestureInterceptionChanged(IWindow window, boolean intercepted) {}
    @Override public void onRectangleOnScreenRequested(IBinder token, Rect rectangle) {}
    @Override public boolean performHapticFeedback(int effect, boolean always, boolean fromIme) { return false; }
    @Override public void performHapticFeedbackAsync(int effect, boolean always, boolean fromIme) {}
    @Override public void updatePointerIcon(IWindow window) {}
}
