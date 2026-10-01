package android.app;

import android.content.ComponentName;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PersistableBundle;
import android.os.RemoteException;
import android.window.SizeConfigurationBuckets;

/** Hidden; only the calls our ActivityClientController implements. */
public interface IActivityClientController {
    void activityIdle(IBinder token, Configuration config, boolean stopProfiling) throws RemoteException;
    void activityResumed(IBinder token, boolean handleSplashScreenExit) throws RemoteException;
    void activityTopResumedStateLost() throws RemoteException;
    void activityRefreshed(IBinder token) throws RemoteException;
    void activityPaused(IBinder token) throws RemoteException;
    void activityStopped(IBinder token, Bundle state, PersistableBundle persistentState, CharSequence description)
            throws RemoteException;
    void activityDestroyed(IBinder token) throws RemoteException;
    void activityLocalRelaunch(IBinder token) throws RemoteException;
    void activityRelaunched(IBinder token) throws RemoteException;
    void reportSizeConfigurations(IBinder token, SizeConfigurationBuckets sizeConfigurations) throws RemoteException;
    int getDisplayId(IBinder activityToken) throws RemoteException;
    int getTaskForActivity(IBinder token, boolean onlyRoot) throws RemoteException;
    Configuration getTaskConfiguration(IBinder activityToken) throws RemoteException;
    boolean isTopOfTask(IBinder token) throws RemoteException;
    boolean willActivityBeVisible(IBinder token) throws RemoteException;
    int getRequestedOrientation(IBinder token) throws RemoteException;
    void setRequestedOrientation(IBinder token, int requestedOrientation) throws RemoteException;
    String getCallingPackage(IBinder token) throws RemoteException;
    ComponentName getCallingActivity(IBinder token) throws RemoteException;
    int getLaunchedFromUid(IBinder token) throws RemoteException;
    String getLaunchedFromPackage(IBinder token) throws RemoteException;
    boolean isImmersive(IBinder token) throws RemoteException;
    void setImmersive(IBinder token, boolean immersive) throws RemoteException;
    void setTaskDescription(IBinder token, ActivityManager.TaskDescription values) throws RemoteException;
    boolean finishActivity(IBinder token, int code, Intent data, int finishTask) throws RemoteException;
    boolean moveActivityTaskToBack(IBinder token, boolean nonRoot) throws RemoteException;
    void onBackPressed(IBinder activityToken, IRequestFinishCallback callback) throws RemoteException;
    void splashScreenAttached(IBinder token) throws RemoteException;
    void reportActivityFullyDrawn(IBinder token, boolean restoredFromBundle) throws RemoteException;
    void overridePendingTransition(IBinder token, String packageName, int enterAnim, int exitAnim, int backgroundColor)
            throws RemoteException;
    void setRecentsScreenshotEnabled(IBinder token, boolean enabled) throws RemoteException;
    void invalidateHomeTaskSnapshot(IBinder homeToken) throws RemoteException;

    abstract class Stub extends Binder implements IActivityClientController {
        public Stub() {}
    }
}
