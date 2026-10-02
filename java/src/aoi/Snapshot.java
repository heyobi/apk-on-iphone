package aoi;

import android.os.Handler;
import android.os.Looper;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** The whole process saved by the host (core/snap.c): opening /dev/aoi_snapshot asks
 *  for it, and the next launch resumes from there; the open itself always fails.
 *  With HWUI on the GPU (AOI_HWUI) the host's GL contexts and surfaces cannot be
 *  saved, so the windows first drop them, the way Android does when an app goes to
 *  the background (WindowManagerGlobal.trimMemory(TRIM_MEMORY_COMPLETE): renderers
 *  destroyed, RenderThread's EGL context gone), and draw again afterwards: here, and
 *  in a process resumed from the snapshot, which carries on from the same place.
 *  /dev/aoi_gpu_live fails with EBUSY while the host still holds something. */
final class Snapshot {
    private static final int TRIM_MEMORY_COMPLETE = 80;

    private Snapshot() {}

    static void take() {
        boolean gpu = System.getenv("AOI_HWUI") != null;
        if (gpu) {
            onMain(new Runnable() {
                @Override public void run() {
                    try {
                        Class<?> g = Class.forName("android.view.WindowManagerGlobal");
                        Object wmg = g.getMethod("getInstance").invoke(null);
                        g.getMethod("trimMemory", int.class).invoke(wmg, TRIM_MEMORY_COMPLETE);
                    } catch (Exception e) {
                        System.out.println("aoi: snapshot: trimMemory " + e);
                    }
                }
            });
            for (int i = 0; i < 100 && gpuLive(); i++) sleep(50);
            if (gpuLive()) System.out.println("aoi: snapshot: the GPU still holds state");
        }
        try {
            new FileInputStream("/dev/aoi_snapshot").close();
        } catch (IOException e) {
            // expected: the host has taken it, or does not want one
        }
        if (gpu) {
            sleep(300);                                            /* taken at the next time slice */
            onMain(new Runnable() {
                @Override public void run() { redraw(); }
            });
        }
    }

    private static boolean gpuLive() {
        try {
            new FileInputStream("/dev/aoi_gpu_live").close();
        } catch (IOException e) {
            return String.valueOf(e.getMessage()).contains("EBUSY");
        }
        return false;
    }

    /** Every window draws again: its renderer comes back in ViewRootImpl.draw. */
    private static void redraw() {
        try {
            Class<?> g = Class.forName("android.view.WindowManagerGlobal");
            Object wmg = g.getMethod("getInstance").invoke(null);
            java.lang.reflect.Field f = g.getDeclaredField("mRoots");
            f.setAccessible(true);
            for (Object root : ((java.util.List<?>) f.get(wmg)).toArray()) {
                Object v = root.getClass().getMethod("getView").invoke(root);
                if (v != null) v.getClass().getMethod("invalidate").invoke(v);
            }
        } catch (Exception e) {
            System.out.println("aoi: snapshot: redraw " + e);
        }
    }

    private static void onMain(final Runnable r) {
        final CountDownLatch done = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override public void run() {
                try { r.run(); } finally { done.countDown(); }
            }
        });
        try {
            done.await(5, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            // go on
        }
    }

    private static void sleep(long ms) {
        try { Thread.sleep(ms); } catch (InterruptedException e) { /* go on */ }
    }
}
