package android.content.res;

public final class Configuration {
    public int densityDpi, screenWidthDp, screenHeightDp, smallestScreenWidthDp, orientation, uiMode;
    public int screenLayout, touchscreen, keyboard, navigation;
    public float fontScale;
    public Configuration() {}
    public void setToDefaults() {}
    public void setLocale(java.util.Locale loc) {}
}
