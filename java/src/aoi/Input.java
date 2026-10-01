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
 *  becomes a MotionEvent sent on the server end of the newest window's input channel,
 *  as InputDispatcher would. */
final class Input {
    private static final int SOURCE_TOUCHSCREEN = 0x1002;

    private final WindowSession session;
    private Handler handler;
    private android.os.Looper looper;
    private InputChannel channel;
    private InputEventSender sender;
    private int seq;
    private long downTime;

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

    private void send(int action, float x, float y) {
        InputChannel c = session.input;
        if (c == null) return;
        if (c != channel) {                                        /* a new window: a new sender */
            channel = c;
            sender = new InputEventSender(c, looper) {};
        }
        long now = SystemClock.uptimeMillis();
        if (action == 0) downTime = now;
        MotionEvent.PointerProperties pp = new MotionEvent.PointerProperties();
        pp.id = 0;
        pp.toolType = 1;                                           /* TOOL_TYPE_FINGER: Compose's Touch */
        MotionEvent.PointerCoords pc = new MotionEvent.PointerCoords();
        pc.x = x; pc.y = y; pc.pressure = 1f; pc.size = 0.05f;
        MotionEvent ev = MotionEvent.obtain(downTime, now, action, 1, new MotionEvent.PointerProperties[] { pp },
                new MotionEvent.PointerCoords[] { pc }, 0, 0, 1f, 1f, 0, 0, SOURCE_TOUCHSCREEN, 0);
        sender.sendInputEvent(++seq, ev);
        ev.recycle();
    }
}
