package aoi;

import android.os.Binder;
import android.os.Parcel;

/** "power" (IPowerManager): the screen is on. With the default answers of a stand-in
 *  PowerManager.isInteractive() was false, and an app may then hold back what it
 *  does while the screen is off. Every other call gets the default answer. */
final class PowerService extends Binder {
    private static final String DESCRIPTOR = "android.os.IPowerManager";
    private final int isInteractive = code("isInteractive"), isDisplayInteractive = code("isDisplayInteractive");

    private static int code(String method) {
        try {
            java.lang.reflect.Field f = Class.forName(DESCRIPTOR + "$Stub").getDeclaredField("TRANSACTION_" + method);
            f.setAccessible(true);
            return f.getInt(null);
        } catch (Exception e) {
            return -1;                                             /* not in this Android: never matches */
        }
    }

    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
        if (code >= 0x00ffffff) return false;                      /* PING, INTERFACE, DUMP...: Binder's */
        if (code == isInteractive || code == isDisplayInteractive) {
            reply.writeNoException();
            reply.writeInt(1);                                     /* true */
            return true;
        }
        return Services.missing(this, new AbstractMethodError("IPowerManager call " + code), reply);
    }
}
