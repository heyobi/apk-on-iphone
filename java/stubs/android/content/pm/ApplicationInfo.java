package android.content.pm;

public class ApplicationInfo {
    public String packageName, processName, sourceDir, publicSourceDir, dataDir, nativeLibraryDir;
    public String credentialProtectedDataDir, deviceProtectedDataDir, primaryCpuAbi;
    public android.os.Bundle metaData;
    public int uid, flags, targetSdkVersion;
}
