package android.content.pm;

import android.content.ComponentName;
import android.content.Intent;
import android.os.Binder;
import android.os.RemoteException;
import java.util.Map;

/** Hidden; only the calls our PackageManager implements (signatures: tools/dexsig.py). */
public interface IPackageManager {
    void notifyDexLoad(String loadingPackageName, Map classLoaderContextMap, String loaderIsa) throws RemoteException;
    ApplicationInfo getApplicationInfo(String packageName, long flags, int userId) throws RemoteException;
    PackageInfo getPackageInfo(String packageName, long flags, int userId) throws RemoteException;
    ActivityInfo getActivityInfo(ComponentName className, long flags, int userId) throws RemoteException;
    ServiceInfo getServiceInfo(ComponentName className, long flags, int userId) throws RemoteException;
    ProviderInfo getProviderInfo(ComponentName className, long flags, int userId) throws RemoteException;
    ActivityInfo getReceiverInfo(ComponentName className, long flags, int userId) throws RemoteException;
    boolean hasSystemFeature(String name, int version) throws RemoteException;
    int getComponentEnabledSetting(ComponentName componentName, int userId) throws RemoteException;
    String getInstallerPackageName(String packageName) throws RemoteException;
    String getNameForUid(int uid) throws RemoteException;
    String[] getPackagesForUid(int uid) throws RemoteException;
    int getPackageUid(String packageName, long flags, int userId) throws RemoteException;
    boolean isPackageAvailable(String packageName, int userId) throws RemoteException;
    boolean isSafeMode() throws RemoteException;
    int checkPermission(String permName, String pkgName, int userId) throws RemoteException;
    ResolveInfo resolveIntent(Intent intent, String resolvedType, long flags, int userId) throws RemoteException;
    ParceledListSlice queryIntentActivities(Intent intent, String resolvedType, long flags, int userId)
            throws RemoteException;
    ProviderInfo resolveContentProvider(String name, long flags, int userId) throws RemoteException;
    ParceledListSlice getSystemAvailableFeatures() throws RemoteException;
    ParceledListSlice queryProperty(String propertyName, int componentType) throws RemoteException;
    PackageManager.Property getPropertyAsUser(String propertyName, String packageName, String className, int userId)
            throws RemoteException;

    abstract class Stub extends Binder implements IPackageManager {
        public Stub(android.os.PermissionEnforcer enforcer) {}
    }
}
