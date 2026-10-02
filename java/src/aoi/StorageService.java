package aoi;

import android.os.Binder;
import android.os.Parcel;

/** "mount" (IStorageManager): no storage volumes. StorageManager.getVolumeList
 *  (Context.getExternalFilesDir: libGDX asks at start) gets an empty list instead of
 *  a null service (NullPointerException); there is no external storage, so
 *  getExternalFilesDir answers null. Every other call gets the default answer. */
final class StorageService extends Binder {
    private static final String DESCRIPTOR = "android.os.storage.IStorageManager";
    private final int getVolumeList = code("getVolumeList");

    private static int code(String method) {
        try {
            java.lang.reflect.Field f = Class.forName(DESCRIPTOR + "$Stub").getDeclaredField("TRANSACTION_" + method);
            f.setAccessible(true);
            return f.getInt(null);
        } catch (Exception e) {
            System.out.println("aoi: storage: no " + method + ": " + e);
            return -1;
        }
    }

    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
        if (code >= 0x00ffffff) return false;                      /* PING, INTERFACE, DUMP...: Binder's */
        if (code == getVolumeList) {
            reply.writeNoException();
            reply.writeInt(0);                                     /* StorageVolume[0] */
            return true;
        }
        return Services.missing(this, new AbstractMethodError("IStorageManager call " + code), reply);
    }
}
