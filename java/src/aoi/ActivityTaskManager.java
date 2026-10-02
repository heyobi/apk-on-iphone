package aoi;

import android.app.IActivityClientController;
import android.app.IActivityTaskManager;
import android.content.pm.ConfigurationInfo;
import java.util.ArrayList;
import java.util.List;

/** The in-process ActivityTaskManager: one task, the app's; activities report to
 *  the ActivityClientController it hands out. */
final class ActivityTaskManager extends IActivityTaskManager.Stub {
    /* Calls go through Stub.onTransact, and one we have not written gets the default
     * answer instead of an AbstractMethodError (aoi.Services). */
    @Override public android.os.IInterface queryLocalInterface(String descriptor) { return null; }
    @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags)
            throws android.os.RemoteException {
        try { return super.onTransact(code, data, reply, flags); }
        catch (AbstractMethodError e) { return Services.missing(this, e, reply); }
    }

    private final ActivityClientController client;

    ActivityTaskManager(App app) { client = new ActivityClientController(app); }

    @Override public IActivityClientController getActivityClientController() { return client; }

    @Override
    public ConfigurationInfo getDeviceConfigurationInfo() {
        ConfigurationInfo c = new ConfigurationInfo();
        c.reqTouchScreen = 3;                                      /* TOUCHSCREEN_FINGER */
        c.reqGlEsVersion = 0x30002;                                /* OpenGL ES 3.2 (ANGLE on Metal, later) */
        return c;
    }

    /** An activity of the app goes on top of its task (aoi.Activities); another app's
     *  is not here: START_INTENT_NOT_RESOLVED, which the caller sees as
     *  ActivityNotFoundException. Results (startActivityForResult) are not sent back yet. */
    @Override
    public int startActivity(android.app.IApplicationThread caller, String pkg, String feature,
            android.content.Intent intent, String type, android.os.IBinder resultTo, String resultWho,
            int requestCode, int flags, android.app.ProfilerInfo profiler, android.os.Bundle options) {
        android.content.pm.ActivityInfo a = Activities.resolve(intent);
        if (a == null) {
            System.out.println("aoi: no activity for " + intent);
            return -91;                                            /* START_INTENT_NOT_RESOLVED */
        }
        Activities.launch(a, intent);
        return 0;                                                  /* START_SUCCESS */
    }

    @Override public List getTasks(int max, boolean recents, boolean keepIntent, int display) { return new ArrayList(); }
    @Override public List getAppTasks(String pkg) { return new ArrayList(); }
    @Override public int getLastResumedActivityUserId() { return 0; }
    @Override public boolean isInLockTaskMode() { return false; }
    @Override public int getLockTaskModeState() { return 0; }
}
