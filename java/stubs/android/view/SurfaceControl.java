package android.view;

public final class SurfaceControl {
    public void copyFrom(SurfaceControl other, String callsite) {}
    public int getLayerId() { return 0; }
    public boolean isValid() { return false; }
    public void release() {}

    public static class Transaction {
    }

    public static class Builder {
        public Builder(SurfaceSession session) {}
        public Builder setName(String name) { return this; }
        public Builder setBufferSize(int w, int h) { return this; }
        public Builder setFormat(int format) { return this; }
        public Builder setBLASTLayer() { return this; }
        public Builder setCallsite(String site) { return this; }
        public SurfaceControl build() { return null; }
    }
}
