package org.psprecomp.setup;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * "Report an issue" of the setup app: the same report as the game's button. The zip (scrubbed logs + report.json with the system facts) is made by
 * the game's own code in the engine (runtime/src/psp_report.cpp, Engine.reportBuild); this class names the files, adds what only Java knows
 * (the last exit reason Android recorded, memory state), signs through the engine (the key stays in runtime/src/report_key.h) and POSTs to
 * <base>/v1/report. files/report_url.txt (one line) replaces the service for tests. Everything blocks: call it from a worker thread.
 */
final class ReportClient {
    static final String DEFAULT_BASE = "https://party.schindlershadow.com/dissidia-report";

    static final class Spec {
        String version, spaceDir;
        String[] files, facts;
        List<String> names = new ArrayList<>();
    }

    static final class Result {
        boolean ok;
        int number;
        String issueUrl = "", text = "", savedPath = "";
    }

    static String base(Context c) {
        File f = new File(c.getFilesDir(), "report_url.txt");
        if (f.isFile()) {
            try (FileInputStream in = new FileInputStream(f)) {
                String s = new String(readAll(in), "UTF-8").trim();
                if (!s.isEmpty()) return s.replaceAll("/+$", "");
            } catch (Exception e) {
                // the default
            }
        }
        return DEFAULT_BASE;
    }

    static boolean isTestService(Context c) {
        return !base(c).equals(DEFAULT_BASE);
    }

    /** What would be sent: the install log, the game's last run, the updater's log, crash reports; facts about the device and the failure. */
    static Spec spec(Context c, File workdir, String version, String runKind, String failure, String lastStep, String lastLines, boolean allFiles) {
        Spec s = new Spec();
        s.version = version;
        s.spaceDir = workdir.isDirectory() ? workdir.getPath() : c.getFilesDir().getPath();
        ArrayList<String> files = new ArrayList<>();
        add(files, s, "setup.log", new File(workdir, "setup.log"));
        try {
            File game = new File(Engine.gameDir(), "last_run.log");
            if (game.canRead()) add(files, s, "last_run.log", game);
            File prev = new File(Engine.gameDir(), "prev_run.log");
            if (prev.canRead()) add(files, s, "prev_run.log", prev);
        } catch (Throwable t) {
            // no game folder yet
        }
        add(files, s, "update.log", new File(c.getFilesDir(), "update/update.log"));
        for (File f : CrashReports.list(c)) add(files, s, f.getName(), f);
        s.files = files.toArray(new String[0]);
        ArrayList<String> f = new ArrayList<>();
        fact(f, "app_package", c.getPackageName());
        fact(f, "abis", Arrays.toString(Build.SUPPORTED_ABIS));
        fact(f, "build_fingerprint", Build.FINGERPRINT);
        fact(f, "run_kind", runKind);
        fact(f, "all_files_access", allFiles ? "granted" : "not granted");
        if (failure != null && !failure.isEmpty()) fact(f, "failure", failure);
        if (lastStep != null && !lastStep.isEmpty()) fact(f, "last_step", lastStep);
        if (lastLines != null && !lastLines.isEmpty()) fact(f, "last_lines", lastLines);
        try {
            ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            fact(f, "memory", (mi.availMem >> 20) + " MB available of " + (mi.totalMem >> 20) + " MB" + (mi.lowMemory ? " (low memory)" : ""));
            if (Build.VERSION.SDK_INT >= 30) {   // why Android ended this app the last times (low memory killer, crash, ...)
                List<ApplicationExitInfo> ex = am.getHistoricalProcessExitReasons(c.getPackageName(), 0, 3);
                StringBuilder b = new StringBuilder();
                for (ApplicationExitInfo i : ex) b.append(b.length() > 0 ? "; " : "").append(reasonName(i.getReason())).append(" (").append(i.getDescription()).append(")");
                if (b.length() > 0) fact(f, "last_exit_reasons", b.toString());
            }
        } catch (Throwable t) {
            // facts are a bonus
        }
        s.facts = f.toArray(new String[0]);
        return s;
    }

    private static String reasonName(int r) {
        switch (r) {
            case 1: return "exit self";
            case 2: return "signaled";
            case 3: return "low memory (killed)";
            case 4: return "crash";
            case 5: return "native crash";
            case 6: return "ANR";
            case 7: return "initialization failure";
            case 8: return "permission change";
            case 9: return "excessive resource usage";
            case 10: return "user requested";
            case 11: return "user stopped";
            case 12: return "dependency died";
            case 13: return "other";
            case 14: return "freezer";
            case 15: return "package state change";
            case 16: return "package updated";
            default: return "reason " + r;
        }
    }

    private static void fact(List<String> l, String k, String v) {
        l.add(k);
        l.add(v);
    }

    private static void add(List<String> l, Spec s, String rel, File f) {
        if (!f.isFile()) return;
        l.add(rel);
        l.add(f.getPath());
        s.names.add(rel);
    }

    /** "name (size)" lines for the page: what the report holds. */
    static List<String> preview(Spec s) {
        ArrayList<String> out = new ArrayList<>();
        try {
            for (String line : Engine.reportPreview(s.version, s.spaceDir, s.files, s.facts)) {
                int bar = line.lastIndexOf('|');
                if (bar < 0 || line.startsWith("report.json")) continue;
                out.add(line.substring(0, bar) + " (" + BuildRunner.size(Long.parseLong(line.substring(bar + 1))) + ")");
            }
        } catch (Throwable t) {
            // the page just lists less
        }
        return out;
    }

    /** Builds, signs and sends. A report that could not be sent stays as files/reports/report-<time>.zip (Result.savedPath). */
    static Result send(Context c, Spec s, String title, String desc) {
        Result r = new Result();
        File dir = new File(c.getFilesDir(), "reports");
        dir.mkdirs();
        File zip = new File(dir, "report-" + (System.currentTimeMillis() / 1000) + ".zip");
        String err = Engine.reportBuild(s.version, s.spaceDir, s.files, s.facts, title, desc, zip.getPath());
        if (!err.isEmpty()) {
            r.text = "Could not build the report: " + err;
            return r;
        }
        try {
            byte[] body;
            try (FileInputStream in = new FileInputStream(zip)) {
                body = readAll(in);
            }
            String time = Long.toString(System.currentTimeMillis() / 1000);
            String sig = Engine.reportSign(time, zip.getPath());
            HttpURLConnection h = (HttpURLConnection) new URL(base(c) + "/v1/report").openConnection();
            h.setRequestMethod("POST");
            h.setDoOutput(true);
            h.setConnectTimeout(15000);
            h.setReadTimeout(60000);
            h.setFixedLengthStreamingMode(body.length);
            h.setRequestProperty("Content-Type", "application/zip");
            h.setRequestProperty("X-Report-Time", time);
            h.setRequestProperty("X-Report-Sig", sig);
            try (OutputStream o = h.getOutputStream()) {
                o.write(body);
            }
            int code = h.getResponseCode();
            InputStream is = code >= 400 ? h.getErrorStream() : h.getInputStream();
            String answer = is == null ? "" : new String(readAll(is), "UTF-8");
            JSONObject j;
            try {
                j = new JSONObject(answer);
            } catch (Exception e) {
                j = new JSONObject();
            }
            if (code >= 200 && code < 300 && j.optBoolean("ok") && !j.optString("issue_url").isEmpty()) {
                r.ok = true;
                r.issueUrl = j.optString("issue_url");
                r.number = j.optInt("number");
                r.text = "Issue #" + r.number + " opened";
                zip.delete();
                return r;
            }
            String why = j.optString("error");
            r.text = "The report was not sent: " + (why.isEmpty() ? "the service answered " + code : why) + " (" + code + ")";
        } catch (Exception e) {
            r.text = "Could not reach the report service: " + e.getMessage();
        }
        r.savedPath = zip.getPath();
        return r;
    }

    private static byte[] readAll(InputStream in) throws java.io.IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        return b.toByteArray();
    }

    static String first(String s, int n) {
        s = s.trim().replace('\n', ' ');
        return s.length() <= n ? s : s.substring(0, n);
    }

    private ReportClient() {}
}
