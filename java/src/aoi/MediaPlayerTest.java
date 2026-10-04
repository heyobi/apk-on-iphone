package aoi;

import android.os.ServiceManager;

/** MediaPlayer, with Android's own service (NuPlayer) in this process (guest/media.c):
 *  a WAV file of 1 s of 440 Hz is prepared and played; it checks the duration and that
 *  the position moves (the sound itself goes through core/af.c: AOI_AUDIO_OUT). */
public final class MediaPlayerTest {
    static String descriptor(String name) throws Exception {
        Object b = ServiceManager.getService(name);
        return b == null ? null : (String) b.getClass().getMethod("getInterfaceDescriptor").invoke(b);
    }

    public static void main(String[] args) throws Exception {
        ServiceManager.addService("audio", new AudioService());
        ServiceManager.addService("package", new AudioTrackTest.Packages());
        ServiceManager.addService(MediaPlayerService.NAME, new MediaPlayerService());
        System.load(args[0]);
        String path = "/data/local/tmp/tone.wav";
        int rate = 44100, n = rate;
        java.io.DataOutputStream o = new java.io.DataOutputStream(new java.io.FileOutputStream(path));
        o.writeBytes("RIFF"); o.writeInt(Integer.reverseBytes(36 + 2 * n)); o.writeBytes("WAVEfmt ");
        o.writeInt(Integer.reverseBytes(16)); o.writeShort(Short.reverseBytes((short) 1)); o.writeShort(Short.reverseBytes((short) 1));
        o.writeInt(Integer.reverseBytes(rate)); o.writeInt(Integer.reverseBytes(2 * rate));
        o.writeShort(Short.reverseBytes((short) 2)); o.writeShort(Short.reverseBytes((short) 16));
        o.writeBytes("data"); o.writeInt(Integer.reverseBytes(2 * n));
        for (int i = 0; i < n; i++) o.writeShort(Short.reverseBytes((short) (10000 * Math.sin(2 * Math.PI * 440 * i / rate))));
        o.close();
        for (int i = 0; i < 600 && !"android.media.IMediaPlayerService".equals(descriptor("media.player")); i++)
            Thread.sleep(100);
        android.os.Looper.prepare();
        Class<?> mp = Class.forName("android.media.MediaPlayer");
        Object p = mp.getConstructor().newInstance();
        java.io.FileInputStream in = new java.io.FileInputStream(path);       /* (setDataSource(path) wants a Context) */
        java.lang.reflect.Method set = mp.getDeclaredMethod("_setDataSource", java.io.FileDescriptor.class, long.class, long.class);
        set.setAccessible(true);
        set.invoke(p, in.getFD(), 0L, 44L + 2 * n);
        mp.getMethod("prepare").invoke(p);
        int duration = (Integer) mp.getMethod("getDuration").invoke(p);
        mp.getMethod("start").invoke(p);
        int pos = 0;
        boolean playing = false;
        for (int i = 0; i < 300 && pos < 300; i++) {                          /* (its decoder first: seconds here) */
            Thread.sleep(200);
            pos = Math.max(pos, (Integer) mp.getMethod("getCurrentPosition").invoke(p));
            if (System.getenv("AOI_MP_TRACE") != null) System.out.println("mediaplayer: at " + pos);
            playing |= (Boolean) mp.getMethod("isPlaying").invoke(p);
        }
        mp.getMethod("release").invoke(p);
        System.out.println("mediaplayer: duration " + (Math.abs(duration - 1000) < 50 ? "1 s" : duration + " ms")
                + ", playing " + playing + ", position moved " + (pos > 200));
        System.exit(0);
    }
}
