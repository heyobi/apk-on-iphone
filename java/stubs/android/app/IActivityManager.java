package android.app;

import android.os.Binder;
import android.os.RemoteException;

/** Hidden; only the calls our ActivityManager implements are declared. */
public interface IActivityManager {
    void attachApplication(IApplicationThread thread, long startSeq) throws RemoteException;
    void finishAttachApplication(long startSeq) throws RemoteException;
    void handleApplicationCrash(android.os.IBinder app, ApplicationErrorReport.ParcelableCrashInfo crashInfo)
            throws RemoteException;
    int checkPermission(String permission, int pid, int uid) throws RemoteException;
    int checkPermissionForDevice(String permission, int pid, int uid, int deviceId) throws RemoteException;
    int checkUriPermission(android.net.Uri uri, int pid, int uid, int mode, int userId, android.os.IBinder callerToken)
            throws RemoteException;
    int getCurrentUserId() throws RemoteException;
    boolean isUserAMonkey() throws RemoteException;
    java.util.List getRunningAppProcesses() throws RemoteException;
    android.content.Intent registerReceiverWithFeature(IApplicationThread caller, String callerPackage,
            String callingFeatureId, String receiverId, android.content.IIntentReceiver receiver,
            android.content.IntentFilter filter, String requiredPermission, int userId, int flags) throws RemoteException;
    void unregisterReceiver(android.content.IIntentReceiver receiver) throws RemoteException;
    int broadcastIntentWithFeature(IApplicationThread caller, String callingFeatureId, android.content.Intent intent,
            String resolvedType, android.content.IIntentReceiver resultTo, int resultCode, String resultData,
            android.os.Bundle map, String[] requiredPermissions, String[] excludePermissions,
            String[] excludePackages, int appOp, android.os.Bundle options, boolean serialized, boolean sticky,
            int userId) throws RemoteException;
    void finishReceiver(android.os.IBinder who, int resultCode, String resultData, android.os.Bundle map,
            boolean abortBroadcast, int flags) throws RemoteException;
    ContentProviderHolder getContentProvider(IApplicationThread caller, String callingPackage, String name, int userId,
            boolean stable) throws RemoteException;
    void publishContentProviders(IApplicationThread caller, java.util.List providers) throws RemoteException;
    void getMemoryInfo(ActivityManager.MemoryInfo outInfo) throws RemoteException;
    void getMyMemoryState(ActivityManager.RunningAppProcessInfo outInfo) throws RemoteException;
    boolean isAppFreezerSupported() throws RemoteException;
    void setProcessImportant(android.os.IBinder token, int pid, boolean isForeground, String reason)
            throws RemoteException;
    void setRenderThread(int tid) throws RemoteException;
    int getUidProcessState(int uid, String callingPackage) throws RemoteException;

    abstract class Stub extends Binder implements IActivityManager {
        public Stub() {}
    }
}
