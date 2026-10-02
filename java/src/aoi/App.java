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

    /** The provider's info, with its manifest <meta-data> (androidx.startup reads its
     *  initializers from there). */
    android.content.pm.ProviderInfo provider(String className) {
        for (PackageParser.Provider p : pkg.providers)
            if (p.info.name.equals(className)) {
                if (p.info.metaData == null) p.info.metaData = p.metaData;
                return p.info;
            }
        return null;
    }

    /** A <service> of the manifest, with its <meta-data> (Chromium checks that its
     *  child-process services exist: getServiceInfo). */
    android.content.pm.ServiceInfo service(String className) {
        for (PackageParser.Service sv : pkg.services)
            if (sv.info.name.equals(className)) {
                if (sv.info.metaData == null) sv.info.metaData = sv.metaData;
                return sv.info;
            }
        return null;
    }

    ActivityInfo receiver(String className) {
        for (PackageParser.Activity a : pkg.receivers)
            if (a.info.name.equals(className)) return a.info;
        return null;
    }

    ActivityInfo activity(String className) {
        for (PackageParser.Activity a : pkg.activities)
            if (a.info.name.equals(className)) return a.info;
        return null;
    }
}
