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
        /* what every app needs, before it is known: the services that do not depend on
         * it. Started before its app (AOI_WARM: the iOS app's warm process), the process
         * is saved here once and waits; the host then makes it the app's (its /data). */
        ServiceManager.addService("permissionmgr", new PermissionManager());   /* PackageParser asks it */
        ServiceManager.addService("user", new UserManager());
        ServiceManager.addService("display", new DisplayManager());
        ServiceManager.addService("window", new WindowManager());
        ServiceManager.addService("input_method", new InputMethodManager());
        ServiceManager.addService("content", new ContentService());
        ServiceManager.addService("clipboard", new Clipboard());
        ServiceManager.addService("input", new InputService());
        ServiceManager.addService("mount", new StorageService());
        ServiceManager.addService("power", new PowerService());
        ServiceManager.addService("connectivity", new ConnectivityService());
        ServiceManager.addService("audio", new AudioService());
        ServiceManager.addService(WebViewUpdate.NAME, new WebViewUpdate());
        ServiceManager.addService(MediaPlayerService.NAME, new MediaPlayerService());
        codecs();
        if (System.getenv("AOI_WARM") != null) warm();
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
        new File("/data/misc/profiles/cur/0/" + pkg.packageName).mkdirs();   /* the app's profile: ProfileInstaller, */
        new File("/data/misc/profiles/ref/" + pkg.packageName).mkdirs();     /* ART's JIT (dex2oat speed-profile) */
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
        ServiceManager.addService(Keystore.NAME, new Keystore("/data/misc/keystore/aoi"));
        Keystore.installProvider();                                /* AndroidKeyStore (the zygote's job) */
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

    /** The warm process: classes every app start uses, then saved (the host's snapshot
     *  of it, once per build), then waiting until the host hands it an app. */
    /** Android's software codecs (Codec2: AAC, MP3, Opus, Vorbis, FLAC, AVC, VP9...) in
     *  this process: guest/media.c registers their store, on a thread of its own, so
     *  MediaCodec finds them (a phone runs them in the media.swcodec process). */
    static void codecs() {
        final String lib = "/system/lib64/libaoi_media.so";
        if (!new File(lib).exists()) return;
        Thread t = new Thread(new Runnable() {
            public void run() {
                try { System.load(lib); } catch (Throwable e) { System.out.println("aoi: codecs: " + e); }
            }
        }, "aoi-codecs");
        t.setDaemon(true);
        t.start();
    }

    private static void warm() {
        String[] classes = { "android.content.pm.PackageParser", "android.content.pm.PackageParser$Package",
            "android.content.res.AssetManager", "android.content.res.Resources", "android.app.ActivityThread",
            "android.app.LoadedApk", "android.app.ContextImpl", "android.graphics.Typeface",
            "android.view.ViewRootImpl", "android.view.ThreadedRenderer", "android.view.Choreographer",
            "android.widget.TextView", "android.app.Activity" };
        for (String c : classes) {
            try { Class.forName(c, true, null); } catch (Throwable t) { /* (not in this build) */ }
        }
        try {                                                      /* the package parser's own first run (4 s) */
            File w = new File("/system/product/app/webview/webview.apk");
            if (w.exists()) new PackageParser().parsePackage(w, 0);
        } catch (Throwable t) { /* only the warm-up */ }
        System.out.println("aoi: android is up, waiting for its app");
        for (String dev : new String[] { "/dev/aoi_snapshot", "/dev/aoi_warm" }) {
            try { new java.io.FileInputStream(dev).close(); } catch (java.io.IOException e) { /* always ENOENT */ }
        }
        System.out.println("aoi: the app is here");
    }
}
