package aoi;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import java.util.HashMap;

/**
 * The "settings" content provider (Settings.Global/Secure/System): what a freshly
 * set-up phone would answer. android.provider.Settings reads a value with
 * call("GET_global", name) and takes the reply's "value"; writes are kept in memory.
 */
final class SettingsProvider extends ContentProvider {
    private final HashMap<String, String> values = new HashMap<String, String>();

    SettingsProvider() {
        String[] d = {
            "global/animator_duration_scale", "1.0", "global/transition_animation_scale", "1.0",
            "global/window_animation_scale", "1.0", "global/development_settings_enabled", "0",
            "global/adb_enabled", "0", "global/auto_time", "1", "global/auto_time_zone", "1",
            "global/airplane_mode_on", "0", "global/device_provisioned", "1", "global/stay_on_while_plugged_in", "0",
            "secure/user_setup_complete", "1", "secure/android_id", "a0b1c2d3e4f50617",
            "secure/accessibility_enabled", "0", "secure/touch_exploration_enabled", "0",
            "secure/show_ime_with_hard_keyboard", "0", "secure/location_mode", "0",
            "system/font_scale", "1.0", "system/time_12_24", "24", "system/screen_brightness", "200",
            "system/haptic_feedback_enabled", "1", "system/sound_effects_enabled", "0",
            "system/accelerometer_rotation", "1", "system/show_touches", "0",
        };
        for (int i = 0; i < d.length; i += 2) values.put(d[i], d[i + 1]);
    }

    @Override
    public Bundle call(String method, String name, Bundle extras) {
        Bundle b = new Bundle();
        if (method == null) return b;
        int u = method.indexOf('_');
        String table = u > 0 ? method.substring(u + 1).toLowerCase() : "";
        if (method.startsWith("GET_")) {
            b.putString("value", values.get(table + "/" + name));
        } else if (method.startsWith("PUT_") && extras != null) {
            values.put(table + "/" + name, extras.getString("value"));
        }
        return b;
    }

    @Override public boolean onCreate() { return true; }
    @Override public Cursor query(Uri u, String[] p, String s, String[] a, String o) { return null; }
    @Override public String getType(Uri u) { return null; }
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String s, String[] a) { return 0; }
    @Override public int update(Uri u, ContentValues v, String s, String[] a) { return 0; }
}
