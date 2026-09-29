package com.andwayland.companion;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/**
 * Manages compositor configuration and gesture engine tuning.
 * Reads and writes JSON preferences to /data/wayland/config.json.
 */
public class PreferencesManager {

    private static final String TAG = "PreferencesManager";
    private static final String CONFIG_FILE_PATH = "/data/wayland/config.json";
    private static final String PREFS_NAME = "anative_drawer_prefs";

    private final Context context;
    private final SharedPreferences sp;

    public int holdThresholdMs = 450;
    public float scrollSensitivity = 1.0f;
    public int doubleTapMs = 250;
    public boolean invertScroll = false;
    public boolean forceSsd = true;
    public boolean persistentNotification = true;

    public PreferencesManager(Context context) {
        this.context = context.getApplicationContext();
        this.sp = this.context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        load();
    }

    public synchronized void load() {
        // First try reading /data/wayland/config.json
        File configFile = new File(CONFIG_FILE_PATH);
        if (configFile.exists() && configFile.canRead()) {
            try (FileInputStream fis = new FileInputStream(configFile)) {
                byte[] data = new byte[(int) configFile.length()];
                int read = fis.read(data);
                if (read > 0) {
                    String jsonStr = new String(data, 0, read, StandardCharsets.UTF_8);
                    JSONObject json = new JSONObject(jsonStr);
                    holdThresholdMs = json.optInt("holdThresholdMs", 450);
                    scrollSensitivity = (float) json.optDouble("scrollSensitivity", 1.0);
                    doubleTapMs = json.optInt("doubleTapMs", 250);
                    invertScroll = json.optBoolean("invertScroll", false);
                    forceSsd = json.optBoolean("forceSsd", true);
                    persistentNotification = json.optBoolean("persistentNotification", true);
                    return;
                }
            } catch (Exception e) {
                Log.w(TAG, "Failed to read " + CONFIG_FILE_PATH + ", falling back to SharedPreferences", e);
            }
        }

        // Fallback to local SharedPreferences
        holdThresholdMs = sp.getInt("holdThresholdMs", 450);
        scrollSensitivity = sp.getFloat("scrollSensitivity", 1.0f);
        doubleTapMs = sp.getInt("doubleTapMs", 250);
        invertScroll = sp.getBoolean("invertScroll", false);
        forceSsd = sp.getBoolean("forceSsd", true);
        persistentNotification = sp.getBoolean("persistentNotification", true);
    }

    public synchronized void save() {
        // Save to SharedPreferences
        sp.edit()
                .putInt("holdThresholdMs", holdThresholdMs)
                .putFloat("scrollSensitivity", scrollSensitivity)
                .putInt("doubleTapMs", doubleTapMs)
                .putBoolean("invertScroll", invertScroll)
                .putBoolean("forceSsd", forceSsd)
                .putBoolean("persistentNotification", persistentNotification)
                .apply();

        // Write to /data/wayland/config.json
        try {
            JSONObject json = new JSONObject();
            json.put("holdThresholdMs", holdThresholdMs);
            json.put("scrollSensitivity", (double) scrollSensitivity);
            json.put("doubleTapMs", doubleTapMs);
            json.put("invertScroll", invertScroll);
            json.put("forceSsd", forceSsd);
            json.put("persistentNotification", persistentNotification);

            String jsonStr = json.toString(2);
            File dir = new File("/data/wayland");
            if (!dir.exists()) {
                dir.mkdirs();
            }

            File configFile = new File(CONFIG_FILE_PATH);
            try (FileOutputStream fos = new FileOutputStream(configFile)) {
                fos.write(jsonStr.getBytes(StandardCharsets.UTF_8));
                fos.flush();
            } catch (Exception e) {
                // If permission denied in unprivileged context, write via su
                writeViaSu(jsonStr);
            }
        } catch (Exception e) {
            Log.e(TAG, "Error writing config", e);
        }
    }

    private void writeViaSu(String jsonStr) {
        try {
            String escaped = jsonStr.replace("\"", "\\\"");
            Process p = Runtime.getRuntime().exec(new String[]{
                    "su", "-c", "echo \"" + escaped + "\" > " + CONFIG_FILE_PATH + " && chmod 666 " + CONFIG_FILE_PATH
            });
            p.waitFor();
        } catch (Exception ignored) {
        }
    }
}
