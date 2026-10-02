package aoi;

import android.os.Bundle;
import android.os.IUserManager;

/** One user, 0: the owner, unlocked, running in the foreground, unrestricted. */
final class UserManager extends IUserManager.Stub {
    @Override public boolean isUserUnlocked(int u) { return u == 0; }
    @Override public boolean isUserUnlockingOrUnlocked(int u) { return u == 0; }
    @Override public boolean isUserRunning(int u) { return u == 0; }
    @Override public boolean isUserForeground(int u) { return u == 0; }
    @Override public boolean isUserVisible(int u) { return u == 0; }
    @Override public boolean hasUserRestriction(String key, int u) { return false; }
    @Override public Bundle getUserRestrictions(int u) { return new Bundle(); }
    @Override public boolean isDemoUser(int u) { return false; }
    @Override public boolean isRestricted(int u) { return false; }
    @Override public boolean isQuietModeEnabled(int u) { return false; }
    @Override public boolean canHaveRestrictedProfile(int u) { return false; }
    @Override public int getUserSerialNumber(int u) { return u; }
    @Override public int getUserHandle(int serial) { return serial; }
    @Override public int getMainUserId() { return 0; }
    @Override public int[] getProfileIds(int u, boolean enabledOnly) { return new int[] { 0 }; }
    @Override public String getProfileType(int u) { return ""; }
    @Override public long getUserCreationTime(int u) { return 1700000000000L; }
    @Override public Bundle getApplicationRestrictionsForUser(String pkg, int u) { return new Bundle(); }
}
