package aoi;

import android.graphics.Point;
import android.graphics.Region;
import android.os.IBinder;
import android.view.IDisplayWindowListener;
import android.view.IRotationWatcher;
import android.view.ISystemGestureExclusionListener;
import android.view.IWindowManager;
import android.view.IWindowSession;
import android.view.IWindowSessionCallback;
import android.view.InsetsState;

/** The in-process WindowManager: one display, the app's windows full screen on it,
 *  no navigation bar (the iPhone uses gestures), no keyguard. Windows live in the
 *  Session it hands out. */
final class WindowManager extends IWindowManager.Stub {
    private final WindowSession session = new WindowSession();
    { Input.start(session); }                                      /* touches from the host */

    WindowManager() { super(GrantAll.INSTANCE); }

    @Override public IWindowSession openSession(IWindowSessionCallback cb) { return session; }

    /* Window contexts (Context.createWindowContext: Chromium, dialogs, toasts): the
     * display's configuration, display 0. */
    @Override
    public android.window.WindowContextInfo attachWindowContextToDisplayArea(android.app.IApplicationThread t,
            IBinder token, int type, int displayId, android.os.Bundle options) {
        return new android.window.WindowContextInfo(WindowSession.windowConfig(), 0);
    }
    @Override
    public android.window.WindowContextInfo attachWindowContextToDisplayContent(android.app.IApplicationThread t,
            IBinder token, int displayId) {
        return new android.window.WindowContextInfo(WindowSession.windowConfig(), 0);
    }
    @Override public void attachWindowContextToWindowToken(android.app.IApplicationThread t, IBinder token, IBinder w) {}
    @Override public void detachWindowContext(IBinder token) {}
    @Override public boolean hasNavigationBar(int displayId) { return false; }

    @Override
    public void getInitialDisplaySize(int displayId, Point size) {
        size.x = DisplayManager.WIDTH; size.y = DisplayManager.HEIGHT;
    }

    @Override public void getBaseDisplaySize(int displayId, Point size) { getInitialDisplaySize(displayId, size); }
    @Override public int getInitialDisplayDensity(int displayId) { return DisplayManager.DPI; }
    @Override public int getBaseDisplayDensity(int displayId) { return DisplayManager.DPI; }
    @Override public boolean isKeyguardLocked() { return false; }
    @Override public boolean isKeyguardSecure(int userId) { return false; }
    @Override public float getCurrentAnimatorScale() { return 1f; }
    @Override public float getAnimationScale(int which) { return 1f; }
    @Override public float[] getAnimationScales() { return new float[] { 1f, 1f, 1f }; }
    @Override public int getDefaultDisplayRotation() { return 0; }
    @Override public int watchRotation(IRotationWatcher watcher, int displayId) { return 0; }
    @Override public boolean getWindowInsets(int displayId, IBinder token, InsetsState out) { return false; }
    @Override public boolean isInTouchMode(int displayId) { return true; }
    @Override public void setInTouchMode(boolean inTouch, int displayId) {}
    @Override public int getDockedStackSide() { return -1; }
    @Override public int getPreferredOptionsPanelGravity(int displayId) { return 0x51; }   /* CENTER_HORIZONTAL | BOTTOM */
    @Override public boolean isSafeModeEnabled() { return false; }
    @Override public int getWindowingMode(int displayId) { return 1; }   /* WINDOWING_MODE_FULLSCREEN */
    @Override public boolean isTaskSnapshotSupported() { return false; }
    @Override public int getImeDisplayId() { return 0; }
    @Override public int[] registerDisplayWindowListener(IDisplayWindowListener l) { return new int[] { 0 }; }
    @Override public void registerSystemGestureExclusionListener(ISystemGestureExclusionListener l, int displayId) {}
    @Override public Region getCurrentImeTouchRegion() { return null; }
    @Override public boolean isLayerTracing() { return false; }
    @Override public String[] getSupportedDisplayHashAlgorithms() { return new String[0]; }
}
