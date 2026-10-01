package android.permission;

import android.os.Binder;
import android.os.RemoteException;
import java.util.List;

/** Hidden; only the calls our PermissionManager implements. */
public interface IPermissionManager {
    List getSplitPermissions() throws RemoteException;
    int checkPermission(String packageName, String permissionName, String persistentDeviceId, int userId)
            throws RemoteException;
    int checkUidPermission(int uid, String permissionName, int deviceId) throws RemoteException;

    abstract class Stub extends Binder implements IPermissionManager {
        public Stub(android.os.PermissionEnforcer enforcer) {}
    }
}
