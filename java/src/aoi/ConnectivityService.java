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
 *  every other call gets the default answer (Services.missing). A network callback
 *  (requestNetwork, listenForNetwork: registerNetworkCallback and its kin) gets its
 *  NetworkRequest back, which ConnectivityManager keeps the callback under (without
 *  one, unregisterNetworkCallback throws: WhatsApp's "NetworkCallback was not
 *  registered"), then onAvailable for the one network. */
final class ConnectivityService extends Binder {
    private static final String STUB = "android.net.IConnectivityManager$Stub";
    private static final int NET_ID = 100, TYPE_WIFI = 1, TRANSPORT_WIFI = 1;
    private static final int[] CAPS = { 11, 12, 13, 14, 15, 16, 18, 19, 20, 21 };   /* NOT_METERED, INTERNET,
        NOT_RESTRICTED, TRUSTED, NOT_VPN, VALIDATED, NOT_ROAMING, FOREGROUND, NOT_CONGESTED, NOT_SUSPENDED */
    private final HashMap<Integer, String> names = new HashMap<Integer, String>();
    private static final String DESCRIPTOR = "android.net.IConnectivityManager";
    private static final int CALLBACK_AVAILABLE = 2;               /* ConnectivityManager.CallbackHandler */
    private Object network, info, caps, link;
    private int nextRequest = 1;

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

    /** A NetworkRequest for a callback (the framework's hidden constructor), answered
     *  at once with onAvailable through the caller's Messenger. */
    private Object request(Parcel data, boolean listen) throws Exception {
        data.enforceInterface(DESCRIPTOR);
        Class<?> ncc = Class.forName("android.net.NetworkCapabilities");
        Class<?> tc = Class.forName("android.net.NetworkRequest$Type");
        Object nc, type;
        android.os.Messenger messenger;
        int legacy = -1;                                           /* TYPE_NONE */
        if (listen) {                                              /* (caps, messenger, binder, flags, pkg, tag) */
            nc = typedIn(data, ncc);
            messenger = (android.os.Messenger) typedIn(data, android.os.Messenger.class);
            type = Enum.valueOf((Class) tc, "LISTEN");
        } else {                                                   /* (uid, caps, type, messenger, timeout, binder, legacy, ...) */
            data.readInt();
            nc = typedIn(data, ncc);
            int t = data.readInt();
            messenger = (android.os.Messenger) typedIn(data, android.os.Messenger.class);
            data.readInt();
            data.readStrongBinder();
            legacy = data.readInt();
            Object[] all = tc.getEnumConstants();
            type = all[t >= 0 && t < all.length ? t : 0];
        }
        if (nc == null) nc = caps;
        Object req = null;
        Class<?> rc = Class.forName("android.net.NetworkRequest");
        for (Constructor<?> k : rc.getDeclaredConstructors()) {
            Class<?>[] pt = k.getParameterTypes();
            if (pt.length == 4 && pt[0] == ncc && pt[1] == int.class && pt[2] == int.class && pt[3] == tc) {
                k.setAccessible(true);
                synchronized (this) { req = k.newInstance(nc, legacy, nextRequest++, type); }
            }
        }
        if (req != null && messenger != null) {
            android.os.Message msg = android.os.Message.obtain();
            msg.what = CALLBACK_AVAILABLE;
            android.os.Bundle b = new android.os.Bundle();
            b.putParcelable("NetworkRequest", (Parcelable) req);
            b.putParcelable("Network", (Parcelable) network);
            b.putParcelable("NetworkCapabilities", (Parcelable) caps);
            b.putParcelable("LinkProperties", (Parcelable) link);
            msg.setData(b);
            try { messenger.send(msg); } catch (Exception e) { /* the app's handler: gone */ }
        }
        return req;
    }

    private static Object typedIn(Parcel data, Class<?> c) throws Exception {   /* readTypedObject */
        if (data.readInt() == 0) return null;
        android.os.Parcelable.Creator<?> cr = (android.os.Parcelable.Creator<?>) c.getField("CREATOR").get(null);
        return cr.createFromParcel(data);
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
            if ("requestNetwork".equals(m) || "listenForNetwork".equals(m)) {
                Object req = request(data, "listenForNetwork".equals(m));
                if (req != null) { typed(reply, req); return true; }
            }
            if ("releaseNetworkRequest".equals(m)) { reply.writeNoException(); return true; }
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
