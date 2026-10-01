package aoi;

import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageParser;

/** The one installed app: the APK as PackageParser read it, shared by our services. */
final class App {
    final PackageParser.Package pkg;
    final ApplicationInfo info;
    final ActivityInfo launcher;

    App(PackageParser.Package pkg, ApplicationInfo info, ActivityInfo launcher) {
        this.pkg = pkg;
        this.info = info;
        this.launcher = launcher;
    }

    ActivityInfo activity(String className) {
        for (PackageParser.Activity a : pkg.activities)
            if (a.info.name.equals(className)) return a.info;
        return null;
    }
}
