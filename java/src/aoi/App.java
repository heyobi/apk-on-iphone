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

    static final String WEBVIEW = "com.android.webview";
    static final String WEBVIEW_APK = "/product/app/webview/webview.apk";
    private static App webview;
    private static boolean webviewRead;

    /** The system image's WebView package (aoi.WebViewUpdate names it): installed as a
     *  device has it, a system app whose libraries stay in the APK (stored, page-aligned:
     *  loaded from there). Null without it. */
    static synchronized App webview() {
        if (webviewRead) return webview;
        webviewRead = true;
        try {
            java.io.File apk = new java.io.File(WEBVIEW_APK);
            if (!apk.exists()) return null;
            PackageParser.Package p = new PackageParser().parsePackage(apk, 0);
            ApplicationInfo ai = p.applicationInfo;
            ai.sourceDir = ai.publicSourceDir = WEBVIEW_APK;
            ai.dataDir = ai.credentialProtectedDataDir = "/data/data/" + p.packageName;
            ai.deviceProtectedDataDir = "/data/user_de/0/" + p.packageName;
            ai.nativeLibraryDir = "/product/app/webview/lib/arm64";
            ai.nativeLibraryRootDir = "/product/app/webview/lib";
            ai.primaryCpuAbi = "arm64-v8a";
            ai.flags |= ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED;
            ai.uid = 10099;
            if (ai.metaData == null) ai.metaData = p.mAppMetaData;   /* com.android.webview.WebViewLibrary */
            webview = new App(p, ai, null);
        } catch (Throwable e) {
            System.out.println("aoi: webview: " + e);
        }
        return webview;
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

    private boolean signed;
    private android.content.pm.Signature[] signatures;
    private android.content.pm.SigningInfo signingInfo;

    /** The APK's signing certificates (v3/v2 signature block, or v1 JAR signature), read
     *  without checking the digests, as the package manager knew them from install:
     *  apps check their own signature (Google's APIs send its SHA-1; ACRA, NewPipe). */
    private synchronized void readSignatures() {
        if (signed) return;
        signed = true;
        try {
            Object in = Class.forName("android.content.pm.parsing.result.ParseTypeImpl")
                    .getMethod("forParsingWithoutPlatformCompat").invoke(null);
            Class<?> pi = Class.forName("android.content.pm.parsing.result.ParseInput");
            Object r = Class.forName("android.util.apk.ApkSignatureVerifier")
                    .getMethod("unsafeGetCertsWithoutVerification", pi, String.class, int.class)
                    .invoke(null, in, info.sourceDir, 1);
            Class<?> pr = Class.forName("android.content.pm.parsing.result.ParseResult");
            if ((Boolean) pr.getMethod("isError").invoke(r)) {
                System.out.println("aoi: signatures: " + pr.getMethod("getErrorMessage").invoke(r));
                return;
            }
            Object details = pr.getMethod("getResult").invoke(r);
            signatures = (android.content.pm.Signature[]) details.getClass().getMethod("getSignatures").invoke(details);
            java.lang.reflect.Constructor<?> k = android.content.pm.SigningInfo.class
                    .getDeclaredConstructor(Class.forName("android.content.pm.SigningDetails"));
            k.setAccessible(true);
            signingInfo = (android.content.pm.SigningInfo) k.newInstance(details);
        } catch (Throwable e) {
            System.out.println("aoi: signatures: " + e);
        }
    }

    android.content.pm.Signature[] signatures() { readSignatures(); return signatures; }
    android.content.pm.SigningInfo signingInfo() { readSignatures(); return signingInfo; }

    ActivityInfo activity(String className) {
        for (PackageParser.Activity a : pkg.activities)
            if (a.info.name.equals(className)) return a.info;
        return null;
    }
}
