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

    @Override public void activityIdle(IBinder t, Configuration c, boolean stopProfiling) { log("idle"); }
    @Override public void activityResumed(IBinder t, boolean splash) { log("resumed"); }
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

    @Override
    public boolean finishActivity(IBinder t, int code, Intent data, int finishTask) {
        log("finish requested");
        return true;
    }

    @Override public boolean moveActivityTaskToBack(IBinder t, boolean nonRoot) { return true; }
    @Override public void onBackPressed(IBinder t, IRequestFinishCallback cb) {}
    @Override public void splashScreenAttached(IBinder t) {}
    @Override public void reportActivityFullyDrawn(IBinder t, boolean restored) { log("fully drawn"); }
    @Override public void overridePendingTransition(IBinder t, String pkg, int enter, int exit, int bg) {}
    @Override public void setRecentsScreenshotEnabled(IBinder t, boolean enabled) {}
    @Override public void invalidateHomeTaskSnapshot(IBinder t) {}

    private static Configuration ActivityManager_phone() { return aoi.ActivityManager.phone(); }
}
