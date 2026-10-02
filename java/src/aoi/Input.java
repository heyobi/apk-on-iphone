package aoi;

import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.view.InputChannel;
import android.view.InputEventSender;
import android.view.MotionEvent;
import java.io.DataInputStream;
import java.io.FileInputStream;

/** Touches from the host: /dev/aoi_input (core/proc.c, aoi_proc_touch) gives 16-byte
 *  records (action 0 down / 1 up / 2 move, x, y in screen pixels, little-endian); each
 *  becomes a MotionEvent sent on the server end of a window's input channel, as
 *  InputDispatcher would: the gesture goes to the window it went down in (or a touch
 *  modal one above it, a menu that closes on a touch outside), in its coordinates;
 *  windows above that watch outside touches get ACTION_OUTSIDE. A held finger gives
 *  its window focus (WindowSession.focus: text selection), a touch on it takes it.
 *  Action 4 cancels the gesture (the host took it), action 5 asks for a snapshot
 *  (the host's, as the app goes to the background: aoi.Snapshot), and action 3 is "back": the
 *  top activity's onBackPressed on the main thread (a KEYCODE_BACK event would
 *  need window focus, and focus makes text cursors blink: a full repaint twice a
 *  second). */
final class Input {
    private static final int SOURCE_TOUCHSCREEN = 0x1002;

    private final WindowSession session;
    private Handler handler;
    private android.os.Looper looper;
    private final java.util.HashMap<InputChannel, InputEventSender> senders =
            new java.util.HashMap<InputChannel, InputEventSender>();
    private WindowSession.Win target;                          /* the gesture's window */
    private int seq;
    private long downTime;
    private float downX, downY;
    /** A finger held still this long (ms) gives its window focus: a long press is coming,
     *  and Compose shows its selection's handles and toolbar only in a focused window. */
    private static final int HOLD = 800;
    private final Runnable hold = new Runnable() {
        @Override public void run() { if (target != null) session.focus(target, true); }
    };

    private Input(WindowSession session) { this.session = session; }

    static void start(WindowSession session) {
        final Input in = new Input(session);
        HandlerThread t = new HandlerThread("aoi-input");
        t.start();
        in.looper = t.getLooper();
        in.handler = new Handler(in.looper);
        Thread reader = new Thread(new Runnable() {
            @Override public void run() { in.read(); }
        }, "aoi-input-reader");
        reader.setDaemon(true);
        reader.start();
    }

    private static int le(byte[] b, int o) {
        return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8 | (b[o + 2] & 0xff) << 16 | (b[o + 3] & 0xff) << 24;
    }

    private void read() {
        try {
            DataInputStream s = new DataInputStream(new FileInputStream("/dev/aoi_input"));
            byte[] rec = new byte[16];
            for (;;) {
                s.readFully(rec);
                final int action = le(rec, 0);
                final float x = Float.intBitsToFloat(le(rec, 4)), y = Float.intBitsToFloat(le(rec, 8));
                handler.post(new Runnable() {
                    @Override public void run() { send(action, x, y); }
                });
            }
        } catch (Exception e) {
            System.out.println("aoi: input: " + e);
        }
    }

    /** The task's top activity (ActivityThread's record of it) presses back, on the main thread. */
    private static void back() {
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object thread = at.getMethod("currentActivityThread").invoke(null);
            java.lang.reflect.Field f = at.getDeclaredField("mActivities");
            f.setAccessible(true);
            Object rec = ((java.util.Map<?, ?>) f.get(thread)).get(Activities.top());   /* the task's top one */
            if (rec == null) return;
            java.lang.reflect.Field af = rec.getClass().getDeclaredField("activity");
            af.setAccessible(true);
            final Object a = af.get(rec);
            if (a == null) return;
            final java.lang.reflect.Method m = a.getClass().getMethod("onBackPressed");
            new Handler(android.os.Looper.getMainLooper()).post(new Runnable() {
                @Override public void run() {
                    try { m.invoke(a); } catch (Exception e) { System.out.println("aoi: back: " + e); }
                }
            });
        } catch (Exception e) {
            System.out.println("aoi: back: " + e);
        }
    }

    private void send(int action, float x, float y) {
        if (action == 3) { back(); return; }
        if (action == 5) {
            Thread t = new Thread(new Runnable() {
                @Override public void run() { Snapshot.take(); }
            }, "aoi-snapshot");
            t.setDaemon(true);
            t.start();
            return;
        }
        long now = SystemClock.uptimeMillis();
        if (action == 0) {
            downTime = now;
            target = session.target(x, y);
            downX = x; downY = y;
            session.focus(target, false);                          /* a touch on the window itself: selecting is over */
            handler.postDelayed(hold, HOLD);
            for (WindowSession.Win w : session.watchers(target)) event(w, 4, x, y, now);   /* ACTION_OUTSIDE */
        }
        if (action != 0 && (action != 2 || Math.abs(x - downX) + Math.abs(y - downY) > 24)) handler.removeCallbacks(hold);
        if (target == null) return;
        event(target, action == 4 ? 3 : action, x, y, now);   /* 4 from the host: MotionEvent.ACTION_CANCEL */
        if (action == 1 || action == 4) target = null;
    }

    private void event(WindowSession.Win w, int action, float x, float y, long now) {
        InputEventSender sender = senders.get(w.input);
        if (sender == null) {                                      /* a new window: a new sender */
            sender = new InputEventSender(w.input, looper) {};
            senders.put(w.input, sender);
        }
        MotionEvent.PointerProperties pp = new MotionEvent.PointerProperties();
        pp.id = 0;
        pp.toolType = 1;                                           /* TOOL_TYPE_FINGER: Compose's Touch */
        MotionEvent.PointerCoords pc = new MotionEvent.PointerCoords();
        pc.x = x - w.frame.left; pc.y = y - w.frame.top; pc.pressure = 1f; pc.size = 0.05f;
        MotionEvent ev = MotionEvent.obtain(downTime, now, action, 1, new MotionEvent.PointerProperties[] { pp },
                new MotionEvent.PointerCoords[] { pc }, 0, 0, 1f, 1f, 0, 0, SOURCE_TOUCHSCREEN, 0);
        sender.sendInputEvent(++seq, ev);
        ev.recycle();
    }
}
