package android.os;

public class Binder implements IBinder {
    public Binder() {}
    public IInterface queryLocalInterface(String descriptor) { return null; }
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException { return false; }
}
