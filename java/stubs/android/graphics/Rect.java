package android.graphics;

public class Rect {
    public int left, top, right, bottom;
    public Rect() {}
    public Rect(int l, int t, int r, int b) {}
    public Rect(Rect r) {}
    public void set(int l, int t, int r, int b) {}
    public void set(Rect r) {}
    public final int width() { return 0; }
    public final int height() { return 0; }
    public boolean contains(int x, int y) { return false; }
}
