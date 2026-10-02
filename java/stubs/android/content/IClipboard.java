package android.content;

import android.os.Binder;
import android.os.RemoteException;

/** Hidden; signatures from the guest's framework.jar (tools/dexsig.py). */
public interface IClipboard {
    void setPrimaryClip(ClipData clip, String pkg, String attributionTag, int userId, int deviceId) throws RemoteException;
    void setPrimaryClipAsPackage(ClipData clip, String pkg, String attributionTag, int userId, int deviceId,
            String sourcePackage) throws RemoteException;
    void clearPrimaryClip(String pkg, String attributionTag, int userId, int deviceId) throws RemoteException;
    ClipData getPrimaryClip(String pkg, String attributionTag, int userId, int deviceId) throws RemoteException;
    ClipDescription getPrimaryClipDescription(String pkg, String attributionTag, int userId, int deviceId)
            throws RemoteException;
    boolean hasPrimaryClip(String pkg, String attributionTag, int userId, int deviceId) throws RemoteException;
    void addPrimaryClipChangedListener(IOnPrimaryClipChangedListener l, String pkg, String attributionTag, int userId,
            int deviceId) throws RemoteException;
    void removePrimaryClipChangedListener(IOnPrimaryClipChangedListener l, String pkg, String attributionTag,
            int userId, int deviceId) throws RemoteException;
    boolean hasClipboardText(String pkg, String attributionTag, int userId, int deviceId) throws RemoteException;
    String getPrimaryClipSource(String pkg, String attributionTag, int userId, int deviceId) throws RemoteException;
    boolean areClipboardAccessNotificationsEnabledForUser(int userId) throws RemoteException;
    void setClipboardAccessNotificationsEnabledForUser(boolean enable, int userId) throws RemoteException;

    abstract class Stub extends Binder implements IClipboard {
        public Stub(android.os.PermissionEnforcer enforcer) {}
    }
}
