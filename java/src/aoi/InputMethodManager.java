package aoi;

import android.os.IBinder;
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
import com.android.internal.view.IInputMethodManager;
import java.util.ArrayList;
import java.util.List;

/** No input method is installed yet (the iPhone's keyboard will come in here). */
final class InputMethodManager extends IInputMethodManager.Stub {
    /* Calls go through Stub.onTransact, and one we have not written gets the default
     * answer instead of an AbstractMethodError (aoi.Services). */
    @Override public android.os.IInterface queryLocalInterface(String descriptor) { return null; }
    @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags)
            throws android.os.RemoteException {
        try { return super.onTransact(code, data, reply, flags); }
        catch (AbstractMethodError e) { return Services.missing(this, e, reply); }
    }

    InputMethodManager() { super(GrantAll.INSTANCE); }

    @Override public void addClient(IInputMethodClient c, IRemoteInputConnection ic, int displayId) {}
    @Override public List getInputMethodList(int userId, int awareness) { return new ArrayList(); }
    @Override public List getEnabledInputMethodList(int userId) { return new ArrayList(); }
    @Override public List getEnabledInputMethodSubtypeList(String id, boolean implicit, int userId) { return new ArrayList(); }
    @Override public InputMethodSubtype getLastInputMethodSubtype(int userId) { return null; }
    @Override public InputMethodSubtype getCurrentInputMethodSubtype(int userId) { return null; }
    @Override public InputMethodInfo getCurrentInputMethodInfoAsUser(int userId) { return null; }

    @Override
    public InputBindResult startInputOrWindowGainedFocus(int reason, IInputMethodClient c, IBinder window, int flags,
            int softInputMode, int windowFlags, EditorInfo editor, IRemoteInputConnection ic,
            IRemoteAccessibilityInputConnection aic, int targetSdk, int userId, ImeOnBackInvokedDispatcher d) {
        return InputBindResult.NO_IME;
    }

    @Override
    public void startInputOrWindowGainedFocusAsync(int reason, IInputMethodClient c, IBinder window, int flags,
            int softInputMode, int windowFlags, EditorInfo editor, IRemoteInputConnection ic,
            IRemoteAccessibilityInputConnection aic, int targetSdk, int userId, ImeOnBackInvokedDispatcher d, int seq) {}

    @Override
    public boolean showSoftInput(IInputMethodClient c, IBinder window, ImeTracker.Token t, int flags, int tool,
            ResultReceiver r, int reason) {
        return false;
    }

    @Override
    public boolean hideSoftInput(IInputMethodClient c, IBinder window, ImeTracker.Token t, int flags, ResultReceiver r,
            int reason) {
        return false;
    }

    @Override public boolean isImeTraceEnabled() { return false; }
    @Override public IImeTracker getImeTrackerService() { return null; }
    @Override public int getInputMethodWindowVisibleHeight(IInputMethodClient c) { return 0; }
    @Override public void reportPerceptibleAsync(IBinder window, boolean perceptible) {}
    @Override public void removeImeSurfaceFromWindowAsync(IBinder window) {}
    @Override public boolean isStylusHandwritingAvailableAsUser(int userId, boolean connectionless) { return false; }
    @Override public boolean isInputMethodPickerShownForTest() { return false; }
}
