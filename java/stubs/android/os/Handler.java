package android.os;

public class Handler {
    public Handler(Looper looper) {}
    public final boolean post(Runnable r) { return false; }
    public final boolean postDelayed(Runnable r, long delayMillis) { return false; }
    public final void removeCallbacks(Runnable r) {}
}
