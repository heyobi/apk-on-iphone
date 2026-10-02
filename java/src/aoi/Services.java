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
                if (returnsSlice(data, code)) {                    /* a null one is an NPE in the manager */
                    reply.writeInt(1);
                    new ParceledListSlice(new ArrayList()).writeToParcel(reply, 0);
                }
            }
            return true;
        }
    }

    private static final HashMap<String, Boolean> slices = new HashMap<String, Boolean>();

    /** Whether the AIDL call returns a ParceledListSlice. Its interface is the token
     *  that opens the data (strict mode policy, work source, header, then the name);
     *  the method is the Stub's TRANSACTION_ field with this code. Everything else's
     *  zeros read as empty (arrays, lists) or as values. */
    static boolean returnsSlice(Parcel data, int code) {
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
        if (desc == null) return false;
        String key = desc + "#" + code;
        synchronized (slices) {
            Boolean b = slices.get(key);
            if (b != null) return b;
        }
        boolean r = false;
        try {
            Class<?> itf = Class.forName(desc);
            for (Field f : Class.forName(desc + "$Stub").getDeclaredFields()) {
                if (!f.getName().startsWith("TRANSACTION_") || f.getType() != int.class) continue;
                f.setAccessible(true);
                if (f.getInt(null) != code) continue;
                String m = f.getName().substring("TRANSACTION_".length());
                for (Method x : itf.getMethods())
                    if (x.getName().equals(m)) r = ParceledListSlice.class.isAssignableFrom(x.getReturnType());
            }
        } catch (Throwable e) { /* no Stub: zeros */ }
        synchronized (slices) { slices.put(key, r); }
        return r;
    }

    /** Names whose managers do better without a service than with zeros. */
    private static final String[] KEEP_ABSENT = {
        "textclassification",                                      /* falls back to the local classifier */
        "autofill", "content_capture",                             /* else a 5 s wait for a reply that never comes */
    };

    /** Every Context service name with no service yet gets a NullService. */
    static void standIns() {
        int n = 0;
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
