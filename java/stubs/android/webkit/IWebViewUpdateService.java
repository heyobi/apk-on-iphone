package android.webkit;

public interface IWebViewUpdateService extends android.os.IInterface {
    void notifyRelroCreationCompleted() throws android.os.RemoteException;
    WebViewProviderResponse waitForAndGetProvider() throws android.os.RemoteException;
    String changeProviderAndSetting(String name) throws android.os.RemoteException;
    WebViewProviderInfo[] getValidWebViewPackages() throws android.os.RemoteException;
    WebViewProviderInfo[] getAllWebViewPackages() throws android.os.RemoteException;
    String getCurrentWebViewPackageName() throws android.os.RemoteException;
    android.content.pm.PackageInfo getCurrentWebViewPackage() throws android.os.RemoteException;
    boolean isMultiProcessEnabled() throws android.os.RemoteException;
    void enableMultiProcess(boolean on) throws android.os.RemoteException;
    WebViewProviderInfo getDefaultWebViewPackage() throws android.os.RemoteException;

    abstract class Stub extends android.os.Binder implements IWebViewUpdateService {
        public Stub() {}
        public android.os.IBinder asBinder() { return this; }
    }
}
