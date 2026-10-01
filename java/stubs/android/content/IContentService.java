package android.content;

import android.os.Binder;
import android.os.RemoteException;

/** Hidden; signatures from the guest's framework.jar (tools/dexsig.py). */
public interface IContentService {
    android.content.SyncAdapterType[] getSyncAdapterTypes() throws RemoteException;
    android.content.SyncAdapterType[] getSyncAdapterTypesAsUser(int a0) throws RemoteException;
    android.content.SyncStatusInfo getSyncStatus(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2) throws RemoteException;
    android.content.SyncStatusInfo getSyncStatusAsUser(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2, int a3) throws RemoteException;
    android.os.Bundle getCache(java.lang.String a0, android.net.Uri a1, int a2) throws RemoteException;
    boolean getMasterSyncAutomatically() throws RemoteException;
    boolean getMasterSyncAutomaticallyAsUser(int a0) throws RemoteException;
    boolean getSyncAutomatically(android.accounts.Account a0, java.lang.String a1) throws RemoteException;
    boolean getSyncAutomaticallyAsUser(android.accounts.Account a0, java.lang.String a1, int a2) throws RemoteException;
    boolean isSyncActive(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2) throws RemoteException;
    boolean isSyncPending(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2) throws RemoteException;
    boolean isSyncPendingAsUser(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2, int a3) throws RemoteException;
    int getIsSyncable(android.accounts.Account a0, java.lang.String a1) throws RemoteException;
    int getIsSyncableAsUser(android.accounts.Account a0, java.lang.String a1, int a2) throws RemoteException;
    java.lang.String getSyncAdapterPackageAsUser(java.lang.String a0, java.lang.String a1, int a2) throws RemoteException;
    java.lang.String[] getSyncAdapterPackagesForAuthorityAsUser(java.lang.String a0, int a1) throws RemoteException;
    java.util.List getCurrentSyncs() throws RemoteException;
    java.util.List getCurrentSyncsAsUser(int a0) throws RemoteException;
    java.util.List getPeriodicSyncs(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2) throws RemoteException;
    void addPeriodicSync(android.accounts.Account a0, java.lang.String a1, android.os.Bundle a2, long a3) throws RemoteException;
    void addStatusChangeListener(int a0, android.content.ISyncStatusObserver a1) throws RemoteException;
    void cancelRequest(android.content.SyncRequest a0) throws RemoteException;
    void cancelSync(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2) throws RemoteException;
    void cancelSyncAsUser(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2, int a3) throws RemoteException;
    void notifyChange(android.net.Uri[] a0, android.database.IContentObserver a1, boolean a2, int a3, int a4, int a5, java.lang.String a6) throws RemoteException;
    void onDbCorruption(java.lang.String a0, java.lang.String a1, java.lang.String a2) throws RemoteException;
    void putCache(java.lang.String a0, android.net.Uri a1, android.os.Bundle a2, int a3) throws RemoteException;
    void registerContentObserver(android.net.Uri a0, boolean a1, android.database.IContentObserver a2, int a3, int a4) throws RemoteException;
    void removePeriodicSync(android.accounts.Account a0, java.lang.String a1, android.os.Bundle a2) throws RemoteException;
    void removeStatusChangeListener(android.content.ISyncStatusObserver a0) throws RemoteException;
    void requestSync(android.accounts.Account a0, java.lang.String a1, android.os.Bundle a2, java.lang.String a3) throws RemoteException;
    void resetTodayStats() throws RemoteException;
    void setIsSyncable(android.accounts.Account a0, java.lang.String a1, int a2) throws RemoteException;
    void setIsSyncableAsUser(android.accounts.Account a0, java.lang.String a1, int a2, int a3) throws RemoteException;
    void setMasterSyncAutomatically(boolean a0) throws RemoteException;
    void setMasterSyncAutomaticallyAsUser(boolean a0, int a1) throws RemoteException;
    void setSyncAutomatically(android.accounts.Account a0, java.lang.String a1, boolean a2) throws RemoteException;
    void setSyncAutomaticallyAsUser(android.accounts.Account a0, java.lang.String a1, boolean a2, int a3) throws RemoteException;
    void sync(android.content.SyncRequest a0, java.lang.String a1) throws RemoteException;
    void syncAsUser(android.content.SyncRequest a0, int a1, java.lang.String a2) throws RemoteException;
    void unregisterContentObserver(android.database.IContentObserver a0) throws RemoteException;

    abstract class Stub extends Binder implements IContentService {
        public Stub(android.os.PermissionEnforcer enforcer) {}
    }
}
