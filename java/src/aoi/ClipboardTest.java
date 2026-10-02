package aoi;

import android.content.ClipData;

/** aoi.Clipboard against the host's clipboard (aoiproc AOI_CLIP): reads the host's
 *  text, then copies one of its own and reads that back. */
public final class ClipboardTest {
    public static void main(String[] args) {
        Clipboard c = new Clipboard();
        ClipData got = c.getPrimaryClip("aoi", null, 0, 0);
        System.out.println("clipboard: host has text " + c.hasClipboardText("aoi", null, 0, 0) + ", paste gives \""
                + (got != null && got.getItemAt(0) != null ? got.getItemAt(0).getText() : null) + "\"");
        c.setPrimaryClip(ClipData.newPlainText("text", "kopyalandı 42"), "aoi", null, 0, 0);
        got = c.getPrimaryClip("aoi", null, 0, 0);
        System.out.println("clipboard: after copy, paste gives \"" + (got != null ? got.getItemAt(0).getText() : null) + "\"");
    }
}
