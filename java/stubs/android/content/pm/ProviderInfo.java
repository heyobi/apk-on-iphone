package android.content.pm;

public class ProviderInfo {
    public String authority, name, packageName, processName;
    public ApplicationInfo applicationInfo;
    public boolean exported, enabled = true;
    public android.os.Bundle metaData;                          /* PackageItemInfo */
    public ProviderInfo() {}
}
