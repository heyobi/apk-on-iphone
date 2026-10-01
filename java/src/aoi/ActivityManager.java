package aoi;

import android.app.ApplicationErrorReport;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfoList;
import android.content.res.CompatibilityInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import java.util.ArrayList;
import java.util.HashMap;

/**
 * The in-process ActivityManager: only what an app process calls. A call it does
 * not implement throws AbstractMethodError, naming the next method to write.
 */
final class ActivityManager extends IActivityManager.Stub {
    private final ApplicationInfo app;
    private final ActivityInfo launcher;
    IApplicationThread thread;

    ActivityManager(ApplicationInfo app, ActivityInfo launcher) {
        this.app = app;
        this.launcher = launcher;
    }

    /** ActivityThread.attach(): the app is up; tell it which application it runs. */
    @Override
    public void attachApplication(IApplicationThread t, long startSeq) throws RemoteException {
        thread = t;
        System.out.println("aoi: attachApplication, binding " + app.packageName);
        Configuration config = phone();
        t.bindApplication(app.packageName, app, null, null, false, ProviderInfoList.fromList(new ArrayList()),
                null, null, null, null, null, 0,
                false, false, false, false, config, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                new HashMap<String, Object>(), new Bundle(), "unknown", null, null, new long[0], null, 0, 0);
    }

    /** What the app is told about the device: a phone-sized portrait screen (iPhone-like
     *  density), English, normal UI mode, touch, no keyboard. */
    static Configuration phone() {
        Configuration c = new Configuration();
        c.setToDefaults();
        c.setLocale(java.util.Locale.US);
        c.densityDpi = 460;
        c.screenWidthDp = 393; c.screenHeightDp = 852; c.smallestScreenWidthDp = 393;
        c.orientation = 1;                                         /* ORIENTATION_PORTRAIT */
        c.uiMode = 0x11;                                           /* TYPE_NORMAL | NIGHT_NO */
        c.screenLayout = 0x12;                                     /* SIZE_NORMAL | LONG_NO */
        c.touchscreen = 3;                                         /* TOUCHSCREEN_FINGER */
        c.keyboard = 1; c.navigation = 1;                          /* NOKEYS, NONAV */
        c.fontScale = 1f;
        return c;
    }

    /** The application is bound and created: launch its launcher activity and resume
     *  it, as the real ActivityManager does from here (realStartActivityLocked). */
    @Override
    public void finishAttachApplication(long startSeq) throws RemoteException {
        if (launcher == null) { System.out.println("aoi: no launcher activity"); return; }
        System.out.println("aoi: launching " + launcher.name);
        android.os.IBinder token = new android.os.Binder();
        android.content.Intent intent = android.content.Intent.makeMainActivity(
                new android.content.ComponentName(app.packageName, launcher.name));
        Configuration config = phone();
        android.app.servertransaction.ClientTransaction tr = android.app.servertransaction.ClientTransaction.obtain(thread);
        tr.addTransactionItem(android.app.servertransaction.LaunchActivityItem.obtain(token, intent, 1, launcher, config,
                new Configuration(), 0, null, null, 2 /* PROCESS_STATE_TOP */, null, null, null, null, null, true, null,
                new android.os.Binder(), null, new android.os.Binder(), false, null, null,
                new android.window.ActivityWindowInfo()));
        tr.addTransactionItem(android.app.servertransaction.ResumeActivityItem.obtain(token, true, false));
        thread.scheduleTransaction(tr);
    }

    /* ---- permissions: one app, everything granted, but it is not the system UI ---- */
    private static int grant(String permission) {
        return "android.permission.STATUS_BAR_SERVICE".equals(permission) ? -1 : 0;
    }

    @Override public int checkPermission(String perm, int pid, int uid) { return grant(perm); }
    @Override public int checkPermissionForDevice(String perm, int pid, int uid, int dev) { return grant(perm); }

    @Override
    public int checkUriPermission(android.net.Uri uri, int pid, int uid, int mode, int userId, IBinder token) {
        return 0;
    }

    /* ---- the user and the process ---- */
    @Override public int getCurrentUserId() { return 0; }
    @Override public boolean isUserAMonkey() { return false; }
    @Override public java.util.List getRunningAppProcesses() { return new ArrayList(); }
    @Override public boolean isAppFreezerSupported() { return false; }
    @Override public void setProcessImportant(IBinder token, int pid, boolean fg, String reason) {}
    @Override public void setRenderThread(int tid) {}
    @Override public int getUidProcessState(int uid, String pkg) { return 2; }   /* PROCESS_STATE_TOP */

    @Override
    public void getMemoryInfo(android.app.ActivityManager.MemoryInfo m) {
        m.totalMem = 4L << 30; m.availMem = 2L << 30; m.threshold = 256L << 20; m.lowMemory = false;
    }

    @Override public void getMyMemoryState(android.app.ActivityManager.RunningAppProcessInfo out) {}

    /* ---- broadcasts: receivers are registered but nothing is ever sent to them yet ---- */
    @Override
    public android.content.Intent registerReceiverWithFeature(IApplicationThread caller, String pkg, String feature,
            String receiverId, android.content.IIntentReceiver receiver, android.content.IntentFilter filter,
            String perm, int userId, int flags) {
        return null;                                               /* no sticky broadcast */
    }

    @Override public void unregisterReceiver(android.content.IIntentReceiver receiver) {}

    @Override
    public int broadcastIntentWithFeature(IApplicationThread caller, String feature, android.content.Intent intent,
            String type, android.content.IIntentReceiver resultTo, int code, String data, Bundle map,
            String[] perms, String[] excludePerms, String[] excludePkgs, int appOp, Bundle options,
            boolean serialized, boolean sticky, int userId) {
        return 0;                                                  /* BROADCAST_SUCCESS */
    }

    @Override public void finishReceiver(IBinder who, int code, String data, Bundle map, boolean abort, int flags) {}

    /* ---- content providers: none outside the app ---- */
    @Override
    public android.app.ContentProviderHolder getContentProvider(IApplicationThread caller, String pkg, String name,
            int userId, boolean stable) {
        return null;
    }

    @Override public void publishContentProviders(IApplicationThread caller, java.util.List providers) {}

    /** The app's uncaught-exception handler reports here before it kills the process. */
    @Override
    public void handleApplicationCrash(IBinder app, ApplicationErrorReport.ParcelableCrashInfo crash) {
        System.out.println("aoi: the app crashed: " + crash.exceptionClassName + ": " + crash.exceptionMessage
                + " at " + crash.throwClassName + "." + crash.throwMethodName + ":" + crash.throwLineNumber);
    }
}
