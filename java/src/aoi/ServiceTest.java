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

        /* A stand-in answers a ParceledListSlice call with an empty slice, not null. */
        Object shortcuts = call("android.content.pm.IShortcutService", "getShortcuts");
        Object jobs = call("android.app.job.IJobScheduler", "getAllPendingJobsInNamespace");
        System.out.println("stand-in: getShortcuts " + size(shortcuts) + ", getAllPendingJobsInNamespace " + size(jobs));
    }

    private static Object call(String itf, String method) {
        try {
            IBinder s = new Services.NullService("aoi.test." + method);
            Object p = Class.forName(itf + "$Stub").getMethod("asInterface", IBinder.class).invoke(null, s);
            for (java.lang.reflect.Method m : Class.forName(itf).getMethods()) {
                if (!m.getName().equals(method)) continue;
                Class<?>[] t = m.getParameterTypes();
                Object[] a = new Object[t.length];
                for (int i = 0; i < t.length; i++)
                    a[i] = t[i] == int.class ? (Object) 0 : t[i] == long.class ? (Object) 0L
                         : t[i] == boolean.class ? (Object) false : null;
                return m.invoke(p, a);
            }
        } catch (Throwable e) {
            return e;
        }
        return "no method";
    }

    private static String size(Object o) {
        try {
            return o == null ? "null" : "" + ((java.util.List) o.getClass().getMethod("getList").invoke(o)).size();
        } catch (Throwable e) {
            return String.valueOf(o);
        }
    }
}
