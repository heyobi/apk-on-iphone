package aoi;

import android.os.PermissionEnforcer;

/** The enforcer of our services' @EnforcePermission checks: one app, everything granted
 *  (the default one wants the system context of a system_server we do not have). */
final class GrantAll extends PermissionEnforcer {
    static final GrantAll INSTANCE = new GrantAll();

    @Override public void enforcePermission(String permission, int pid, int uid) {}
    @Override public void enforcePermissionAllOf(String[] permissions, int pid, int uid) {}
    @Override public void enforcePermissionAnyOf(String[] permissions, int pid, int uid) {}
}
