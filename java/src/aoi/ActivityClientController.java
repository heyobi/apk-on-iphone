package aoi;

import android.app.ActivityManager;
import android.app.IActivityClientController;
import android.app.IRequestFinishCallback;
import android.content.ComponentName;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PersistableBundle;
import android.window.SizeConfigurationBuckets;

/** What an activity tells the system about itself (lifecycle reports) and asks it:
 *  one task on display 0, the app's own, at the top. */
final class ActivityClientController extends IActivityClientController.Stub {
    private final App app;

    ActivityClientController(App app) { this.app = app; }

    private static void log(String what) { System.out.println("aoi: activity " + what); }

    private static boolean snapshotAsked;

    /** Once the app has settled (idle, and a few seconds for late work), the host may
     *  save the whole process (core/snap.c): opening /dev/aoi_snapshot asks for it, and
     *  the next launch resumes from there. The open itself always fails. */
    @Override public void activityIdle(IBinder t, Configuration c, boolean stopProfiling) {
        log("idle");
        synchronized (ActivityClientController.class) {
            if (snapshotAsked) return;
            snapshotAsked = true;
        }
        Thread s = new Thread(new Runnable() {
            @Override public void run() {
                try {
                    Thread.sleep(3000);
                    new java.io.FileInputStream("/dev/aoi_snapshot").close();
                } catch (Exception e) {
                    // expected: the host has taken it, or does not want one
                }
            }
        }, "aoi-snapshot");
        s.setDaemon(true);
        s.start();
    }
    @Override public void activityResumed(IBinder t, boolean splash) {
        log("resumed");
        try {                                                      /* what the app sees of its screen (reflection: */
            Class<?> at = Class.forName("android.app.ActivityThread");   /* no Activity stub) */
            Object a = at.getMethod("getActivity", IBinder.class).invoke(at.getMethod("currentActivityThread").invoke(null), t);
            Object res = a.getClass().getMethod("getResources").invoke(a);
            System.out.println("aoi: metrics " + res.getClass().getMethod("getDisplayMetrics").invoke(res));
            System.out.println("aoi: config " + res.getClass().getMethod("getConfiguration").invoke(res));
            Object wm = a.getClass().getMethod("getWindowManager").invoke(a);
            Object m = wm.getClass().getMethod("getCurrentWindowMetrics").invoke(wm);
            System.out.println("aoi: window " + m.getClass().getMethod("getBounds").invoke(m) + " "
                    + m.getClass().getMethod("getWindowInsets").invoke(m));
        } catch (Throwable e) {
            System.out.println("aoi: metrics: " + e);
        }
    }
    @Override public void activityTopResumedStateLost() {}
    @Override public void activityRefreshed(IBinder t) {}
    @Override public void activityPaused(IBinder t) { log("paused"); }
    @Override public void activityStopped(IBinder t, Bundle s, PersistableBundle p, CharSequence d) { log("stopped"); }
    @Override public void activityDestroyed(IBinder t) { log("destroyed"); }
    @Override public void activityLocalRelaunch(IBinder t) {}
    @Override public void activityRelaunched(IBinder t) {}
    @Override public void reportSizeConfigurations(IBinder t, SizeConfigurationBuckets b) {}
    @Override public int getDisplayId(IBinder t) { return 0; }
    @Override public int getTaskForActivity(IBinder t, boolean onlyRoot) { return 1; }
    @Override public Configuration getTaskConfiguration(IBinder t) { return ActivityManager_phone(); }
    @Override public boolean isTopOfTask(IBinder t) { return true; }
    @Override public boolean willActivityBeVisible(IBinder t) { return true; }
    @Override public int getRequestedOrientation(IBinder t) { return -1; }   /* UNSPECIFIED */
    @Override public void setRequestedOrientation(IBinder t, int o) {}
    @Override public String getCallingPackage(IBinder t) { return null; }
    @Override public ComponentName getCallingActivity(IBinder t) { return null; }
    @Override public int getLaunchedFromUid(IBinder t) { return app.info.uid; }
    @Override public String getLaunchedFromPackage(IBinder t) { return app.pkg.packageName; }
    @Override public boolean isImmersive(IBinder t) { return false; }
    @Override public void setImmersive(IBinder t, boolean immersive) {}
    @Override public void setTaskDescription(IBinder t, ActivityManager.TaskDescription d) {}

    /** Leaving the app (back on its root activity, finish, task to back): as Android's
     *  launcher would come up, the host shows its own (open "/dev/aoi_home"); the
     *  process stays as it is, ready to be shown again. */
    private static void home() {
        try {
            new java.io.FileInputStream("/dev/aoi_home").close();
        } catch (Exception e) {
            // expected: the open always fails
        }
    }

    @Override
    public boolean finishActivity(IBinder t, int code, Intent data, int finishTask) {
        log("finish requested");
        home();
        return true;
    }

    @Override public boolean moveActivityTaskToBack(IBinder t, boolean nonRoot) { home(); return true; }
    @Override public void onBackPressed(IBinder t, IRequestFinishCallback cb) { log("back at the root"); home(); }
    @Override public void splashScreenAttached(IBinder t) {}
    @Override public void reportActivityFullyDrawn(IBinder t, boolean restored) { log("fully drawn"); }
    @Override public void overridePendingTransition(IBinder t, String pkg, int enter, int exit, int bg) {}
    @Override public void setRecentsScreenshotEnabled(IBinder t, boolean enabled) {}
    @Override public void invalidateHomeTaskSnapshot(IBinder t) {}

    private static Configuration ActivityManager_phone() { return aoi.ActivityManager.phone(); }
}
