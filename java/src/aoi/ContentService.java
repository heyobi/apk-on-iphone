package aoi;

import android.content.IContentService;

/** "content": no sync adapters, no observers delivered yet (registerContentObserver
 *  must not fail: Settings and the app's own providers use it). */
final class ContentService extends IContentService.Stub {
    /* Calls go through Stub.onTransact, and one we have not written gets the default
     * answer instead of an AbstractMethodError (aoi.Services). */
    @Override public android.os.IInterface queryLocalInterface(String descriptor) { return null; }
    @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags)
            throws android.os.RemoteException {
        try { return super.onTransact(code, data, reply, flags); }
        catch (AbstractMethodError e) { return Services.missing(this, e, reply); }
    }

    ContentService() { super(GrantAll.INSTANCE); }

    @Override public android.content.SyncAdapterType[] getSyncAdapterTypes() { return null; }
    @Override public android.content.SyncAdapterType[] getSyncAdapterTypesAsUser(int a0) { return null; }
    @Override public android.content.SyncStatusInfo getSyncStatus(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2) { return null; }
    @Override public android.content.SyncStatusInfo getSyncStatusAsUser(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2, int a3) { return null; }
    @Override public android.os.Bundle getCache(java.lang.String a0, android.net.Uri a1, int a2) { return null; }
    @Override public boolean getMasterSyncAutomatically() { return false; }
    @Override public boolean getMasterSyncAutomaticallyAsUser(int a0) { return false; }
    @Override public boolean getSyncAutomatically(android.accounts.Account a0, java.lang.String a1) { return false; }
    @Override public boolean getSyncAutomaticallyAsUser(android.accounts.Account a0, java.lang.String a1, int a2) { return false; }
    @Override public boolean isSyncActive(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2) { return false; }
    @Override public boolean isSyncPending(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2) { return false; }
    @Override public boolean isSyncPendingAsUser(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2, int a3) { return false; }
    @Override public int getIsSyncable(android.accounts.Account a0, java.lang.String a1) { return 0; }
    @Override public int getIsSyncableAsUser(android.accounts.Account a0, java.lang.String a1, int a2) { return 0; }
    @Override public java.lang.String getSyncAdapterPackageAsUser(java.lang.String a0, java.lang.String a1, int a2) { return null; }
    @Override public java.lang.String[] getSyncAdapterPackagesForAuthorityAsUser(java.lang.String a0, int a1) { return null; }
    @Override public java.util.List getCurrentSyncs() { return new java.util.ArrayList(); }
    @Override public java.util.List getCurrentSyncsAsUser(int a0) { return new java.util.ArrayList(); }
    @Override public java.util.List getPeriodicSyncs(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2) { return new java.util.ArrayList(); }
    @Override public void addPeriodicSync(android.accounts.Account a0, java.lang.String a1, android.os.Bundle a2, long a3) { }
    @Override public void addStatusChangeListener(int a0, android.content.ISyncStatusObserver a1) { }
    @Override public void cancelRequest(android.content.SyncRequest a0) { }
    @Override public void cancelSync(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2) { }
    @Override public void cancelSyncAsUser(android.accounts.Account a0, java.lang.String a1, android.content.ComponentName a2, int a3) { }
    @Override public void notifyChange(android.net.Uri[] a0, android.database.IContentObserver a1, boolean a2, int a3, int a4, int a5, java.lang.String a6) { }
    @Override public void onDbCorruption(java.lang.String a0, java.lang.String a1, java.lang.String a2) { }
    @Override public void putCache(java.lang.String a0, android.net.Uri a1, android.os.Bundle a2, int a3) { }
    @Override public void registerContentObserver(android.net.Uri a0, boolean a1, android.database.IContentObserver a2, int a3, int a4) { }
    @Override public void removePeriodicSync(android.accounts.Account a0, java.lang.String a1, android.os.Bundle a2) { }
    @Override public void removeStatusChangeListener(android.content.ISyncStatusObserver a0) { }
    @Override public void requestSync(android.accounts.Account a0, java.lang.String a1, android.os.Bundle a2, java.lang.String a3) { }
    @Override public void resetTodayStats() { }
    @Override public void setIsSyncable(android.accounts.Account a0, java.lang.String a1, int a2) { }
    @Override public void setIsSyncableAsUser(android.accounts.Account a0, java.lang.String a1, int a2, int a3) { }
    @Override public void setMasterSyncAutomatically(boolean a0) { }
    @Override public void setMasterSyncAutomaticallyAsUser(boolean a0, int a1) { }
    @Override public void setSyncAutomatically(android.accounts.Account a0, java.lang.String a1, boolean a2) { }
    @Override public void setSyncAutomaticallyAsUser(android.accounts.Account a0, java.lang.String a1, boolean a2, int a3) { }
    @Override public void sync(android.content.SyncRequest a0, java.lang.String a1) { }
    @Override public void syncAsUser(android.content.SyncRequest a0, int a1, java.lang.String a2) { }
    @Override public void unregisterContentObserver(android.database.IContentObserver a0) { }
}
