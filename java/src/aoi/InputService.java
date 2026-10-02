package aoi;

import android.os.Binder;
import android.os.Parcel;
import android.os.Parcelable;

/** "input" (IInputManager): the one input device every Android has, the virtual
 *  keyboard (id -1, KeyCharacterMap.VIRTUAL_KEYBOARD). KeyCharacterMap.load asks for
 *  it, e.g. when a plain Activity's action bar prepares its menu (PhoneWindow
 *  .preparePanel): without it, UnavailableException. Its key map is the empty one.
 *  Every other call gets the default answer (aoi.Services). */
final class InputService extends Binder {
    private static final String DESCRIPTOR = "android.hardware.input.IInputManager";
    private static final int VIRTUAL_KEYBOARD = -1;
    private final int getInputDevice = code("getInputDevice"), getInputDeviceIds = code("getInputDeviceIds");
    private Parcelable keyboard;

    private static int code(String method) {
        try {
            java.lang.reflect.Field f = Class.forName(DESCRIPTOR + "$Stub").getDeclaredField("TRANSACTION_" + method);
            f.setAccessible(true);
            return f.getInt(null);
        } catch (Exception e) {
            System.out.println("aoi: input: no " + method + ": " + e);
            return -1;
        }
    }

    /** InputDevice.Builder (hidden): id -1, "Virtual", a full keyboard, the empty key map. */
    private Parcelable keyboard() {
        if (keyboard != null) return keyboard;
        try {
            Class<?> b = Class.forName("android.view.InputDevice$Builder");
            Object o = b.getConstructor().newInstance();
            b.getMethod("setId", int.class).invoke(o, VIRTUAL_KEYBOARD);
            b.getMethod("setName", String.class).invoke(o, "Virtual");
            b.getMethod("setDescriptor", String.class).invoke(o, "virtual");
            b.getMethod("setSources", int.class).invoke(o, 0x101);                  /* SOURCE_KEYBOARD */
            b.getMethod("setKeyboardType", int.class).invoke(o, 2);                 /* KEYBOARD_TYPE_ALPHABETIC */
            Class<?> kcm = Class.forName("android.view.KeyCharacterMap");
            Object map = kcm.getMethod("obtainEmptyMap", int.class).invoke(null, VIRTUAL_KEYBOARD);
            b.getMethod("setKeyCharacterMap", kcm).invoke(o, map);
            keyboard = (Parcelable) b.getMethod("build").invoke(o);
        } catch (Throwable e) {
            System.out.println("aoi: input: virtual keyboard: " + e);
        }
        return keyboard;
    }

    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
        if (code >= 0x00ffffff) return false;                      /* PING, INTERFACE, DUMP...: Binder's */
        if (code == getInputDevice) {
            data.enforceInterface(DESCRIPTOR);
            int id = data.readInt();
            Parcelable d = id == VIRTUAL_KEYBOARD ? keyboard() : null;
            reply.writeNoException();
            if (d == null) reply.writeInt(0);
            else { reply.writeInt(1); d.writeToParcel(reply, 0); }
            return true;
        }
        if (code == getInputDeviceIds) {
            reply.writeNoException();
            reply.writeIntArray(keyboard() != null ? new int[] { VIRTUAL_KEYBOARD } : new int[0]);
            return true;
        }
        return Services.missing(this, new AbstractMethodError("IInputManager call " + code), reply);
    }
}
