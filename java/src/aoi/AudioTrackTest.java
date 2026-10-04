package aoi;

import android.media.AudioTrack;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.*;
import android.os.ServiceManager;
import java.util.Map;

/** Sound: an AudioTrack (stream mode, 44.1 kHz stereo 16-bit) plays a 440 Hz tone
 *  through our AudioFlinger (core/af.c), which takes it from the track's shared buffer
 *  as the output clock goes (AOI_AUDIO_OUT on the host: the mixed output, raw s16le
 *  48 kHz stereo). It checks that the minimum buffer size is known, the track is made,
 *  all of it is written and the playback position moves. */
public final class AudioTrackTest {
    /** Just enough of a package manager for AttributionSource (the app's own answers it in aoi.Main). */
    static final class Packages extends android.content.pm.IPackageManager.Stub {
        Packages() { super(GrantAll.INSTANCE); }
        @Override public android.os.IInterface queryLocalInterface(String descriptor) { return null; }
        @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags)
                throws android.os.RemoteException {
            try { return super.onTransact(code, data, reply, flags); }
            catch (AbstractMethodError e) { return Services.missing(this, e, reply); }
        }
        @Override public String[] getPackagesForUid(int uid) { return new String[] { "aoi.test" }; }
        @Override public void notifyDexLoad(String loadingPackageName, Map classLoaderContextMap, String loaderIsa) {  }
        @Override public ApplicationInfo getApplicationInfo(String packageName, long flags, int userId) { return null; }
        @Override public PackageInfo getPackageInfo(String packageName, long flags, int userId) { return null; }
        @Override public ActivityInfo getActivityInfo(ComponentName className, long flags, int userId) { return null; }
        @Override public ServiceInfo getServiceInfo(ComponentName className, long flags, int userId) { return null; }
        @Override public ProviderInfo getProviderInfo(ComponentName className, long flags, int userId) { return null; }
        @Override public ActivityInfo getReceiverInfo(ComponentName className, long flags, int userId) { return null; }
        @Override public boolean hasSystemFeature(String name, int version) { return false; }
        @Override public boolean hasSigningCertificate(String packageName, byte[] certificate, int type) { return false; }
        @Override public int getComponentEnabledSetting(ComponentName componentName, int userId) { return 0; }
        @Override public String getInstallerPackageName(String packageName) { return null; }
        @Override public String getNameForUid(int uid) { return null; }
        @Override public int getPackageUid(String packageName, long flags, int userId) { return 0; }
        @Override public boolean isPackageAvailable(String packageName, int userId) { return false; }
        @Override public boolean isSafeMode() { return false; }
        @Override public int checkPermission(String permName, String pkgName, int userId) { return 0; }
        @Override public ResolveInfo resolveIntent(Intent intent, String resolvedType, long flags, int userId) { return null; }
        @Override public ParceledListSlice queryIntentActivities(Intent intent, String resolvedType, long flags, int userId) { return null; }
        @Override public ProviderInfo resolveContentProvider(String name, long flags, int userId) { return null; }
        @Override public ParceledListSlice getSystemAvailableFeatures() { return null; }
        @Override public ParceledListSlice queryProperty(String propertyName, int componentType) { return null; }
        @Override public android.content.pm.PackageManager.Property getPropertyAsUser(String propertyName, String packageName, String className, int userId) { return null; }
    }

    public static void main(String[] args) throws Exception {
        ServiceManager.addService("audio", new AudioService());
        ServiceManager.addService("package", new Packages());
        int rate = 44100, min = AudioTrack.getMinBufferSize(rate, 12, 2);   /* CHANNEL_OUT_STEREO, PCM_16BIT */
        AudioTrack t = new AudioTrack(3, rate, 12, 2, Math.max(min, 16384), 1); /* STREAM_MUSIC, MODE_STREAM */
        short[] b = new short[rate / 10 * 2];                                  /* 100 ms */
        int written = 0;
        t.play();
        for (int k = 0; k < 5; k++) {
            for (int i = 0; i < b.length / 2; i++) {
                short v = (short) (8000 * Math.sin(2 * Math.PI * 440 * (k * b.length / 2 + i) / rate));
                b[2 * i] = v; b[2 * i + 1] = v;
            }
            written += t.write(b, 0, b.length);
        }
        Thread.sleep(800);
        int pos = t.getPlaybackHeadPosition();
        t.stop();
        t.release();
        System.out.println("audiotrack: min " + (min > 0) + ", state " + t.getState() + ", wrote " + written
                + ", played " + (pos >= rate / 4));
        System.out.println("audiotrack: position " + pos);
    }
}
