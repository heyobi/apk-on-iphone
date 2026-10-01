package aoi;

import android.permission.IPermissionManager;
import java.util.ArrayList;
import java.util.List;

/** One app, one user: every permission is granted, no permission was split. */
final class PermissionManager extends IPermissionManager.Stub {
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
