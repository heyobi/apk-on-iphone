package android.view;

public final class MotionEvent extends InputEvent {
    public static final class PointerProperties {
        public int id;
        public int toolType;
    }

    public static final class PointerCoords {
        public float x;
        public float y;
        public float pressure;
        public float size;
    }

    public static MotionEvent obtain(long downTime, long eventTime, int action, float x, float y, int metaState) {
        return null;
    }
    public static MotionEvent obtain(long downTime, long eventTime, int action, int pointerCount,
            PointerProperties[] pointerProperties, PointerCoords[] pointerCoords, int metaState, int buttonState,
            float xPrecision, float yPrecision, int deviceId, int edgeFlags, int source, int flags) {
        return null;
    }
    public final void setSource(int source) {}
    public final void recycle() {}
}
