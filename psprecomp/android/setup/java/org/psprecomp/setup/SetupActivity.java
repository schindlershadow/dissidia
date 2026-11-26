package org.psprecomp.setup;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.media.MediaScannerConnection;
import android.database.Cursor;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.PowerManager;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.animation.DecelerateInterpolator;
import android.view.animation.OvershootInterpolator;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The Android setup app: builds Dissidia 012 on the phone from the player's own disc image and installs it. Native Android pages (no SDL):
 * the flow is the one of the desktop wizard's Android build (setup/setup_main.cpp), the work is done by the C++ engine (Engine,
 * setup/android_engine.cpp). Works with touch, a keyboard or a game controller (d-pad / stick to move, A to choose, B to go back), in
 * portrait and landscape.
 */
public class SetupActivity extends Activity {
    // ---- request codes
    private static final int REQ_PICK = 4711, REQ_SAVE = 4712, REQ_NOTIFY = 4713;
    private static final String ACTION_INSTALLED = "org.psprecomp.setup.INSTALL_RESULT";
    private static final String MANIFEST_URL = "https://github.com/schindlershadow/dissidia/releases/latest/download/manifest.json";
    private static final int PICK_ISO = 0, PICK_DISC = 1;   // PICK_DISC: the second disc of an extra (pickExtra)

    // ---- shell views
    private FrameLayout root, stage, overlay;
    private BackdropView backdrop;
    private LinearLayout topBar;
    private TextView topTitle;
    private View backButton;
    private HintBar hints;
    private Page page;
    private View pageView;
    private Runnable rebuildPage;   // builds the current page again (rotation)
    private final Handler ui = new Handler(Looper.getMainLooper());
    private boolean resumed;

    // ---- state (install.ini, the choices of this run)
    private final Map<String, String> st = new LinkedHashMap<>();
    private Options opt = new Options();
    private String stateFile, workdir, iso = "", isoName = "", isoRef = "", pickedUriPath = "";
    private Runnable earlyAllowNext;   // continues the install once "install unknown apps" is allowed (asked before the long build)
    private boolean prepared;
    private String problem = "";
    private boolean extrasMode;
    private int pickKind = PICK_ISO;
    private Runnable beforePick;

    // the last plan request (Try again runs it again)
    private String runKind = "install", runOpts = "";
    private String runPrev;          // Change extras: the installed options the plan was made against
    private JSONObject pickExtra;    // PICK_DISC: the extra (from Engine.extrasJson) whose disc is picked
    private JSONArray extras = new JSONArray();   // the extras (setup/extras.cpp): the Options page is made from them

    // install / update
    private static volatile String installStatus = "";
    private BroadcastReceiver installReceiver;
    private boolean pendingInstall;
    private static volatile long installStartedAt;
    // Android's install confirmation, received while this app was not on screen: Android 10+ does not let an app start it from the
    // background (it is silently dropped and the page waits forever), so it is kept and shown when the app is back (onResume).
    private Intent pendingConfirm;
    // The last confirmation Android asked for. Started while this app was on screen it can still end up hidden: it opens in its own task,
    // so a player who leaves and comes back through the app icon sees "Waiting for Android" with the dialog behind it. Coming back from
    // the background (onStop, not just the dialog covering this page) while it is still unanswered opens it again.
    private Intent lastConfirm;
    private boolean wasStopped;
    private static final long INSTALL_HINT_MS = 30000;   // no answer from Android after this: the waiting page explains what to do
    private String pendingInstallApk;
    private JSONObject upd;
    private volatile int checkSeq = 0;
    private Downloader updDl;
    private String saveSource;   // the file the "save as" picker writes

    // ------------------------------------------------------------------------------------------------------------- lifecycle

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        CrashReports.install(this);
        Theme.init(this);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        Window w = getWindow();
        w.setStatusBarColor(0);
        w.setNavigationBarColor(0);
        w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        if (Build.VERSION.SDK_INT >= 28) w.getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;

        root = new FrameLayout(this);
        backdrop = new BackdropView(this);
        root.addView(backdrop, new FrameLayout.LayoutParams(-1, -1));
        LinearLayout shell = new LinearLayout(this);
        shell.setOrientation(LinearLayout.VERTICAL);
        topBar = buildTopBar();
        shell.addView(topBar, new LinearLayout.LayoutParams(-1, Theme.dp(56)));
        stage = new FrameLayout(this);
        shell.addView(stage, new LinearLayout.LayoutParams(-1, 0, 1));
        root.addView(shell, new FrameLayout.LayoutParams(-1, -1));
        overlay = new FrameLayout(this);
        root.addView(overlay, new FrameLayout.LayoutParams(-1, -1));
        root.setOnApplyWindowInsetsListener((v, ins) -> {
            shell.setPadding(ins.getSystemWindowInsetLeft(), ins.getSystemWindowInsetTop(), ins.getSystemWindowInsetRight(), ins.getSystemWindowInsetBottom());
            return ins;
        });
        setContentView(root);

        BuildRunner.marker = new File(getFilesDir(), "build.running");
        if (BuildRunner.current == null && BuildRunner.marker.isFile()) {   // a plan was running when the app last ended and it never finished: killed or crashed
            String mk = readText(BuildRunner.marker, 1 << 16);
            int nl = mk.indexOf('\n');
            interruptedKind = (nl < 0 ? mk : mk.substring(0, nl)).trim();
            interruptedOpts = nl < 0 ? "" : mk.substring(nl + 1);
            if (interruptedKind.isEmpty()) interruptedKind = "install";
            BuildRunner.marker.delete();
        }
        stateFile = new File(getFilesDir(), "install.ini").getPath();   // where the SDL wizard kept it (its pref path): old installs carry over
        workdir = new File(getFilesDir(), "work").getPath();
        UpdateCheckService.schedule(this);
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission("android.permission.POST_NOTIFICATIONS") != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] {"android.permission.POST_NOTIFICATIONS"}, REQ_NOTIFY);   // build progress + update notifications

        if (BuildRunner.current != null && prepared()) {   // the activity came back while a build runs (or just finished)
            loadState();
            showBuild();
        } else showBootstrap();
    }

    private static volatile boolean sPrepared;

    private boolean prepared() {
        return sPrepared;
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        BuildService.clearFinished(this);
        if (page == null) return;
        if (page.id.equals("permission") && hasAllFilesAccess()) {   // came back from Android's settings with access switched on
            refreshPage();
            ui.postDelayed(() -> { if (page != null && page.id.equals("permission") && hasAllFilesAccess()) afterPermission(); }, 650);
        }
        resumedAt = System.currentTimeMillis();
        hideGameFromGallery(false, false);
        if (pendingConfirm != null && page.id.equals("installing")) {
            Intent c = pendingConfirm;
            pendingConfirm = null;
            showConfirm(c);
        } else if (wasStopped && lastConfirm != null && page.id.equals("installing") && installStatus.equals("confirm"))
            showConfirm(lastConfirm);   // still unanswered after coming back: it is hidden in its own task, show it again
        wasStopped = false;
        if (pendingInstall && canInstallPackages()) {
            pendingInstall = false;
            doInstall(pendingInstallApk, installUpdate);
        } else if (page.id.equals("install-allow") && canInstallPackages()) refreshPage();
        if (earlyAllowNext != null && page.id.equals("install-allow-early") && canInstallPackages()) {   // allowed before the build: go on with it
            Runnable r = earlyAllowNext;
            earlyAllowNext = null;
            r.run();
        }
    }

    @Override
    protected void onStop() {
        super.onStop();
        wasStopped = true;
    }

    @Override
    protected void onPause() {
        super.onPause();
        resumed = false;
    }

    @Override
    public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        // portrait <-> landscape (and window size changes): the same page, laid out again
        ui.post(() -> { if (rebuildPage != null) present(rebuildPage, 0, false); });
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (installReceiver != null) try { unregisterReceiver(installReceiver); } catch (Exception e) { /* gone */ }
    }

    // ------------------------------------------------------------------------------------------------------------- shell

    private LinearLayout buildTopBar() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(Theme.dp(8), 0, Theme.dp(18), 0);
        FrameLayout bb = new FrameLayout(this) {
            final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
            @Override
            protected void dispatchDraw(Canvas c) {
                if (isFocused() || isPressed()) {
                    p.setColor(isPressed() ? 0x40E2C47A : 0x28E2C47A);
                    c.drawCircle(getWidth() / 2f, getHeight() / 2f, Math.min(getWidth(), getHeight()) / 2f, p);
                }
                Icons.draw(c, Icons.BACK, getWidth() / 2f, getHeight() / 2f, Theme.dpf(22), Theme.GOLD_LIGHT, Theme.dpf(2));
            }
        };
        bb.setWillNotDraw(false);
        bb.setFocusable(true);
        bb.setClickable(true);
        bb.setContentDescription("Back");
        bb.setOnClickListener(v -> goBack());
        bb.setOnFocusChangeListener((v, f) -> v.invalidate());
        backButton = bb;
        bar.addView(bb, new LinearLayout.LayoutParams(Theme.dp(44), Theme.dp(44)));
        topTitle = Theme.single(Theme.text(this, "DISSIDIA 012  ·  RECOMP SETUP", 13, Theme.GOLD, Theme.title));
        topTitle.setLetterSpacing(0.14f);
        LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(0, -2, 1);
        tp.leftMargin = Theme.dp(6);
        bar.addView(topTitle, tp);
        // the version, or (while a controller is used) the controller hints in its place
        FrameLayout end = new FrameLayout(this);
        TextView ver = Theme.text(this, "v" + appVersion(), 12, Theme.TEXT_FAINT, Theme.body);
        end.addView(ver, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER_VERTICAL | Gravity.END));
        hints = new HintBar(this);
        hints.version = ver;
        end.addView(hints, new FrameLayout.LayoutParams(-2, Theme.dp(30), Gravity.CENTER_VERTICAL | Gravity.END));
        bar.addView(end, new LinearLayout.LayoutParams(-2, -1));
        return bar;
    }

    /** Shows a page. dir: +1 forward (slides in from the right), -1 back, 0 in place (a rotation / refresh). */
    private void present(Runnable builder, int dir, boolean animate) {
        rebuildPage = builder;
        pendingDir = animate ? dir : 0;
        builder.run();
    }

    private void show(Page p, int dir) {
        final boolean firstPage = page == null;
        final boolean samePage = page != null && page.id.equals(p.id);
        page = p;
        boolean landscape = root.getWidth() > 0 ? root.getWidth() > root.getHeight() * 1.05f
                : getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
        int width = root.getWidth() > 0 ? root.getWidth() : getResources().getDisplayMetrics().widthPixels;
        View v = p.build(landscape, width);
        final View old = pageView;
        pageView = v;
        stage.addView(v, new FrameLayout.LayoutParams(-1, -1));
        if (old != null) {
            old.setFocusable(false);
            if (old instanceof ViewGroup) ((ViewGroup) old).setDescendantFocusability(ViewGroup.FOCUS_BLOCK_DESCENDANTS);
            if (dir == 0 || samePage) stage.removeView(old);
            else old.animate().alpha(0).translationX(-dir * Theme.dp(48)).setDuration(200).setInterpolator(new DecelerateInterpolator())
                    .withEndAction(() -> stage.removeView(old)).start();
        }
        if (dir != 0 && !samePage) {
            v.setTranslationX(dir * Theme.dp(64));
            v.animate().translationX(0).setDuration(420).setInterpolator(new DecelerateInterpolator(2.2f)).start();
            p.animateIn(dir);
        } else if (firstPage) p.animateIn(1);
        boolean hasBack = p.back != null;
        backButton.animate().alpha(hasBack ? 1 : 0).setDuration(200).start();
        backButton.setFocusable(hasBack);
        backButton.setClickable(hasBack);
        hints.setBack(hasBack);
        backdrop.setCrystal(p.hero == null && p.custom == null);
        hints.setY(p.onY != null ? yLabel(p) : null);
        ui.post(() -> {
            View f = p.firstFocus();
            if (f != null && !v.isInTouchMode()) f.requestFocus();
        });
    }

    private String yLabel(Page p) {
        return p.id.equals("build") ? "Log size" : "Logs";
    }

    private void refreshPage() {
        if (rebuildPage != null) present(rebuildPage, 0, false);
    }

    private void go(Runnable builder) {
        present(builder, 1, true);
    }

    private void goBackTo(Runnable builder) {
        present(builder, -1, true);
    }

    private void goBack() {
        if (dismissDialog()) return;
        if (page != null && page.back != null) page.back.run();
    }

    @Override
    public void onBackPressed() {
        if (dismissDialog()) return;
        if (page != null && page.back != null) page.back.run();
        else moveTaskToBack(true);   // the start page: like home (a running build keeps going in the service)
    }

    // ---- input: a game controller's A chooses (as the d-pad centre), B goes back, Y / L / R are page shortcuts; the stick moves like the d-pad
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int src = e.getSource();
        boolean pad = (src & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (src & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD
                || (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
        if (pad || (src & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD) hints.reveal(pad);
        int k = e.getKeyCode();
        if (k == KeyEvent.KEYCODE_BUTTON_A || k == KeyEvent.KEYCODE_BUTTON_START) {
            if (getCurrentFocus() == null && page != null) {
                View f = dialogFocus != null ? dialogFocus : page.firstFocus();
                if (f != null) f.requestFocus();
                return true;
            }
            return super.dispatchKeyEvent(new KeyEvent(e.getDownTime(), e.getEventTime(), e.getAction(), KeyEvent.KEYCODE_DPAD_CENTER, e.getRepeatCount(),
                    e.getMetaState(), e.getDeviceId(), e.getScanCode(), e.getFlags(), e.getSource()));
        }
        if (k == KeyEvent.KEYCODE_BUTTON_B) {
            if (e.getAction() == KeyEvent.ACTION_UP) onBackPressed();
            return true;
        }
        if (k == KeyEvent.KEYCODE_BUTTON_Y) {
            if (e.getAction() == KeyEvent.ACTION_UP && page != null && page.onY != null && overlay.getChildCount() == 0) page.onY.run();
            return true;
        }
        if ((k == KeyEvent.KEYCODE_BUTTON_L1 || k == KeyEvent.KEYCODE_BUTTON_R1) && page != null && page.console != null) {
            if (e.getAction() == KeyEvent.ACTION_DOWN) page.console.page(k == KeyEvent.KEYCODE_BUTTON_L1 ? -1 : 1);
            return true;
        }
        if (e.getAction() == KeyEvent.ACTION_DOWN && getCurrentFocus() == null && page != null
                && (k == KeyEvent.KEYCODE_DPAD_DOWN || k == KeyEvent.KEYCODE_DPAD_UP || k == KeyEvent.KEYCODE_DPAD_LEFT || k == KeyEvent.KEYCODE_DPAD_RIGHT)) {
            View f = dialogFocus != null ? dialogFocus : page.firstFocus();
            if (f != null && f.requestFocus()) return true;
        }
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        if (e.getActionMasked() == MotionEvent.ACTION_DOWN) hints.reveal(false);
        return super.dispatchTouchEvent(e);
    }

    // ---- dialogs (an overlay in the app's look) and the snackbar
    private View dialogFocus;
    private Runnable dialogCancel;

    private static final class Btn {
        final String label;
        final int icon, style;
        final Runnable run;

        Btn(String label, int icon, int style, Runnable run) {
            this.label = label;
            this.icon = icon;
            this.style = style;
            this.run = run;
        }
    }

    private void dialog(String title, String message, int icon, Btn... buttons) {
        dismissDialog(false);
        final FrameLayout scrim = new FrameLayout(this);
        scrim.setBackgroundColor(0xB0020308);
        scrim.setClickable(true);
        scrim.setAlpha(0);
        LinearLayout panel = new LinearLayout(this) {
            final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
            final android.graphics.Path path = new android.graphics.Path();
            @Override
            protected void dispatchDraw(Canvas c) {
                Card.chamferPath(path, 1, 1, getWidth() - 1, getHeight() - 1, Theme.dp(16));
                p.setStyle(Paint.Style.FILL);
                p.setColor(0xFFFFFFFF);   // a shader is multiplied by the paint's alpha
                p.setShader(new android.graphics.LinearGradient(0, 0, 0, getHeight(), 0xF41C2648, 0xF40B1028, android.graphics.Shader.TileMode.CLAMP));
                c.drawPath(path, p);
                p.setShader(null);
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(Theme.dpf(1.3f));
                p.setColor(0xB0E2C47A);
                c.drawPath(path, p);
                super.dispatchDraw(c);
            }
        };
        panel.setWillNotDraw(false);
        panel.setOrientation(LinearLayout.VERTICAL);
        int pad = Theme.dp(22);
        panel.setPadding(pad, pad, pad, Theme.dp(12));
        if (icon != Icons.NONE) {
            Icons.IconView iv = new Icons.IconView(this, icon, icon == Icons.WARN ? Theme.DANGER : Theme.GOLD);
            panel.addView(iv, new LinearLayout.LayoutParams(Theme.dp(34), Theme.dp(34)));
        }
        TextView t = Theme.heading(this, title, 21);
        LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(-2, -2);
        tp.topMargin = Theme.dp(8);
        panel.addView(t, tp);
        TextView m = Theme.body(this, message);
        m.setTextColor(Theme.TEXT);
        LinearLayout.LayoutParams mp = new LinearLayout.LayoutParams(-1, -2);
        mp.topMargin = Theme.dp(10);
        mp.bottomMargin = Theme.dp(16);
        android.widget.ScrollView ms = new android.widget.ScrollView(this);
        ms.addView(m);
        panel.addView(ms, mp);
        dialogFocus = null;
        dialogCancel = null;
        for (Btn b : buttons) {
            Card k = new Card(this, Card.ACTION, b.icon, b.label, null).style(b.style == Card.STYLE_NORMAL ? Card.STYLE_QUIET : b.style);
            k.onClick(v -> {
                dismissDialog(false);
                if (b.run != null) b.run.run();
            });
            LinearLayout.LayoutParams kp = new LinearLayout.LayoutParams(-1, -2);
            kp.bottomMargin = Theme.dp(8);
            panel.addView(k, kp);
            if (dialogFocus == null) dialogFocus = k;
        }
        dialogCancel = buttons.length > 0 ? buttons[buttons.length - 1].run : null;
        int maxW = Math.min(root.getWidth() - Theme.dp(32), Theme.dp(480));
        FrameLayout.LayoutParams pp = new FrameLayout.LayoutParams(maxW > 0 ? maxW : -1, -2, Gravity.CENTER);
        pp.topMargin = pp.bottomMargin = Theme.dp(24);
        scrim.addView(panel, pp);
        overlay.addView(scrim, new FrameLayout.LayoutParams(-1, -1));
        stage.setDescendantFocusability(ViewGroup.FOCUS_BLOCK_DESCENDANTS);
        topBar.setDescendantFocusability(ViewGroup.FOCUS_BLOCK_DESCENDANTS);
        scrim.animate().alpha(1).setDuration(180).start();
        panel.setScaleX(0.9f);
        panel.setScaleY(0.9f);
        panel.animate().scaleX(1).scaleY(1).setDuration(320).setInterpolator(new OvershootInterpolator(1.4f)).start();
        final View f = dialogFocus;
        ui.post(() -> { if (f != null && !f.isInTouchMode()) f.requestFocus(); });
    }

    private boolean dismissDialog() {
        return dismissDialog(true);
    }

    /** Closes the dialog; back / B (asCancel) also does what its last button does. True if there was one. */
    private boolean dismissDialog(boolean asCancel) {
        if (overlay.getChildCount() == 0) return false;
        for (int i = overlay.getChildCount() - 1; i >= 0; i--) {
            View s = overlay.getChildAt(i);
            if (s.getTag() != null) continue;   // the snackbar
            overlay.removeView(s);
        }
        stage.setDescendantFocusability(ViewGroup.FOCUS_AFTER_DESCENDANTS);
        topBar.setDescendantFocusability(ViewGroup.FOCUS_AFTER_DESCENDANTS);
        boolean had = dialogFocus != null;
        dialogFocus = null;
        Runnable c = dialogCancel;
        dialogCancel = null;
        if (had && asCancel && c != null) ui.post(c);
        if (page != null && !root.isInTouchMode()) {
            View f = page.firstFocus();
            if (f != null) f.requestFocus();
        }
        return had;
    }

    private void snack(String text) {
        TextView t = Theme.text(this, text, 14, Theme.TEXT, Theme.body);
        t.setTag("snack");
        t.setPadding(Theme.dp(18), Theme.dp(12), Theme.dp(18), Theme.dp(12));
        android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
        bg.setColor(0xF0202B52);
        bg.setStroke(Theme.dp(1), 0x90E2C47A);
        bg.setCornerRadius(Theme.dp(22));
        t.setBackground(bg);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        lp.bottomMargin = Theme.dp(56);
        lp.leftMargin = lp.rightMargin = Theme.dp(24);
        overlay.addView(t, lp);
        t.setAlpha(0);
        t.setTranslationY(Theme.dp(20));
        t.animate().alpha(1).translationY(0).setDuration(260).start();
        ui.postDelayed(() -> t.animate().alpha(0).translationY(Theme.dp(10)).setDuration(260).withEndAction(() -> overlay.removeView(t)).start(), 2800);
    }

    // ------------------------------------------------------------------------------------------------------------- start

    private void showBootstrap() {
        present(() -> {
            Page p = new Page(this, "bootstrap");
            ProgressEmblem em = new ProgressEmblem(this);
            p.hero = em;
            p.heroHeightDp = 210;
            p.head("First start", "Preparing", "Unpacking the build tools that come with this app (the compiler, Python and the game's tools). This happens once after each install or update of the app.");
            TextView pct = Theme.text(this, "", 14, Theme.GOLD_LIGHT, Theme.bodyBold);
            p.headerExtra.add(pct);
            show(p, 0);
            if (sPrepared) {
                em.setFraction(1);
                afterBootstrap();
                return;
            }
            final String lib = getApplicationInfo().nativeLibraryDir, files = getFilesDir().getPath();
            new Thread(() -> {
                final long[] last = {0};
                String err;
                try {
                    err = Engine.bootstrap(lib, files, f -> {
                        long now = System.currentTimeMillis();
                        if (now - last[0] < 50) return;
                        last[0] = now;
                        ui.post(() -> { em.setFraction((float) f); });
                    });
                } catch (Throwable t) {
                    err = t.toString();
                }
                final String e = err;
                ui.post(() -> {
                    if (!e.isEmpty()) problem = "Could not prepare the app: " + e;
                    else sPrepared = true;
                    em.setFraction(1);
                    ui.postDelayed(this::afterBootstrap, 250);
                });
            }, "setup-unpack").start();
        }, 0, false);
    }

    private void afterBootstrap() {
        if (problem.isEmpty()) problem = Engine.layoutProblem();
        loadState();
        prepared = true;
        loadExtras();
        go(this::showHome);
        if (problem.isEmpty() && opt.checkUpdates()) beginCheck(false);   // silent; an update shows its page only from the start page
        ui.postDelayed(this::offerCrashReport, 900);
    }

    /** The extras list of the engine (setup/extras.cpp): the same list the desktop wizard uses, so a new extra shows here by itself. */
    private void loadExtras() {
        try {
            extras = new JSONArray(Engine.extrasJson());
        } catch (Exception e) {
            extras = new JSONArray();
        }
        Options.defaults.clear();
        Options.discKeys.clear();
        Options.actionKeys.clear();
        for (int i = 0; i < extras.length(); i++) {
            JSONObject x = extras.optJSONObject(i);
            Options.defaults.put(x.optString("key"), x.optString("def"));
            JSONObject d = x.optJSONObject("disc");
            if (d != null) Options.discKeys.add(d.optString("key"));
            if (x.optBoolean("action")) Options.actionKeys.add(x.optString("key"));
        }
    }

    private JSONObject extra(String key) {
        for (int i = 0; i < extras.length(); i++)
            if (extras.optJSONObject(i).optString("key").equals(key)) return extras.optJSONObject(i);
        return null;
    }

    private static int iconOf(String name) {
        switch (name) {
        case "texture": return Icons.TEXTURE;
        case "voice": return Icons.VOICE;
        case "sword": return Icons.SWORD;
        case "sliders": return Icons.SLIDERS;
        case "disc": return Icons.DISC;
        case "download": return Icons.DOWNLOAD;
        case "play": return Icons.PLAY;
        default: return Icons.CRYSTAL;
        }
    }

    private void loadState() {
        st.clear();
        String text = Engine.loadState(stateFile);
        for (String l : text.split("\n")) {
            int eq = l.indexOf('=');
            if (eq > 0) st.put(l.substring(0, eq), l.substring(eq + 1));
        }
        opt = Options.from(st);
        iso = "";   // a descriptor number ("fd:N") means nothing after a restart: the picker is asked again
    }

    private void saveState() {
        StringBuilder b = new StringBuilder();
        for (Map.Entry<String, String> e : st.entrySet()) b.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        Engine.saveState(stateFile, b.toString());
    }

    private String stWorkdir() {
        String w = st.get("workdir");
        return w == null || w.isEmpty() ? workdir : w;
    }

    private boolean installed() {
        String w = st.get("workdir");
        return w != null && !w.isEmpty() && new File(w, "android/dissidia.apk").isFile();
    }

    /** "fast" (default on a phone) or "full": how the game is compiled here (games/dissidia/tools/android_build.py reads work/build_profile.txt). */
    private String buildProfile() {
        return "full".equals(readText(new File(getFilesDir(), "build_profile.txt"), 16).trim()) ? "full" : "fast";   // own file: the engine's install.ini only keeps keys it knows
    }

    private static final String[] PROFILE_LABELS = {"Fast", "Full"};
    private static final String PROFILE_HELP_FAST = "Fast: a shorter install; the parts of the game that run a lot are optimised fully, the rest for size. The game can be a little slower.";
    private static final String PROFILE_HELP_FULL = "Full: the longest install (about half as long again) and the fastest game. You can switch later with Rebuild.";

    private void setProfile(String p) {
        try (java.io.FileOutputStream o = new java.io.FileOutputStream(new File(getFilesDir(), "build_profile.txt"))) {
            o.write((p + "\n").getBytes("UTF-8"));
        } catch (Exception e) {
            // the choice is then not kept
        }
    }

    /** The build's choice goes to the work folder: the build script reads it (the object cache keys include the flags, so switching rebuilds only what differs). */
    private void writeProfile(String dir) {
        try {
            new File(dir).mkdirs();
            try (java.io.FileOutputStream o = new java.io.FileOutputStream(new File(dir, "build_profile.txt"))) {
                o.write((buildProfile() + "\n").getBytes("UTF-8"));
            }
        } catch (Exception e) {
            // the build then uses its default (fast)
        }
    }

    private boolean needRebuild() {
        if (!installed()) return false;
        String fp = Engine.payloadFingerprint(), built = st.get("built_fp");
        return !fp.equals(built == null ? "" : built);
    }

    // ------------------------------------------------------------------------------------------------------------- home

    private void showHome() {
        Page p = new Page(this, "home");
        backdrop.setMood(BackdropView.MOOD_CRYSTAL);
        backdrop.setEnergy(0);
        ProgressEmblem em = new ProgressEmblem(this);
        em.setShowPercent(false);
        p.hero = em;
        p.heroHeightDp = 190;
        p.onY = () -> go(this::showLogs);
        if (!problem.isEmpty()) {
            em.setMode(ProgressEmblem.FAILED);
            backdrop.setMood(BackdropView.MOOD_CHAOS);
            p.head("Setup", "Something is missing", problem);
            p.card(new Card(this, Card.ACTION, Icons.BUG, "Report an issue", "Tell the developer about this problem.")).onClick(v -> beginReport(this::backHome, problem, "", ""));
            p.card(new Card(this, Card.ACTION, Icons.LOG, "Logs and reports", "Look at, save or share what the app wrote down.")).onClick(v -> go(this::showLogs));
            p.card(new Card(this, Card.ACTION, Icons.QUIT, "Close", null).style(Card.STYLE_QUIET)).onClick(v -> finishAndRemoveTask());
        } else if (installed()) {
            boolean upd = needRebuild();
            em.setMode(ProgressEmblem.DONE);
            em.setFraction(1);
            p.head(upd ? "Update ready" : "Ready", "Dissidia 012", upd
                    ? "This app was updated since the game was built. Update the game to rebuild it from the files already on this device: no disc needed, about 5 minutes."
                    : "The game is built on this device. Install it as an app, change its extras, or build it again from your disc.");
            if (upd)
                p.card(new Card(this, Card.ACTION, Icons.REFRESH, "Update the game", "Builds the game again from the files already on this device, then installs it.").style(Card.STYLE_PRIMARY).pulse())
                        .onClick(v -> { workdir = stWorkdir(); startPlan("rebuild", "", Options.from(st), null); });
            Card ins = p.card(new Card(this, Card.ACTION, Icons.INSTALL, "Install the game", "Installs the built game as an app. Android asks you to confirm."));
            if (!upd) ins.style(Card.STYLE_PRIMARY);
            ins.onClick(v -> installGame(new File(stWorkdir(), "android/dissidia.apk").getPath(), false));
            saveApkCard(p);
            final boolean isFull = "full".equals(buildProfile());
            p.card(new Card(this, Card.ACTION, Icons.REFRESH, isFull ? "Rebuild game (Fast)" : "Rebuild game (Full)", isFull
                    ? "Builds again with the shorter install; the game can be a little slower. Now: Full. No disc needed."
                    : "Builds again with full optimisation: the longest install, the fastest game. Now: Fast. No disc needed. Only part of the game is compiled again."))
                    .onClick(v -> { setProfile(isFull ? "fast" : "full"); workdir = stWorkdir(); startPlan("rebuild", "", Options.from(st), null); });
            p.card(new Card(this, Card.ACTION, Icons.SLIDERS, "Change extras", "HD textures, texture pack, Japanese voices or Bartz's Dissidia 1 moves, without building again.")).onClick(v -> beginExtras());
            p.card(new Card(this, Card.ACTION, Icons.DISC, "Build it again", "Pick your game file and build from scratch. Your saves are kept.")).onClick(v -> beginPick(PICK_ISO, this::backHome));
            p.card(new Card(this, Card.ACTION, Icons.DOWNLOAD, "Check for updates", "Looks for a newer version of this app on github.com. Nothing about you or your game is sent.")).onClick(v -> beginCheck(true));
            p.card(new Card(this, Card.ACTION, Icons.BUG, "Report an issue", "Something wrong? Tell the developer: you see what is sent first.").style(Card.STYLE_QUIET)).onClick(v -> beginReport(this::backHome, "", "", ""));
            p.card(new Card(this, Card.ACTION, Icons.LOG, "Logs and reports", "The build log, the game's last run and crash reports: view, save or share them.").style(Card.STYLE_QUIET)).onClick(v -> go(this::showLogs));
        } else {
            em.setMode(ProgressEmblem.SPIN);
            p.head("Welcome", "Dissidia 012", "Builds the game on this device from your own copy of Dissidia 012 (EU or US, ISO or CSO) and installs it. Nothing from the game is downloaded or shipped with this app.");
            p.card(new Card(this, Card.ACTION, Icons.BUILD, "Build the game", "About 10 to 20 minutes. You can use your phone meanwhile; the build keeps going.").style(Card.STYLE_PRIMARY).pulse())
                    .onClick(v -> beginPick(PICK_ISO, this::backHome));
            p.card(new Card(this, Card.ACTION, Icons.DOWNLOAD, "Check for updates", "Looks for a newer version of this app on github.com. Nothing about you or your game is sent.")).onClick(v -> beginCheck(true));
            p.card(new Card(this, Card.ACTION, Icons.BUG, "Report an issue", "Something wrong? Tell the developer: you see what is sent first.").style(Card.STYLE_QUIET)).onClick(v -> beginReport(this::backHome, "", "", ""));
            p.card(new Card(this, Card.ACTION, Icons.LOG, "Logs and reports", "The build log, the game's last run and crash reports.").style(Card.STYLE_QUIET)).onClick(v -> go(this::showLogs));
        }
        show(p, dirOf(p));
    }

    private int pendingDir = 1;

    private int dirOf(Page p) {
        int d = pendingDir;
        pendingDir = 1;
        return d;
    }

    private void backHome() {
        extrasMode = false;
        pendingDir = -1;
        present(this::showHome, -1, true);
    }

    // ------------------------------------------------------------------------------------------------------------- picking the disc images

    private void beginPick(int kind, Runnable back) {
        pickKind = kind;
        beforePick = back;
        go(this::showPickInfo);
    }

    private void showPickInfo() {
        Page p = new Page(this, "pick-info-" + pickKind);
        p.back = () -> { pendingDir = -1; present(beforePick, -1, true); };
        backdrop.setMood(BackdropView.MOOD_CRYSTAL);
        String how = "Android's file picker opens next: browse to the file (SD card, Downloads...) and tap it. The app only reads it; nothing is uploaded.";
        JSONObject disc = pickKind == PICK_DISC && pickExtra != null ? pickExtra.optJSONObject("disc") : null;
        if (disc != null) {   // an extra's second disc: its texts come with the extra (setup/extras.cpp)
            p.head(disc.optString("eyebrow"), disc.optString("title"), how);
            p.card(new Card(this, Card.INFO, iconOf(pickExtra.optString("icon")), disc.optString("what") + " (" + disc.optString("exts") + ")", disc.optString("what_help")));
            p.card(new Card(this, Card.INFO, Icons.CHECK, disc.optString("valid"), disc.optString("valid_help")));
        } else {
            p.head("Step 1", "Your game file", how);
            p.card(new Card(this, Card.INFO, Icons.DISC, "A Dissidia 012 disc image", "Your own copy of the PSP game Dissidia 012 [duodecim] Final Fantasy as a .iso or .cso (compressed ISO) of the whole disc."));
            p.card(new Card(this, Card.INFO, Icons.CHECK, "Valid: EU or US release", "The European release (ULES-01505, also the PSN version) or the US release (ULUS-10566). Not an archive: unpack .zip / .7z first."));
        }
        Card choose = p.card(new Card(this, Card.ACTION, Icons.FOLDER, "Choose the file", "Opens Android's file picker.").style(Card.STYLE_PRIMARY));
        choose.onClick(v -> launchPicker());
        p.focus = choose;
        show(p, dirOf(p));
    }

    private void launchPicker() {
        try {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            startActivityForResult(i, REQ_PICK);
        } catch (Exception e) {
            dialog("No file picker", "Android's file picker could not be opened (" + e.getMessage() + "). Use the simple browser instead.", Icons.WARN,
                    new Btn("Use the simple browser", Icons.FOLDER, Card.STYLE_PRIMARY, this::openBrowser), new Btn("Close", Icons.CLOSE, Card.STYLE_NORMAL, null));
            return;
        }
        if (page == null || !page.id.equals("waiting")) go(this::showWaiting);
    }

    private void showWaiting() {
        Page p = new Page(this, "waiting");
        p.back = () -> { pendingDir = -1; present(beforePick, -1, true); };
        ProgressEmblem em = new ProgressEmblem(this);
        em.setMode(ProgressEmblem.SPIN);
        em.setShowPercent(false);
        p.hero = em;
        p.heroHeightDp = 150;
        JSONObject disc = pickKind == PICK_DISC && pickExtra != null ? pickExtra.optJSONObject("disc") : null;
        String what = disc != null ? "the file for " + pickExtra.optString("title") + " (" + disc.optString("exts") + ")" : "your Dissidia 012 disc image (.iso or .cso)";
        p.head("Choose a file", "Waiting for the picker", "Pick " + what + " in Android's file picker. If it closed without a file, open it again or browse inside this app.");
        p.card(new Card(this, Card.ACTION, Icons.FOLDER, "Open the file picker", "From the SD card, Download or anywhere else.").style(Card.STYLE_PRIMARY)).onClick(v -> launchPicker());
        p.card(new Card(this, Card.ACTION, Icons.STORAGE, "Use the simple browser instead", "Browse the storage inside this app. Needs the \"all files\" permission.")).onClick(v -> {
            if (hasAllFilesAccess()) openBrowser(); else go(this::showPermission);
        });
        show(p, dirOf(p));
    }

    private boolean checkedDisc;   // picked(): the disc check passed (second call)

    private static void closeFd(String path) {
        if (!path.startsWith("fd:")) return;
        try {
            ParcelFileDescriptor.adoptFd(Integer.parseInt(path.substring(3))).close();
        } catch (Exception e) {
            // already closed
        }
    }

    private void picked(String path, String name) {
        if (pickKind == PICK_DISC) {
            final JSONObject disc = pickExtra == null ? null : pickExtra.optJSONObject("disc");
            if (disc != null && !checkedDisc) {   // the right game, before anything is installed with it (setup/extras.cpp disc check)
                final String key = pickExtra.optString("key");
                new Thread(() -> {
                    final String why = Engine.discProblem(key, path);
                    ui.post(() -> {
                        if (why.isEmpty()) {
                            checkedDisc = true;
                            picked(path, name);
                            checkedDisc = false;
                            return;
                        }
                        opt.set(disc.optString("key"), "");
                        closeFd(path);   // the picker's descriptor of the refused file
                        dialog(isDiscFile(disc) ? "Wrong disc" : "Wrong file", why, Icons.WARN,
                                new Btn("Choose another file", Icons.FOLDER, Card.STYLE_PRIMARY, () -> go(this::showPickInfo)),
                                new Btn("Back", Icons.BACK, Card.STYLE_NORMAL, () -> present(this::showOptionsPage, -1, true)));
                    });
                }, "setup-disc-check").start();
                return;
            }
            if (disc != null) {
                opt.set(disc.optString("key"), path);
                if (disc.optString("key").equals("texture_file")) opt.set("texture_file_name", name);   // the pack is named after the file
            }
            present(this::showOptionsPage, -1, true);
            startInstallOrAsk();
            return;
        }
        iso = path;
        isoName = name;
        isoRef = path.startsWith("fd:") ? pickedUriPath : path;   // a real path when Android's picker gave one (primary / SD card storage)
        new File(workdir).mkdirs();
        if (hasAllFilesAccess()) go(this::showOptionsPage); else go(this::showPermission);
    }

    // ---- the simple browser (fallback; needs all-files access)
    private File browseDir;

    /** The file endings the pick lists: an extra's own (its disc "patterns", e.g. .zip for a texture pack), else a disc image. */
    private List<String> pickPatterns() {
        JSONObject disc = pickKind == PICK_DISC && pickExtra != null ? pickExtra.optJSONObject("disc") : null;
        JSONArray a = disc == null ? null : disc.optJSONArray("patterns");
        List<String> out = new ArrayList<>();
        if (a != null) for (int i = 0; i < a.length(); i++) out.add(a.optString(i));
        if (out.isEmpty()) out.addAll(Arrays.asList(".iso", ".cso"));
        return out;
    }

    private boolean listed(String name) {
        String l = name.toLowerCase(Locale.US);
        for (String e : pickPatterns()) if (l.endsWith(e)) return true;
        return false;
    }

    /** An extra's file that is a disc image (undub, Dissidia 1) rather than another kind of file (a texture pack archive). */
    private static boolean isDiscFile(JSONObject disc) {
        JSONArray a = disc == null ? null : disc.optJSONArray("patterns");
        return a == null || a.length() == 0 || ".iso".equals(a.optString(0));
    }

    private void openBrowser() {
        File start = new File("/storage/emulated/0");
        browseDir = start;
        go(this::showBrowser);
    }

    private void showBrowser() {
        Page p = new Page(this, "browser:" + browseDir.getPath());
        p.back = () -> { pendingDir = -1; present(beforePick, -1, true); };
        String shown = browseDir.getPath().startsWith("/storage/emulated/0") ? "Internal storage" + browseDir.getPath().substring(19) : browseDir.getPath();
        p.head("Simple browser", browseDir.getName().isEmpty() || browseDir.getPath().equals("/storage") ? "Storage" : browseDir.getName(), shown);
        File parent = browseDir.getPath().equals("/storage/emulated/0") ? new File("/storage") : browseDir.getParentFile();
        if (parent != null && !browseDir.getPath().equals("/storage")) {
            final File up = parent;
            p.card(new Card(this, Card.ACTION, Icons.UP, "Up one folder", null).style(Card.STYLE_QUIET)).onClick(v -> { browseDir = up; pendingDir = -1; present(this::showBrowser, -1, true); });
        }
        File[] list = browseDir.listFiles();
        ArrayList<File> dirs = new ArrayList<>(), files = new ArrayList<>();
        if (list != null)
            for (File f : list) {
                String n = f.getName();
                if (n.startsWith(".")) continue;
                if (browseDir.getPath().equals("/storage")) {
                    if (n.equals("self")) continue;
                    dirs.add(n.equals("emulated") ? new File("/storage/emulated/0") : f);
                } else if (f.isDirectory()) dirs.add(f);
                else if (listed(n)) files.add(f);
            }
        java.util.Comparator<File> byName = (a, b) -> a.getName().compareToIgnoreCase(b.getName());
        files.sort(byName);
        dirs.sort(byName);
        final int fileIcon = pickKind == PICK_DISC && pickExtra != null && !isDiscFile(pickExtra.optJSONObject("disc")) ? Icons.FILE : Icons.DISC;
        for (File f : files) p.card(new Card(this, Card.ACTION, fileIcon, f.getName(), BuildRunner.size(f.length())).style(Card.STYLE_PRIMARY)).onClick(v -> picked(f.getPath(), f.getName()));
        for (File d : dirs) {
            String label = browseDir.getPath().equals("/storage") ? (d.getPath().equals("/storage/emulated/0") ? "Internal storage" : "SD card " + d.getName()) : d.getName();
            p.card(new Card(this, Card.ACTION, d.getParent() != null && d.getParent().equals("/storage") || d.getPath().equals("/storage/emulated/0") ? Icons.STORAGE : Icons.FOLDER, label, null))
                    .onClick(v -> { browseDir = d; go(this::showBrowser); });
        }
        if (files.isEmpty() && dirs.isEmpty())
            p.card(new Card(this, Card.INFO, Icons.INFO, "No " + String.join(" or ", pickPatterns()) + " files here", "Go up, or open another folder."));
        show(p, dirOf(p));
    }

    // ------------------------------------------------------------------------------------------------------------- permission

    private boolean hasAllFilesAccess() {
        return Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }

    private void requestAllFilesAccess() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (Exception e2) {
                snack("Open Settings > Apps > Dissidia Recomp Setup > Permissions and allow access to all files.");
            }
        }
    }

    private void showPermission() {
        Page p = new Page(this, "permission");
        p.back = this::backHome;
        boolean ok = hasAllFilesAccess();
        Icons.IconView hero = new Icons.IconView(this, ok ? Icons.CHECK : Icons.KEY, ok ? Theme.CRYSTAL : Theme.GOLD);
        hero.strokeDp = 2.2f;
        p.hero = hero;
        p.heroHeightDp = 84;
        p.head("Permission", "Allow access to files", "The game reads its files from a shared folder (Android/media). To put them there, this app needs \"all files access\". Android opens a settings page: switch it on for this app, then come back.");
        Card allow = p.card(new Card(this, Card.ACTION, Icons.KEY, "Allow access to files", ok ? "Allowed. Thank you." : "Opens Android's settings page for this app.")
                .value(ok ? "Allowed" : "Needed").style(ok ? Card.STYLE_NORMAL : Card.STYLE_PRIMARY));
        allow.onClick(v -> { if (hasAllFilesAccess()) afterPermission(); else requestAllFilesAccess(); });
        Card cont = p.card(new Card(this, Card.ACTION, Icons.CHEVRON, "Continue", iso.isEmpty() ? "Pick your game file." : "On to the options."));
        if (ok) cont.style(Card.STYLE_PRIMARY);
        cont.onClick(v -> {
            if (hasAllFilesAccess()) afterPermission();
            else dialog("Allow access", "Access to files is not allowed yet. Choose \"Allow access to files\" and switch it on for this app.", Icons.KEY,
                    new Btn("Open the settings", Icons.KEY, Card.STYLE_PRIMARY, this::requestAllFilesAccess), new Btn("Not now", Icons.CLOSE, Card.STYLE_NORMAL, null));
        });
        p.focus = ok ? cont : allow;
        show(p, dirOf(p));
    }

    private void afterPermission() {
        if (!iso.isEmpty()) {
            new File(workdir).mkdirs();
            go(this::showOptionsPage);
        } else openBrowser();
    }

    // ------------------------------------------------------------------------------------------------------------- options

    private void showOptionsPage() {
        Page p = new Page(this, extrasMode ? "extras" : "options");
        p.back = this::backHome;
        backdrop.setMood(BackdropView.MOOD_CRYSTAL);
        if (extrasMode) p.head("Installed game", "Change extras", "Turn extras on or off without building the game again. Extras built from a second disc keep their files when turned off.");
        else p.head("Step 2", "Options", (isoName.isEmpty() ? "" : "Game file: " + isoName + ". ") + "Extras can be changed later without building again.");

        // one card per extra, in the list's order (setup/extras.cpp, shared with the desktop wizard); a sub-choice shows while its switch is on
        final Map<String, ArrayList<Card>> children = new LinkedHashMap<>();
        for (int i = 0; i < extras.length(); i++) {
            final JSONObject x = extras.optJSONObject(i);
            final String key = x.optString("key"), parent = x.optString("parent"), help = x.optString("help");
            final JSONArray ch = x.optJSONArray("choices");
            final JSONObject disc = x.optJSONObject("disc");
            final boolean available = x.optBoolean("available");
            Card k;
            if (ch == null || ch.length() == 0) {
                k = p.card(new Card(this, Card.TOGGLE, iconOf(x.optString("icon")), x.optString("title"), help).toggle(opt.on(key)));
                k.onChange(c -> {
                    opt.set(key, c.isOn() ? "1" : "0");
                    if (!c.isOn() && disc != null) opt.set(disc.optString("key"), "");   // its disc is picked again when turned on
                    ArrayList<Card> kids = children.get(key);
                    if (kids != null) for (Card kid : kids) kid.setVisibility(c.isOn() ? View.VISIBLE : View.GONE);
                });
            } else {
                final String[] labels = new String[ch.length()], values = new String[ch.length()], helps = new String[ch.length()];
                int sel = 0;
                for (int j = 0; j < ch.length(); j++) {
                    labels[j] = ch.optJSONObject(j).optString("label");
                    values[j] = ch.optJSONObject(j).optString("value");
                    helps[j] = ch.optJSONObject(j).optString("help");
                    if (values[j].equals(opt.get(key))) sel = j;
                }
                k = p.card(new Card(this, Card.CHOICE, iconOf(x.optString("icon")), x.optString("title"), (help + " " + helps[sel]).trim()).choices(labels, sel));
                k.onChange(c -> { opt.set(key, values[c.choice()]); c.summary((help + " " + helps[c.choice()]).trim()); });
            }
            if (!available) k.disabled("Not yet");
            if (!parent.isEmpty()) {
                ArrayList<Card> kids = children.get(parent);
                if (kids == null) children.put(parent, kids = new ArrayList<>());
                kids.add(k);
                k.setVisibility(opt.on(parent) && available ? View.VISIBLE : View.GONE);
            }
        }
        if (!extrasMode) {
            Card up = p.card(new Card(this, Card.TOGGLE, Icons.REFRESH, "Check for updates", "Look for a newer version of this app when it starts, and once a day in the background (a notification only).").toggle(opt.checkUpdates()));
            up.onChange(k -> opt.set("check_updates", k.isOn() ? "1" : "0"));
        }
        if (!extrasMode) {
            final int sel0 = "full".equals(buildProfile()) ? 1 : 0;
            Card bp = p.card(new Card(this, Card.CHOICE, Icons.BUILD, "Build", (sel0 == 0 ? PROFILE_HELP_FAST : PROFILE_HELP_FULL)).choices(PROFILE_LABELS, sel0));
            bp.onChange(c -> { setProfile(c.choice() == 1 ? "full" : "fast"); c.summary(c.choice() == 1 ? PROFILE_HELP_FULL : PROFILE_HELP_FAST); });
        }
        long free = Engine.freeSpace(workdir);
        String space = free >= 0 ? "Free space: " + BuildRunner.size(free) + (free < (6L << 30) && !extrasMode ? " (about 6 GB are needed!)" : "") + "." : "";
        Card start = p.card(new Card(this, Card.ACTION, extrasMode ? Icons.CHECK : Icons.BUILD, extrasMode ? "Apply changes" : "Start building",
                extrasMode ? "Adds the extras you turned on and removes those you turned off (where they can be removed)." : "Reads your game file and builds the game here. You can cancel and continue later. " + space)
                .style(Card.STYLE_PRIMARY));
        start.onClick(v -> startInstallOrAsk());
        show(p, dirOf(p));
    }

    private void beginExtras() {
        extrasMode = true;
        workdir = stWorkdir();
        opt = Options.from(st);
        opt.clearDiscs();   // a saved path (or Android descriptor) from the install is no longer valid
        go(this::showOptionsPage);
    }

    private void startInstallOrAsk() {
        String prev = extrasMode ? Options.from(st).text() : null;
        String[] need = Engine.extrasNeedingDisc(opt.text(), prev);   // extras built from a second disc (undub, Dissidia 1...) need it first
        // ... but look next to the first image first: a disc with the right id / voices in the same folder is used without asking
        final String ref = !isoRef.isEmpty() ? isoRef : st.get("iso_ref") == null ? "" : st.get("iso_ref");
        autoDiscNote = "";
        while (need.length > 0 && extra(need[0]) != null && !ref.isEmpty() && hasAllFilesAccess()) {
            JSONObject disc = extra(need[0]).optJSONObject("disc");
            String found = disc == null ? "" : Engine.findExtraDisc(need[0], ref);
            if (found.isEmpty()) break;
            opt.set(disc.optString("key"), found);
            autoDiscNote += (autoDiscNote.isEmpty() ? "" : " ") + "Used " + new File(found).getName() + " (same folder) for " + extra(need[0]).optString("title") + ".";
            need = Engine.extrasNeedingDisc(opt.text(), prev);
        }
        if (need.length > 0 && extra(need[0]) != null) {
            pickExtra = extra(need[0]);
            beginPick(PICK_DISC, this::showOptionsPage);
            return;
        }
        if (extrasMode) {   // Change extras: the engine removes what was turned off and adds what was turned on (no rebuild)
            startPlan("extras", "", opt, prev);
            return;
        }
        File w = new File(workdir);
        w.mkdirs();
        if (!w.isDirectory()) { failPage("Cannot create " + workdir + ".", ""); return; }
        File demo = new File(getFilesDir(), "ui_demo");   // test switch (run-as on a debuggable build): the build page without the tools
        if (demo.isFile()) {   // "fail" / "failmem" / "failiso" / "failspace" make a step fail, "lowspace" fakes a full drive, "noperm" skips the early permission page
            String dt = readText(demo, 64);
            String mode = dt.contains("failmem") ? "mem" : dt.contains("failiso") ? "iso" : dt.contains("failspace") ? "space" : dt.contains("fail") ? "compile" : "";
            startPlan(mode.isEmpty() ? "demo" : "demo-fail:" + mode, iso, opt, null);
            return;
        }
        startPlan("install", iso, opt, null);
    }

    // ------------------------------------------------------------------------------------------------------------- the build

    private static final int SKIP_SPACE = 1, SKIP_ALLOW = 2;

    private void startPlan(String kind, String isoPath, Options o, String prev) {
        startPlan(kind, isoPath, o, prev, resumeNext ? SKIP_SPACE | SKIP_ALLOW : 0);
    }

    /** Before the long part: is there room for the whole install (and its extras), and may this app install the APK at the end? Both are
     *  asked now, not after 20 minutes. True = a dialog / page took over; it calls startPlan again with the skip flag. */
    private boolean preflight(final String kind, final String isoPath, final Options o, final String prev, int skip) {
        final boolean demo = kind.startsWith("demo");
        final String demoText = demo ? readText(new File(getFilesDir(), "ui_demo"), 64) : "";
        final String wd = kind.equals("install") || demo ? workdir : stWorkdir();
        if ((skip & SKIP_SPACE) == 0 && (!demo || demoText.contains("lowspace"))) {
            long need = Engine.neededBytes(kind.equals("rebuild") ? "@none" : o.text(), wd, kind.equals("extras"));
            if (demoText.contains("lowspace")) need = Math.max(need, Engine.freeSpace(wd) + (3L << 30));
            final String why = Engine.spaceProblem(wd, need);
            if (!why.isEmpty()) {
                dialog("Not enough space", why, Icons.WARN,
                        new Btn("Cancel", Icons.CLOSE, Card.STYLE_PRIMARY, this::backHome),
                        new Btn("Try anyway", Icons.RETRY, Card.STYLE_NORMAL, () -> startPlan(kind, isoPath, o, prev, skip | SKIP_SPACE)));
                return true;
            }
        }
        if ((skip & SKIP_ALLOW) == 0 && (kind.equals("install") || kind.equals("rebuild") || demo) && !demoText.contains("noperm") && !canInstallPackages()) {
            earlyAllowNext = () -> startPlan(kind, isoPath, o, prev, skip | SKIP_SPACE | SKIP_ALLOW);
            go(this::showEarlyAllow);
            return true;
        }
        return false;
    }

    private void showEarlyAllow() {
        Page p = new Page(this, "install-allow-early");
        p.back = () -> { earlyAllowNext = null; backHome(); };
        boolean ok = canInstallPackages();
        Icons.IconView hero = new Icons.IconView(this, ok ? Icons.CHECK : Icons.KEY, ok ? Theme.CRYSTAL : Theme.GOLD);
        hero.strokeDp = 2.2f;
        p.hero = hero;
        p.heroHeightDp = 84;
        p.head("Before the build", "Allow installing now", "When the game is built, Android has to be allowed to install it from this app. Switch it on now, so the build can finish without waiting for you. "
                + "Android opens a settings page: switch on \"Allow from this source\", then come back: the build starts by itself.");
        p.card(new Card(this, Card.ACTION, Icons.KEY, "Open the setting", "Opens Android's settings page for this app.").value(ok ? "Allowed" : "Needed").style(Card.STYLE_PRIMARY)).onClick(v -> {
            if (canInstallPackages()) { Runnable r = earlyAllowNext; earlyAllowNext = null; if (r != null) r.run(); }
            else startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getPackageName())));
        });
        p.card(new Card(this, Card.ACTION, Icons.CHEVRON, "Build without it", "The build runs now; Android asks for the permission at the end, before the game is installed.").style(Card.STYLE_QUIET)).onClick(v -> {
            Runnable r = earlyAllowNext;
            earlyAllowNext = null;
            if (r != null) r.run();
        });
        show(p, dirOf(p));
    }

    private void startPlan(String kind, String isoPath, Options o, String prev, int skip) {
        if ((kind.equals("rebuild") || kind.equals("extras")) && !Engine.discPresent(stWorkdir())) {
            // the only copy of the disc files is in the game's folder: the player deleted it. They are made again from the game file.
            dialog("The game's files are gone", "The game's files are no longer on this device (the game's folder was deleted or emptied), and "
                    + (kind.equals("rebuild") ? "the game cannot be updated" : "extras cannot be added") + " without them. Pick your Dissidia 012 game file (.iso or .cso) again: "
                    + "it is read once and the game is built again. Your saves are kept.", Icons.WARN,
                    new Btn("Pick the game file", Icons.DISC, Card.STYLE_PRIMARY, () -> beginPick(PICK_ISO, this::backHome)),
                    new Btn("Cancel", Icons.CLOSE, Card.STYLE_NORMAL, this::backHome));
            return;
        }
        if (preflight(kind, isoPath, o, prev, skip)) return;
        if (!kind.startsWith("demo")) hideGameFromGallery(true, false);
        runKind = kind;
        runOpts = o.text();
        runPrev = prev;
        if (!kind.equals("install") && !kind.startsWith("demo")) workdir = stWorkdir();
        JSONArray plan;
        try {
            plan = new JSONArray(Engine.plan(kind, isoPath, workdir, runOpts, prev == null ? "" : prev));
        } catch (Exception e) {
            failPage("Could not plan the steps: " + e.getMessage(), "");
            return;
        }
        if (plan.length() == 0) {
            dialog("Extras", "Nothing needed to be added: the extras you chose are already in place.", Icons.CHECK, new Btn("OK", Icons.CHECK, Card.STYLE_PRIMARY, this::backHome));
            return;
        }
        if (kind.equals("install") || kind.equals("rebuild")) writeProfile(workdir);
        BuildRunner r = new BuildRunner(this, kind, plan, new File(workdir, "setup.log"), runOpts, resumeNext);
        resumeNext = false;
        r.start();
        keepAwake(true);
        BuildService.start(this);
        go(this::showBuild);
    }

    private boolean logBig;
    private String autoDiscNote = "";   // which extra discs were found next to the game file (said on the finished page)

    private void showBuild() {
        final BuildRunner r = BuildRunner.current;
        if (r == null) { backHome(); return; }
        Page p = new Page(this, "build");
        boolean landscape = root.getWidth() > 0 ? root.getWidth() > root.getHeight() * 1.05f
                : getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
        backdrop.setMood(BackdropView.MOOD_CRYSTAL);
        backdrop.setEnergy(1);
        backdrop.setSlow(true);
        final ProgressEmblem em = new ProgressEmblem(this);
        final TextView eyebrow = Theme.eyebrow(this, r.kind.equals("rebuild") ? "Updating the game" : r.kind.equals("extras") ? "Extras" : "Building");
        final TextView stage = Theme.heading(this, "Starting...", landscape ? 22 : 21);
        final TextView stepOf = Theme.text(this, "", 13, Theme.GOLD, Theme.bodyBold);
        final TextView explain = Theme.body(this, "");
        explain.setTextSize(13.5f);
        final TextView detail = Theme.single(Theme.text(this, "", 13, Theme.TEXT, Theme.mono));
        final TextView elapsed = Theme.text(this, "", 12.5f, Theme.TEXT_DIM, Theme.body), eta = Theme.text(this, "", 12.5f, Theme.CRYSTAL, Theme.bodyBold);
        final StepTrack track = new StepTrack(this);
        float[] weights = new float[r.plan.length()];
        for (int i = 0; i < weights.length; i++) weights[i] = (float) r.plan.optJSONObject(i).optDouble("weight", 0.1);
        track.setPlan(weights);
        final LogConsole console = new LogConsole(this);
        console.setEmptyText("The tools' output appears here as they run.");
        synchronized (r.history) { console.setAll(r.history); }
        p.console = console;
        Card cancel = new Card(this, Card.ACTION, Icons.CLOSE, "Cancel", null).style(Card.STYLE_DANGER);
        cancel.onClick(v -> dialog("Cancel the build?", "The work done so far is kept: building again continues where it stopped.", Icons.WARN,
                new Btn("Keep building", Icons.BUILD, Card.STYLE_PRIMARY, null),
                new Btn("Yes, cancel", Icons.CLOSE, Card.STYLE_DANGER, () -> { BuildRunner b = BuildRunner.current; if (b != null) b.cancel(); }),
                new Btn("Back", Icons.BACK, Card.STYLE_NORMAL, null)));
        Card size = new Card(this, Card.ACTION, Icons.NONE, logBig ? "Smaller log" : "Bigger log", null).style(Card.STYLE_QUIET);

        // layout: landscape = [emblem beside the title, status, buttons | log], portrait = emblem / status / log / buttons
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.VERTICAL);
        head.addView(eyebrow);
        head.addView(stage);
        head.addView(stepOf);
        LinearLayout status = new LinearLayout(this);
        status.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(-1, -2);
        tp.topMargin = Theme.dp(10);
        tp.bottomMargin = Theme.dp(8);
        status.addView(track, tp);
        status.addView(detail);
        LinearLayout times = new LinearLayout(this);
        times.setOrientation(LinearLayout.HORIZONTAL);
        times.addView(elapsed, new LinearLayout.LayoutParams(0, -2, 1));
        times.addView(eta);
        LinearLayout.LayoutParams tsp = new LinearLayout.LayoutParams(-1, -2);
        tsp.topMargin = Theme.dp(4);
        status.addView(times, tsp);
        LinearLayout.LayoutParams ep = new LinearLayout.LayoutParams(-1, -2);
        ep.topMargin = Theme.dp(8);
        if (!logBig) status.addView(explain, ep);
        LinearLayout btns = new LinearLayout(this);
        boolean stackBtns = landscape && logBig;   // the narrow column: one above the other
        btns.setOrientation(stackBtns ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams bp = stackBtns ? new LinearLayout.LayoutParams(-1, -2) : new LinearLayout.LayoutParams(0, -2, 1);
        if (stackBtns) bp.bottomMargin = Theme.dp(8); else bp.rightMargin = Theme.dp(8);
        btns.addView(cancel, bp);
        btns.addView(size, stackBtns ? new LinearLayout.LayoutParams(-1, -2) : new LinearLayout.LayoutParams(0, -2, 1));

        View layout;
        if (landscape) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setPadding(Theme.dp(24), Theme.dp(4), Theme.dp(24), Theme.dp(16));
            LinearLayout left = new LinearLayout(this);
            left.setOrientation(LinearLayout.VERTICAL);
            left.setGravity(Gravity.CENTER_VERTICAL);
            LinearLayout top = new LinearLayout(this);
            top.setOrientation(LinearLayout.HORIZONTAL);
            top.setGravity(Gravity.CENTER_VERTICAL);
            em.setShowPercent(false);
            top.addView(em, new LinearLayout.LayoutParams(Theme.dp(84), Theme.dp(112)));
            LinearLayout.LayoutParams hp = new LinearLayout.LayoutParams(0, -2, 1);
            hp.leftMargin = Theme.dp(10);
            top.addView(head, hp);
            left.addView(top);
            left.addView(status);
            LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(-1, -2);
            blp.topMargin = Theme.dp(12);
            left.addView(btns, blp);
            android.widget.ScrollView ls = new android.widget.ScrollView(this);
            ls.setFillViewport(true);
            ls.setVerticalScrollBarEnabled(false);
            ls.addView(left);
            row.addView(ls, new LinearLayout.LayoutParams(0, -1, logBig ? 0.36f : 0.44f));
            View sp = new View(this);
            row.addView(sp, new LinearLayout.LayoutParams(Theme.dp(20), 1));
            row.addView(console, new LinearLayout.LayoutParams(0, -1, logBig ? 0.64f : 0.56f));
            layout = row;
        } else {
            LinearLayout col = new LinearLayout(this);
            col.setOrientation(LinearLayout.VERTICAL);
            int side = Theme.dp(18);
            int w = root.getWidth() > 0 ? root.getWidth() : getResources().getDisplayMetrics().widthPixels;
            if (w > Theme.dp(640)) side = (w - Theme.dp(600)) / 2;
            col.setPadding(side, 0, side, Theme.dp(14));
            if (!logBig) col.addView(em, new LinearLayout.LayoutParams(-1, 0, 0.62f));
            col.addView(head);
            col.addView(status);
            LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(-1, 0, logBig ? 3 : 1.15f);
            cp.topMargin = Theme.dp(12);
            col.addView(console, cp);
            LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(-1, -2);
            blp.topMargin = Theme.dp(10);
            col.addView(btns, blp);
            layout = col;
        }
        size.onClick(v -> { logBig = !logBig; refreshPage(); });
        p.onY = () -> { logBig = !logBig; refreshPage(); };
        p.custom = layout;
        p.focus = cancel;
        p.back = () -> dialog("Leave this page?", "The build keeps going in the background (see the notification). Come back any time.", Icons.INFO,
                new Btn("Go to the home screen", Icons.CHEVRON, Card.STYLE_NORMAL, () -> moveTaskToBack(true)),
                new Btn("Cancel the build", Icons.CLOSE, Card.STYLE_DANGER, () -> { BuildRunner b = BuildRunner.current; if (b != null) b.cancel(); }),
                new Btn("Stay", Icons.BUILD, Card.STYLE_PRIMARY, null));
        show(p, dirOf(p));

        final String title = r.kind.equals("rebuild") ? "Updating the game" : r.kind.equals("extras") ? "Adding extras" : "Building the game";
        BuildRunner.Listener l = new BuildRunner.Listener() {
            public void lines(String[] lines) {
                console.append(lines);
            }

            public void progress(JSONObject j) {
                float f = (float) j.optDouble("fraction");
                em.setFraction(f);
                int step = j.optInt("step");
                String s = j.optString("stage");
                if (!s.equals(stage.getText().toString())) {   // a new stage: the title fades over
                    stage.animate().alpha(0).setDuration(120).withEndAction(() -> { stage.setText(s); stage.animate().alpha(1).setDuration(260).start(); }).start();
                }
                stepOf.setText((r.plan.length() > 1 ? "Step " + (step + 1) + " of " + r.plan.length() + "  \u00B7  " : "") + Math.round(f * 100) + "%");
                explain.setText(j.optString("explain"));
                String d = j.optString("detail");
                detail.setText(d.isEmpty() ? "Working..." : d);
                elapsed.setText(j.optString("elapsed"));
                eta.setText(j.optString("eta"));
                JSONObject st = r.plan.optJSONObject(Math.min(step, r.plan.length() - 1));
                double base = st == null ? 0 : st.optDouble("base"), w = st == null ? 1 : st.optDouble("weight", 1);
                track.set(step, (float) ((f - base) / Math.max(0.0001, w)), false);
                BuildService.update(SetupActivity.this, title, s + " · " + Math.round(f * 100) + "%", Math.round(f * 100));
            }

            public void finished(boolean ok, String error, String note) {
                onBuildFinished(r, ok, error, note);
            }
        };
        r.setListener(l);
        if (r.lastProgress != null) l.progress(r.lastProgress);
        if (!r.running) ui.post(() -> onBuildFinished(r, r.ok, r.error, r.note));
    }

    private void onBuildFinished(BuildRunner r, boolean ok, String error, String note) {
        if (BuildRunner.current != r) return;
        BuildRunner.current = null;
        r.setListener(null);
        keepAwake(false);
        BuildService.stop(this);
        backdrop.setEnergy(0);
        backdrop.setSlow(false);
        if (!ok) {
            if (!resumed) BuildService.finished(this, false, error);
            try {
                failStage = r.lastProgress == null ? "" : r.lastProgress.optString("stage");
            } catch (Exception e) {
                failStage = "";
            }
            failPage(error, r.recentText(6), r.reason);
            return;
        }
        if (r.kind.startsWith("demo")) { go(this::showDone); return; }   // the test plan built nothing
        hideGameFromGallery(false, true);
        // remember what was built (install.ini)
        if (r.kind.equals("install")) {
            st.put("workdir", workdir);
            st.put("iso", iso);
            if (!isoRef.isEmpty()) st.put("iso_ref", isoRef);   // where the first image was: the extras' discs are looked for next to it later
            st.put("version", "dev");
            Options done = opt.copy();
            done.clearActions();   // an added texture pack is done: the switch is off again next time
            done.putInto(st);
            for (String k : Options.actionKeys) st.remove(k);
            st.remove("texture_file_name");
            writeSettings();
        }
        if (!r.kind.equals("extras")) st.put("built_fp", Engine.payloadFingerprint());
        else {
            Options o = opt.copy();
            o.clearDiscs();
            o.clearActions();
            o.putInto(st);
            for (String k : Options.actionKeys) st.remove(k);
            st.remove("texture_file_name");
        }
        saveState();
        if (!resumed) BuildService.finished(this, true, r.kind.equals("extras") ? "The extras are in place." : "Open the setup app and install the game.");
        if (r.kind.equals("extras")) {
            extrasMode = false;
            backdrop.setMood(BackdropView.MOOD_GOLD);
            dialog("Done", (note.isEmpty() ? "The extras are in place." : note) + (autoDiscNote.isEmpty() ? "" : " " + autoDiscNote), Icons.CHECK, new Btn("OK", Icons.CHECK, Card.STYLE_PRIMARY, this::backHome));
            return;
        }
        go(this::showDone);
    }

    /** settings.ini is the game's own file (key=value): only add the update option if the player has no entry yet. */
    private void writeSettings() {
        File f = new File(workdir, "settings.ini");
        String content = readText(f, 1 << 20);
        if (content.contains("check_updates=")) return;
        if (!content.isEmpty() && !content.endsWith("\n")) content += "\n";
        content += "check_updates=" + (opt.checkUpdates() ? "1" : "0") + "\n";
        try (FileOutputStream o = new FileOutputStream(f)) {
            o.write(content.getBytes("UTF-8"));
        } catch (Exception e) {
            // the game asks again
        }
    }

    private void showDone() {
        Page p = new Page(this, "done");
        p.back = this::backHome;
        backdrop.setMood(BackdropView.MOOD_GOLD);
        ProgressEmblem em = new ProgressEmblem(this);
        em.setFraction(1);
        em.setMode(ProgressEmblem.DONE);
        em.setShowPercent(false);
        p.hero = em;
        p.heroHeightDp = 200;
        p.head("Finished", "All done", "The game is built and its files are in place. Install it as an app: Android asks you to confirm." + (autoDiscNote.isEmpty() ? "" : " " + autoDiscNote));
        p.card(new Card(this, Card.ACTION, Icons.INSTALL, "Install the game", "Then open \"Dissidia 012\" from your app list.").style(Card.STYLE_PRIMARY).pulse())
                .onClick(v -> installGame(new File(stWorkdir(), "android/dissidia.apk").getPath(), false));
        saveApkCard(p);
        p.card(new Card(this, Card.ACTION, Icons.BACK, "Skip for now", "Install it later from the start page.").style(Card.STYLE_QUIET)).onClick(v -> skipInstall());
        p.card(new Card(this, Card.ACTION, Icons.LOG, "View the build log", null).style(Card.STYLE_QUIET)).onClick(v -> viewLog(new File(stWorkdir(), "setup.log"), "Build log"));
        show(p, dirOf(p));
    }

    private String[] failReason;   // { kind, title, text } from the engine (setup/install.cpp explain_failure) for the run that failed

    private void failPage(String msg, String tail) {
        failPage(msg, tail, null);
    }

    private void failPage(String msg, String tail, String[] reason) {
        failReason = reason;
        failMsg = msg;
        failTail = tail;
        go(this::showFailed);
    }

    private String failMsg = "", failTail = "", failStage = "";
    private String interruptedOpts = "";
    private boolean resumeNext;   // the next startPlan continues an interrupted run
    private String interruptedKind = "";   // set at start when the last install did not end (offered once)

    private void showFailed() {
        Page p = new Page(this, "failed");
        p.back = this::backHome;
        backdrop.setMood(BackdropView.MOOD_CHAOS);
        ProgressEmblem em = new ProgressEmblem(this);
        em.setFraction(BuildRunner.current == null ? 0.35f : 0.35f);
        em.setMode(ProgressEmblem.FAILED);
        em.setShowPercent(false);
        p.hero = em;
        p.heroHeightDp = 150;
        final String[] rs = failReason;
        final String[] parts = rs != null && rs.length >= 3 ? rs[2].split("\n\n") : null;   // what failed / Why: ... / What to do: ...
        if (parts != null) p.head("Stopped", rs[1], parts[0]);
        else p.head("Stopped", "Not finished", failMsg + " Trying again continues where it stopped.");
        if (!failTail.isEmpty()) {
            TextView tail = Theme.text(this, failTail, 11.5f, 0xFFE8C6C0, Theme.mono);
            tail.setPadding(Theme.dp(12), Theme.dp(10), Theme.dp(12), Theme.dp(10));
            android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
            bg.setColor(0xC0140810);
            bg.setStroke(Theme.dp(1), 0x60E0675A);
            bg.setCornerRadius(Theme.dp(6));
            tail.setBackground(bg);
            tail.setTextIsSelectable(true);
            p.headerExtra.add(tail);
        }
        if (parts != null)
            for (int i = 1; i < parts.length; i++) {
                String t = parts[i], head = t.startsWith("Why: ") ? "Why" : t.startsWith("What to do: ") ? "What to do" : "More";
                p.card(new Card(this, Card.INFO, head.equals("Why") ? Icons.INFO : Icons.CHECK, head, t.substring(t.indexOf(": ") + 2 > 1 && !head.equals("More") ? t.indexOf(": ") + 2 : 0)));
            }
        final File log = new File(workdir, "setup.log");
        p.card(new Card(this, Card.ACTION, Icons.RETRY, "Try again", "Runs the same steps again; finished work is kept.").style(Card.STYLE_PRIMARY))
                .onClick(v -> {
                    if (runKind.equals("install") && iso.isEmpty()) { beginPick(PICK_ISO, this::backHome); return; }
                    startPlan(runKind, iso, Options.parse(runOpts), runPrev);
                });
        p.card(new Card(this, Card.ACTION, Icons.BUG, "Report an issue", "Tell the developer what went wrong: you see what is sent first, then it opens a GitHub issue.").style(Card.STYLE_PRIMARY))
                .onClick(v -> beginReport(() -> goBackTo(this::showFailed), failMsg, failStage, failTail));
        p.card(new Card(this, Card.ACTION, Icons.SHARE, "Share the log", "Send it to another app: messages, e-mail, a cloud drive...")).onClick(v -> shareFile(log, "dissidia-setup-log.txt", "Dissidia Recomp setup log"));
        p.card(new Card(this, Card.ACTION, Icons.SAVE, "Save the log", "Save it as a file where you choose.")).onClick(v -> saveFile(log, "dissidia-setup-log.txt"));
        p.card(new Card(this, Card.ACTION, Icons.EYE, "View the full log", null).style(Card.STYLE_QUIET)).onClick(v -> viewLog(log, "Build log"));
        show(p, dirOf(p));
    }

    private void keepAwake(boolean on) {
        if (on) {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);   // the screen stays on while this page is shown
            BuildService.hold(this);   // the CPU stays on when it is not (the process-wide lock, given back when the build ends)
        } else {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            BuildService.release();
        }
    }

    // ------------------------------------------------------------------------------------------------------------- installing the APK

    private boolean canInstallPackages() {
        return Build.VERSION.SDK_INT < 26 || getPackageManager().canRequestPackageInstalls();
    }

    private void installGame(String apk, boolean update) {
        if (!new File(apk).isFile()) {
            dialog("Not built", "The game's app was not found (" + apk + "). Build the game again.", Icons.WARN, new Btn("OK", Icons.CHECK, Card.STYLE_PRIMARY, null));
            return;
        }
        if (!canInstallPackages()) {
            pendingInstall = true;
            pendingInstallApk = apk;
            installUpdate = update;
            go(this::showInstallAllow);
            return;
        }
        doInstall(apk, update);
    }

    private boolean installUpdate;

    private void showInstallAllow() {
        Page p = new Page(this, "install-allow");
        p.back = () -> { pendingInstall = false; backHome(); };
        boolean ok = canInstallPackages();
        Icons.IconView hero = new Icons.IconView(this, ok ? Icons.CHECK : Icons.KEY, ok ? Theme.CRYSTAL : Theme.GOLD);
        hero.strokeDp = 2.2f;
        p.hero = hero;
        p.heroHeightDp = 84;
        p.head("Permission", "Allow installing", "Android asks once whether this app may install apps. Switch on \"Allow from this source\", then come back: the installation continues by itself.");
        p.card(new Card(this, Card.ACTION, Icons.KEY, "Open the setting", null).value(ok ? "Allowed" : "Needed").style(Card.STYLE_PRIMARY)).onClick(v -> {
            if (canInstallPackages()) { pendingInstall = false; doInstall(pendingInstallApk, installUpdate); }
            else startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getPackageName())));
        });
        show(p, dirOf(p));
    }

    private void doInstall(final String apk, boolean update) {
        installUpdate = update;
        pendingInstallApk = apk;
        installStatus = "working";
        installStartedAt = System.currentTimeMillis();
        pendingConfirm = null;
        lastConfirm = null;
        if (installReceiver == null) {
            installReceiver = new BroadcastReceiver() {
                public void onReceive(Context c, Intent i) {
                    int s = i.getIntExtra(PackageInstaller.EXTRA_STATUS, -1);
                    if (s == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                        Intent confirm = (Intent) i.getParcelableExtra(Intent.EXTRA_INTENT);
                        if (confirm != null) {
                            confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                            lastConfirm = confirm;
                            showConfirm(confirm);
                        }
                        installStatus = "confirm";
                    } else if (s == PackageInstaller.STATUS_SUCCESS) { installStatus = "ok"; lastConfirm = null; }
                    else if (s == PackageInstaller.STATUS_FAILURE_ABORTED) installStatus = "failed: cancelled";
                    else if (s == PackageInstaller.STATUS_FAILURE_STORAGE) installStatus = "failed: storage";
                    else if (s == PackageInstaller.STATUS_FAILURE_CONFLICT || s == PackageInstaller.STATUS_FAILURE_INCOMPATIBLE) installStatus = "failed: conflict";
                    else if (s == PackageInstaller.STATUS_FAILURE_BLOCKED) installStatus = "failed: blocked";
                    else installStatus = "failed: " + i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                    ui.post(() -> { if (page != null && page.id.equals("installing")) refreshPage(); });
                }
            };
            if (Build.VERSION.SDK_INT >= 33) registerReceiver(installReceiver, new IntentFilter(ACTION_INSTALLED), Context.RECEIVER_NOT_EXPORTED);
            else registerReceiver(installReceiver, new IntentFilter(ACTION_INSTALLED));
        }
        go(this::showInstalling);
        new Thread(() -> {
            try {
                PackageInstaller pi = getPackageManager().getPackageInstaller();
                PackageInstaller.SessionParams sp = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
                int id = pi.createSession(sp);
                try (PackageInstaller.Session s = pi.openSession(id)) {
                    try (InputStream in = new FileInputStream(apk); OutputStream out = s.openWrite("app.apk", 0, new File(apk).length())) {
                        byte[] buf = new byte[1 << 16];
                        int n;
                        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                        s.fsync(out);
                    }
                    Intent cb = new Intent(ACTION_INSTALLED).setPackage(getPackageName());
                    int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
                    s.commit(PendingIntent.getBroadcast(this, id, cb, flags).getIntentSender());
                }
            } catch (Exception e) {
                installStatus = "failed: " + e.getMessage();
                ui.post(() -> { if (page != null && page.id.equals("installing")) refreshPage(); });
            }
        }, "setup-install").start();
    }

    /** Leave the install for later: the built game and its files stay; the start page offers Install the game. */
    private long resumedAt;

    /**
     * Keeps the game's files out of the gallery and the photo pickers. The game reads them from Android/media/org.psprecomp.game, a folder
     * Android's media scanner indexes, so the HD textures (thousands of PNGs) showed up as photos. A .nomedia file there hides the folder
     * and everything below it from the media index; the game still reads the files directly. A rescan of the folder makes Android drop
     * what it had already indexed (done once for an existing install, and after every build / extras change).
     * create: make the folder if it is not there yet (before a build). rescan: rescan even when the .nomedia file was already there.
     */
    private void hideGameFromGallery(boolean create, boolean rescan) {
        if (!hasAllFilesAccess()) return;
        final File root = new File(Environment.getExternalStorageDirectory(), "Android/media/org.psprecomp.game");
        if (!root.isDirectory() && !(create && root.mkdirs())) return;
        File nm = new File(root, ".nomedia");
        boolean made = false;
        try {
            if (!nm.isFile()) made = nm.createNewFile();
        } catch (Exception e) {
            return;
        }
        boolean first = !"1".equals(st.get("media_hidden"));
        if (!made && !rescan && !first) return;
        try {
            MediaScannerConnection.scanFile(getApplicationContext(), new String[]{root.getPath()}, null, null);
        } catch (Exception e) {
            return;   // the next run tries again (media_hidden stays unset)
        }
        if (first) {
            st.put("media_hidden", "1");
            saveState();
        }
    }

    /**
     * Shows Android's install confirmation. Not while this app is in the background (Android 10+ drops or hides it): then onResume shows
     * it. Started in the same moment the app comes back to the front, Android aborted it (START_ABORTED, emulator, Android 15), so a
     * start right after onResume waits until the return has settled; the page re-checks that the confirmation is still unanswered.
     */
    private void showConfirm(final Intent c) {
        if (!resumed) { pendingConfirm = c; return; }
        long wait = Math.max(0, 800 - (System.currentTimeMillis() - resumedAt));
        ui.postDelayed(() -> {
            if (!resumed) { pendingConfirm = c; return; }
            if (page == null || !page.id.equals("installing") || installStatus.startsWith("failed") || installStatus.equals("ok")) return;
            try { startActivity(c); } catch (Exception e) { /* the page offers Install again */ }
        }, wait);
    }

    private void skipInstall() {
        pendingConfirm = null;
        lastConfirm = null;
        backHome();
        if (!installUpdate) ui.postDelayed(() -> snack("The game is built. Install it from the start page whenever you like."), 400);
    }

    private void saveApkCard(Page p) {
        p.card(new Card(this, Card.ACTION, Icons.SAVE, "Save the APK to Downloads",
                "To install the game yourself from a file manager, e.g. if Android's confirmation does not show up.")).onClick(v -> saveApk());
    }

    private void saveApk() {
        final File apk = new File(stWorkdir(), "android/dissidia.apk");
        if (!apk.isFile()) {
            snack("The game's app was not found: build the game again.");
            return;
        }
        snack("Saving the APK...");
        new Thread(() -> {
            String msg;
            try {
                msg = "Saved as " + copyToDownloads(apk, "Dissidia012.apk") + ". Open it in your file manager to install the game.";
            } catch (Exception e) {
                msg = "Could not save the APK: " + e.getMessage();
            }
            final String m = msg;
            ui.post(() -> snack(m));
        }, "setup-save-apk").start();
    }

    /** Copies `src` into the public Downloads folder (replacing an earlier copy of the same name) and says where it went. */
    private String copyToDownloads(File src, String name) throws Exception {
        byte[] buf = new byte[1 << 16];
        int n;
        if (Build.VERSION.SDK_INT >= 29) {
            ContentResolver cr = getContentResolver();
            Uri coll = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
            try {
                cr.delete(coll, MediaStore.MediaColumns.DISPLAY_NAME + "=?", new String[]{name});
            } catch (Exception e) {
                // not ours to delete: Android names the new copy "Dissidia012 (1).apk"
            }
            ContentValues v = new ContentValues();
            v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
            v.put(MediaStore.MediaColumns.MIME_TYPE, "application/vnd.android.package-archive");
            v.put(MediaStore.MediaColumns.IS_PENDING, 1);
            Uri u = cr.insert(coll, v);
            if (u == null) throw new Exception("Android did not create the file");
            try (InputStream in = new FileInputStream(src); OutputStream out = cr.openOutputStream(u)) {
                if (out == null) throw new Exception("Android did not open the file");
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            } catch (Exception e) {
                cr.delete(u, null, null);
                throw e;
            }
            v.clear();
            v.put(MediaStore.MediaColumns.IS_PENDING, 0);
            cr.update(u, v, null, null);
            return "Download/" + name;
        }
        File dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        dir.mkdirs();
        File out = new File(dir, name);
        try (InputStream in = new FileInputStream(src); OutputStream o = new FileOutputStream(out)) {
            while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
        }
        return out.getPath();
    }

    /** Why Android refused the game's APK, in plain words with what to do (the last step of the install). */
    private String installFailureWords(String s) {
        String why = s.substring(Math.min(s.length(), 8));
        switch (why) {
            case "storage":
                return "Why: the phone has no room for the game's app (about 100 MB; the game's files are already in place). What to do: free some space (Settings > Storage), then Install again.";
            case "conflict":
                return "Why: another Dissidia 012 app with a different signature is installed (for example one from another source). What to do: uninstall that app (Settings > Apps), then Install again. Back up your saves first if you can.";
            case "blocked":
                return "Why: Android or Play Protect blocked the installation. What to do: Install again and choose More details, then Install anyway; on Samsung phones switch off Auto Blocker (Settings > Security and privacy) for a moment.";
            default:
                return "Android said: " + why + ". If it asked for permission to install apps from this app, allow it and try again.";
        }
    }

    private void showInstalling() {
        Page p = new Page(this, "installing");
        p.back = this::backHome;
        String s = installStatus;
        boolean ok = s.equals("ok"), failed = s.startsWith("failed");
        ProgressEmblem em = new ProgressEmblem(this);
        em.setShowPercent(false);
        if (ok) { em.setFraction(1); em.setMode(ProgressEmblem.DONE); backdrop.setMood(BackdropView.MOOD_GOLD); }
        else if (failed) { em.setFraction(0.3f); em.setMode(ProgressEmblem.FAILED); backdrop.setMood(BackdropView.MOOD_CHAOS); }
        else { em.setMode(ProgressEmblem.SPIN); backdrop.setMood(BackdropView.MOOD_CRYSTAL); }
        p.hero = em;
        p.heroHeightDp = 170;
        String what = installUpdate ? "the update" : "the game";
        if (ok) {
            p.head("Installed", installUpdate ? "Updated" : "Ready to play", installUpdate ? "The new version of the setup app is installed. It opens again by itself or from your app list."
                    : "Open \"Dissidia 012\" from your app list. The first start prepares the graphics for a moment.");
            Intent launch = getPackageManager().getLaunchIntentForPackage("org.psprecomp.game");
            if (!installUpdate && launch != null)
                p.card(new Card(this, Card.ACTION, Icons.PLAY, "Play now", "Starts Dissidia 012.").style(Card.STYLE_PRIMARY).pulse()).onClick(v -> {
                    try { startActivity(launch); } catch (Exception e) { snack("Could not start the game: " + e.getMessage()); }
                });
            p.card(new Card(this, Card.ACTION, Icons.BACK, "Back to the start", null).style(Card.STYLE_QUIET)).onClick(v -> backHome());
        } else if (failed) {
            boolean cancelled = s.equals("failed: cancelled");
            p.head("Not installed", "Installation stopped", cancelled
                    ? "Android's confirmation was closed without installing. Tap Install again when you are ready" + (installUpdate ? "." : ", or save the APK and install it from your file manager.")
                    : installFailureWords(s));
            p.card(new Card(this, Card.ACTION, Icons.RETRY, "Install again", "Asks Android to install " + what + " again.").style(Card.STYLE_PRIMARY)).onClick(v -> installGame(pendingInstallApk, installUpdate));
            if (!installUpdate) saveApkCard(p);
            p.card(new Card(this, Card.ACTION, Icons.BACK, "Back to the start", null).style(Card.STYLE_QUIET)).onClick(v -> backHome());
        } else {
            long waited = System.currentTimeMillis() - installStartedAt;
            boolean slow = waited >= INSTALL_HINT_MS;
            if (!slow)
                p.head("Installing", "Waiting for Android", "Android shows a confirmation: tap Install. The game's files are already in place.");
            else
                p.head("Installing", "Still waiting for Android", "Android has not answered. If no confirmation is on screen, tap Install again. "
                        + "If Play Protect blocks it, choose More details, then Install anyway. On Samsung phones, Auto Blocker "
                        + "(Settings > Security and privacy) stops installs from apps: switch it off for a moment. "
                        + (installUpdate ? "" : "Or save the APK and install it from your file manager. ")
                        + "The game stays built either way.");
            Card again = p.card(new Card(this, Card.ACTION, Icons.RETRY, "Install again", "If the confirmation did not appear."));
            if (slow) again.style(Card.STYLE_PRIMARY);
            again.onClick(v -> installGame(pendingInstallApk, installUpdate));
            if (!installUpdate) saveApkCard(p);
            p.card(new Card(this, Card.ACTION, Icons.BACK, installUpdate ? "Back to the start" : "Skip for now",
                    installUpdate ? null : "The game stays built: install it later from the start page.").style(Card.STYLE_QUIET)).onClick(v -> skipInstall());
            if (!slow) ui.postDelayed(() -> { if (page != null && page.id.equals("installing") && installStatus.equals(s)) refreshPage(); },
                    INSTALL_HINT_MS - waited + 100);
        }
        show(p, dirOf(p));
        if (!ok && !failed) ui.postDelayed(() -> { if (page != null && page.id.equals("installing") && !installStatus.equals(s)) refreshPage(); }, 600);
    }

    // ------------------------------------------------------------------------------------------------------------- updates

    private File updDir() {
        File d = new File(getFilesDir(), "update");
        d.mkdirs();
        return d;
    }

    private void updateLog(String s) {
        try (FileOutputStream o = new FileOutputStream(new File(updDir(), "update.log"), true)) {
            o.write((new SimpleDateFormat("yyyy-MM-dd HH:mm:ss ", Locale.US).format(new Date()) + s + "\n").getBytes("UTF-8"));
        } catch (Exception e) {
            // a convenience
        }
    }

    private void beginCheck(final boolean manual) {
        if (!problem.isEmpty()) return;
        final int seq = ++checkSeq;
        if (manual) go(this::showChecking);
        final File dir = updDir();
        new Thread(() -> {
            JSONObject u = check(dir);
            ui.post(() -> finishCheck(seq, manual, u));
        }, "setup-check").start();
    }

    private JSONObject check(File dir) {
        try {
            File man = new File(dir, "manifest.json"), sig = new File(dir, "manifest.json.sig");
            man.delete();
            sig.delete();
            String url = MANIFEST_URL;
            String over = readText(new File(getFilesDir(), "manifest_url.txt"), 4096).trim();   // debug / test switch: another manifest (still needs a valid signature)
            if (!over.isEmpty()) url = over.split("\n")[0].trim();
            Downloader d = new Downloader();
            String r = d.fetch(url, man, null);
            if (r != null) return new JSONObject().put("error", r);
            d.fetch(url + ".sig", sig, null);   // a missing signature is judged by update.py (an error when this app carries a key)
            String[] out = Engine.runTool(Engine.checkArgv(man.getPath(), sig.getPath(), appVersion()), workdir, 30000);
            String result = "";
            for (String l : out) {
                updateLog("check: " + l);
                if (l.startsWith("UPDATE") || l.startsWith("NOUPDATE") || l.startsWith("ERROR")) result = l;
            }
            if (result.isEmpty())   // the checker did not answer (it could not run): not "no update"
                return new JSONObject().put("error", "the update checker did not run (Logs and reports > Updater log)");
            return new JSONObject(Engine.parseUpdate(result));
        } catch (Exception e) {
            try { return new JSONObject().put("error", e.toString()); } catch (Exception e2) { return new JSONObject(); }
        }
    }

    private void showChecking() {
        Page p = new Page(this, "checking");
        p.back = () -> { checkSeq++; backHome(); };
        ProgressEmblem em = new ProgressEmblem(this);
        em.setMode(ProgressEmblem.SPIN);
        em.setShowPercent(false);
        p.hero = em;
        p.heroHeightDp = 170;
        p.head("Updates", "Checking", "Looking for a newer version on the release page. Nothing about you or your game is sent.");
        p.card(new Card(this, Card.ACTION, Icons.CLOSE, "Skip", null).style(Card.STYLE_QUIET)).onClick(v -> { checkSeq++; backHome(); });
        show(p, dirOf(p));
    }

    private void finishCheck(int seq, boolean manual, JSONObject u) {
        if (seq != checkSeq) return;   // skipped
        upd = u;
        if (!manual && (page == null || !page.id.equals("home"))) return;   // the silent start-up check never interrupts what the player is doing
        if (u.optBoolean("available")) { go(this::showUpdateAvail); return; }
        if (manual) {
            String err = u.optString("error");
            dialog("Updates", err.isEmpty() ? "You have the newest version." : "Could not check for updates: " + err, err.isEmpty() ? Icons.CHECK : Icons.WARN,
                    new Btn("OK", Icons.CHECK, Card.STYLE_PRIMARY, this::backHome));
        }
    }

    private void showUpdateAvail() {
        Page p = new Page(this, "update-avail");
        p.back = this::backHome;
        backdrop.setMood(BackdropView.MOOD_GOLD);
        Icons.IconView hero = new Icons.IconView(this, Icons.DOWNLOAD, Theme.GOLD_LIGHT);
        hero.strokeDp = 2;
        p.hero = hero;
        p.heroHeightDp = 90;
        p.head("Update available", "Version " + upd.optString("version"), "You have " + upd.optString("current") + ". Download size: " + BuildRunner.size(upd.optLong("size"))
                + ". Android asks you to confirm the installation. If the update changes how the game is built, you are offered to build it again afterwards (no game file needed).");
        p.card(new Card(this, Card.ACTION, Icons.DOWNLOAD, "Update now", "The game and your saves are not touched.").style(Card.STYLE_PRIMARY).pulse()).onClick(v -> startUpdateDownload());
        p.card(new Card(this, Card.ACTION, Icons.CLOSE, "Not now", "You can update later from the start page.").style(Card.STYLE_QUIET)).onClick(v -> backHome());
        show(p, dirOf(p));
    }

    private ProgressEmblem dlEmblem;
    private TextView dlText;

    private void startUpdateDownload() {
        final File apk = new File(updDir(), "dissidia-setup.apk");
        final Downloader d = new Downloader();
        updDl = d;
        go(this::showDownload);
        new Thread(() -> {
            updateLog("downloading " + upd.optString("url"));
            String r = d.fetch(upd.optString("url"), apk, (done, total) -> {
                long t = total > 0 ? total : upd.optLong("size");
                ui.post(() -> {
                    if (dlEmblem != null && t > 0) dlEmblem.setFraction((float) done / t);
                    if (dlText != null) dlText.setText(BuildRunner.size(done) + (t > 0 ? " of " + BuildRunner.size(t) : ""));
                });
            });
            if (r != null) {
                updateLog("download failed: " + r);
                ui.post(() -> { if (!d.cancelled()) dialog("Update failed", "The download failed: " + r, Icons.WARN, new Btn("OK", Icons.CHECK, Card.STYLE_PRIMARY, this::backHome)); });
                return;
            }
            ui.post(() -> { if (dlText != null) dlText.setText("Checking the download..."); });
            updateLog("verifying " + apk + " sha256=" + upd.optString("sha256") + " size=" + upd.optLong("size"));
            boolean ok = false;
            for (String l : Engine.runTool(Engine.verifyArgv(apk.getPath(), upd.optString("sha256"), upd.optLong("size")), workdir, 120000)) {
                updateLog("verify: " + l);
                if (l.equals("OK")) ok = true;
            }
            final boolean good = ok;
            ui.post(() -> {
                if (d.cancelled()) return;
                if (good) installGame(apk.getPath(), true);
                else dialog("Update failed", "The downloaded file does not match what the release lists, so it was deleted. Try again later.", Icons.WARN,
                        new Btn("OK", Icons.CHECK, Card.STYLE_PRIMARY, this::backHome));
            });
        }, "setup-update").start();
    }

    private void showDownload() {
        Page p = new Page(this, "download");
        Runnable cancel = () -> { if (updDl != null) updDl.cancel(); backHome(); };
        p.back = cancel;
        ProgressEmblem em = new ProgressEmblem(this);
        dlEmblem = em;
        p.hero = em;
        p.heroHeightDp = 190;
        p.head("Updating", "Version " + upd.optString("version"), "Downloading the new version of this app. Android then asks you to confirm the installation. The game and your saves are not touched.");
        dlText = Theme.text(this, "Starting...", 14, Theme.GOLD_LIGHT, Theme.bodyBold);
        p.headerExtra.add(dlText);
        p.card(new Card(this, Card.ACTION, Icons.CLOSE, "Cancel", null).style(Card.STYLE_DANGER)).onClick(v -> cancel.run());
        show(p, dirOf(p));
    }

    // ------------------------------------------------------------------------------------------------------------- logs and reports

    private static final class LogEntry {
        final String title, summary, shareName;
        final File file;
        final int icon;

        LogEntry(String title, String summary, File file, String shareName, int icon) {
            this.title = title;
            this.summary = summary;
            this.file = file;
            this.shareName = shareName;
            this.icon = icon;
        }
    }

    private List<LogEntry> logEntries() {
        ArrayList<LogEntry> l = new ArrayList<>();
        File build = new File(stWorkdir(), "setup.log");
        if (build.isFile()) l.add(new LogEntry("Build log", "Everything the build tools printed, every run. " + when(build), build, "dissidia-setup-log.txt", Icons.BUILD));
        File game = new File(Engine.gameDir(), "last_run.log");
        if (game.canRead()) l.add(new LogEntry("Game log (last run)", "What the game printed the last time it ran: useful when it crashed or closed. " + when(game), game, "dissidia-game-last-run-log.txt", Icons.PLAY));
        else if (game.exists() || !hasAllFilesAccess())
            l.add(new LogEntry("Game log (last run)", "Needs \"all files access\" to be read (the game keeps it in Android/media).", null, "", Icons.PLAY));
        File update = new File(getFilesDir(), "update/update.log");
        if (update.isFile()) l.add(new LogEntry("Updater log", "What the update checks did. " + when(update), update, "dissidia-updater-log.txt", Icons.DOWNLOAD));
        for (File f : CrashReports.list(this))
            l.add(new LogEntry("Setup app crash report", "The setup app closed unexpectedly. " + when(f), f, f.getName(), Icons.BUG));
        return l;
    }

    private static String when(File f) {
        return "Written " + new SimpleDateFormat("d MMM yyyy, HH:mm", Locale.getDefault()).format(new Date(f.lastModified())) + ", " + BuildRunner.size(f.length()) + ".";
    }

    private void showLogs() {
        Page p = new Page(this, "logs");
        p.back = this::backHome;
        backdrop.setMood(BackdropView.MOOD_CRYSTAL);
        p.head("Logs and reports", "Logs", "Something went wrong? Share a log with whoever helps you, or save it as a file. Logs contain file paths and tool output, nothing else about you.");
        List<LogEntry> l = logEntries();
        if (l.isEmpty()) p.card(new Card(this, Card.INFO, Icons.INFO, "No logs yet", "The build log appears after the first build."));
        for (LogEntry e : l) {
            Card k = p.card(new Card(this, Card.ACTION, e.icon, e.title, e.summary));
            if (e.file == null) k.onClick(v -> go(this::showPermission));
            else k.onClick(v -> viewLog(e.file, e.title, e.shareName));
        }
        p.card(new Card(this, Card.ACTION, Icons.BUG, "Report an issue", "Send these logs and your description to the developer as a GitHub issue.").style(Card.STYLE_PRIMARY))
                .onClick(v -> beginReport(() -> goBackTo(this::showLogs), "", "", ""));
        show(p, dirOf(p));
    }

    private File viewing;
    private String viewingTitle, viewingShare;

    private void viewLog(File f, String title) {
        viewLog(f, title, f.getName());
    }

    private void viewLog(File f, String title, String shareName) {
        viewing = f;
        viewingTitle = title;
        viewingShare = shareName;
        final Runnable back = rebuildPage;
        viewBack = back;
        go(this::showLogViewer);
    }

    private Runnable viewBack;

    private void showLogViewer() {
        Page p = new Page(this, "viewer");
        final Runnable back = viewBack;
        p.back = () -> { pendingDir = -1; present(back != null ? back : this::showHome, -1, true); };
        boolean landscape = root.getWidth() > root.getHeight() * 1.05f;
        LogConsole console = new LogConsole(this);
        console.setEmptyText("Loading...");
        console.setLive(false);
        p.console = console;
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(landscape ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        col.setPadding(Theme.dp(18), 0, Theme.dp(18), Theme.dp(14));
        LinearLayout side = new LinearLayout(this);
        side.setOrientation(LinearLayout.VERTICAL);
        side.addView(Theme.eyebrow(this, "Log"));
        side.addView(Theme.heading(this, viewingTitle, 22));
        TextView info = Theme.body(this, viewing.getName() + " · " + BuildRunner.size(viewing.length()));
        info.setTextSize(13);
        side.addView(info);
        Card share = new Card(this, Card.ACTION, Icons.SHARE, "Share", null);
        share.onClick(v -> shareFile(viewing, viewingShare, viewingTitle));
        Card save = new Card(this, Card.ACTION, Icons.SAVE, "Save as file", null);
        save.onClick(v -> saveFile(viewing, viewingShare));
        LinearLayout btns = new LinearLayout(this);
        btns.setOrientation(landscape ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams b1 = landscape ? new LinearLayout.LayoutParams(-1, -2) : new LinearLayout.LayoutParams(0, -2, 1);
        if (landscape) b1.bottomMargin = Theme.dp(8); else b1.rightMargin = Theme.dp(8);
        btns.addView(share, b1);
        btns.addView(save, landscape ? new LinearLayout.LayoutParams(-1, -2) : new LinearLayout.LayoutParams(0, -2, 1));
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(-1, -2);
        bp.topMargin = Theme.dp(12);
        if (landscape) {
            side.addView(btns, bp);
            col.addView(side, new LinearLayout.LayoutParams(0, -2, 0.32f));
            View sp = new View(this);
            col.addView(sp, new LinearLayout.LayoutParams(Theme.dp(18), 1));
            col.addView(console, new LinearLayout.LayoutParams(0, -1, 0.68f));
        } else {
            col.addView(side);
            LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(-1, 0, 1);
            cp.topMargin = Theme.dp(12);
            col.addView(console, cp);
            col.addView(btns, bp);
        }
        p.custom = col;
        p.focus = share;
        show(p, dirOf(p));
        final File f = viewing;
        new Thread(() -> {
            List<String> lines = tailLines(f, 4000);
            ui.post(() -> { console.setAll(lines); console.setEmptyText("(empty)"); });
        }, "setup-log").start();
    }

    private static List<String> tailLines(File f, int max) {
        ArrayList<String> l = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String s;
            while ((s = r.readLine()) != null) {
                l.add(s);
                if (l.size() > max * 2) l.subList(0, max).clear();
            }
        } catch (Exception e) {
            l.add("Could not read " + f + ": " + e.getMessage());
        }
        return l.size() > max ? l.subList(l.size() - max, l.size()) : l;
    }

    private static String readText(File f, int max) {
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] b = new byte[(int) Math.min(max, f.length())];
            int n = 0, k;
            while (n < b.length && (k = in.read(b, n, b.length - n)) > 0) n += k;
            return new String(b, 0, n, "UTF-8");
        } catch (Exception e) {
            return "";
        }
    }

    private static void copy(InputStream in, OutputStream out) throws java.io.IOException {
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
    }

    /** The share sheet: a copy of the file goes to cache/share, ShareProvider hands it out read-only. */
    private void shareFile(File src, String name, String subject) {
        if (src == null || !src.isFile()) { snack("There is no log yet."); return; }
        try {
            File dst = new File(ShareProvider.dir(this), name);
            try (InputStream in = new FileInputStream(src); OutputStream out = new FileOutputStream(dst)) {
                copy(in, out);
            }
            Uri u = ShareProvider.uri(name);
            Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("text/plain");
            send.putExtra(Intent.EXTRA_STREAM, u);
            send.putExtra(Intent.EXTRA_SUBJECT, subject);
            send.putExtra(Intent.EXTRA_TEXT, subject + " (Dissidia Recomp setup " + appVersion() + ", " + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE + ")");
            send.setClipData(ClipData.newRawUri(name, u));
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(send, "Share " + subject.toLowerCase(Locale.US)));
        } catch (Exception e) {
            snack("Could not share: " + e.getMessage());
        }
    }

    /** "Save as": Android's document picker, the file is copied to the place chosen. */
    private void saveFile(File src, String name) {
        if (src == null || !src.isFile()) { snack("There is no log yet."); return; }
        saveSource = src.getPath();
        try {
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("text/plain");
            i.putExtra(Intent.EXTRA_TITLE, name);
            startActivityForResult(i, REQ_SAVE);
        } catch (Exception e) {
            snack("Could not open the file picker: " + e.getMessage());
        }
    }

    /** The app closed unexpectedly last time: offer the report once. */
    private void offerCrashReport() {
        final File f = CrashReports.unseen(this);
        final String killed = interruptedKind;
        if (overlay.getChildCount() > 0) return;
        if (f == null && !killed.isEmpty()) {   // no crash report, but a run never ended: Android killed the app (memory?) or the device restarted
            interruptedKind = "";
            final String kOpts = interruptedOpts;
            final boolean canResume = (killed.equals("install") || killed.equals("rebuild") || killed.equals("demo")) && new File(killed.equals("install") || killed.equals("demo") ? workdir : stWorkdir(), "steps_done.txt").isFile();
            if (canResume) {
                dialog("The last build did not finish", "The app was closed while it built the game. Resume continues where it stopped: the steps and the compiled files that are done are kept, only the rest is built. Start over builds everything again (your game file is asked for again).", Icons.REFRESH,
                        new Btn("Resume the build", Icons.REFRESH, Card.STYLE_PRIMARY, () -> { resumeNext = true; workdir = killed.equals("install") || killed.equals("demo") ? workdir : stWorkdir(); startPlan(killed, "", Options.parse(kOpts), null); }),
                        new Btn("Start over", Icons.BUILD, Card.STYLE_NORMAL, () -> { new File(killed.equals("install") || killed.equals("demo") ? workdir : stWorkdir(), "steps_done.txt").delete(); backHome(); }),
                        new Btn("Report an issue", Icons.BUG, Card.STYLE_NORMAL, () -> beginReport(this::backHome, "The last " + killed + " did not finish: the app was closed while it worked.", "", "")));
                return;
            }
            dialog("The last " + (killed.equals("install") ? "build" : killed) + " did not finish", "The app was closed while it worked (Android may have stopped it, or it crashed). The build log is kept. Sending it to the developer helps to fix this; you see what is sent first.", Icons.BUG,
                    new Btn("Report an issue", Icons.BUG, Card.STYLE_PRIMARY, () -> beginReport(this::backHome, "The last " + killed + " did not finish: the app was closed while it worked.", "", "")),
                    new Btn("Not now", Icons.CLOSE, Card.STYLE_NORMAL, null));
            return;
        }
        if (f == null) return;
        interruptedKind = "";
        CrashReports.markSeen(f);
        dialog("The setup app closed unexpectedly", "It wrote a crash report. Sharing it helps to fix the problem. You find it later under Logs and reports too.", Icons.BUG,
                new Btn("Report an issue", Icons.BUG, Card.STYLE_PRIMARY, () -> beginReport(this::backHome, "The setup app closed unexpectedly (crash report " + f.getName() + ").", "", "")),
                new Btn("Share the report", Icons.SHARE, Card.STYLE_NORMAL, () -> shareFile(f, f.getName(), "Dissidia Recomp setup crash report")),
                new Btn("Save the report", Icons.SAVE, Card.STYLE_NORMAL, () -> saveFile(f, f.getName())),
                new Btn("Not now", Icons.CLOSE, Card.STYLE_NORMAL, null));
    }

    // ------------------------------------------------------------------------------------------------------------- report an issue

    private String reportText = "", reportFailure = "", reportStage = "", reportLines = "", reportStatus = "";
    private boolean reportSending;
    private Runnable reportBack;
    private ReportClient.Spec reportSpec;
    private ReportClient.Result reportResult;

    private void beginReport(Runnable back, String failure, String stage, String lines) {
        reportBack = back;
        reportFailure = failure == null ? "" : failure;
        reportStage = stage == null ? "" : stage;
        reportLines = lines == null ? "" : lines;
        reportStatus = "";
        reportResult = null;
        if (!reportSending) reportText = "";
        go(this::showReport);
    }

    private void showReport() {
        Page p = new Page(this, "report");
        p.back = reportSending ? null : reportBack;
        backdrop.setMood(BackdropView.MOOD_CRYSTAL);
        final boolean done = reportResult != null && reportResult.ok;
        p.head("Report an issue", done ? "Sent" : "What went wrong?", done
                ? "Thank you. The issue is open on GitHub, where you can follow it."
                : "Describe the problem in a few words: what you did, what you expected, what happened. The developer reads it on GitHub.");
        if (!done) {
            final EditText et = new EditText(this);
            et.setText(reportText);
            et.setHint("For example: the build stops at 40 percent");
            et.setHintTextColor(Theme.TEXT_FAINT);
            et.setTextColor(Theme.TEXT);
            et.setTextSize(15);
            et.setTypeface(Theme.body);
            et.setMinLines(3);
            et.setGravity(Gravity.TOP | Gravity.START);
            et.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE | android.text.InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
            et.setEnabled(!reportSending);
            et.setPadding(Theme.dp(12), Theme.dp(10), Theme.dp(12), Theme.dp(10));
            android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
            bg.setColor(0xC0101830);
            bg.setStroke(Theme.dp(1), 0x90E2C47A);
            bg.setCornerRadius(Theme.dp(8));
            et.setBackground(bg);
            et.addTextChangedListener(new android.text.TextWatcher() {
                public void beforeTextChanged(CharSequence a, int b, int c, int d) {}
                public void onTextChanged(CharSequence a, int b, int c, int d) { reportText = a.toString(); }
                public void afterTextChanged(android.text.Editable a) {}
            });
            p.headerExtra.add(et);
            p.focus = null;
        }
        // what is sent, exactly
        reportSpec = ReportClient.spec(this, new File(stWorkdir()), appVersion(), runKind, reportFailure, reportStage, reportLines, hasAllFilesAccess());
        StringBuilder sent = new StringBuilder("Your description; system information (app version, device, Android version, CPU, memory, free space");
        sent.append(reportFailure.isEmpty() ? "" : ", which step failed").append("); ");
        List<String> files = ReportClient.preview(reportSpec);
        if (files.isEmpty()) sent.append("no log files yet. ");
        else sent.append("files: ").append(android.text.TextUtils.join(", ", files)).append(". ");
        sent.append("Never sent: your game file, your saves, cloud settings. Paths and tokens in the logs are scrubbed.");
        p.card(new Card(this, Card.INFO, Icons.EYE, "What is sent", sent.toString()));
        if (!reportStatus.isEmpty()) p.card(new Card(this, Card.INFO, reportResult != null && reportResult.ok ? Icons.CHECK : Icons.WARN, reportSending ? "Sending" : reportResult != null && reportResult.ok ? reportStatus : "Not sent", reportSending ? reportStatus : reportResult != null && reportResult.ok ? "The issue opens in your browser." : reportStatus));
        if (reportResult != null && !reportResult.ok && !reportResult.savedPath.isEmpty()) {
            final File z = new File(reportResult.savedPath);
            p.card(new Card(this, Card.ACTION, Icons.SHARE, "Share the saved report", "A copy is saved at " + z.getPath() + ". Send it to the developer by hand.")).onClick(v -> shareZip(z));
            p.card(new Card(this, Card.ACTION, Icons.SAVE, "Save the report as a file", "Save the zip where you choose.")).onClick(v -> saveZip(z));
        }
        if (done) {
            final String u = reportResult.issueUrl;
            p.card(new Card(this, Card.ACTION, Icons.EYE, "Open the issue again", u).style(Card.STYLE_PRIMARY)).onClick(v -> openUrl(u));
        } else {
            Card send = p.card(new Card(this, Card.ACTION, Icons.UP, reportSending ? "Sending..." : "Send the report", "Sends it to the developer's issue tracker, then opens the new issue in your browser.").style(Card.STYLE_PRIMARY));
            if (!reportSending) send.onClick(v -> sendReport());
        }
        if (!reportSending) p.card(new Card(this, Card.ACTION, Icons.BACK, done ? "Done" : "Back", null).style(Card.STYLE_QUIET)).onClick(v -> { if (reportBack != null) reportBack.run(); });
        show(p, dirOf(p));
    }

    private void sendReport() {
        if (reportSending) return;
        if (reportText.trim().isEmpty() && reportFailure.isEmpty()) { snack("Write a few words about what went wrong first."); return; }
        reportSending = true;
        reportStatus = "Collecting and sending the report...";
        final String desc = reportText.trim() + (reportFailure.isEmpty() ? "" : (reportText.trim().isEmpty() ? "" : "\n\n") + "Failed: " + (reportStage.isEmpty() ? "" : reportStage + ": ") + reportFailure);
        final String title = "Installer (Android): " + ReportClient.first(reportText.trim().isEmpty() ? reportFailure : reportText, 70);
        final ReportClient.Spec spec = reportSpec;
        refreshPage();
        new Thread(() -> {
            final ReportClient.Result r = ReportClient.send(this, spec, title, desc);
            ui.post(() -> {
                reportSending = false;
                reportResult = r;
                reportStatus = r.text;
                if (r.ok) {
                    reportText = "";
                    if (!ReportClient.isTestService(this)) openUrl(r.issueUrl);   // (a test service's fake issue address is not worth a browser)
                    snack(r.text);
                }
                if (page != null && page.id.equals("report")) refreshPage();
            });
        }, "report").start();
    }

    private void openUrl(String u) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(u)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        } catch (Exception e) {
            snack("No browser found. The issue is at " + u);
        }
    }

    private void shareZip(File z) {
        try {
            File dst = new File(ShareProvider.dir(this), z.getName());
            try (InputStream in = new FileInputStream(z); OutputStream out = new FileOutputStream(dst)) {
                copy(in, out);
            }
            Uri u = ShareProvider.uri(z.getName());
            Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("application/zip");
            send.putExtra(Intent.EXTRA_STREAM, u);
            send.putExtra(Intent.EXTRA_SUBJECT, "Dissidia Recomp setup report");
            send.setClipData(ClipData.newRawUri(z.getName(), u));
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(send, "Share the report"));
        } catch (Exception e) {
            snack("Could not share: " + e.getMessage());
        }
    }

    private void saveZip(File z) {
        saveSource = z.getPath();
        try {
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("application/zip");
            i.putExtra(Intent.EXTRA_TITLE, z.getName());
            startActivityForResult(i, REQ_SAVE);
        } catch (Exception e) {
            snack("Could not open the file picker: " + e.getMessage());
        }
    }

    // ------------------------------------------------------------------------------------------------------------- activity results

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        if (req == REQ_SAVE) {
            if (res != RESULT_OK || data == null || data.getData() == null) return;
            try (InputStream in = new FileInputStream(saveSource); OutputStream out = getContentResolver().openOutputStream(data.getData(), "wt")) {
                copy(in, out);
                snack("Saved.");
            } catch (Exception e) {
                snack("Could not save: " + e.getMessage());
            }
            return;
        }
        if (req != REQ_PICK) {
            super.onActivityResult(req, res, data);
            return;
        }
        if (res != RESULT_OK || data == null || data.getData() == null) return;   // cancelled: the waiting page stays
        try {
            Uri u = data.getData();
            String name = "game";
            try (Cursor c = getContentResolver().query(u, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (c != null && c.moveToFirst()) name = c.getString(0);
            }
            pickedUriPath = uriToPath(u);
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(u, "r");
            int fd = pfd.detachFd();
            Engine.inheritFd(fd);   // the build tools inherit it (setup_from_iso.py "fd:N")
            picked("fd:" + fd, name);
        } catch (Exception e) {
            dialog("Cannot open the file", e.getMessage() == null ? e.toString() : e.getMessage(), Icons.WARN, new Btn("OK", Icons.CHECK, Card.STYLE_PRIMARY, null));
        }
    }

    /** The real file path of a picked document where Android's storage provider names one (primary:Dir/file, <card>:Dir/file), else "". */
    private String uriToPath(Uri u) {
        try {
            if ("com.android.externalstorage.documents".equals(u.getAuthority())) {
                String id = android.provider.DocumentsContract.getDocumentId(u);
                int c = id.indexOf(':');
                if (c > 0) {
                    String vol = id.substring(0, c), rel = id.substring(c + 1);
                    return (vol.equals("primary") ? "/storage/emulated/0" : "/storage/" + vol) + "/" + rel;
                }
            }
            try (Cursor cu = getContentResolver().query(u, new String[] {"_data"}, null, null, null)) {   // some providers still hand out the path
                if (cu != null && cu.moveToFirst() && cu.getString(0) != null && cu.getString(0).startsWith("/")) return cu.getString(0);
            }
        } catch (Exception e) {
            // no path: the extras' discs are asked for
        }
        return "";
    }

    // ------------------------------------------------------------------------------------------------------------- small things

    private String appVersion() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "";
        }
    }

    /**
     * What the player chose (setup::Options): check_updates and the extras by key (setup/extras.cpp; a second disc's path under its disc
     * key). Travels to the engine as install.ini lines; a missing key means the extra's default.
     */
    static final class Options {
        static final Map<String, String> defaults = new LinkedHashMap<>();   // from Engine.extrasJson
        static final ArrayList<String> discKeys = new ArrayList<>();
        static final ArrayList<String> actionKeys = new ArrayList<>();   // extras that are actions (add a texture pack): off again after a run
        private static final List<String> NOT_OPTIONS = Arrays.asList("workdir", "iso", "iso_ref", "version", "built_fp");
        final LinkedHashMap<String, String> m = new LinkedHashMap<>();

        static Options from(Map<String, String> state) {
            Options o = new Options();
            for (Map.Entry<String, String> e : state.entrySet())
                if (!NOT_OPTIONS.contains(e.getKey())) o.m.put(e.getKey(), e.getValue());
            return o;
        }

        static Options parse(String text) {
            Map<String, String> m = new LinkedHashMap<>();
            for (String l : text.split("\n")) {
                int eq = l.indexOf('=');
                if (eq > 0) m.put(l.substring(0, eq), l.substring(eq + 1));
            }
            return from(m);
        }

        String get(String key) {
            String v = m.get(key);
            if (v != null) return v;
            v = defaults.get(key);
            return v == null ? "" : v;
        }

        boolean on(String key) {
            return "1".equals(get(key));
        }

        void set(String key, String value) {
            m.put(key, value);
        }

        boolean checkUpdates() {
            return !"0".equals(m.get("check_updates"));
        }

        void clearDiscs() {
            for (String k : discKeys) m.put(k, "");
        }

        void clearActions() {
            for (String k : actionKeys) m.remove(k);
            m.remove("texture_file_name");
        }

        Options copy() {
            return parse(text());
        }

        void putInto(Map<String, String> state) {
            state.putAll(m);
        }

        String text() {
            StringBuilder b = new StringBuilder();
            for (Map.Entry<String, String> e : m.entrySet()) b.append(e.getKey()).append('=').append(e.getValue()).append('\n');
            return b.toString();
        }
    }

    /** Controller hints in the corner (A choose, B back, Y ...): only while a controller or keyboard is in use. */
    static final class HintBar extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private boolean back = false;
        private String y;
        private boolean pad;
        View version;

        HintBar(Context c) {
            super(c);
            setAlpha(0);
            setVisibility(GONE);
            p.setTypeface(Theme.bodyBold);
            p.setTextSize(Theme.dpf(12));
        }

        void reveal(boolean controller) {
            if (controller != pad) {
                pad = controller;
                requestLayout();
                invalidate();
            }
            if (controller) setVisibility(VISIBLE);
            animate().alpha(controller ? 1 : 0).setDuration(controller ? 200 : 400)
                    .withEndAction(() -> { if (!pad) setVisibility(GONE); }).start();   // gone: it takes no room from the title
            if (version != null) version.animate().alpha(controller ? 0 : 1).setDuration(300).start();
        }

        void setBack(boolean b) {
            back = b;
            requestLayout();
            invalidate();
        }

        void setY(String s) {
            y = s;
            requestLayout();
            invalidate();
        }

        private String[][] items() {
            ArrayList<String[]> l = new ArrayList<>();
            l.add(new String[] {"A", "Choose"});
            l.add(new String[] {"B", back ? "Back" : "Leave"});
            if (y != null) l.add(new String[] {"Y", y});
            return l.toArray(new String[0][]);
        }

        @Override
        protected void onMeasure(int w, int h) {
            float x = Theme.dpf(10);
            for (String[] it : items()) x += Theme.dpf(24) + p.measureText(it[1]) + Theme.dpf(14);
            setMeasuredDimension((int) x, Theme.dp(30));
        }

        @Override
        protected void onDraw(Canvas c) {
            float h = getHeight();
            p.setColor(0xD0101830);
            c.drawRoundRect(0, 0, getWidth(), h, h / 2, h / 2, p);
            float x = Theme.dpf(10);
            for (String[] it : items()) {
                int col = it[0].equals("A") ? 0xFF86EBCB : it[0].equals("B") ? 0xFFE0675A : 0xFFE2C47A;
                p.setColor(col);
                c.drawCircle(x + Theme.dpf(9), h / 2, Theme.dpf(9), p);
                p.setColor(0xFF0A0F22);
                p.setTextAlign(Paint.Align.CENTER);
                c.drawText(it[0], x + Theme.dpf(9), h / 2 - (p.ascent() + p.descent()) / 2, p);
                p.setTextAlign(Paint.Align.LEFT);
                p.setColor(Theme.TEXT);
                c.drawText(it[1], x + Theme.dpf(24), h / 2 - (p.ascent() + p.descent()) / 2, p);
                x += Theme.dpf(24) + p.measureText(it[1]) + Theme.dpf(14);
            }
        }
    }
}
