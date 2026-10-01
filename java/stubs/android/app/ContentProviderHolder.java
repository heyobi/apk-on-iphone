package android.app;

import android.content.IContentProvider;
import android.content.pm.ProviderInfo;

public class ContentProviderHolder {
    public final ProviderInfo info = null;          /* set by the constructor */
    public IContentProvider provider;
    public boolean noReleaseNeeded;
    public ContentProviderHolder(ProviderInfo info) {}
}
