package android.content;

public class ClipData {
    public static class Item {
        public CharSequence getText() { return null; }
        public String getHtmlText() { return null; }
    }
    public static ClipData newPlainText(CharSequence label, CharSequence text) { return null; }
    public ClipDescription getDescription() { return null; }
    public int getItemCount() { return 0; }
    public Item getItemAt(int index) { return null; }
}
