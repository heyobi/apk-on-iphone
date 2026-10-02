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
 *  GLSurfaceViews (games) keep their own EGL context: they are paused around the
 *  snapshot, as when their activity stops (onPause releases the context, onResume
 *  makes a new one and the app's renderer gets onSurfaceCreated again; one that
 *  asked to keep its context on pause, libGDX, is told not to for that pause).
 *  The host takes the snapshot the moment its last context or surface goes
 *  (/dev/aoi_snapshot_gpu_free), so a window drawing again just after (the user
 *  tapping) cannot get in between. */
final class Snapshot {
    private static final int TRIM_MEMORY_COMPLETE = 80;

    private Snapshot() {}

    static void take() {
        if (!busy("/dev/aoi_snapshot_wanted")) return;            /* the host does not want one */
        boolean gpu = System.getenv("AOI_HWUI") != null;
        final java.util.List<Object> gl = new java.util.ArrayList<Object>();
        if (gpu) {
            long t0 = System.currentTimeMillis();                  /* the host saves it the moment the last */
            busy("/dev/aoi_snapshot_gpu_free");                    /* context goes: a window drawing again */
            onMain(new Runnable() {                                /* (a tap) can't slip in between */
                @Override public void run() {
                    glViews(gl);
                    for (Object v : gl) {                          /* libGDX keeps its context on pause: not here */
                        preserve(v, false);
                        call(v, "onPause");
                    }
                    trim();
                }
            });
            for (int i = 0; i < 200 && busy("/dev/aoi_snapshot_pending"); i++) sleep(50);
            if (busy("/dev/aoi_snapshot_pending")) {
                busy("/dev/aoi_snapshot_cancel");
                System.out.println("aoi: snapshot: the GPU still holds state");
                busy("/dev/aoi_threads");                          /* where everyone is, to the log */
            } else {
                System.out.println("aoi: snapshot: done after " + (System.currentTimeMillis() - t0) + " ms");
            }
        } else {
            try {
                new FileInputStream("/dev/aoi_snapshot").close();
            } catch (IOException e) {
                // expected: the host has taken it, or does not want one
            }
        }
        if (gpu) {
            onMain(new Runnable() {
                @Override public void run() {
                    for (Object v : gl) {
                        call(v, "onResume");
                        preserve(v, true);
                    }
                    redraw();
                }
            });
        }
    }

    private static void trim() {
        try {
            Class<?> g = Class.forName("android.view.WindowManagerGlobal");
            Object wmg = g.getMethod("getInstance").invoke(null);
            g.getMethod("trimMemory", int.class).invoke(wmg, TRIM_MEMORY_COMPLETE);
        } catch (Exception e) {
            System.out.println("aoi: snapshot: trimMemory " + e);
        }
    }

    /** Every GLSurfaceView in the app's windows. */
    private static void glViews(java.util.List<Object> out) {
        try {
            Class<?> g = Class.forName("android.view.WindowManagerGlobal"), vg = Class.forName("android.view.ViewGroup"),
                    gl = Class.forName("android.opengl.GLSurfaceView");
            Object wmg = g.getMethod("getInstance").invoke(null);
            java.lang.reflect.Field f = g.getDeclaredField("mViews");
            f.setAccessible(true);
            java.util.ArrayList<Object> todo = new java.util.ArrayList<Object>(((java.util.List<?>) f.get(wmg)));
            while (!todo.isEmpty()) {
                Object v = todo.remove(todo.size() - 1);
                if (gl.isInstance(v)) out.add(v);
                if (!vg.isInstance(v)) continue;
                int n = (Integer) vg.getMethod("getChildCount").invoke(v);
                for (int i = 0; i < n; i++) todo.add(vg.getMethod("getChildAt", int.class).invoke(v, i));
            }
        } catch (Exception e) {
            System.out.println("aoi: snapshot: views " + e);
        }
    }

    /** GLSurfaceView.setPreserveEGLContextOnPause: off around the snapshot (the context
     *  must go), back on after if the app had it on (remembered in `kept`). */
    private static final java.util.Set<Object> kept = java.util.Collections.newSetFromMap(
            new java.util.IdentityHashMap<Object, Boolean>());

    private static void preserve(Object v, boolean restore) {
        try {
            Class<?> g = Class.forName("android.opengl.GLSurfaceView");
            if (!restore) {
                if ((Boolean) g.getMethod("getPreserveEGLContextOnPause").invoke(v)) {
                    kept.add(v);
                    g.getMethod("setPreserveEGLContextOnPause", boolean.class).invoke(v, false);
                }
            } else if (kept.remove(v)) {
                g.getMethod("setPreserveEGLContextOnPause", boolean.class).invoke(v, true);
            }
        } catch (Exception e) {
            System.out.println("aoi: snapshot: preserve " + e);
        }
    }

    private static void call(Object o, String method) {
        try {
            o.getClass().getMethod(method).invoke(o);
        } catch (Exception e) {
            System.out.println("aoi: snapshot: " + method + " " + e);
        }
    }

    /** The host's yes/no files: opening one fails, with EBUSY for yes. */
    private static boolean busy(String path) {
        try {
            new FileInputStream(path).close();
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
