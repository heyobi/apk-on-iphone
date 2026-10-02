package aoi;

import android.app.ActivityThread;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageParser;
import android.os.ServiceManager;
import java.io.File;

/**
 * The app process, AIM ADR 0013 style: no system_server. Parses the APK with the
 * framework's own PackageParser, registers our in-process system services with the
 * servicemanager of core/binder.c, then hands over to ActivityThread.main(), the
 * entry point of every Android app process.
 *
 * usage: app_process64 /system/bin aoi.Main /data/app/APP.apk
 */
public final class Main {
    public static void main(String[] args) throws Exception {
        String apk = args[0];
        ServiceManager.addService("permissionmgr", new PermissionManager());   /* PackageParser asks it */
        PackageParser.Package pkg = new PackageParser().parsePackage(new File(apk), 0);
        ApplicationInfo ai = pkg.applicationInfo;
        String data = "/data/data/" + pkg.packageName;
        ai.sourceDir = ai.publicSourceDir = apk;
        ai.dataDir = ai.credentialProtectedDataDir = data;
        ai.deviceProtectedDataDir = "/data/user_de/0/" + pkg.packageName;
        ai.nativeLibraryDir = new File(new File(apk).getParentFile(), "lib/arm64").getPath();
        ai.primaryCpuAbi = "arm64-v8a";
        ai.uid = 10100;
        if (ai.metaData == null) ai.metaData = pkg.mAppMetaData;   /* <application> <meta-data> (Play services' version check) */
        new File(ai.dataDir).mkdirs();                             /* the app's data dirs: Kiwi found none (ENOENT) */
        new File(ai.deviceProtectedDataDir).mkdirs();
        ActivityInfo launcher = null;
        for (PackageParser.Activity a : pkg.activities) {
            if (a.intents == null) continue;
            for (PackageParser.ActivityIntentInfo ii : a.intents)
                if (ii.hasAction("android.intent.action.MAIN") && ii.hasCategory("android.intent.category.LAUNCHER"))
                    launcher = a.info;
            if (launcher != null) break;
        }
        System.out.println("aoi: package " + pkg.packageName + ", launcher "
                + (launcher != null ? launcher.name : "none") + ", targetSdk " + ai.targetSdkVersion);
        App app = new App(pkg, ai, launcher);
        ServiceManager.addService("package", new PackageManager(app));
        ServiceManager.addService("activity", new ActivityManager(app));
        ServiceManager.addService("activity_task", new ActivityTaskManager(app));
        ServiceManager.addService("user", new UserManager());
        ServiceManager.addService("display", new DisplayManager());
        ServiceManager.addService("window", new WindowManager());
        ServiceManager.addService("input_method", new InputMethodManager());
        ServiceManager.addService("content", new ContentService());
        ServiceManager.addService("clipboard", new Clipboard());
        ServiceManager.addService("input", new InputService());
        ServiceManager.addService("mount", new StorageService());
        ServiceManager.addService("power", new PowerService());
        ServiceManager.addService("audio", new AudioService());
        Services.standIns();                                       /* the rest: default answers */
        /* Windows draw in software (Skia on the CPU) unless AOI_HWUI is set: then HWUI's
         * GPU pipeline runs on our OpenGL ES driver (guest/gles.c, gpu/host.c). */
        if (System.getenv("AOI_HWUI") == null) {
            java.lang.reflect.Field hw = Class.forName("android.view.ThreadedRenderer").getDeclaredField("sRendererEnabled");
            hw.setAccessible(true);
            hw.setBoolean(null, false);
        }
        ActivityThread.main(new String[0]);
    }
}
