package com.android.internal.view;

import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ResultReceiver;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ImeTracker;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodSubtype;
import android.window.ImeOnBackInvokedDispatcher;
import com.android.internal.inputmethod.IImeTracker;
import com.android.internal.inputmethod.IInputMethodClient;
import com.android.internal.inputmethod.IRemoteAccessibilityInputConnection;
import com.android.internal.inputmethod.IRemoteInputConnection;
import com.android.internal.inputmethod.InputBindResult;
import java.util.List;

/** Hidden; signatures from the guest's framework.jar (tools/dexsig.py). */
public interface IInputMethodManager {
    void addClient(IInputMethodClient client, IRemoteInputConnection inputConnection, int selfReportedDisplayId)
            throws RemoteException;
    List getInputMethodList(int userId, int directBootAwareness) throws RemoteException;
    List getEnabledInputMethodList(int userId) throws RemoteException;
    List getEnabledInputMethodSubtypeList(String imiId, boolean allowsImplicitlyEnabledSubtypes, int userId)
            throws RemoteException;
    InputMethodSubtype getLastInputMethodSubtype(int userId) throws RemoteException;
    InputMethodSubtype getCurrentInputMethodSubtype(int userId) throws RemoteException;
    InputMethodInfo getCurrentInputMethodInfoAsUser(int userId) throws RemoteException;
    InputBindResult startInputOrWindowGainedFocus(int startInputReason, IInputMethodClient client, IBinder windowToken,
            int startInputFlags, int softInputMode, int windowFlags, EditorInfo editorInfo,
            IRemoteInputConnection inputConnection, IRemoteAccessibilityInputConnection remoteAccessibilityInputConnection,
            int unverifiedTargetSdkVersion, int userId, ImeOnBackInvokedDispatcher imeDispatcher) throws RemoteException;
    void startInputOrWindowGainedFocusAsync(int startInputReason, IInputMethodClient client, IBinder windowToken,
            int startInputFlags, int softInputMode, int windowFlags, EditorInfo editorInfo,
            IRemoteInputConnection inputConnection, IRemoteAccessibilityInputConnection remoteAccessibilityInputConnection,
            int unverifiedTargetSdkVersion, int userId, ImeOnBackInvokedDispatcher imeDispatcher, int startInputSeq)
            throws RemoteException;
    boolean showSoftInput(IInputMethodClient client, IBinder windowToken, ImeTracker.Token statsToken, int flags,
            int lastClickToolType, ResultReceiver resultReceiver, int reason) throws RemoteException;
    boolean hideSoftInput(IInputMethodClient client, IBinder windowToken, ImeTracker.Token statsToken, int flags,
            ResultReceiver resultReceiver, int reason) throws RemoteException;
    boolean isImeTraceEnabled() throws RemoteException;
    IImeTracker getImeTrackerService() throws RemoteException;
    int getInputMethodWindowVisibleHeight(IInputMethodClient client) throws RemoteException;
    void reportPerceptibleAsync(IBinder windowToken, boolean perceptible) throws RemoteException;
    void removeImeSurfaceFromWindowAsync(IBinder windowToken) throws RemoteException;
    boolean isStylusHandwritingAvailableAsUser(int userId, boolean connectionless) throws RemoteException;
    boolean isInputMethodPickerShownForTest() throws RemoteException;

    abstract class Stub extends Binder implements IInputMethodManager {
        public Stub(android.os.PermissionEnforcer enforcer) {}
    }
}
