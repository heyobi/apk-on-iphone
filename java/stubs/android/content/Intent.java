package android.content;

public class Intent {
    public Intent() {}
    public Intent(String action) {}
    public String[] getStringArrayExtra(String name) { return null; }
    public Intent putExtra(String name, String[] value) { return this; }
    public Intent putExtra(String name, int[] value) { return this; }
    public static Intent makeMainActivity(ComponentName mainActivity) { return null; }
    public ComponentName getComponent() { return null; }
    public String getAction() { return null; }
    public String getDataString() { return null; }
    public String getPackage() { return null; }
    public Intent setComponent(ComponentName component) { return this; }
}
