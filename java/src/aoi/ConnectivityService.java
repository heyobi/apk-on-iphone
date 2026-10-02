package aoi;

import android.os.Binder;
import android.os.Parcel;
import android.os.Parcelable;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.HashMap;

/** "connectivity" (IConnectivityManager): one network, Wi-Fi, connected and validated.
 *  The guest's sockets are the host's (core/proc.c), so the iPhone's connection is the
 *  app's: apps that ask ConnectivityManager first ("An internet connection is required
 *  to activate WhatsApp") see it up. The answers are the framework's own objects,
 *  made by reflection (Network 100, a CONNECTED NetworkInfo of TYPE_WIFI, capabilities
 *  INTERNET, VALIDATED, NOT_METERED... over TRANSPORT_WIFI, LinkProperties "wlan0").
 *  Calls are matched by name from IConnectivityManager.Stub's TRANSACTION_ fields;
 *  every other call gets the default answer (Services.missing). Network callbacks
 *  (registerNetworkCallback) are not answered yet. */
final class ConnectivityService extends Binder {
    private static final String STUB = "android.net.IConnectivityManager$Stub";
    private static final int NET_ID = 100, TYPE_WIFI = 1, TRANSPORT_WIFI = 1;
    private static final int[] CAPS = { 11, 12, 13, 14, 15, 16, 18, 19, 20, 21 };   /* NOT_METERED, INTERNET,
        NOT_RESTRICTED, TRUSTED, NOT_VPN, VALIDATED, NOT_ROAMING, FOREGROUND, NOT_CONGESTED, NOT_SUSPENDED */
    private final HashMap<Integer, String> names = new HashMap<Integer, String>();
    private Object network, info, caps, link;

    ConnectivityService() {
        try {
            for (Field f : Class.forName(STUB).getDeclaredFields()) {
                if (!f.getName().startsWith("TRANSACTION_")) continue;
                f.setAccessible(true);
                names.put(f.getInt(null), f.getName().substring(12));
            }
        } catch (Exception e) {
            System.out.println("aoi: connectivity: " + e);
        }
    }

    private void make() throws Exception {
        if (network != null) return;
        Class<?> n = Class.forName("android.net.Network");
        Constructor<?> nc = n.getDeclaredConstructor(int.class);
        nc.setAccessible(true);
        network = nc.newInstance(NET_ID);

        Class<?> ni = Class.forName("android.net.NetworkInfo");
        Constructor<?> nic = ni.getDeclaredConstructor(int.class, int.class, String.class, String.class);
        nic.setAccessible(true);
        info = nic.newInstance(TYPE_WIFI, 0, "WIFI", "");
        Class<?> ds = Class.forName("android.net.NetworkInfo$DetailedState");
        Method sds = ni.getDeclaredMethod("setDetailedState", ds, String.class, String.class);
        sds.setAccessible(true);
        sds.invoke(info, ds.getField("CONNECTED").get(null), null, null);
        Method sa = ni.getDeclaredMethod("setIsAvailable", boolean.class);
        sa.setAccessible(true);
        sa.invoke(info, true);

        Class<?> cb = Class.forName("android.net.NetworkCapabilities$Builder");
        Object b = cb.getConstructor().newInstance();
        Method add = cb.getMethod("addCapability", int.class), tr = cb.getMethod("addTransportType", int.class);
        for (int c : CAPS) add.invoke(b, c);
        tr.invoke(b, TRANSPORT_WIFI);
        caps = cb.getMethod("build").invoke(b);

        Class<?> lp = Class.forName("android.net.LinkProperties");
        link = lp.getConstructor().newInstance();
        lp.getMethod("setInterfaceName", String.class).invoke(link, "wlan0");
    }

    private static void typed(Parcel reply, Object o) {           /* writeTypedObject, as AIDL replies */
        reply.writeNoException();
        if (o == null) { reply.writeInt(0); return; }
        reply.writeInt(1);
        ((Parcelable) o).writeToParcel(reply, 1);                  /* PARCELABLE_WRITE_RETURN_VALUE */
    }

    private static void array(Parcel reply, Object o) {           /* writeTypedArray of one */
        reply.writeNoException();
        reply.writeInt(1);
        reply.writeInt(1);
        ((Parcelable) o).writeToParcel(reply, 1);
    }

    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
        if (code >= 0x00ffffff) return false;                      /* PING, INTERFACE, DUMP...: Binder's */
        String m = names.get(code);
        try {
            make();
            if ("getActiveNetwork".equals(m) || "getActiveNetworkForUid".equals(m) || "getNetworkForType".equals(m)) {
                typed(reply, network); return true;
            }
            if ("getActiveNetworkInfo".equals(m) || "getActiveNetworkInfoForUid".equals(m)
                    || "getNetworkInfo".equals(m) || "getNetworkInfoForUid".equals(m)) {
                typed(reply, info); return true;
            }
            if ("getAllNetworks".equals(m)) { array(reply, network); return true; }
            if ("getAllNetworkInfo".equals(m)) { array(reply, info); return true; }
            if ("getNetworkCapabilities".equals(m)) { typed(reply, caps); return true; }
            if ("getDefaultNetworkCapabilitiesForUser".equals(m)) { array(reply, caps); return true; }
            if ("getLinkProperties".equals(m) || "getActiveLinkProperties".equals(m)
                    || "getLinkPropertiesForType".equals(m)) {
                typed(reply, link); return true;
            }
        } catch (Exception e) {
            System.out.println("aoi: connectivity " + m + ": " + e);
        }
        return Services.missing(this, new AbstractMethodError("IConnectivityManager." + m), reply);
    }
}
