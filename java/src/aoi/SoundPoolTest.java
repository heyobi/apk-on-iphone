package aoi;

import android.os.ServiceManager;

/** android.media.SoundPool (games' sound effects: cube.run) builds and releases with
 *  our "audio" service: libsoundpool.so is in the bundle, and the IAudioService
 *  calls a player makes (trackPlayer...) get answers. Reflection: no SoundPool stub. */
public final class SoundPoolTest {
    public static void main(String[] args) throws Exception {
        ServiceManager.addService("audio", new AudioService());
        android.os.Looper.prepare();
        Class<?> b = Class.forName("android.media.SoundPool$Builder");
        Object builder = b.getConstructor().newInstance();
        b.getMethod("setMaxStreams", int.class).invoke(builder, 4);
        Object sp = b.getMethod("build").invoke(builder);
        sp.getClass().getMethod("release").invoke(sp);
        System.out.println("soundpool: built and released");
    }
}
