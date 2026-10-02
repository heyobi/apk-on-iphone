package aoi;

import android.permission.IPermissionManager;
import java.util.ArrayList;
import java.util.List;

/** One app, one user: every permission is granted, no permission was split. */
final class PermissionManager extends IPermissionManager.Stub {
    /* Calls go through Stub.onTransact, and one we have not written gets the default
     * answer instead of an AbstractMethodError (aoi.Services). */
    @Override public android.os.IInterface queryLocalInterface(String descriptor) { return null; }
    @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags)
            throws android.os.RemoteException {
        try { return super.onTransact(code, data, reply, flags); }
        catch (AbstractMethodError e) { return Services.missing(this, e, reply); }
    }

    PermissionManager() { super(GrantAll.INSTANCE); }
    @Override
    public List getSplitPermissions() { return new ArrayList(); }

    @Override
    public int checkPermission(String packageName, String permissionName, String persistentDeviceId, int userId) {
        return 0;                                                  /* PERMISSION_GRANTED */
    }

    @Override
    public int checkUidPermission(int uid, String permissionName, int deviceId) { return 0; }
}
