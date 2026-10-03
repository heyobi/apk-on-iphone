package android.view;

public interface IWindowId extends android.os.IInterface {
    void registerFocusObserver(IWindowFocusObserver observer) throws android.os.RemoteException;
    void unregisterFocusObserver(IWindowFocusObserver observer) throws android.os.RemoteException;
    boolean isFocused() throws android.os.RemoteException;

    abstract class Stub extends android.os.Binder implements IWindowId {
        public Stub() {}
        public android.os.IBinder asBinder() { return this; }
    }
}
