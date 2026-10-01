package android.view;

import android.os.Looper;

/** Hidden; signatures from the guest's framework.jar (tools/dexsig.py). */
public abstract class InputEventSender {
    public InputEventSender(InputChannel inputChannel, Looper looper) {}
    public void onInputEventFinished(int seq, boolean handled) {}
    public final boolean sendInputEvent(int seq, InputEvent event) { return false; }
}
