package android.os;

public class PermissionEnforcer {
    protected PermissionEnforcer() {}
    public void enforcePermission(String permission, int pid, int uid) {}
    public void enforcePermissionAllOf(String[] permissions, int pid, int uid) {}
    public void enforcePermissionAnyOf(String[] permissions, int pid, int uid) {}
}
