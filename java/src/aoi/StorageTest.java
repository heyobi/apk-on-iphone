package aoi;

import android.os.ServiceManager;

/** Storage in the app process: aoi.StorageService's volume through the framework's
 *  IStorageManager proxy (what StorageManager.getVolumeList and so Environment's
 *  external storage state and directory read), and an ashmem region the
 *  way libcutils makes one (a memfd): android.os.SharedMemory mapped, written and
 *  read back, and a CursorWindow holding a 100 KB blob. */
public final class StorageTest {
    public static void main(String[] args) throws Exception {
        ServiceManager.addService("mount", new StorageService());
        Object ism = Class.forName("android.os.storage.IStorageManager$Stub").getMethod("asInterface", android.os.IBinder.class)
                .invoke(null, ServiceManager.getService("mount"));
        Object[] vols = (Object[]) ism.getClass().getMethod("getVolumeList", int.class, String.class, int.class)
                .invoke(ism, 0, "aoi.test", 0);
        Object state = vols.length == 0 ? "none" : vols[0].getClass().getMethod("getState").invoke(vols[0]);
        Object dir = vols.length == 0 ? null : vols[0].getClass().getMethod("getPathFile").invoke(vols[0]);

        Class<?> sm = Class.forName("android.os.SharedMemory");
        Object mem = sm.getMethod("create", String.class, int.class).invoke(null, "aoi.test", 8192);
        java.nio.ByteBuffer buf = (java.nio.ByteBuffer) sm.getMethod("mapReadWrite").invoke(mem);
        buf.putInt(4096, 0x41534d21);
        int size = (Integer) sm.getMethod("getSize").invoke(mem);
        int back = buf.getInt(4096);

        Class<?> cw = Class.forName("android.database.CursorWindow");
        Object w = cw.getConstructor(String.class).newInstance("aoi.test");
        cw.getMethod("setNumColumns", int.class).invoke(w, 1);
        cw.getMethod("allocRow").invoke(w);
        byte[] blob = new byte[100 * 1024];
        blob[blob.length - 1] = 42;
        boolean put = (Boolean) cw.getMethod("putBlob", byte[].class, int.class, int.class).invoke(w, blob, 0, 0);
        byte[] got = (byte[]) cw.getMethod("getBlob", int.class, int.class).invoke(w, 0, 0);
        System.out.println("storage: " + state + " at " + dir + ", shared memory " + size + " "
                + Integer.toHexString(back) + ", cursor window " + put + " " + (got == null ? -1 : got[got.length - 1]));
    }
}
