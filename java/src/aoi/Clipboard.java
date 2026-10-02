package aoi;

import android.content.ClipData;
import android.content.ClipDescription;
import android.content.IClipboard;
import android.content.IOnPrimaryClipChangedListener;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;

/** The clipboard is the iPhone's: text the app copies goes to the iOS pasteboard, and
 *  a paste reads it. Through the host (core/proc.c): the text travels in
 *  /data/local/tmp/aoi.clip, and opening /dev/aoi_clip/s (set), /g (get) or /h (has
 *  text) asks the host to act on it. Without a host clipboard (the desktop runner)
 *  the clip stays in this process. */
final class Clipboard extends IClipboard.Stub {
    private static final File FILE = new File("/data/local/tmp/aoi.clip");

    private ClipData clip;                                     /* the last one set here */
    private String clipText;                                   /* its text, as the host got it */
    private String source;
    private final ArrayList<IOnPrimaryClipChangedListener> listeners = new ArrayList<IOnPrimaryClipChangedListener>();

    Clipboard() { super(GrantAll.INSTANCE); }

    private static void host(char op) {
        try { new FileInputStream("/dev/aoi_clip/" + op).close(); } catch (IOException e) { /* always ENOENT */ }
    }

    private static String read() {
        if (!FILE.exists()) return null;
        try {
            FileInputStream in = new FileInputStream(FILE);
            byte[] b = new byte[(int) FILE.length()];
            int n = 0, k;
            while (n < b.length && (k = in.read(b, n, b.length - n)) > 0) n += k;
            in.close();
            return new String(b, 0, n, "UTF-8");
        } catch (IOException e) {
            return null;
        }
    }

    /** The host's text ('g'), or whether it has some ('h' gives "1"/"0"); null: no host. */
    private static String ask(char op) {
        FILE.delete();
        host(op);
        String s = read();
        FILE.delete();
        return s;
    }

    private static String textOf(ClipData c) {
        if (c == null || c.getItemCount() == 0 || c.getItemAt(0) == null) return null;
        CharSequence t = c.getItemAt(0).getText();
        if (t == null && c.getItemAt(0).getHtmlText() != null) t = c.getItemAt(0).getHtmlText();
        return t != null ? t.toString() : null;
    }

    private void changed(ClipData c, String pkg) {
        String t = textOf(c);
        if (t != null) {                                       /* to the iPhone's pasteboard */
            try {
                FileOutputStream out = new FileOutputStream(FILE);
                out.write(t.getBytes("UTF-8"));
                out.close();
                host('s');
            } catch (IOException e) {
                System.out.println("aoi: clipboard: " + e);
            }
            FILE.delete();
        }
        ArrayList<IOnPrimaryClipChangedListener> ls;
        synchronized (this) {
            clip = c; clipText = t; source = c != null ? pkg : null;
            ls = new ArrayList<IOnPrimaryClipChangedListener>(listeners);
        }
        for (IOnPrimaryClipChangedListener l : ls)
            try { l.dispatchPrimaryClipChanged(); } catch (Exception e) { /* gone */ }
    }

    /** What a paste gets: the pasteboard's text (our own clip while it is still that text). */
    private synchronized ClipData current() {
        String t = ask('g');
        if (t == null) {                                       /* no host, or nothing there */
            return ask('h') == null ? clip : null;
        }
        if (clip != null && t.equals(clipText)) return clip;
        return ClipData.newPlainText("text", t);
    }

    private synchronized boolean hasText() {
        String h = ask('h');
        return h != null ? h.startsWith("1") : textOf(clip) != null;
    }

    @Override public void setPrimaryClip(ClipData c, String pkg, String tag, int u, int d) { changed(c, pkg); }
    @Override public void setPrimaryClipAsPackage(ClipData c, String pkg, String tag, int u, int d, String src) { changed(c, src); }
    @Override public void clearPrimaryClip(String pkg, String tag, int u, int d) { changed(null, null); }
    @Override public ClipData getPrimaryClip(String pkg, String tag, int u, int d) { return current(); }
    @Override public ClipDescription getPrimaryClipDescription(String pkg, String tag, int u, int d) {
        if (!hasText()) return null;                           /* not read: iOS asks the user on a read */
        ClipData c = ClipData.newPlainText("text", "");
        return c != null ? c.getDescription() : null;
    }
    @Override public boolean hasPrimaryClip(String pkg, String tag, int u, int d) { return hasText() || clip != null; }
    @Override public synchronized void addPrimaryClipChangedListener(IOnPrimaryClipChangedListener l, String pkg, String tag, int u, int d) {
        listeners.add(l);
    }
    @Override public synchronized void removePrimaryClipChangedListener(IOnPrimaryClipChangedListener l, String pkg, String tag, int u, int d) {
        listeners.remove(l);
    }
    @Override public boolean hasClipboardText(String pkg, String tag, int u, int d) { return hasText(); }
    @Override public synchronized String getPrimaryClipSource(String pkg, String tag, int u, int d) { return source; }
    @Override public boolean areClipboardAccessNotificationsEnabledForUser(int u) { return false; }
    @Override public void setClipboardAccessNotificationsEnabledForUser(boolean enable, int u) {}
}
