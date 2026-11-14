package org.psprecomp.setup;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.PowerManager;
import android.provider.Settings;
import android.view.WindowManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

/** SDL's activity plus the few Android services the setup wizard (native, C++) needs: storage access, installing the built APK, keeping awake. */
public class SetupActivity extends SDLActivity {
    private static final String ACTION = "org.psprecomp.setup.INSTALL_RESULT";
    private static volatile String installStatus = "";
    private static PowerManager.WakeLock wakeLock;
    private static BroadcastReceiver receiver;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        UpdateCheckService.schedule(this);   // the daily background check for updates (a notification only)
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission("android.permission.POST_NOTIFICATIONS") != android.content.pm.PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] {"android.permission.POST_NOTIFICATIONS"}, 1);   // Android 13+: asks once, for the update notifications
    }

    /** Where the executables (lib*.so) were unpacked: the one folder an app may run files from. */
    public static String nativeLibDir() {
        return mSingleton.getApplicationInfo().nativeLibraryDir;
    }

    /** All-files access ("manage external storage"): needed to read the disc image from the SD card or Download. */
    public static boolean hasAllFilesAccess() {
        return Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }

    public static void requestAllFilesAccess() {
        final SDLActivity a = mSingleton;
        a.runOnUiThread(new Runnable() {
            public void run() {
                try {
                    Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + a.getPackageName()));
                    a.startActivity(i);
                } catch (Exception e) {
                    a.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                }
            }
        });
    }

    /** Keep the CPU running (and the screen on) during the long build. */
    public static void keepAwake(final boolean on) {
        final SDLActivity a = mSingleton;
        a.runOnUiThread(new Runnable() {
            public void run() {
                if (on) {
                    a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    if (wakeLock == null) {
                        PowerManager pm = (PowerManager) a.getSystemService(Context.POWER_SERVICE);
                        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "psprecomp:build");
                    }
                    if (!wakeLock.isHeld()) wakeLock.acquire();
                } else {
                    a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
                }
            }
        });
    }

    public static boolean canInstallPackages() {
        return Build.VERSION.SDK_INT < 26 || mSingleton.getPackageManager().canRequestPackageInstalls();
    }

    public static void requestInstallPermission() {
        final SDLActivity a = mSingleton;
        a.runOnUiThread(new Runnable() {
            public void run() {
                a.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + a.getPackageName())));
            }
        });
    }

    /** Install an APK with the PackageInstaller (no file provider needed). Android shows its own confirmation. Result: installStatus(). */
    public static void installApk(final String path) {
        final SDLActivity a = mSingleton;
        installStatus = "working";
        try {
            if (receiver == null) {
                receiver = new BroadcastReceiver() {
                    public void onReceive(Context c, Intent i) {
                        int st = i.getIntExtra(PackageInstaller.EXTRA_STATUS, -1);
                        if (st == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                            Intent confirm = (Intent) i.getParcelableExtra(Intent.EXTRA_INTENT);
                            confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                            c.startActivity(confirm);
                        } else if (st == PackageInstaller.STATUS_SUCCESS) {
                            installStatus = "ok";
                        } else {
                            installStatus = "failed: " + i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                        }
                    }
                };
                a.registerReceiver(receiver, new IntentFilter(ACTION));
            }
            PackageInstaller pi = a.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams p = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            int id = pi.createSession(p);
            PackageInstaller.Session s = pi.openSession(id);
            InputStream in = new FileInputStream(path);
            OutputStream out = s.openWrite("dissidia.apk", 0, -1);
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            s.fsync(out);
            in.close();
            out.close();
            Intent cb = new Intent(ACTION).setPackage(a.getPackageName());
            int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
            s.commit(PendingIntent.getBroadcast(a, id, cb, flags).getIntentSender());
            s.close();
        } catch (Exception e) {
            installStatus = "failed: " + e;
        }
    }

    // ---- Android's own file picker (Storage Access Framework): it hands us an open file descriptor, not a path, so no storage permission is needed
    // to read the disc image. The wizard passes the descriptor on to the build tools as "fd:N" (games/dissidia/tools/setup_from_iso.py).
    private static final int REQ_PICK = 4711;
    private static volatile String pickStatus = "";

    public static void pickFile() {
        final SDLActivity a = mSingleton;
        pickStatus = "pending";
        a.runOnUiThread(new Runnable() {
            public void run() {
                try {
                    Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    i.setType("*/*");
                    a.startActivityForResult(i, REQ_PICK);
                } catch (Exception e) {
                    pickStatus = "failed: " + e;
                }
            }
        });
    }

    /** "" (never asked) | "pending" | "cancelled" | "failed: ..." | "ok:<fd>:<file name>" */
    public static void clearPick() {
        pickStatus = "";
    }

    public static String pickStatus() {
        return pickStatus;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQ_PICK) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            pickStatus = "cancelled";
            return;
        }
        try {
            Uri u = data.getData();
            String name = "game";
            Cursor c = getContentResolver().query(u, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null);
            if (c != null) {
                if (c.moveToFirst()) name = c.getString(0);
                c.close();
            }
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(u, "r");
            pickStatus = "ok:" + pfd.detachFd() + ":" + name;
        } catch (Exception e) {
            pickStatus = "failed: " + e;
        }
    }

    // ---- updates: HTTPS downloads go through Android's own stack (the bundled Python is musl: no resolver config, no CA store on Android).
    private static volatile String downloadStatus = "";
    private static volatile boolean downloadCancel = false;

    /** The app's own version name (set at build time from the release version). */
    public static String appVersion() {
        try {
            return mSingleton.getPackageManager().getPackageInfo(mSingleton.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "";
        }
    }

    /** Downloads url to dest on a thread. downloadStatus(): "" | "working:<bytes>:<total>" | "ok" | "failed: ..." */
    public static void download(final String url, final String dest) {
        downloadStatus = "working:0:0";
        downloadCancel = false;
        new Thread(new Runnable() {
            public void run() {
                HttpURLConnection c = null;
                try {
                    c = (HttpURLConnection) new URL(url).openConnection();
                    c.setConnectTimeout(15000);
                    c.setReadTimeout(30000);
                    c.setInstanceFollowRedirects(true);
                    int code = c.getResponseCode();
                    if (code != 200) {
                        downloadStatus = "failed: HTTP " + code;
                        return;
                    }
                    long total = c.getContentLengthLong();
                    File part = new File(dest + ".part");
                    InputStream in = c.getInputStream();
                    OutputStream out = new FileOutputStream(part);
                    byte[] buf = new byte[1 << 16];
                    long done = 0;
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        if (downloadCancel) {
                            out.close();
                            in.close();
                            part.delete();
                            downloadStatus = "failed: cancelled";
                            return;
                        }
                        out.write(buf, 0, n);
                        done += n;
                        downloadStatus = "working:" + done + ":" + total;
                    }
                    out.close();
                    in.close();
                    File d = new File(dest);
                    d.delete();
                    if (!part.renameTo(d)) {
                        downloadStatus = "failed: cannot write " + dest;
                        return;
                    }
                    downloadStatus = "ok";
                } catch (Exception e) {
                    downloadStatus = "failed: " + e;
                } finally {
                    if (c != null) c.disconnect();
                }
            }
        }).start();
    }

    public static void cancelDownload() {
        downloadCancel = true;
    }

    public static String downloadStatus() {
        return downloadStatus;
    }

    public static String installStatus() {
        return installStatus;
    }
}
