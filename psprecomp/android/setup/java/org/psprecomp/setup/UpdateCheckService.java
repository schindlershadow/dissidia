package org.psprecomp.setup;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileReader;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URLConnection;
import java.net.URL;

import org.json.JSONObject;

/**
 * Once a day, in the background (JobScheduler: only on a network, survives reboots), looks at the latest release's manifest and posts a
 * notification when
 *   - a newer setup app is available, or
 *   - the game built on this device is out of date (the installed setup app builds it differently: "Update the game" on its start page).
 * The check is only a hint: nothing is downloaded or installed here. Tapping the notification opens the app, whose own check verifies the
 * manifest's signature (update.py android-check) before it offers anything. Off when check_updates=0 in install.ini (Options > Check for updates).
 */
public class UpdateCheckService extends JobService {
    private static final int JOB_ID = 4711;
    private static final String CHANNEL = "updates";
    private static final String MANIFEST = "https://github.com/schindlershadow/dissidia/releases/latest/download/manifest.json";

    /** Schedules the daily check (once; a pending job is kept). */
    public static void schedule(Context c) {
        try {
            JobScheduler js = (JobScheduler) c.getSystemService(Context.JOB_SCHEDULER_SERVICE);
            if (js == null || js.getPendingJob(JOB_ID) != null) return;
            JobInfo.Builder b = new JobInfo.Builder(JOB_ID, new ComponentName(c, UpdateCheckService.class))
                    .setPeriodic(24L * 60 * 60 * 1000)
                    .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                    .setPersisted(true);
            js.schedule(b.build());
        } catch (Exception e) {
            // no notifications about updates then; the in-app check still works
        }
    }

    @Override
    public boolean onStartJob(final JobParameters params) {
        new Thread(new Runnable() {
            public void run() {
                try {
                    check(UpdateCheckService.this);
                } catch (Exception e) {
                    // offline or GitHub unreachable: the next run tries again
                }
                jobFinished(params, false);
            }
        }).start();
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        return true;   // retried at the next period
    }

    // ---- the check ----

    private static String readLine(File f) {
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String l = r.readLine();
            return l == null ? "" : l.trim();
        } catch (Exception e) {
            return "";
        }
    }

    private static String iniValue(File ini, String key) {
        try (BufferedReader r = new BufferedReader(new FileReader(ini))) {
            String l;
            while ((l = r.readLine()) != null) {
                l = l.trim();
                if (l.startsWith(key + "=")) return l.substring(key.length() + 1);
            }
        } catch (Exception e) {
            // none
        }
        return "";
    }

    /** The build fingerprint of the setup app's unpacked payload (files/release-<hash>/android/build_fingerprint.txt): what the game would be built with now. */
    private static String installedFingerprint(File files) {
        File[] dirs = files.listFiles();
        String best = "";
        long newest = -1;
        if (dirs != null)
            for (File d : dirs) {
                if (!d.getName().startsWith("release-")) continue;
                File fp = new File(d, "android/build_fingerprint.txt");
                if (fp.isFile() && fp.lastModified() > newest) {
                    newest = fp.lastModified();
                    best = readLine(fp);
                }
            }
        return best;
    }

    private static int cmpVersion(String a, String b) {   // like update.py version_key: numeric parts compare as numbers
        String[] x = a.replaceFirst("^v", "").replace('-', '.').split("\\.");
        String[] y = b.replaceFirst("^v", "").replace('-', '.').split("\\.");
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            String p = i < x.length ? x[i] : "0", q = i < y.length ? y[i] : "0";
            boolean pn = p.matches("\\d+"), qn = q.matches("\\d+");
            int c = pn && qn ? Long.compare(Long.parseLong(p), Long.parseLong(q)) : p.compareTo(q);
            if (c != 0) return c;
        }
        return 0;
    }

    static void check(Context ctx) throws Exception {
        File files = ctx.getFilesDir();
        File ini = new File(files, "install.ini");
        if ("0".equals(iniValue(ini, "check_updates"))) return;   // the player turned update checks off
        String url = MANIFEST;
        String over = readLine(new File(files, "manifest_url.txt"));   // the debug switch the app has too
        if (!over.isEmpty()) url = over;
        URLConnection c = new URL(url).openConnection();   // https; a file: URL works too (the test switch)
        c.setConnectTimeout(15000);
        c.setReadTimeout(20000);
        if (c instanceof HttpURLConnection) ((HttpURLConnection) c).setInstanceFollowRedirects(true);
        String body;
        try {
            if (c instanceof HttpURLConnection && ((HttpURLConnection) c).getResponseCode() != 200) return;
            InputStream in = c.getInputStream();
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0 && o.size() < (1 << 20)) o.write(buf, 0, n);
            body = o.toString("UTF-8");
        } finally {
            if (c instanceof HttpURLConnection) ((HttpURLConnection) c).disconnect();
        }
        JSONObject m = new JSONObject(body);
        JSONObject p = m.getJSONObject("platforms").optJSONObject("android-aarch64");
        if (p == null) return;
        String latest = m.getString("version");
        String have = ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).versionName;
        String built = iniValue(ini, "built_fp");   // empty: no game built yet
        String newFp = p.optString("build_fingerprint", "");

        String key = null, title = null, text = null;
        if (cmpVersion(latest, have) > 0) {
            key = "app:" + latest;
            title = "Setup app update available";
            text = "Version " + latest + " is available (you have " + have + ")."
                    + (!built.isEmpty() && !newFp.isEmpty() && !newFp.equals(built) ? " The game needs updating afterwards too (no disc needed)." : "");
        } else {
            String now = installedFingerprint(files);
            if (!built.isEmpty() && !now.isEmpty() && !now.equals(built)) {
                key = "game:" + now;
                title = "Game update available";
                text = "Open Dissidia Recomp Setup and choose Update the game (no disc needed).";
            }
        }
        if (key == null) return;
        SharedPreferences sp = ctx.getSharedPreferences("updates", Context.MODE_PRIVATE);
        if (key.equals(sp.getString("notified", ""))) return;   // told about this one already
        if (notify(ctx, title, text)) sp.edit().putString("notified", key).apply();
    }

    private static boolean notify(Context ctx, String title, String text) {
        if (Build.VERSION.SDK_INT >= 33 && ctx.checkSelfPermission("android.permission.POST_NOTIFICATIONS") != PackageManager.PERMISSION_GRANTED)
            return false;   // not allowed (yet): tried again at the next check
        NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return false;
        if (Build.VERSION.SDK_INT >= 26)
            nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Updates", NotificationManager.IMPORTANCE_DEFAULT));
        Intent open = ctx.getPackageManager().getLaunchIntentForPackage(ctx.getPackageName());
        PendingIntent pi = PendingIntent.getActivity(ctx, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(ctx, CHANNEL) : new Notification.Builder(ctx);
        b.setSmallIcon(ctx.getApplicationInfo().icon).setContentTitle(title).setContentText(text)
                .setStyle(new Notification.BigTextStyle().bigText(text)).setContentIntent(pi).setAutoCancel(true);
        nm.notify(JOB_ID, b.build());
        return true;
    }
}
