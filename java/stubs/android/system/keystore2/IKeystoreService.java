package android.system.keystore2;

import android.os.IBinder;
import android.os.RemoteException;

public interface IKeystoreService extends android.os.IInterface {
    IKeystoreSecurityLevel getSecurityLevel(int securityLevel) throws RemoteException;
    KeyEntryResponse getKeyEntry(KeyDescriptor key) throws RemoteException;
    void updateSubcomponent(KeyDescriptor key, byte[] publicCert, byte[] certificateChain) throws RemoteException;
    KeyDescriptor[] listEntries(int domain, long nspace) throws RemoteException;
    void deleteKey(KeyDescriptor key) throws RemoteException;
    KeyDescriptor grant(KeyDescriptor key, int granteeUid, int accessVector) throws RemoteException;
    void ungrant(KeyDescriptor key, int granteeUid) throws RemoteException;
    int getNumberOfEntries(int domain, long nspace) throws RemoteException;
    KeyDescriptor[] listEntriesBatched(int domain, long nspace, String startingPastAlias) throws RemoteException;
    int getInterfaceVersion() throws RemoteException;
    String getInterfaceHash() throws RemoteException;

    abstract class Stub extends android.os.Binder implements IKeystoreService {
        public Stub() {}
        public IBinder asBinder() { return this; }
        public static IKeystoreService asInterface(IBinder b) { return null; }
    }
}
