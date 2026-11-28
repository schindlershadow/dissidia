package org.psprecomp.setup;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

/**
 * Keeps the app alive while the game is built (10 to 20 minutes): a foreground service with a progress notification, so switching to
 * another app does not stop the build. The work itself runs in BuildRunner; this only holds the process in the foreground and shows
 * where it is. When the build ends while the app is not on screen, a notification says so.
 */
public class BuildService extends Service {
    private static final String CHANNEL = "build", CHANNEL_DONE = "build_done";
    private static final int ID = 7101, ID_DONE = 7102;
    private static volatile String title = "Building the game", text = "";
    private static volatile int percent = -1;

    static void start(Context c) {
        hold(c);
        try {
            Intent i = new Intent(c, BuildService.class);
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i); else c.startService(i);
        } catch (Exception e) {
            // without it the build still runs while the app is open (the wake lock keeps the CPU on)
        }
    }

    static void stop(Context c) {
        c.stopService(new Intent(c, BuildService.class));
        release();
    }

    // The CPU stays on for the whole build (the screen may turn off): ONE lock for the process, taken when the build starts and given back
    // when it ends (BuildRunner), fails or is cancelled, or when the service goes. A crash drops it with the process; the timeout is the last net.
    private static PowerManager.WakeLock lock;

    static synchronized void hold(Context c) {
        try {
            if (lock == null) {
                PowerManager pm = (PowerManager) c.getApplicationContext().getSystemService(POWER_SERVICE);
                lock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "psprecomp:build");
                lock.setReferenceCounted(false);
            }
            if (!lock.isHeld()) lock.acquire(3 * 60 * 60 * 1000L);
        } catch (Exception e) {
            // no lock: the foreground service still keeps the process running
        }
    }

    static synchronized void release() {
        try {
            if (lock != null && lock.isHeld()) lock.release();
        } catch (Exception e) {
            // already released
        }
    }

    static boolean holding() {
        return lock != null && lock.isHeld();
    }

    /** New progress for the notification (cheap: only posted when something visible changed). */
    static void update(Context c, String t, String x, int pct) {
        if (pct == percent && t.equals(title) && x.equals(text)) return;
        title = t;
        text = x;
        percent = pct;
        NotificationManager nm = (NotificationManager) c.getSystemService(NOTIFICATION_SERVICE);
        if (nm != null && allowed(c)) nm.notify(ID, build(c));
    }

    /** The build ended while the player was elsewhere. */
    static void finished(Context c, boolean ok, String msg) {
        NotificationManager nm = (NotificationManager) c.getSystemService(NOTIFICATION_SERVICE);
        if (nm == null || !allowed(c)) return;
        channels(nm);
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(c, CHANNEL_DONE) : new Notification.Builder(c);
        b.setSmallIcon(c.getResources().getIdentifier("ic_stat_crystal", "drawable", c.getPackageName())).setContentTitle(ok ? "The game is ready" : "The setup stopped").setContentText(msg)
                .setStyle(new Notification.BigTextStyle().bigText(msg)).setContentIntent(open(c)).setAutoCancel(true);
        nm.notify(ID_DONE, b.build());
    }

    static void clearFinished(Context c) {
        NotificationManager nm = (NotificationManager) c.getSystemService(NOTIFICATION_SERVICE);
        if (nm != null) nm.cancel(ID_DONE);
    }

    private static boolean allowed(Context c) {
        return Build.VERSION.SDK_INT < 33 || c.checkSelfPermission("android.permission.POST_NOTIFICATIONS") == PackageManager.PERMISSION_GRANTED;
    }

    private static void channels(NotificationManager nm) {
        if (Build.VERSION.SDK_INT < 26) return;
        NotificationChannel ch = new NotificationChannel(CHANNEL, "Build progress", NotificationManager.IMPORTANCE_LOW);
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL_DONE, "Build finished", NotificationManager.IMPORTANCE_DEFAULT));
    }

    private static PendingIntent open(Context c) {
        Intent i = new Intent(c, SetupActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(c, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private static Notification build(Context c) {
        NotificationManager nm = (NotificationManager) c.getSystemService(NOTIFICATION_SERVICE);
        if (nm != null) channels(nm);
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(c, CHANNEL) : new Notification.Builder(c);
        b.setSmallIcon(c.getResources().getIdentifier("ic_stat_crystal", "drawable", c.getPackageName())).setContentTitle(title).setContentText(text).setOngoing(true).setOnlyAlertOnce(true)
                .setContentIntent(open(c)).setProgress(100, Math.max(0, percent), percent < 0);
        if (Build.VERSION.SDK_INT >= 31) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        return b.build();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            if (Build.VERSION.SDK_INT >= 29) startForeground(ID, build(this), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            else startForeground(ID, build(this));
        } catch (Exception e) {
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        release();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent i) {
        return null;
    }
}
