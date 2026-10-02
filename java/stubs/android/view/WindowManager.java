package android.view;

import android.graphics.Rect;

public interface WindowManager {
    class LayoutParams {
        public int width, height, flags, type, format, gravity, x, y;
        public float dimAmount;
        public android.os.IBinder token;
        public final Rect surfaceInsets = null;
        public CharSequence getTitle() { return null; }
    }
}
