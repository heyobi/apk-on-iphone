package aoi;

import android.media.MediaCodec;
import android.media.MediaFormat;
import java.nio.ByteBuffer;

/** Android's software codecs in this process (guest/media.c, loaded first): 0.5 s of a
 *  440 Hz tone (44.1 kHz mono) goes through c2.android.aac.encoder and back through
 *  c2.android.aac.decoder; the decoded tone's zero crossings give its pitch. */
public final class CodecTest {
    static final int RATE = 44100;

    public static void main(String[] args) throws Exception {
        if (args.length > 0) System.load(args[0]);                              /* guest/media.c: the codecs */
        for (int i = 0; i < 600 && android.os.ServiceManager.checkService(
                "android.hardware.media.c2.IComponentStore/software") == null; i++) Thread.sleep(100);
        short[] pcm = new short[RATE / 2];
        for (int i = 0; i < pcm.length; i++) pcm[i] = (short) (12000 * Math.sin(2 * Math.PI * 440 * i / RATE));
        java.util.ArrayList<byte[]> aac = new java.util.ArrayList<byte[]>();
        byte[] csd = encode(pcm, aac);
        short[] out = decode(csd, aac);
        int cross = 0, from = out.length / 4, to = out.length * 3 / 4;
        for (int i = from + 1; i < to; i++) if ((out[i - 1] < 0) != (out[i] < 0)) cross++;
        double hz = cross / 2.0 / ((to - from) / (double) RATE);
        System.out.println("codec: aac " + aac.size() + " frames, decoded " + out.length + " samples, pitch "
                + (Math.abs(hz - 440) < 10 ? "440 Hz" : Math.round(hz) + " Hz"));
        System.exit(0);
    }

    static byte[] encode(short[] pcm, java.util.List<byte[]> frames) throws Exception {
        MediaCodec c = MediaCodec.createEncoderByType("audio/mp4a-latm");
        MediaFormat f = MediaFormat.createAudioFormat("audio/mp4a-latm", RATE, 1);
        f.setInteger("bitrate", 64000);
        f.setInteger("aac-profile", 2);                                          /* AAC LC */
        c.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        c.start();
        byte[] csd = null;
        int fed = 0;
        boolean inEnd = false;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        for (int spin = 0; spin < 5000; spin++) {
            if (!inEnd) {
                int i = c.dequeueInputBuffer(10000);
                if (i >= 0) {
                    ByteBuffer b = c.getInputBuffer(i);
                    int n = Math.min(b.remaining() / 2, Math.min(1024, pcm.length - fed));
                    for (int k = 0; k < n; k++) { b.put((byte) pcm[fed + k]); b.put((byte) (pcm[fed + k] >> 8)); }
                    fed += n;
                    inEnd = fed >= pcm.length;
                    c.queueInputBuffer(i, 0, 2 * n, fed * 1000000L / RATE, inEnd ? MediaCodec.BUFFER_FLAG_END_OF_STREAM : 0);
                }
            }
            int o = c.dequeueOutputBuffer(info, 10000);
            if (o >= 0) {
                ByteBuffer b = c.getOutputBuffer(o);
                byte[] d = new byte[info.size];
                b.position(info.offset);
                b.get(d);
                if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) csd = d;
                else if (d.length > 0) frames.add(d);
                c.releaseOutputBuffer(o, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
            }
        }
        c.stop();
        c.release();
        return csd;
    }

    static short[] decode(byte[] csd, java.util.List<byte[]> frames) throws Exception {
        MediaCodec c = MediaCodec.createDecoderByType("audio/mp4a-latm");
        MediaFormat f = MediaFormat.createAudioFormat("audio/mp4a-latm", RATE, 1);
        f.setInteger("is-adts", 0);
        java.lang.reflect.Method setBuf = f.getClass().getMethod("setByteBuffer", String.class, ByteBuffer.class);
        setBuf.invoke(f, "csd-0", ByteBuffer.wrap(csd));
        c.configure(f, null, null, 0);
        c.start();
        java.io.ByteArrayOutputStream pcm = new java.io.ByteArrayOutputStream();
        int fed = 0;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        for (int spin = 0; spin < 5000; spin++) {
            if (fed <= frames.size()) {
                int i = c.dequeueInputBuffer(10000);
                if (i >= 0) {
                    if (fed < frames.size()) {
                        c.getInputBuffer(i).put(frames.get(fed));
                        c.queueInputBuffer(i, 0, frames.get(fed).length, fed * 1024L * 1000000 / RATE, 0);
                    } else c.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                    fed++;
                }
            }
            int o = c.dequeueOutputBuffer(info, 10000);
            if (o >= 0) {
                ByteBuffer b = c.getOutputBuffer(o);
                byte[] d = new byte[info.size];
                b.position(info.offset);
                b.get(d);
                pcm.write(d, 0, d.length);
                c.releaseOutputBuffer(o, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
            }
        }
        c.stop();
        c.release();
        byte[] all = pcm.toByteArray();
        short[] s = new short[all.length / 2];
        for (int i = 0; i < s.length; i++) s[i] = (short) ((all[2 * i] & 0xff) | all[2 * i + 1] << 8);
        return s;
    }
}
