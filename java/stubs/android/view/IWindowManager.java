package android.view;

import android.graphics.Point;
import android.graphics.Region;
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;

/** Hidden; only the calls our WindowManager implements. */
public interface IWindowManager {
    IWindowSession openSession(IWindowSessionCallback callback) throws RemoteException;
    android.window.WindowContextInfo attachWindowContextToDisplayArea(android.app.IApplicationThread appThread,
            IBinder clientToken, int type, int displayId, android.os.Bundle options) throws RemoteException;
    android.window.WindowContextInfo attachWindowContextToDisplayContent(android.app.IApplicationThread appThread,
            IBinder clientToken, int displayId) throws RemoteException;
    void attachWindowContextToWindowToken(android.app.IApplicationThread appThread, IBinder clientToken,
            IBinder token) throws RemoteException;
    void detachWindowContext(IBinder clientToken) throws RemoteException;
    boolean hasNavigationBar(int displayId) throws RemoteException;
    void getInitialDisplaySize(int displayId, Point size) throws RemoteException;
    void getBaseDisplaySize(int displayId, Point size) throws RemoteException;
    int getInitialDisplayDensity(int displayId) throws RemoteException;
    int getBaseDisplayDensity(int displayId) throws RemoteException;
    boolean isKeyguardLocked() throws RemoteException;
    boolean isKeyguardSecure(int userId) throws RemoteException;
    float getCurrentAnimatorScale() throws RemoteException;
    float getAnimationScale(int which) throws RemoteException;
    float[] getAnimationScales() throws RemoteException;
    int getDefaultDisplayRotation() throws RemoteException;
    int watchRotation(IRotationWatcher watcher, int displayId) throws RemoteException;
    boolean getWindowInsets(int displayId, IBinder token, InsetsState outInsetsState) throws RemoteException;
    boolean isInTouchMode(int displayId) throws RemoteException;
    void setInTouchMode(boolean inTouch, int displayId) throws RemoteException;
    int getDockedStackSide() throws RemoteException;
    int getPreferredOptionsPanelGravity(int displayId) throws RemoteException;
    boolean isSafeModeEnabled() throws RemoteException;
    int getWindowingMode(int displayId) throws RemoteException;
    boolean isTaskSnapshotSupported() throws RemoteException;
    int getImeDisplayId() throws RemoteException;
    int[] registerDisplayWindowListener(IDisplayWindowListener listener) throws RemoteException;
    void registerSystemGestureExclusionListener(ISystemGestureExclusionListener listener, int displayId)
            throws RemoteException;
    Region getCurrentImeTouchRegion() throws RemoteException;
    boolean isLayerTracing() throws RemoteException;
    String[] getSupportedDisplayHashAlgorithms() throws RemoteException;

    abstract class Stub extends Binder implements IWindowManager {
        public Stub(android.os.PermissionEnforcer enforcer) {}
    }
}
