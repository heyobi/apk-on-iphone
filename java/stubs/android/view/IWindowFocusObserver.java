package android.view;
public interface IWindowFocusObserver extends android.os.IInterface {
    void focusGained(android.os.IBinder inputToken) throws android.os.RemoteException;
    void focusLost(android.os.IBinder inputToken) throws android.os.RemoteException;
}
