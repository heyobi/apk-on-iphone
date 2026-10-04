package aoi;

/** What MediaCodec finds here: the decoders MediaCodecList lists, and whether one for
 *  MP3 can be made (reflection: no stubs for android.media). */
public final class CodecTest {
    public static void main(String[] args) throws Exception {
        if (args.length > 0) System.load(args[0]);                              /* guest/media.c: the codecs */
        Class<?> list = Class.forName("android.media.MediaCodecList");
        Object l = list.getConstructor(int.class).newInstance(1);              /* ALL_CODECS */
        Object[] infos = (Object[]) list.getMethod("getCodecInfos").invoke(l);
        StringBuilder names = new StringBuilder();
        for (Object i : infos) names.append(i.getClass().getMethod("getName").invoke(i)).append(' ');
        System.out.println("codecs: " + infos.length + " " + names);
        try {
            Object c = Class.forName("android.media.MediaCodec").getMethod("createDecoderByType", String.class)
                    .invoke(null, "audio/mpeg");
            System.out.println("codec: mp3 decoder " + c.getClass().getMethod("getName").invoke(c));
        } catch (Throwable e) {
            System.out.println("codec: mp3 decoder failed: " + (e.getCause() != null ? e.getCause() : e));
        }
    }
}
