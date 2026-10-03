package aoi;

import android.os.Binder;
import android.os.Parcel;

/** "mount" (IStorageManager): one volume, the primary shared storage, emulated and
 *  mounted, at /data/media/0 (in the app's data, so writable). StorageManager.getVolumeList
 *  is what Environment.getExternalStorageState/-Directory and
 *  Context.getExternalFilesDir read; with no volume at all getExternalStorageState
 *  indexes an empty array (WhatsApp died of it). Every other call gets the default answer. */
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

    static final String PATH = "/data/media/0";
    private Object volume;

    /** StorageVolume(id, path, internalPath, description, primary, removable, emulated,
     *  externallyManaged, allowMassStorage, maxFileSize, owner, uuid, fsUuid, state). */
    private synchronized Object volume() {
        if (volume != null) return volume;
        try {
            java.io.File dir = new java.io.File(PATH);
            dir.mkdirs();
            Class<?> c = Class.forName("android.os.storage.StorageVolume");
            Object owner = Class.forName("android.os.UserHandle").getMethod("of", int.class).invoke(null, 0);
            for (java.lang.reflect.Constructor<?> k : c.getDeclaredConstructors()) {
                Class<?>[] t = k.getParameterTypes();
                if (t.length != 14 || t[0] != String.class || t[1] != java.io.File.class) continue;
                k.setAccessible(true);
                volume = k.newInstance("emulated;0", dir, dir, "Internal shared storage",
                        true, false, true, false, false, 0L, owner, null, null, "mounted");
            }
            if (volume == null) System.out.println("aoi: storage: no StorageVolume constructor to use");
        } catch (Exception e) {
            System.out.println("aoi: storage: " + e);
        }
        return volume;
    }

    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
        if (code >= 0x00ffffff) return false;                      /* PING, INTERFACE, DUMP...: Binder's */
        if (code == getVolumeList) {
            reply.writeNoException();
            Object v = volume();
            if (v == null) { reply.writeInt(0); return true; }     /* StorageVolume[0] */
            reply.writeInt(1);                                     /* writeTypedArray: one, non-null */
            reply.writeInt(1);
            try {
                v.getClass().getMethod("writeToParcel", Parcel.class, int.class).invoke(v, reply, 0);
            } catch (Exception e) {
                System.out.println("aoi: storage: " + e);
            }
            return true;
        }
        return Services.missing(this, new AbstractMethodError("IStorageManager call " + code), reply);
    }
}
