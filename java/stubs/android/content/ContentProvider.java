package android.content;

import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;

public abstract class ContentProvider {
    public ContentProvider() {}
    public abstract boolean onCreate();
    public abstract Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder);
    public abstract String getType(Uri uri);
    public abstract Uri insert(Uri uri, ContentValues values);
    public abstract int delete(Uri uri, String selection, String[] selectionArgs);
    public abstract int update(Uri uri, ContentValues values, String selection, String[] selectionArgs);
    public Bundle call(String method, String arg, Bundle extras) { return null; }
    public void attachInfo(Context context, ProviderInfo info) {}
    public IContentProvider getIContentProvider() { return null; }
}
