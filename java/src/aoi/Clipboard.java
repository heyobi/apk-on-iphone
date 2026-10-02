package aoi;

import android.content.ClipData;
import android.content.ClipDescription;
import android.content.IClipboard;
import android.content.IOnPrimaryClipChangedListener;
import java.util.ArrayList;

/** The clipboard, inside the app: what it copies it can paste (Qalculate's copy of a
 *  result asks for it). Not shared with the iPhone's pasteboard yet. */
final class Clipboard extends IClipboard.Stub {
    private ClipData clip;
    private String source;
    private final ArrayList<IOnPrimaryClipChangedListener> listeners = new ArrayList<IOnPrimaryClipChangedListener>();

    Clipboard() { super(GrantAll.INSTANCE); }

    private void changed(ClipData c, String pkg) {
        ArrayList<IOnPrimaryClipChangedListener> ls;
        synchronized (this) { clip = c; source = c != null ? pkg : null; ls = new ArrayList<IOnPrimaryClipChangedListener>(listeners); }
        for (IOnPrimaryClipChangedListener l : ls)
            try { l.dispatchPrimaryClipChanged(); } catch (Exception e) { /* gone */ }
    }

    @Override public void setPrimaryClip(ClipData c, String pkg, String tag, int u, int d) { changed(c, pkg); }
    @Override public void setPrimaryClipAsPackage(ClipData c, String pkg, String tag, int u, int d, String src) { changed(c, src); }
    @Override public void clearPrimaryClip(String pkg, String tag, int u, int d) { changed(null, null); }
    @Override public synchronized ClipData getPrimaryClip(String pkg, String tag, int u, int d) { return clip; }
    @Override public synchronized ClipDescription getPrimaryClipDescription(String pkg, String tag, int u, int d) {
        return clip != null ? clip.getDescription() : null;
    }
    @Override public synchronized boolean hasPrimaryClip(String pkg, String tag, int u, int d) { return clip != null; }
    @Override public synchronized void addPrimaryClipChangedListener(IOnPrimaryClipChangedListener l, String pkg, String tag, int u, int d) {
        listeners.add(l);
    }
    @Override public synchronized void removePrimaryClipChangedListener(IOnPrimaryClipChangedListener l, String pkg, String tag, int u, int d) {
        listeners.remove(l);
    }
    @Override public synchronized boolean hasClipboardText(String pkg, String tag, int u, int d) {
        ClipDescription desc = clip != null ? clip.getDescription() : null;
        return desc != null && clip.getItemCount() > 0 && (desc.hasMimeType("text/*"));
    }
    @Override public synchronized String getPrimaryClipSource(String pkg, String tag, int u, int d) { return source; }
    @Override public boolean areClipboardAccessNotificationsEnabledForUser(int u) { return false; }
    @Override public void setClipboardAccessNotificationsEnabledForUser(boolean enable, int u) {}
}
