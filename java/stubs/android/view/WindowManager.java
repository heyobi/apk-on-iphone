package android.view;

public interface WindowManager {
    class LayoutParams {
        public int width, height, flags, type, format;
        public CharSequence getTitle() { return null; }
    }
}
