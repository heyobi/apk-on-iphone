package android.content.pm;

public class PackageInfo {
    public String packageName, versionName;
    public int versionCode;
    public ApplicationInfo applicationInfo;
    public ActivityInfo[] activities;
    public long firstInstallTime, lastUpdateTime;
    public Signature[] signatures;
    public SigningInfo signingInfo;
    public PackageInfo() {}
    public void setLongVersionCode(long v) {}
}
