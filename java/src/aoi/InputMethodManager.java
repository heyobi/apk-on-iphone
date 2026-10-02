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

/** The input method: the host's keyboard (the iPhone's). An app window that gains
 *  focus starts input here with its focused editor's EditorInfo and input
 *  connection; showSoftInput / hideSoftInput show and hide the host's keyboard
 *  (/dev/aoi_ime/1, /dev/aoi_ime/0: core/proc.c, p->ime); what is typed there comes
 *  back as input records (aoi.Input: 6 a character, 7 backspace, 8 enter, 9 the
 *  keyboard was closed) and goes to the editor's InputConnection on the main thread.
 *  The connection is the app's own (RemoteInputConnectionImpl, in this process):
 *  we call its InputConnection directly, as no IME session exists to go through.
 *  We answer NO_IME: there is no input method service to bind. */
final class InputMethodManager extends IInputMethodManager.Stub {
    private static volatile Object conn;                       /* the served IRemoteInputConnection */
    private static volatile EditorInfo editor;
    private static volatile boolean showing;
    private static final boolean DEBUG = System.getenv("AOI_IME_DEBUG") != null;

    static boolean showing() { return showing; }

    private static void host(int show) {
        showing = show != 0;
        try { new java.io.FileInputStream("/dev/aoi_ime/" + show).close(); } catch (java.io.IOException e) { /* ENOENT */ }
    }

    /** The editor's InputConnection: RemoteInputConnectionImpl.getInputConnection(). */
    private static Object inputConnection() {
        Object c = conn;
        if (c == null) return null;
        try {
            java.lang.reflect.Method m = c.getClass().getDeclaredMethod("getInputConnection");
            m.setAccessible(true);
            return m.invoke(c);
        } catch (Exception e) {
            System.out.println("aoi: ime: no input connection (" + c.getClass().getName() + "): " + e);
            return null;
        }
    }

    private static void call(Object ic, String name, Class<?>[] types, Object... args) throws Exception {
        Class<?> i = Class.forName("android.view.inputmethod.InputConnection");
        i.getMethod(name, types).invoke(ic, args);
    }

    private static void keyEvent(Object ic, int code) throws Exception {
        Class<?> k = Class.forName("android.view.KeyEvent");
        for (int a = 0; a < 2; a++)
            call(ic, "sendKeyEvent", new Class<?>[] { k }, k.getConstructor(int.class, int.class).newInstance(a, code));
    }

    /** A key from the host's keyboard (aoi.Input), done on the main thread. */
    static void key(final int action, final int value) {
        new android.os.Handler(android.os.Looper.getMainLooper()).post(new Runnable() {
            @Override public void run() {
                if (action == 9) {                                 /* the user closed the keyboard */
                    showing = false;
                    WindowSession s = WindowSession.instance;
                    if (s != null && s.focused() != null && !s.hasPopup(s.focused())) s.focus(s.focused(), false);
                    return;
                }
                Object ic = inputConnection();
                if (ic == null) { System.out.println("aoi: ime: key " + action + " with no editor"); return; }
                try {
                    if (action == 6) {
                        call(ic, "commitText", new Class<?>[] { CharSequence.class, int.class },
                                new String(Character.toChars(value)), 1);
                    } else if (action == 7) {                      /* backspace: the selection, or a character */
                        Object sel = Class.forName("android.view.inputmethod.InputConnection")
                                .getMethod("getSelectedText", int.class).invoke(ic, 0);
                        if (sel != null && ((CharSequence) sel).length() > 0)
                            call(ic, "commitText", new Class<?>[] { CharSequence.class, int.class }, "", 1);
                        else call(ic, "deleteSurroundingTextInCodePoints", new Class<?>[] { int.class, int.class }, 1, 0);
                    } else {
                        EditorInfo e = editor;
                        int ime = e != null ? e.imeOptions & 0xff : 0, type = e != null ? e.inputType : 0;
                        if (ime > 1 && (type & 0x20000) == 0)      /* an action (not NONE/UNSPECIFIED), not multi-line */
                            call(ic, "performEditorAction", new Class<?>[] { int.class }, ime);
                        else keyEvent(ic, 66);                     /* KEYCODE_ENTER */
                    }
                } catch (Exception e) {
                    System.out.println("aoi: ime: key " + action + ": " + e);
                }
            }
        });
    }

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
        if (DEBUG) System.out.println("aoi: ime: startInput reason " + reason + " editor " + (editor != null) + " ic "
                + (ic != null ? ic.getClass().getName() : null) + " at " + android.os.SystemClock.uptimeMillis());
        if (editor != null && ic != null) {
            conn = ic;
            InputMethodManager.editor = editor;
            System.out.println("aoi: ime: input started, inputType " + Integer.toHexString(editor.inputType)
                    + ", imeOptions " + Integer.toHexString(editor.imeOptions));
        }
        return InputBindResult.NO_IME;
    }

    @Override
    public void startInputOrWindowGainedFocusAsync(int reason, IInputMethodClient c, IBinder window, int flags,
            int softInputMode, int windowFlags, EditorInfo editor, IRemoteInputConnection ic,
            IRemoteAccessibilityInputConnection aic, int targetSdk, int userId, ImeOnBackInvokedDispatcher d, int seq) {}

    @Override
    public boolean showSoftInput(IInputMethodClient c, IBinder window, ImeTracker.Token t, int flags, int tool,
            ResultReceiver r, int reason) {
        if (conn == null || editor == null || editor.inputType == 0) return false;
        System.out.println("aoi: ime: show");
        host(1);
        return true;
    }

    @Override
    public boolean hideSoftInput(IInputMethodClient c, IBinder window, ImeTracker.Token t, int flags, ResultReceiver r,
            int reason) {
        if (!showing) return false;
        System.out.println("aoi: ime: hide");
        host(0);
        return true;
    }

    @Override public boolean isImeTraceEnabled() { return false; }
    @Override public IImeTracker getImeTrackerService() { return null; }
    @Override public int getInputMethodWindowVisibleHeight(IInputMethodClient c) { return 0; }
    @Override public void reportPerceptibleAsync(IBinder window, boolean perceptible) {}
    @Override public void removeImeSurfaceFromWindowAsync(IBinder window) {}
    @Override public boolean isStylusHandwritingAvailableAsUser(int userId, boolean connectionless) { return false; }
    @Override public boolean isInputMethodPickerShownForTest() { return false; }
}
