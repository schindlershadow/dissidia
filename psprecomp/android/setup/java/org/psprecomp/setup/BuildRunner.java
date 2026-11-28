package org.psprecomp.setup;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Locale;

/**
 * Runs a plan (Engine.plan) on its own thread, one step after the other: commands through the engine (their output becomes progress there),
 * download steps with Downloader. Every output line goes to setup.log in the work folder and, as it arrives, to the listener (the live log
 * on the page). One runner at a time (current); the page reattaches to it after a rotation or when the app comes back.
 */
final class BuildRunner {
    interface Listener {
        void lines(String[] lines);
        void progress(JSONObject p);
        void finished(boolean ok, String error, String note);
    }

    static volatile BuildRunner current;
    /** Exists while a plan runs (set by the activity): found at the next start with no runner = the app was killed or crashed mid-install. */
    static volatile File marker;

    final String kind;
    final JSONArray plan;
    final File log;
    final ArrayList<String> history = new ArrayList<>();   // what the live log shows (capped)
    private final ArrayDeque<String> recent = new ArrayDeque<>();   // the last lines (the failure page shows them)
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile Listener listener;
    private volatile boolean cancel;
    private Downloader dl;
    volatile boolean running = true, ok = false;
    volatile String error = "", note = "";
    volatile JSONObject lastProgress;
    /** What was running when it failed (for the failure reason): the step's label, its exit code, and whether the player cancelled. */
    volatile String failStep = "";
    volatile int failCode = 0;
    volatile boolean wasCancelled = false;
    /** { kind, title, text } of Engine.explainFailure once the run failed (null: not failed, or the engine could not say). */
    volatile String[] reason;
    private Writer out;

    private final Context app;
    private final String opts;
    private final boolean resume;
    private final File doneFile;
    private final java.util.HashSet<String> done = new java.util.HashSet<>();

    /** resume = continue an interrupted run: the steps that finished are skipped (steps_done.txt next to the log); a step that was cut off runs again, and
     *  the build step itself continues where it stopped (psp_build keeps its translation stamp and every finished object, see android_build.py). */
    BuildRunner(Context ctx, String kind, JSONArray plan, File log, String opts, boolean resume) {
        this.app = ctx.getApplicationContext();
        this.opts = opts == null ? "" : opts;
        this.resume = resume;
        this.doneFile = new File(log.getParentFile(), "steps_done.txt");
        this.kind = kind;
        this.plan = plan;
        this.log = log;
    }

    void setListener(Listener l) {
        listener = l;
    }

    String recentText() {
        return recentText(14);
    }

    /** The last n output lines (the failure page shows 14; the failure reason reads 40). */
    String recentText(int n) {
        synchronized (recent) {
            StringBuilder b = new StringBuilder();
            int skip = Math.max(0, recent.size() - n), i = 0;
            for (String s : recent) if (i++ >= skip) b.append(s).append('\n');
            return b.toString().trim();
        }
    }

    void start() {
        current = this;
        new Thread(this::run, "setup-build").start();
    }

    void cancel() {
        cancel = true;
        Engine.kill();
        Downloader d = dl;
        if (d != null) d.cancel();
    }

    private void log(String s) {
        synchronized (history) {
            history.add(s);
            if (history.size() > 6000) history.subList(0, 1000).clear();
        }
        if (!s.trim().isEmpty() && !s.startsWith("FAILED:") && !s.startsWith("Reason (")) {
            synchronized (recent) {
                recent.addLast(s);
                while (recent.size() > 40) recent.removeFirst();
            }
        }
        try {
            if (out != null) {
                out.write(s);
                out.write('\n');
                out.flush();   // a line is on disk before the next runs: a crash or a kill still leaves the log
            }
        } catch (Exception e) {
            // the log file is a convenience
        }
    }

    private void emit(String[] lines) {
        final Listener l = listener;
        if (l != null && lines != null) ui.post(() -> { if (listener == l) l.lines(lines); });
    }

    private long lastProg = 0;

    private void emitProgress(boolean force) {
        long now = System.currentTimeMillis();
        if (!force && now - lastProg < 200) return;
        lastProg = now;
        try {
            JSONObject p = new JSONObject(Engine.progress());
            lastProgress = p;
            final Listener l = listener;
            if (l != null) ui.post(() -> { if (listener == l) l.progress(p); });
        } catch (Exception e) {
            // never mind one frame
        }
    }

    private void say(String s) {
        log(s);
        emit(new String[] {s});
    }

    private void run() {
        try {
            run2();
        } finally {   // whatever ended the build (done, failed, cancelled, an exception): the wake lock and the service go
            BuildService.stop(app);
            if (running) {
                running = false;
                if (error.isEmpty()) error = "The setup stopped unexpectedly.";
                final Listener l = listener;
                if (l != null) ui.post(() -> { if (listener == l) l.finished(false, error, note); });
            }
            if (listener == null) BuildService.finished(app, ok, ok ? "Open the setup app and install the game." : error);
        }
    }

    private void run2() {
        try {
            log.getParentFile().mkdirs();
            out = new OutputStreamWriter(new FileOutputStream(log, true), "UTF-8");
        } catch (Exception e) {
            out = null;
        }
        try {
            File m = marker;
            if (m != null) {
                m.getParentFile().mkdirs();
                try (FileOutputStream mo = new FileOutputStream(m)) {
                    mo.write((kind + "\n" + opts).getBytes("UTF-8"));   // what "Resume the build" needs after a kill
                }
            }
        } catch (Exception e) {
            // no marker: the next start cannot tell
        }
        String stamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());
        say("===== psprecomp_setup run (" + kind + ", " + stamp + ") =====");
        String fail = null;
        if (resume) {
            try (java.io.BufferedReader br = new java.io.BufferedReader(new java.io.InputStreamReader(new java.io.FileInputStream(doneFile), "UTF-8"))) {
                String l;
                while ((l = br.readLine()) != null) done.add(l);
            } catch (Exception e) {
                // nothing recorded: everything runs
            }
            say("Resuming: " + done.size() + " step(s) were finished earlier.");
        } else {
            doneFile.delete();
        }
        try {
            for (int i = 0; i < plan.length() && fail == null; i++) {
                JSONObject step = plan.getJSONObject(i);
                final String stepKey = i + "|" + step.optString("label");
                if (done.contains(stepKey)) {
                    say("===== step " + (i + 1) + "/" + plan.length() + ": " + step.optString("label") + " (done earlier, skipped) =====");
                    continue;
                }
                say("===== step " + (i + 1) + "/" + plan.length() + ": " + step.optString("label") + " =====");
                failStep = step.optString("label");
                String err = Engine.startStep(i);
                emitProgress(true);
                if (!err.isEmpty()) { fail = "Could not start: " + err; break; }
                JSONArray dls = step.optJSONArray("downloads");
                int code = 0;
                if (dls != null && dls.length() > 0) {
                    String r = downloads(dls);
                    if (r != null) {
                        fail = cancel ? "The installation was cancelled." : "The download failed (" + r + "). Check your connection and try again; finished files are kept.";
                        break;
                    }
                } else {
                    while (true) {
                        String[] lines = Engine.readLines();
                        if (lines != null) {
                            for (String s : lines) log(s);
                            emit(lines);
                        }
                        emitProgress(false);
                        if (lines == null && !Engine.busy()) break;
                        if (lines == null) Thread.sleep(60);
                    }
                    code = Engine.exitCode();
                }
                failCode = code;
                if (cancel) { fail = "The installation was cancelled."; break; }
                JSONObject p = new JSONObject(Engine.progress());
                String perr = p.optString("error");
                if (code != 0 || !perr.isEmpty()) {
                    if (step.optBoolean("optional")) {
                        String why = perr.isEmpty() ? p.optString("last") : perr;
                        note = step.optString("label") + " did not work" + (why.isEmpty() ? "." : ": " + why);
                        say("optional step failed: " + note);
                        Engine.clearError();
                    } else {
                        fail = perr.isEmpty() ? "A step failed (exit code " + code + "). The log shows what happened." : perr + ".";
                    }
                }
                if (fail == null) {
                    try (FileOutputStream df = new FileOutputStream(doneFile, true)) {
                        df.write((stepKey + "\n").getBytes("UTF-8"));
                    } catch (Exception e) {
                        // not recorded: the step runs again on a resume
                    }
                }
            }
        } catch (Exception e) {
            fail = "The setup stopped: " + e;
        }
        wasCancelled = cancel;
        if (fail != null) {
            try {
                String stage = "";
                try { stage = new JSONObject(Engine.progress()).optString("stage"); } catch (Exception e2) { /* no stage */ }
                reason = Engine.explainFailure(failStep, stage, fail, recentText(40), log.getParentFile().getPath(), failCode, cancel);
            } catch (Throwable t) {
                reason = null;
            }
            say("FAILED: " + fail);
            if (reason != null && reason.length >= 3) say("Reason (" + reason[0] + "): " + reason[2].replace("\n\n", " | ").replace("\n", " "));
        }
        if (fail == null) say("===== finished =====");
        emitProgress(true);
        try {
            if (out != null) out.close();
        } catch (Exception e) {
            // closed anyway
        }
        File m = marker;
        if (m != null) m.delete();
        ok = fail == null;
        if (ok) doneFile.delete();
        error = fail == null ? "" : fail;
        running = false;
        final Listener l = listener;
        if (l != null) ui.post(() -> { if (listener == l) l.finished(ok, error, note); });
    }

    /** A download step: every file that is not there yet (a finished one from an earlier try is kept). null = done. */
    private String downloads(JSONArray dls) throws Exception {
        int n = dls.length();
        for (int k = 0; k < n; k++) {
            JSONObject d = dls.getJSONObject(k);
            File f = new File(d.getString("file"));
            if (f.isFile()) { say("already downloaded: " + f.getName()); continue; }
            f.getParentFile().mkdirs();
            final int idx = k;
            say("downloading " + d.getString("url"));
            dl = new Downloader();
            if (cancel) return "cancelled";
            final long[] lastLog = {0};
            String r = dl.fetch(d.getString("url"), f, (done, total) -> {
                double frac = total > 0 ? (double) done / total : 0;
                String detail = "File " + (idx + 1) + " of " + n + ": " + size(done) + (total > 0 ? " of " + size(total) : "");
                Engine.noteDownload((idx + frac) / n, detail);
                if (done - lastLog[0] >= (50L << 20)) {   // a log line every 50 MB
                    lastLog[0] = done;
                    say("  " + detail);
                }
                emitProgress(false);
            });
            dl = null;
            if (r != null) return r;
            say("  done: " + f.getName() + " (" + size(f.length()) + ")");
        }
        return null;
    }

    static String size(long n) {
        if (n >= (1L << 30)) return String.format(Locale.US, "%.1f GB", n / (double) (1L << 30));
        if (n >= (1L << 20)) return String.format(Locale.US, "%.0f MB", n / (double) (1L << 20));
        return (n >> 10) + " KB";
    }
}
