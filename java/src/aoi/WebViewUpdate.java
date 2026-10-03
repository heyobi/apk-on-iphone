package aoi;

import android.content.pm.PackageInfo;
import android.webkit.IWebViewUpdateService;
import android.webkit.WebViewProviderInfo;
import android.webkit.WebViewProviderResponse;

/** "webviewupdate": which package is the WebView, the system image's AOSP WebView
 *  (com.android.webview, /product/app/webview). WebViewFactory loads its code into the
 *  app's process from there. Multiprocess off: its renderer runs in the app's process
 *  (a device starts a sandboxed process for it; there are no others here). */
final class WebViewUpdate extends IWebViewUpdateService.Stub {
    static final String NAME = "webviewupdate";

    @Override public android.os.IInterface queryLocalInterface(String descriptor) { return null; }
    @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags)
            throws android.os.RemoteException {
        try { return super.onTransact(code, data, reply, flags); }
        catch (AbstractMethodError e) { return Services.missing(this, e, reply); }
    }

    private static PackageInfo info() {
        App w = App.webview();
        return w == null ? null : PackageManager.packageInfo(w, 0x40);   /* GET_SIGNATURES: WebViewFactory compares them */
    }

    @Override public void notifyRelroCreationCompleted() {}
    @Override public WebViewProviderResponse waitForAndGetProvider() {
        PackageInfo pi = info();
        return new WebViewProviderResponse(pi, pi != null ? 0 : 4);   /* STATUS_SUCCESS, STATUS_FAILED_LISTING_WEBVIEW_PACKAGES */
    }
    @Override public String changeProviderAndSetting(String name) { return App.WEBVIEW; }
    @Override public WebViewProviderInfo[] getValidWebViewPackages() { return getAllWebViewPackages(); }
    @Override public WebViewProviderInfo[] getAllWebViewPackages() {
        return new WebViewProviderInfo[] { getDefaultWebViewPackage() };
    }
    @Override public WebViewProviderInfo getDefaultWebViewPackage() {
        return new WebViewProviderInfo(App.WEBVIEW, "AOSP WebView", true, false, null);
    }
    @Override public String getCurrentWebViewPackageName() { return App.WEBVIEW; }
    @Override public PackageInfo getCurrentWebViewPackage() { return info(); }
    @Override public boolean isMultiProcessEnabled() { return false; }
    @Override public void enableMultiProcess(boolean on) {}
}
