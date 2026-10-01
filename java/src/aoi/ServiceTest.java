package aoi;

import android.os.Binder;
import android.os.IBinder;
import android.os.ServiceManager;

/** A service registered in this process comes back as the same local object. */
public final class ServiceTest {
    public static void main(String[] args) {
        Binder b = new Binder();
        ServiceManager.addService("aoi.test", b);
        IBinder got = ServiceManager.getService("aoi.test");
        IBinder none = ServiceManager.checkService("aoi.none");
        System.out.println(got == b && none == null ? "servicemanager: local binder ok" : "servicemanager: got " + got);
    }
}
