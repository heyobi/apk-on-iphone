package android.system.keystore2;

import android.hardware.security.keymint.KeyParameter;
import android.os.IBinder;
import android.os.RemoteException;

public interface IKeystoreSecurityLevel extends android.os.IInterface {
    CreateOperationResponse createOperation(KeyDescriptor key, KeyParameter[] params, boolean forced) throws RemoteException;
    KeyMetadata generateKey(KeyDescriptor key, KeyDescriptor attestationKey, KeyParameter[] params, int flags, byte[] entropy) throws RemoteException;
    KeyMetadata importKey(KeyDescriptor key, KeyDescriptor attestationKey, KeyParameter[] params, int flags, byte[] keyData) throws RemoteException;
    KeyMetadata importWrappedKey(KeyDescriptor key, KeyDescriptor wrappingKey, byte[] maskingKey, KeyParameter[] params, AuthenticatorSpec[] authenticators) throws RemoteException;
    EphemeralStorageKeyResponse convertStorageKeyToEphemeral(KeyDescriptor storageKey) throws RemoteException;
    void deleteKey(KeyDescriptor key) throws RemoteException;
    int getInterfaceVersion() throws RemoteException;
    String getInterfaceHash() throws RemoteException;

    abstract class Stub extends android.os.Binder implements IKeystoreSecurityLevel {
        public Stub() {}
        public IBinder asBinder() { return this; }
    }
}
