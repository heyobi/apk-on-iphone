package android.os;

/** Hidden API: the in-process servicemanager of core/binder.c answers these. */
public final class ServiceManager {
    public static IBinder getService(String name) { return null; }
    public static IBinder checkService(String name) { return null; }
    public static void addService(String name, IBinder service) {}
}
