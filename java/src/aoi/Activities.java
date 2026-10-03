package aoi;

import android.app.IApplicationThread;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.ClientTransactionItem;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageParser;
import android.content.res.Configuration;
import android.os.IBinder;
import java.util.ArrayList;

/** The app's one task: its activities, bottom to top, each known by the token it was
 *  launched with. A new activity (the launcher, then startActivity) goes on top and the
 *  one below it pauses and stops (its window hides); finishing the top one resumes the
 *  one below; finishing the last one leaves the app (ActivityClientController.home).
 *  Lifecycle changes go to the app as ClientTransactions, as ActivityTaskManager's
 *  realStartActivityLocked and ActivityRecord send them. */
final class Activities {
    private static final class Rec {
        final IBinder token = new android.os.Binder();
        final ActivityInfo info;
        Rec(ActivityInfo info) { this.info = info; }
    }

    private static IApplicationThread thread;
    private static App app;
    private static final ArrayList<Rec> stack = new ArrayList<Rec>();

    private Activities() {}

    static void attach(App a, IApplicationThread t) { app = a; thread = t; }

    private static void send(ClientTransactionItem... items) {
        try {
            ClientTransaction tr = ClientTransaction.obtain(thread);
            for (ClientTransactionItem i : items) tr.addTransactionItem(i);
            thread.scheduleTransaction(tr);
        } catch (android.os.RemoteException e) {
            System.out.println("aoi: activities: " + e);
        }
    }

    /** Puts `info` on top, started with `intent`, and resumes it; the one below stops. */
    static synchronized void launch(ActivityInfo info, Intent intent) {
        Rec below = stack.isEmpty() ? null : stack.get(stack.size() - 1);
        Rec r = new Rec(info);
        stack.add(r);
        System.out.println("aoi: launching " + info.name);
        if (below != null) send(android.app.servertransaction.PauseActivityItem.obtain(below.token, false, true, 0, false, false));
        Configuration config = ActivityManager.phone();
        send(android.app.servertransaction.LaunchActivityItem.obtain(r.token, intent, stack.size(), info, config,
                new Configuration(), 0, null, null, 2 /* PROCESS_STATE_TOP */, null, null, null, null, null, true, null,
                new android.os.Binder(), null, new android.os.Binder(), false, null, null,
                new android.window.ActivityWindowInfo()),
                android.app.servertransaction.ResumeActivityItem.obtain(r.token, true, false));
        if (below != null) send(android.app.servertransaction.StopActivityItem.obtain(below.token, 0));
    }

    /** A result for the activity `to` (startActivityForResult), as when the activity it
     *  started finishes: the app sees onActivityResult / onRequestPermissionsResult. */
    static synchronized void result(IBinder to, String who, int requestCode, int resultCode, Intent data) {
        if (to == null || find(to) < 0) return;
        java.util.ArrayList<android.app.ResultInfo> r = new java.util.ArrayList<android.app.ResultInfo>();
        r.add(new android.app.ResultInfo(who, requestCode, resultCode, data));
        send(android.app.servertransaction.ActivityResultItem.obtain(to, r));
    }

    /** The app's activity an intent names: its component, or the first whose intent
     *  filter takes its action (the intent then names it); null for another app's (none
     *  here). An activity-alias stays itself: ActivityThread instantiates its
     *  targetActivity, as for the launcher. */
    static ActivityInfo resolve(Intent intent) {
        android.content.ComponentName c = intent.getComponent();
        ActivityInfo found = null;
        if (c != null) {
            if (app.pkg.packageName.equals(c.getPackageName())) found = app.activity(c.getClassName());
        } else if (intent.getAction() != null
                && (intent.getPackage() == null || app.pkg.packageName.equals(intent.getPackage()))) {
            for (PackageParser.Activity a : app.pkg.activities) {
                if (a.intents == null) continue;
                for (PackageParser.ActivityIntentInfo ii : a.intents)
                    if (ii.hasAction(intent.getAction())) { found = a.info; break; }
                if (found != null) break;
            }
        }
        if (found != null) intent.setComponent(new android.content.ComponentName(app.pkg.packageName, found.name));
        return found;                                              /* an alias as it is: ActivityThread runs its targetActivity */
    }

    private static int find(IBinder t) {
        for (int i = 0; i < stack.size(); i++) if (stack.get(i).token == t) return i;
        return -1;
    }

    /** Finishes the activity `t`: false when it is the last one (the app is left). */
    static synchronized boolean finish(IBinder t) {
        int i = find(t);
        if (i < 0) return true;
        if (stack.size() == 1) return false;
        boolean top = i == stack.size() - 1;
        stack.remove(i);
        if (top) {
            Rec below = stack.get(stack.size() - 1);
            send(android.app.servertransaction.PauseActivityItem.obtain(t, true, false, 0, false, false));
            send(android.app.servertransaction.ResumeActivityItem.obtain(below.token, true, false));
        }
        send(android.app.servertransaction.DestroyActivityItem.obtain(t, true, 0));
        return true;
    }

    /** finishAffinity(): `t` and the activities below it go (the task has one affinity,
     *  the app's); false when nothing is left above them (the app is left). */
    static synchronized boolean finishAffinity(IBinder t) {
        int i = find(t);
        if (i < 0) return true;
        if (i == stack.size() - 1) return false;
        for (int k = i; k >= 0; k--) send(android.app.servertransaction.DestroyActivityItem.obtain(stack.remove(k).token, true, 0));
        return true;
    }

    static synchronized boolean isRoot(IBinder t) { return find(t) == 0; }

    static synchronized boolean isTop(IBinder t) { return !stack.isEmpty() && stack.get(stack.size() - 1).token == t; }

    /** The activity on top (back goes to it), or null. */
    static synchronized IBinder top() { return stack.isEmpty() ? null : stack.get(stack.size() - 1).token; }
}
