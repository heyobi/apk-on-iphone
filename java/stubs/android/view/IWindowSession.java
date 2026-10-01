package android.view;

import android.graphics.Rect;
import android.graphics.Region;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import android.util.MergedConfiguration;
import android.window.ClientWindowFrames;
import android.window.OnBackInvokedCallbackInfo;
import java.util.List;

/** Hidden; signatures from the guest's framework.jar (tools/dexsig.py). */
public interface IWindowSession {
    int addToDisplayAsUser(IWindow window, WindowManager.LayoutParams attrs, int viewVisibility, int layerStackId,
            int userId, int requestedVisibleTypes, InputChannel outInputChannel, InsetsState insetsState,
            InsetsSourceControl.Array activeControls, Rect attachedFrame, float[] sizeCompatScale) throws RemoteException;
    int addToDisplay(IWindow window, WindowManager.LayoutParams attrs, int viewVisibility, int layerStackId,
            int requestedVisibleTypes, InputChannel outInputChannel, InsetsState insetsState,
            InsetsSourceControl.Array activeControls, Rect attachedFrame, float[] sizeCompatScale) throws RemoteException;
    int relayout(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth, int requestedHeight,
            int viewVisibility, int flags, int seq, int lastSyncSeqId, ClientWindowFrames outFrames,
            MergedConfiguration outMergedConfiguration, SurfaceControl outSurfaceControl, InsetsState insetsState,
            InsetsSourceControl.Array activeControls, Bundle bundle) throws RemoteException;
    void relayoutAsync(IWindow window, WindowManager.LayoutParams attrs, int requestedWidth, int requestedHeight,
            int viewVisibility, int flags, int seq, int lastSyncSeqId) throws RemoteException;
    void finishDrawing(IWindow window, SurfaceControl.Transaction postDrawTransaction, int seqId) throws RemoteException;
    void remove(IBinder clientToken) throws RemoteException;
    boolean outOfMemory(IWindow window) throws RemoteException;
    void setInsets(IWindow window, int touchableInsets, Rect contentInsets, Rect visibleInsets, Region touchableArea)
            throws RemoteException;
    void clearTouchableRegion(IWindow window) throws RemoteException;
    boolean cancelDraw(IWindow window) throws RemoteException;
    void pokeDrawLock(IBinder window) throws RemoteException;
    void updateRequestedVisibleTypes(IWindow window, int requestedVisibleTypes) throws RemoteException;
    void setOnBackInvokedCallbackInfo(IWindow window, OnBackInvokedCallbackInfo callbackInfo) throws RemoteException;
    void reportSystemGestureExclusionChanged(IWindow window, List exclusionRects) throws RemoteException;
    void reportKeepClearAreasChanged(IWindow window, List restricted, List unrestricted) throws RemoteException;
    void reportDecorViewGestureInterceptionChanged(IWindow window, boolean intercepted) throws RemoteException;
    void onRectangleOnScreenRequested(IBinder token, Rect rectangle) throws RemoteException;
    boolean performHapticFeedback(int effectId, boolean always, boolean fromIme) throws RemoteException;
    void performHapticFeedbackAsync(int effectId, boolean always, boolean fromIme) throws RemoteException;
    void updatePointerIcon(IWindow window) throws RemoteException;

    abstract class Stub extends Binder implements IWindowSession {
        public Stub() {}
    }
}
