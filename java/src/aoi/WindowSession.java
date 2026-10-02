package aoi;

import android.content.res.Configuration;
import android.graphics.Rect;
import android.graphics.Region;
import android.os.Bundle;
import android.os.IBinder;
import android.util.MergedConfiguration;
import android.view.IWindow;
import android.view.IWindowSession;
import android.view.InputChannel;
import android.view.InsetsSourceControl;
import android.view.InsetsState;
import android.view.SurfaceControl;
import android.view.Gravity;
import android.view.SurfaceSession;
import android.view.WindowManager;
import android.window.ClientWindowFrames;
import android.window.OnBackInvokedCallbackInfo;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/** The app's windows: each is added with an input channel (we keep the server end,
 *  aoi.Input writes touches to it) and laid out by its gravity, size and offset in
 *  the screen (an activity fills it; a dialog is centred, a popup menu sits where it
 *  asked); relayout gives it a BLAST layer created through our SurfaceFlinger
 *  (core/sf.c) and tells it where that layer goes: /dev/aoi_layer/ID/X/Y/Z/DIM, newer
 *  windows above older ones, a FLAG_DIM_BEHIND window darkening what is below. */
final class WindowSession extends IWindowSession.Stub {
    private static final int FLAG_DIM_BEHIND = 0x2;

    /** One window: its attributes, frame in the screen, layer, input channel. */
    static final class Win {
        IBinder token;
        WindowManager.LayoutParams attrs;
        final Rect frame = new Rect();
        SurfaceControl sc;
        InputChannel input;
        int z;
        boolean shown, focused;
    }

    private final SurfaceSession surfaces = new SurfaceSession();
    private final ArrayList<Win> windows = new ArrayList<Win>();       /* bottom to top */
    private int nextZ;

    private static Rect screen() { return new Rect(0, 0, DisplayManager.WIDTH, DisplayManager.HEIGHT); }

    /** The configuration a window sees: the phone's (its window bounds are the screen). */
    static Configuration windowConfig() { return ActivityManager.phone(); }

    private Win find(IWindow window) {
        IBinder t = window != null ? window.asBinder() : null;
        for (Win w : windows) if (w.token == t) return w;
        return null;
    }

    /** The visible window a touch at (x, y) goes to, top first: the one under it, or
     *  one above it that is touch modal (neither FLAG_NOT_TOUCH_MODAL nor
     *  FLAG_NOT_FOCUSABLE, as InputDispatcher has it: a popup menu or dialog that
     *  closes on a touch outside it; a text selection handle is not). */
    synchronized Win target(float x, float y) {
        for (int i = windows.size() - 1; i >= 0; i--) {
            Win w = windows.get(i);
            if (!w.shown || w.input == null || w.attrs == null || (w.attrs.flags & 0x10) != 0) continue;   /* NOT_TOUCHABLE */
            if (w.frame.contains((int) x, (int) y) || (w.attrs.flags & 0x28) == 0) return w;   /* NOT_TOUCH_MODAL, NOT_FOCUSABLE */
        }
        return null;
    }

    /** The windows above `w` that watch touches outside them (FLAG_WATCH_OUTSIDE_TOUCH). */
    synchronized ArrayList<Win> watchers(Win w) {
        ArrayList<Win> r = new ArrayList<Win>();
        for (int i = windows.size() - 1; i >= 0 && windows.get(i) != w; i--) {
            Win o = windows.get(i);
            if (o.shown && o.input != null && o.attrs != null && (o.attrs.flags & 0x40000) != 0) r.add(o);
        }
        return r;
    }

    /** Window focus for app window `w` (its ViewRootImpl is in this process: the IWindow
     *  is its W). Compose shows a text field's selection handles and its copy/paste
     *  toolbar only in a focused window (and hides the toolbar as soon as it comes up in
     *  an unfocused one), but in a focused window the field's cursor blinks, a full
     *  repaint twice a second. So a window has focus only while text is being selected:
     *  aoi.Input gives it on a held finger (a long press coming) and takes it on a touch
     *  that goes to the window itself and when its last popup (toolbar, handles) goes;
     *  a text toolbar coming up gives it too. */
    synchronized void focus(Win w, boolean on) {
        if (w == null || w.focused == on || w.attrs == null || w.attrs.type < 1 || w.attrs.type > 99) return;
        w.focused = on;
        try {
            java.lang.reflect.Field f = w.token.getClass().getDeclaredField("mViewAncestor");
            f.setAccessible(true);
            Object root = ((java.lang.ref.WeakReference<?>) f.get(w.token)).get();
            if (root == null) return;
            java.lang.reflect.Method m = root.getClass().getDeclaredMethod("windowFocusChanged", boolean.class);
            m.setAccessible(true);
            m.invoke(root, on);
        } catch (Exception e) {
            System.out.println("aoi: focus: " + e);
        }
    }

    /** The app window a sub-window (a popup) belongs to. */
    private Win parent(Win s) {
        for (Win w : windows) if (s.attrs != null && w.token == s.attrs.token) return w;
        return null;
    }

    /** Sub-window `s` went away: when it was the last one up (the toolbar and handles
     *  gone after a copy or paste), selecting is over and its window loses focus. */
    private void subGone(Win s) {
        Win p = s.attrs != null && s.attrs.type >= 1000 && s.attrs.type <= 1999 ? parent(s) : null;
        if (p == null || !p.focused) return;
        for (Win o : windows) if (o != s && o.shown && o.attrs != null && o.attrs.token == p.token) return;
        focus(p, false);
    }

    private static void layer(String cmd) {
        try { new FileInputStream("/dev/aoi_layer/" + cmd).close(); } catch (IOException e) { /* always ENOENT */ }
    }

    private void place(Win w) {
        WindowManager.LayoutParams a = w.attrs;
        Rect in = a != null ? a.surfaceInsets : null;
        int dim = a != null && (a.flags & FLAG_DIM_BEHIND) != 0 ? Math.round(a.dimAmount * 1000) : 0;
        layer(w.sc.getLayerId() + "/" + (w.frame.left - (in != null ? in.left : 0)) + "/"
                + (w.frame.top - (in != null ? in.top : 0)) + "/" + w.z + "/" + dim);
    }

    @Override
    public synchronized int addToDisplayAsUser(IWindow window, WindowManager.LayoutParams attrs, int visibility,
            int layerStack, int userId, int requestedVisibleTypes, InputChannel outInputChannel, InsetsState insets,
            InsetsSourceControl.Array controls, Rect attachedFrame, float[] sizeCompatScale) {
        System.out.println("aoi: window added: " + attrs.getTitle() + " type " + attrs.type);
        Win w = new Win();
        w.token = window.asBinder();
        w.attrs = attrs;
        w.z = ++nextZ;
        w.frame.set(screen());
        windows.add(w);
        if (outInputChannel != null) {
            InputChannel[] pair = InputChannel.openInputChannelPair(String.valueOf(attrs.getTitle()));
            w.input = pair[0];
            pair[1].copyTo(outInputChannel);
        }
        if (insets != null) insets.setDisplayFrame(screen());
        if (attachedFrame != null) attachedFrame.set(screen());   /* a sub-window's parent: an activity */
        if (sizeCompatScale != null && sizeCompatScale.length > 0) sizeCompatScale[0] = 1f;
        return 0x3;                                                /* ADD_OKAY | IN_TOUCH_MODE | APP_VISIBLE */
    }

    @Override
    public int addToDisplay(IWindow window, WindowManager.LayoutParams attrs, int visibility, int layerStack,
            int requestedVisibleTypes, InputChannel outInputChannel, InsetsState insets,
            InsetsSourceControl.Array controls, Rect attachedFrame, float[] sizeCompatScale) {
        return addToDisplayAsUser(window, attrs, visibility, layerStack, 0, requestedVisibleTypes, outInputChannel,
                insets, controls, attachedFrame, sizeCompatScale);
    }

    /** Where a window of the requested size goes, as WindowManager's layout does it:
     *  MATCH_PARENT fills the screen, otherwise its gravity and x/y offset place it,
     *  kept on the screen. */
    private static void frame(WindowManager.LayoutParams a, int w, int h, Rect out) {
        Rect s = screen();
        if (a == null || w <= 0 || h <= 0) { out.set(s); return; }
        if (a.width == -1) w = s.width();
        if (a.height == -1) h = s.height();
        if (w >= s.width() && h >= s.height()) { out.set(s); return; }
        Gravity.apply(a.gravity != 0 ? a.gravity : 0x33, w, h, s, a.x, a.y, out);   /* default TOP|LEFT */
        if ((a.flags & 0x200) == 0) Gravity.applyDisplay(a.gravity, s, out);      /* not LAYOUT_NO_LIMITS */
    }

    @Override
    public synchronized int relayout(IWindow window, WindowManager.LayoutParams attrs, int w, int h, int visibility,
            int flags, int seq, int lastSyncSeqId, ClientWindowFrames frames, MergedConfiguration merged,
            SurfaceControl outSurface, InsetsState insets, InsetsSourceControl.Array controls, Bundle bundle) {
        Win win = find(window);
        if (win == null) {
            win = new Win();
            win.token = window != null ? window.asBinder() : null;
            win.z = ++nextZ;
            windows.add(win);
        }
        if (attrs != null) win.attrs = attrs;
        frame(win.attrs, w, h, win.frame);
        System.out.println("aoi: relayout " + (win.attrs != null ? win.attrs.getTitle() : "") + " " + w + "x" + h
                + " visibility " + visibility + " at " + win.frame.left + "," + win.frame.top);
        if (frames != null) {
            frames.frame.set(win.frame);
            frames.displayFrame.set(0, 0, DisplayManager.WIDTH, DisplayManager.HEIGHT);
            frames.parentFrame.set(0, 0, DisplayManager.WIDTH, DisplayManager.HEIGHT);
            frames.compatScale = 1f;
        }
        if (merged != null) merged.setConfiguration(windowConfig(), new Configuration());
        if (insets != null) insets.setDisplayFrame(screen());
        if (outSurface != null && visibility == 0) {               /* VISIBLE: it gets a layer to draw into */
            if (win.sc == null) {
                Rect in = win.attrs != null ? win.attrs.surfaceInsets : null;
                win.sc = new SurfaceControl.Builder(surfaces)
                        .setName(String.valueOf(win.attrs != null ? win.attrs.getTitle() : "window"))
                        .setBufferSize(win.frame.width() + (in != null ? in.left + in.right : 0),
                                win.frame.height() + (in != null ? in.top + in.bottom : 0))
                        .setFormat(-3)                             /* PixelFormat.TRANSLUCENT */
                        .setBLASTLayer()
                        .setCallsite("aoi.WindowSession.relayout")
                        .build();
            }
            outSurface.copyFrom(win.sc, "aoi.WindowSession.relayout");
            win.shown = true;
            place(win);
            if (win.attrs != null && win.attrs.type == 1005) focus(parent(win), true);   /* a text toolbar */
        } else if (visibility != 0 && win.sc != null && win.shown) {
            win.shown = false;
            layer(win.sc.getLayerId() + "/hide");
            subGone(win);
        }
        return 0;
    }

    @Override
    public synchronized void remove(IBinder token) {
        for (int i = 0; i < windows.size(); i++) {
            Win w = windows.get(i);
            if (w.token != token) continue;
            System.out.println("aoi: window removed: " + (w.attrs != null ? w.attrs.getTitle() : ""));
            windows.remove(i);
            if (w.sc != null) layer(w.sc.getLayerId() + "/hide");
            subGone(w);
            return;
        }
    }

    @Override public void relayoutAsync(IWindow window, WindowManager.LayoutParams attrs, int w, int h, int v, int f, int s, int l) {}

    @Override
    public void finishDrawing(IWindow window, SurfaceControl.Transaction postDraw, int seqId) {
        System.out.println("aoi: finishDrawing");
    }

    @Override public boolean outOfMemory(IWindow window) { return false; }
    @Override public void setInsets(IWindow window, int touchable, Rect content, Rect visible, Region area) {}
    @Override public void clearTouchableRegion(IWindow window) {}
    @Override public boolean cancelDraw(IWindow window) { return false; }
    @Override public void pokeDrawLock(IBinder window) {}
    @Override public void updateRequestedVisibleTypes(IWindow window, int types) {}
    @Override public void setOnBackInvokedCallbackInfo(IWindow window, OnBackInvokedCallbackInfo info) {}
    @Override public void reportSystemGestureExclusionChanged(IWindow window, List rects) {}
    @Override public void reportKeepClearAreasChanged(IWindow window, List restricted, List unrestricted) {}
    @Override public void reportDecorViewGestureInterceptionChanged(IWindow window, boolean intercepted) {}
    @Override public void onRectangleOnScreenRequested(IBinder token, Rect rectangle) {}
    @Override public boolean performHapticFeedback(int effect, boolean always, boolean fromIme) { return false; }
    @Override public void performHapticFeedbackAsync(int effect, boolean always, boolean fromIme) {}
    @Override public void updatePointerIcon(IWindow window) {}
}
