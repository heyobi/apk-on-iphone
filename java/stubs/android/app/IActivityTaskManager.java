package android.app;

import android.content.pm.ConfigurationInfo;
import android.os.Binder;
import android.os.RemoteException;
import java.util.List;

/** Hidden; only the calls our ActivityTaskManager implements. */
public interface IActivityTaskManager {
    IActivityClientController getActivityClientController() throws RemoteException;
    ConfigurationInfo getDeviceConfigurationInfo() throws RemoteException;
    List getTasks(int maxNum, boolean filterOnlyVisibleRecents, boolean keepIntentExtra, int displayId)
            throws RemoteException;
    List getAppTasks(String callingPackage) throws RemoteException;
    int getLastResumedActivityUserId() throws RemoteException;
    boolean isInLockTaskMode() throws RemoteException;
    int getLockTaskModeState() throws RemoteException;
    int startActivity(IApplicationThread caller, String callingPackage, String callingFeatureId,
            android.content.Intent intent, String resolvedType, android.os.IBinder resultTo, String resultWho,
            int requestCode, int flags, ProfilerInfo profilerInfo, android.os.Bundle options) throws RemoteException;

    abstract class Stub extends Binder implements IActivityTaskManager {
        public Stub() {}
    }
}
