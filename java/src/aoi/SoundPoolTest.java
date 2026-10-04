package aoi;

import android.os.ServiceManager;

/** SoundPool, as games use it for effects: a WAV of 440 Hz is loaded (libsoundpool
 *  decodes it with MediaExtractor and MediaCodec in this process, guest/media.c) and
 *  played through an AudioTrack (core/af.c). */
public final class SoundPoolTest {
    public static void main(String[] args) throws Exception {
        ServiceManager.addService("audio", new AudioService());
        ServiceManager.addService("package", new AudioTrackTest.Packages());
        ServiceManager.addService(MediaPlayerService.NAME, new MediaPlayerService());
        System.load(args[0]);
        String path = "/data/local/tmp/tone2.wav";
        int rate = 22050, n = rate / 2;
        java.io.DataOutputStream o = new java.io.DataOutputStream(new java.io.FileOutputStream(path));
        o.writeBytes("RIFF"); o.writeInt(Integer.reverseBytes(36 + 2 * n)); o.writeBytes("WAVEfmt ");
        o.writeInt(Integer.reverseBytes(16)); o.writeShort(Short.reverseBytes((short) 1)); o.writeShort(Short.reverseBytes((short) 1));
        o.writeInt(Integer.reverseBytes(rate)); o.writeInt(Integer.reverseBytes(2 * rate));
        o.writeShort(Short.reverseBytes((short) 2)); o.writeShort(Short.reverseBytes((short) 16));
        o.writeBytes("data"); o.writeInt(Integer.reverseBytes(2 * n));
        for (int i = 0; i < n; i++) o.writeShort(Short.reverseBytes((short) (10000 * Math.sin(2 * Math.PI * 440 * i / rate))));
        o.close();
        Class<?> sp = Class.forName("android.media.SoundPool");
        Object pool = sp.getConstructor(int.class, int.class, int.class).newInstance(4, 3, 0);   /* STREAM_MUSIC */
        int id = (Integer) sp.getMethod("load", String.class, int.class).invoke(pool, path, 1);
        int stream = 0;
        for (int i = 0; i < 300 && stream == 0; i++) {                       /* loaded when play() answers */
            Thread.sleep(200);
            stream = (Integer) sp.getMethod("play", int.class, float.class, float.class, int.class, int.class, float.class)
                    .invoke(pool, id, 1f, 1f, 0, 0, 1f);
        }
        Thread.sleep(1000);
        sp.getMethod("release").invoke(pool);
        System.out.println("soundpool: loaded " + (id > 0) + ", played " + (stream > 0));
        System.exit(0);
    }
}
