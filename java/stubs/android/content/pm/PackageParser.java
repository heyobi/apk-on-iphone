package android.content.pm;

import java.io.File;
import java.util.ArrayList;

/** Hidden (deprecated, still in Android 14's framework.jar). */
public class PackageParser {
    public PackageParser() {}
    public Package parsePackage(File file, int flags) throws Exception { return null; }

    public static final class Package {
        public String packageName, mVersionName;
        public int mVersionCode;
        public ApplicationInfo applicationInfo;
        public final ArrayList<Activity> activities = null;
    }

    public static final class Activity {
        public ActivityInfo info;
        public ArrayList<ActivityIntentInfo> intents;           /* declared on Component */
    }

    public static final class ActivityIntentInfo {
        public final boolean hasAction(String a) { return false; }      /* IntentFilter */
        public final boolean hasCategory(String c) { return false; }
    }
}
