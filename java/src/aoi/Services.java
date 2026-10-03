package aoi;

import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ServiceManager;
import android.content.pm.ParceledListSlice;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;

/** What keeps a missing piece of Android from killing an app.
 *
 *  Our services implement the calls apps have needed so far; a call we have not
 *  written would be an AbstractMethodError in the app. So each service takes its
 *  calls as transactions (queryLocalInterface answers null: the framework talks to it
 *  through its Stub.Proxy, as across processes) and a call that is not there gets the
 *  default answer, "no exception" and zeros: 0, false, null, empty. It is logged once
 *  ("aoi: missing ..."), the list of what to write next.
 *
 *  And every service Android knows by name (Context's *_SERVICE) that we do not
 *  provide is registered as a NullService, which answers every call that way: an app
 *  asking for one gets a working manager instead of null. */
final class Services {
    private static final HashSet<String> logged = new HashSet<String>();

    /** A call the service does not implement: the default answer. */
    static boolean missing(Object service, Throwable e, Parcel reply) {
        String what = service.getClass().getSimpleName() + ": " + e.getMessage();
        synchronized (logged) {
            if (logged.add(what)) System.out.println("aoi: missing " + what + " (default answer)");
        }
        if (reply != null) {
            reply.setDataSize(0);
            reply.setDataPosition(0);
            reply.writeNoException();
        }
        return true;
    }

    /** A service nobody wrote: every call returns "no exception" and zeros. */
    public static final class NullService extends Binder {
        private final String name;
        NullService(String name) { this.name = name; }

        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            if (code >= 0x00ffffff) return false;                  /* PING, INTERFACE, DUMP...: Binder's */
            synchronized (logged) {
                if (logged.add(name)) System.out.println("aoi: service \"" + name + "\" is a stand-in (default answers)");
            }
            if (reply != null) {
                reply.writeNoException();
                Method m = method(data, code);
                Object empty = emptyReturn(m);                      /* a null one is an NPE in the manager */
                if (m != null && m.getReturnType() == boolean.class && m.getName().startsWith("register")) {
                    reply.writeInt(1);                              /* a listener registered: true (thermal) */
                } else if (empty != null) {
                    reply.writeInt(1);                              /* writeTypedObject: non-null */
                    try {
                        empty.getClass().getMethod("writeToParcel", Parcel.class, int.class).invoke(empty, reply, 0);
                    } catch (Exception e) {
                        System.out.println("aoi: stand-in " + name + ": " + e);
                    }
                }
            }
            return true;
        }
    }

    private static final HashMap<String, Method> methods = new HashMap<String, Method>();

    /** The empty value a stand-in answers when the call returns a ParceledListSlice
     *  (ShortcutManager), a LocaleList (LocaleManager.getApplicationLocales: AppCompat)
     *  StorageStats (StorageStatsManager.queryStatsForPackage: Chromium) or a Bundle
     *  (RestrictionsManager.getApplicationRestrictions: Chromium's policies), else null.
     *  Everything else's zeros read as empty (arrays, lists) or as values. */
    static Object emptyReturn(Method m) {
        Class<?> t = m != null ? m.getReturnType() : null;
        if (t == null) return null;
        if (ParceledListSlice.class.isAssignableFrom(t)) return new ParceledListSlice(new ArrayList());
        if (t.getName().equals("android.os.LocaleList")) {
            try { return t.getMethod("getEmptyLocaleList").invoke(null); } catch (Exception e) { return null; }
        }
        if (t == android.os.Bundle.class) return new android.os.Bundle();   /* restrictions & co.: none */
        if (t.getName().equals("android.app.usage.StorageStats")) {   /* queryStatsForPackage: zero bytes */
            try {                                                      /* (Chromium's storage metrics NPE) */
                java.lang.reflect.Constructor<?> k = t.getDeclaredConstructor();
                k.setAccessible(true);
                return k.newInstance();
            } catch (Exception e) { return null; }
        }
        return null;
    }

    /** The interface method of an AIDL call. Its interface is the token that opens the
     *  data (strict mode policy, work source, header, then the name); the method is the
     *  Stub's TRANSACTION_ field with this code. */
    static Method method(Parcel data, int code) {
        String desc = null;
        int at = data.dataPosition();
        for (int off = 12; off >= 4 && desc == null; off -= 4) {
            try {
                data.setDataPosition(off);
                String s = data.readString();
                if (s != null && s.indexOf('.') > 0) { Class.forName(s); desc = s; }
            } catch (Throwable e) { /* not here */ }
        }
        data.setDataPosition(at);
        if (desc == null) return null;
        String key = desc + "#" + code;
        synchronized (methods) {
            if (methods.containsKey(key)) return methods.get(key);
        }
        Method r = null;
        try {
            Class<?> itf = Class.forName(desc);
            for (Field f : Class.forName(desc + "$Stub").getDeclaredFields()) {
                if (!f.getName().startsWith("TRANSACTION_") || f.getType() != int.class) continue;
                f.setAccessible(true);
                if (f.getInt(null) != code) continue;
                String m = f.getName().substring("TRANSACTION_".length());
                for (Method x : itf.getMethods())
                    if (x.getName().equals(m)) r = x;
            }
        } catch (Throwable e) { /* no Stub: zeros */ }
        synchronized (methods) { methods.put(key, r); }
        return r;
    }

    /** Names whose managers do better without a service than with zeros. */
    private static final String[] KEEP_ABSENT = {
        "textclassification",                                      /* falls back to the local classifier */
        "autofill", "content_capture",                             /* else a 5 s wait for a reply that never comes */
    };

    /** Services managers look up by a name Context has no field for. */
    private static final String[] EXTRA = {
        "batteryproperties",                                       /* BatteryManager (else null: NewPipe) */
        "media.camera",                                            /* CameraManager: no cameras (else it retries every second) */
        "media.audio_policy", "media.audio_flinger",               /* native AudioSystem waits for them (Chromium's
                                                                    * AudioThread, and the renderer behind it): its
                                                                    * calls now fail at once, no sound yet */
    };

    /** Every Context service name with no service yet gets a NullService. */
    static void standIns() {
        int n = 0;
        for (String name : EXTRA)
            if (ServiceManager.checkService(name) == null) { ServiceManager.addService(name, new NullService(name)); n++; }
        HashSet<String> keep = new HashSet<String>();
        for (String k : KEEP_ABSENT) keep.add(k);
        for (Field f : android.content.Context.class.getDeclaredFields()) {
            if (!f.getName().endsWith("_SERVICE") || f.getType() != String.class
                    || !Modifier.isStatic(f.getModifiers())) continue;
            try {
                f.setAccessible(true);
                String name = (String) f.get(null);
                if (name == null || keep.contains(name)) continue;
                IBinder b = ServiceManager.checkService(name);
                if (b != null) continue;
                ServiceManager.addService(name, new NullService(name));
                n++;
            } catch (Throwable e) {
                /* a field we cannot read: skip it */
            }
        }
        System.out.println("aoi: " + n + " stand-in services");
    }
}
