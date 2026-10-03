package android.content.pm;

public class ApplicationInfo {
    public String packageName, processName, sourceDir, publicSourceDir, dataDir, nativeLibraryDir, nativeLibraryRootDir;
    public String credentialProtectedDataDir, deviceProtectedDataDir, primaryCpuAbi;
    public android.os.Bundle metaData;
    public int uid, flags, targetSdkVersion;
    public static final int FLAG_SYSTEM = 1, FLAG_INSTALLED = 0x00800000;
}
