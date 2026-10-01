package android.os;

/** Hidden; only the calls our UserManager implements. */
public interface IUserManager {
    boolean isUserUnlocked(int userId) throws RemoteException;
    boolean isUserUnlockingOrUnlocked(int userId) throws RemoteException;
    boolean isUserRunning(int userId) throws RemoteException;
    boolean isUserForeground(int userId) throws RemoteException;
    boolean isUserVisible(int userId) throws RemoteException;
    boolean hasUserRestriction(String restrictionKey, int userId) throws RemoteException;
    Bundle getUserRestrictions(int userId) throws RemoteException;
    boolean isDemoUser(int userId) throws RemoteException;
    boolean isRestricted(int userId) throws RemoteException;
    boolean isQuietModeEnabled(int userId) throws RemoteException;
    boolean canHaveRestrictedProfile(int userId) throws RemoteException;
    int getUserSerialNumber(int userId) throws RemoteException;
    int getUserHandle(int userSerialNumber) throws RemoteException;
    int getMainUserId() throws RemoteException;
    int[] getProfileIds(int userId, boolean enabledOnly) throws RemoteException;
    String getProfileType(int userId) throws RemoteException;
    long getUserCreationTime(int userId) throws RemoteException;

    abstract class Stub extends Binder implements IUserManager {
        public Stub() {}
    }
}
