package android.media;

/** Compile-time stub: the guest framework's AudioTrack is the real one. */
public class AudioTrack {
    public AudioTrack(int streamType, int sampleRate, int channelConfig, int audioFormat, int bufferSize, int mode) {}
    public static int getMinBufferSize(int sampleRate, int channelConfig, int audioFormat) { return 0; }
    public void play() {}
    public void stop() {}
    public void pause() {}
    public void flush() {}
    public void release() {}
    public int write(short[] data, int off, int len) { return 0; }
    public int getPlaybackHeadPosition() { return 0; }
    public int getState() { return 0; }
    public int getSampleRate() { return 0; }
    public boolean getTimestamp(AudioTimestamp t) { return false; }
}
