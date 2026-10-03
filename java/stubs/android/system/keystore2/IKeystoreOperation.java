package android.system.keystore2;

import android.os.IBinder;
import android.os.RemoteException;

public interface IKeystoreOperation extends android.os.IInterface {
    void updateAad(byte[] aadInput) throws RemoteException;
    byte[] update(byte[] input) throws RemoteException;
    byte[] finish(byte[] input, byte[] signature) throws RemoteException;
    void abort() throws RemoteException;
    int getInterfaceVersion() throws RemoteException;
    String getInterfaceHash() throws RemoteException;

    abstract class Stub extends android.os.Binder implements IKeystoreOperation {
        public Stub() {}
        public IBinder asBinder() { return this; }
    }
}
