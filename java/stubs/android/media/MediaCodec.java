package android.media;

import java.nio.ByteBuffer;

/** Compile-time stub: the guest framework's class is the real one. */
public final class MediaCodec {
    public static final int CONFIGURE_FLAG_ENCODE = 1;
    public static final int BUFFER_FLAG_END_OF_STREAM = 4;
    public static final int BUFFER_FLAG_CODEC_CONFIG = 2;
    public static final int INFO_TRY_AGAIN_LATER = -1;
    public static final int INFO_OUTPUT_FORMAT_CHANGED = -2;
    public static final class BufferInfo { public int offset, size, flags; public long presentationTimeUs; }
    public static MediaCodec createDecoderByType(String type) { return null; }
    public static MediaCodec createEncoderByType(String type) { return null; }
    public void configure(MediaFormat format, android.view.Surface surface, MediaCrypto crypto, int flags) {}
    public void start() {}
    public void stop() {}
    public void release() {}
    public String getName() { return null; }
    public int dequeueInputBuffer(long timeoutUs) { return 0; }
    public ByteBuffer getInputBuffer(int index) { return null; }
    public void queueInputBuffer(int index, int offset, int size, long presentationTimeUs, int flags) {}
    public int dequeueOutputBuffer(BufferInfo info, long timeoutUs) { return 0; }
    public ByteBuffer getOutputBuffer(int index) { return null; }
    public void releaseOutputBuffer(int index, boolean render) {}
    public MediaFormat getOutputFormat() { return null; }
}
