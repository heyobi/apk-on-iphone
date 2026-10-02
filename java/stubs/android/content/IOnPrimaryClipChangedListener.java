package android.content;

import android.os.RemoteException;

/** Hidden; signatures from the guest's framework.jar (tools/dexsig.py). */
public interface IOnPrimaryClipChangedListener {
    void dispatchPrimaryClipChanged() throws RemoteException;
}
