package aoi;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageParser;
import android.content.pm.ParceledListSlice;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import java.util.ArrayList;
import java.util.Map;

/** The in-process PackageManager: one package is installed, ours. */
final class PackageManager extends IPackageManager.Stub {
    /* Calls go through Stub.onTransact, and one we have not written gets the default
     * answer instead of an AbstractMethodError (aoi.Services). */
    @Override public android.os.IInterface queryLocalInterface(String descriptor) { return null; }
    @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags)
            throws android.os.RemoteException {
        try { return super.onTransact(code, data, reply, flags); }
        catch (AbstractMethodError e) { return Services.missing(this, e, reply); }
    }

    private final App app;

    PackageManager(App app) {
        super(GrantAll.INSTANCE);
        this.app = app;
    }

    private boolean ours(String name) { return app.pkg.packageName.equals(name); }

    @Override public void notifyDexLoad(String loading, Map map, String isa) {}

    @Override
    public ApplicationInfo getApplicationInfo(String name, long flags, int userId) {
        return ours(name) ? app.info : null;
    }

    @Override
    public PackageInfo getPackageInfo(String name, long flags, int userId) {
        if (!ours(name)) return null;
        PackageInfo pi = new PackageInfo();
        pi.packageName = name;
        pi.versionCode = app.pkg.mVersionCode;
        pi.setLongVersionCode(app.pkg.mVersionCode);
        pi.versionName = app.pkg.mVersionName;
        pi.applicationInfo = app.info;
        pi.firstInstallTime = pi.lastUpdateTime = 1700000000000L;
        if ((flags & 1) != 0) {                                    /* GET_ACTIVITIES */
            ActivityInfo[] a = new ActivityInfo[app.pkg.activities.size()];
            for (int i = 0; i < a.length; i++) a[i] = app.pkg.activities.get(i).info;
            pi.activities = a;
        }
        if ((flags & 0x40) != 0) pi.signatures = app.signatures();             /* GET_SIGNATURES */
        if ((flags & 0x08000000) != 0) pi.signingInfo = app.signingInfo();     /* GET_SIGNING_CERTIFICATES */
        return pi;
    }

    @Override
    public ActivityInfo getActivityInfo(ComponentName c, long flags, int userId) {
        return ours(c.getPackageName()) ? app.activity(c.getClassName()) : null;
    }

    @Override
    public ServiceInfo getServiceInfo(ComponentName c, long flags, int userId) {
        return ours(c.getPackageName()) ? app.service(c.getClassName()) : null;
    }
    @Override
    public ProviderInfo getProviderInfo(ComponentName c, long flags, int userId) {
        return ours(c.getPackageName()) ? app.provider(c.getClassName()) : null;
    }
    @Override
    public ActivityInfo getReceiverInfo(ComponentName c, long flags, int userId) {
        return ours(c.getPackageName()) ? app.receiver(c.getClassName()) : null;
    }
    @Override public boolean hasSystemFeature(String name, int version) { return false; }

    /** type 0: the certificate's SHA-256, 1: the X.509 certificate itself. */
    @Override
    public boolean hasSigningCertificate(String name, byte[] cert, int type) {
        android.content.pm.Signature[] sigs = ours(name) ? app.signatures() : null;
        if (sigs == null || cert == null) return false;
        for (android.content.pm.Signature s : sigs) {
            byte[] b = s.toByteArray();
            try {
                if (type == 0) b = java.security.MessageDigest.getInstance("SHA-256").digest(b);
            } catch (Exception e) {
                return false;
            }
            if (java.util.Arrays.equals(b, cert)) return true;
        }
        return false;
    }
    @Override public int getComponentEnabledSetting(ComponentName c, int userId) { return 0; }   /* DEFAULT */
    @Override public String getInstallerPackageName(String name) { return null; }
    @Override public String getNameForUid(int uid) { return uid == app.info.uid ? app.pkg.packageName : null; }

    @Override
    public String[] getPackagesForUid(int uid) {
        return uid == app.info.uid ? new String[] { app.pkg.packageName } : null;
    }

    @Override public int getPackageUid(String name, long flags, int userId) { return ours(name) ? app.info.uid : -1; }
    @Override public boolean isPackageAvailable(String name, int userId) { return ours(name); }
    @Override public boolean isSafeMode() { return false; }
    @Override public int checkPermission(String perm, String pkg, int userId) { return 0; }   /* GRANTED */
    @Override public ResolveInfo resolveIntent(Intent i, String type, long flags, int userId) { return null; }

    @Override
    public ParceledListSlice queryIntentActivities(Intent i, String type, long flags, int userId) {
        return new ParceledListSlice(new ArrayList());
    }

    @Override public ProviderInfo resolveContentProvider(String name, long flags, int userId) { return null; }
    @Override public ParceledListSlice getSystemAvailableFeatures() { return new ParceledListSlice(new ArrayList()); }
    @Override public ParceledListSlice queryProperty(String name, int type) { return new ParceledListSlice(new ArrayList()); }

    @Override
    public android.content.pm.PackageManager.Property getPropertyAsUser(String name, String pkg, String cls, int userId) {
        return null;                                               /* <property> tags are not read yet */
    }
}
