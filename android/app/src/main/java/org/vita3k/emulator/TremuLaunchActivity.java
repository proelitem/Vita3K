package org.vita3k.emulator;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Environment;
import android.view.Gravity;
import android.widget.TextView;

import org.vita3k.emulator.data.AppStorage;
import org.vita3k.emulator.data.InstallCallback;
import org.vita3k.emulator.data.NativeAppInfo;
import org.vita3k.emulator.data.NativeUser;

import java.io.File;
import java.io.IOException;
import java.util.Locale;

/**
 * Started by Tremu with a game of Download/Roms/vita: installs what is missing (the player's firmware
 * files found in that folder, the game), then opens it in the headset.
 */
public class TremuLaunchActivity extends Activity {
    public static final String EXTRA_PATH = "tremu_path";
    public static final String EXTRA_RETURN_PACKAGE = "tremu_return_package";
    public static final String EXTRA_PAD = "tremu_pad";
    private static final String TURNIP = "Tremu-Turnip";
    private static final File ROMS = new File("/storage/emulated/0/Download/Roms/vita");

    private TextView status;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        status = new TextView(this);
        status.setGravity(Gravity.CENTER);
        status.setTextSize(22);
        setContentView(status);
        show("Vita3K...");
        new Thread(this::prepare, "TremuLaunch").start();
    }

    private void show(String text) {
        runOnUiThread(() -> status.setText(text));
    }

    private final InstallCallback progress = new InstallCallback() {
        @Override
        public void onProgress(int percent, String text) {
            show(text + " " + percent + " %");
        }
    };

    private void prepare() {
        try {
            if (!Environment.isExternalStorageManager()) {
                show("Vita3K needs the \"all files\" permission to read Download/Roms/vita");
                return;
            }
            NativeLib lib = NativeLib.INSTANCE;
            if (!lib.isInitialized() && !lib.init(AppStorage.INSTANCE.storageRootPath(this))) {
                show("Vita3K failed to start");
                return;
            }
            installFirmware(lib);
            installDrivers(lib);
            if (lib.getFirmwareInstallStateMask() == 0) {
                show("Firmware missing: put PSP2UPDAT.PUP in Download/Roms/vita");
                return;
            }
            NativeUser[] users = lib.getUsers();
            if (users.length == 0) lib.createUser("Player");

            String title = getIntent().getStringExtra(Emulator.EXTRA_TITLE_ID);
            String path = getIntent().getStringExtra(EXTRA_PATH);
            if (title == null || title.isEmpty()) {
                show("This game has no title id");
                return;
            }
            if (!installed(lib, title)) {
                show("Installing...");
                if (path == null || !lib.installArchive(path, progress, false)) {
                    show("Could not install " + path);
                    return;
                }
            }
            Intent game = Emulator.createLaunchIntent(this, title, getIntent().getStringExtra(Emulator.EXTRA_GAME_TITLE))
                    .putExtra(EXTRA_RETURN_PACKAGE, getIntent().getStringExtra(EXTRA_RETURN_PACKAGE))
                    .putExtra(EXTRA_PAD, getIntent().getIntArrayExtra(EXTRA_PAD));
            startActivity(game);
            finish();
        } catch (RuntimeException e) {
            show("Vita3K: " + e);
        }
    }

    private static boolean installed(NativeLib lib, String title) {
        for (NativeAppInfo app : lib.getAppListDetailed())
            if (title.equals(app.getTitleId())) return true;
        return false;
    }

    /**
     * Our Turnip (assets/drivers), the default driver: with the headset's Adreno driver Killzone Mercenary
     * loses its weapon and characters, and crashes.
     */
    private void installDrivers(NativeLib lib) {
        for (String installed : lib.getInstalledCustomDrivers())
            if (installed.equals(TURNIP)) return;
        File zip = new File(getCacheDir(), TURNIP + ".zip");
        try (java.io.InputStream in = getAssets().open("drivers/" + TURNIP + ".zip");
             java.io.FileOutputStream out = new java.io.FileOutputStream(zip)) {
            byte[] buffer = new byte[65536];
            for (int n; (n = in.read(buffer)) > 0;) out.write(buffer, 0, n);
            out.close();
            lib.installCustomDriver(zip.getPath());
        } catch (IOException e) {
            android.util.Log.w("Vita3KXr", "driver not installed: " + e);
        }
        zip.delete();
    }

    /** Each .PUP of the folder once: the main firmware, and the font package if the player has it. */
    private void installFirmware(NativeLib lib) {
        File[] files = ROMS.listFiles();
        if (files == null) return;
        File done = new File(AppStorage.INSTANCE.storageRootPath(this));
        for (File f : files) {
            if (!f.getName().toLowerCase(Locale.ROOT).endsWith(".pup")) continue;
            File marker = new File(done, ".installed-" + f.getName() + "-" + f.length());
            if (marker.exists()) continue;
            show("Installing " + f.getName());
            if (lib.installFirmware(f.getPath(), progress).isEmpty()) continue;
            try {
                marker.createNewFile();
            } catch (IOException ignored) {
                // Only means it is installed again next time.
            }
        }
    }
}
