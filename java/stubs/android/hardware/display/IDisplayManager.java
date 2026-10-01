package android.hardware.display;

import android.graphics.Point;
import android.os.Binder;
import android.os.RemoteException;
import android.view.Display;
import android.view.DisplayInfo;

/** Hidden; only the calls our DisplayManager implements. */
public interface IDisplayManager {
    DisplayInfo getDisplayInfo(int displayId) throws RemoteException;
    int[] getDisplayIds(boolean includeDisabled) throws RemoteException;
    void registerCallback(IDisplayManagerCallback callback) throws RemoteException;
    void registerCallbackWithEventMask(IDisplayManagerCallback callback, long eventsMask) throws RemoteException;
    Point getStableDisplaySize() throws RemoteException;
    int getPreferredWideGamutColorSpaceId() throws RemoteException;
    float getBrightness(int displayId) throws RemoteException;
    int getRefreshRateSwitchingType() throws RemoteException;
    boolean shouldAlwaysRespectAppRequestedMode() throws RemoteException;
    boolean isMinimalPostProcessingRequested(int displayId) throws RemoteException;
    Display.Mode getUserPreferredDisplayMode(int displayId) throws RemoteException;
    Display.Mode getSystemPreferredDisplayMode(int displayId) throws RemoteException;
    int[] getSupportedHdrOutputTypes() throws RemoteException;
    android.hardware.OverlayProperties getOverlaySupport() throws RemoteException;

    abstract class Stub extends Binder implements IDisplayManager {
        public Stub(android.os.PermissionEnforcer enforcer) {}
    }
}
