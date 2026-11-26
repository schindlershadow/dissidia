// The engine of the Android setup app: the install logic of install.cpp / proc.cpp / payload.cpp for the native Java UI
// (android/setup/java/org/psprecomp/setup/, class Engine). No SDL and no drawing here: Java shows the pages and drives the flow, this file
// unpacks the payload, makes the plans (the same plan_*_android as the desktop wizard's Android build used), runs their commands (process
// group, so a cancel takes python + zig down together), turns their output into progress (feed_line) and keeps install.ini.
// See docs/android.md "Building on the phone".
#ifdef __ANDROID__
#include <fcntl.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "extras.h"
#include "install.h"
#include "payload.h"
#include "proc.h"
#include "psp_report.h"
#include "report_key.h"

namespace fs = std::filesystem;

namespace {
std::mutex g_mu;
setup::Layout g_layout;
std::string g_root, g_problem = "the app has not been prepared yet";
std::vector<setup::Step> g_plan;
setup::Progress g_prog;
setup::Proc g_proc;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string str(JNIEnv* e, jstring s) {
    if (!s) return "";
    const char* u = e->GetStringUTFChars(s, nullptr);
    std::string r = u ? u : "";
    if (u) e->ReleaseStringUTFChars(s, u);
    return r;
}

// Strings from the tools are not always valid UTF-8 (NewStringUTF aborts the process on bad input): keep ASCII, replace the rest.
jstring jstr(JNIEnv* e, const std::string& s) {
    std::string clean;
    clean.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n > 0 && i + (size_t)n <= s.size();
        for (int k = 1; ok && k < n; k++) ok = ((unsigned char)s[i + (size_t)k] >> 6) == 2;
        if (ok && n == 4) { clean += '?'; i += 4; continue; }   // modified UTF-8 has no 4-byte form
        if (!ok) { clean += '?'; i++; continue; }
        clean.append(s, i, (size_t)n);
        i += (size_t)n;
    }
    return e->NewStringUTF(clean.c_str());
}

std::string json_esc(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

// Options travel as "key=value" lines (the install.ini format): check_updates, android and every extra / disc key of setup/extras.cpp.
setup::Options parse_options(const std::string& text) {
    setup::Options o;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "check_updates") o.check_updates = v != "0";
        else if (k == "android") o.android = v == "1";
        else if (k != "workdir" && k != "iso" && k != "version" && k != "built_fp") o.set(k, v);
    }
    return o;
}

std::string options_text(const setup::Options& o) {
    std::ostringstream s;
    s << "check_updates=" << o.check_updates << "\nandroid=" << o.android << "\n";
    for (const setup::Extra& e : setup::extras()) {
        s << e.key << "=" << o.get(e.key) << "\n";
        if (!e.disc.key.empty()) s << e.disc.key << "=" << o.get(e.disc.key) << "\n";
    }
    return s.str();
}

// A plan that only prints what a build prints (for testing the app's pages where the bundled tools cannot run, e.g. the x86_64 emulator,
// whose arm64 translation does not load the musl Python). Used when files/ui_demo exists (only `run-as` on a debuggable build can make it);
// "fail" in that file makes the build step fail.
std::vector<setup::Step> demo_plan(const std::string& work, const std::string& fail) {   // fail: "" = none, else compile / mem / space / iso
    auto sh = [](const std::string& script) { return std::vector<std::string>{"/system/bin/sh", "-c", script}; };
    setup::Step x, b, c;
    x.label = "Reading your game file";
    x.explain = "Reads the game's files out of your disc image. Nothing is uploaded. (demo)";
    x.argv = sh(fail == "iso" ? "echo '[1/4] Reading ISO x.iso'; sleep 1; echo \"error: not a 2048-byte-sector ISO 9660 image (no 'CD001' at sector 16)\"; exit 3"
                              : "i=0; while [ $i -lt 30 ]; do echo \"extract: PSP_GAME/USRDIR/file_$i.bin\"; i=$((i+1)); sleep 0.08; done; echo 'decrypting EBOOT.BIN'; sleep 0.5");
    x.weight = 0.10;
    b.label = "Building the game on this device";
    b.explain = "Translates the game and compiles it here: the long part. (demo)";
    b.is_build = true;
    b.argv = sh(std::string("echo '[psp-build] STEP 1/5 Preparing the analysis'; for p in 20 60 100; do echo \"[psp-build] PROGRESS $p\"; sleep 0.4; done; "
                            "echo '[psp-build] STEP 2/5 Translating the game'; for p in 10 40 80 100; do echo \"[psp-build] PROGRESS $p\"; sleep 0.4; done; "
                            "echo '[psp-build] STEP 3/5 Translating the overlays'; sleep 0.6; echo '[psp-build] STEP 4/5 Configuring'; sleep 0.4; "
                            "echo '[psp-build] STEP 5/5 Compiling the game'; i=1; while [ $i -le 160 ]; do echo \"[$i/160] Building CXX object generated/batch_$i.cpp.o\"; ") +
                (fail == "compile" ? "if [ $i = 70 ]; then echo 'generated/batch_70.cpp:1234:5: error: use of undeclared identifier'; echo '[psp-build] ERROR compiling batch_70.cpp failed'; exit 1; fi; " :
                 fail == "space" ? "if [ $i = 20 ]; then echo '/data/user/0/org.psprecomp.setup/files/work/output/generated/batch_0000.cpp:1:1: error: CacheCheckFailed'; echo '[psp-build] ERROR compiling failed:'; exit 1; fi; " :
                 fail == "mem" ? "if [ $i = 40 ]; then kill -9 $$; fi; " : "") +
                "if [ $i = 33 ]; then echo 'warning: argument unused during compilation'; fi; if [ $((i % 8)) = 0 ]; then echo \"[psp-build] PROGRESS $((i * 100 / 160))\"; fi; i=$((i+1)); sleep 0.12; done; echo 'Linking libmain.so'; sleep 1");
    b.weight = 0.80;
    c.label = "Putting the game files where the game can read them";
    c.explain = "Copies the game's files to where the game reads them. (demo)";
    c.argv = sh("for d in disc0 ms0 mods overlays; do echo \"copying $d\"; sleep 0.5; done; echo done");
    c.weight = 0.10;
    std::vector<setup::Step> p{x, b, c};
    double base = 0;
    for (auto& s : p) { s.base = base; base += s.weight; }
    (void)work;
    return p;
}

std::string plan_json(const std::vector<setup::Step>& p) {
    std::string o = "[";
    for (size_t i = 0; i < p.size(); i++) {
        const auto& s = p[i];
        if (i) o += ",";
        o += "{\"label\":" + json_esc(s.label) + ",\"explain\":" + json_esc(s.explain) + ",\"weight\":" + std::to_string(s.weight) +
             ",\"base\":" + std::to_string(s.base) + ",\"optional\":" + (s.optional ? "true" : "false") + ",\"build\":" + (s.is_build ? "true" : "false") +
             ",\"downloads\":[";
        for (size_t k = 0; k < s.downloads.size(); k++)
            o += (k ? "," : "") + std::string("{\"url\":") + json_esc(s.downloads[k].first) + ",\"file\":" + json_esc(s.downloads[k].second) + "}";
        o += "]}";
    }
    return o + "]";
}

jobjectArray to_array(JNIEnv* e, const std::vector<std::string>& v) {
    jobjectArray a = e->NewObjectArray((jsize)v.size(), e->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < v.size(); i++) {
        jstring s = jstr(e, v[i]);
        e->SetObjectArrayElement(a, (jsize)i, s);
        e->DeleteLocalRef(s);
    }
    return a;
}
}  // namespace

#define JNI(ret, name) extern "C" JNIEXPORT ret JNICALL Java_org_psprecomp_setup_Engine_##name

// Unpacks lib/libpayload.so (the zip with Python's library, zig's lib, the NDK sysroot, the Android core, the game's tools) into
// files/release-<version> the first time (and after an app update), links the loader the patched Python asks for, finds the tools.
// cb.onProgress(double) is called on this thread. Returns "" or the reason it failed.
JNI(jstring, bootstrap)(JNIEnv* e, jclass, jstring jlib, jstring jfiles, jobject cb) {
    const std::string lib = str(e, jlib), files = str(e, jfiles);
    std::error_code ec;
    fs::remove(fs::path(files) / "ld-musl", ec);
    fs::create_symlink(fs::path(lib) / "libmusl.so", fs::path(files) / "ld-musl", ec);
    fs::path zip = fs::path(lib) / "libpayload.so";
    if (!fs::exists(zip, ec)) return jstr(e, "this app has no payload (libpayload.so)");
    setup::Payload pl;
    pl.offset = 0;
    pl.size = (uint64_t)fs::file_size(zip, ec);
    {   // the version is the size plus a checksum of the zip's tail (its central directory lists every file's CRC): a rebuilt payload is a new one
        uint32_t h = 2166136261u;
        if (FILE* f = std::fopen(zip.string().c_str(), "rb")) {
            long tail = (long)std::min<uint64_t>(pl.size, 1 << 16);
            std::fseek(f, -tail, SEEK_END);
            unsigned char b[4096];
            size_t n;
            while ((n = std::fread(b, 1, sizeof b, f)) > 0)
                for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
            std::fclose(f);
        }
        pl.version = std::to_string(pl.size) + "-" + std::to_string(h);
    }
    fs::path dest = fs::path(files) / ("release-" + pl.version);
    jclass cc = cb ? e->GetObjectClass(cb) : nullptr;
    jmethodID m = cc ? e->GetMethodID(cc, "onProgress", "(D)V") : nullptr;
    int last = -1;
    std::string err = setup::extract_payload(zip, pl, dest, [&](double f) {
        int pm = (int)(f * 1000);
        if (pm != last && m) { last = pm; e->CallVoidMethod(cb, m, (jdouble)f); }
        return true;
    });
    if (!err.empty()) return jstr(e, err);
    for (auto& d : fs::directory_iterator(files, ec))   // an older unpack is not needed any more
        if (d.path().filename().string().rfind("release-", 0) == 0 && d.path() != dest) fs::remove_all(d.path(), ec);
    std::lock_guard<std::mutex> lk(g_mu);
    g_root = dest.string();
    g_layout = setup::find_layout_android(g_root, lib);
    g_problem = g_layout.problem();
    return jstr(e, "");
}

// The extras (setup/extras.cpp) as JSON: the app's Options page is made from it, so a new extra appears there without Java changes.
JNI(jstring, extrasJson)(JNIEnv* e, jclass) { return jstr(e, setup::extras_json(g_layout, true)); }

// The extras that need their disc picked first (prev = installed options for Change extras, null for an install).
JNI(jobjectArray, extrasNeedingDisc)(JNIEnv* e, jclass, jstring jopts, jstring jprev) {
    const setup::Options o = parse_options(str(e, jopts));
    if (!jprev) return to_array(e, setup::extras_needing_disc(g_layout, true, o, nullptr));
    const setup::Options prev = parse_options(str(e, jprev));
    return to_array(e, setup::extras_needing_disc(g_layout, true, o, &prev));
}

// Is the picked file the right disc for this extra (setup/extras.cpp ExtraDisc::check: the Dissidia 1 / undub checks)? "" = yes, else
// what is wrong with it. The path may be "fd:N"; the descriptor's offset is not moved.
JNI(jstring, discProblem)(JNIEnv* e, jclass, jstring key, jstring path) { return jstr(e, setup::extra_disc_problem(str(e, key), str(e, path))); }

// Looks for the extra's disc in the folder of the first image (a real path): "" = none, ask the player.
JNI(jstring, findExtraDisc)(JNIEnv* e, jclass, jstring key, jstring near_file) { return jstr(e, setup::find_extra_disc(str(e, key), str(e, near_file))); }

// Free space the install (kind "install" / "rebuild") or the extras need, and the failure text when the work folder has less.
JNI(jlong, neededBytes)(JNIEnv* e, jclass, jstring jopts, jstring jwork, jboolean extras_only) {
    const std::string os = str(e, jopts);
    setup::Options o = parse_options(os);
    if (os == "@none") for (const setup::Extra& x : setup::extras()) o.set(x.key, "0");   // the build only (a rebuild downloads nothing)
    return (jlong)setup::needed_bytes(true, str(e, jwork), o, extras_only);
}
JNI(jstring, spaceProblem)(JNIEnv* e, jclass, jstring jwork, jlong need) { return jstr(e, setup::space_problem(str(e, jwork), (long long)need, true)); }

// Why a step failed, in words (setup/install.cpp explain_failure): { kind, title, text }.
JNI(jobjectArray, explainFailure)(JNIEnv* e, jclass, jstring step, jstring stage, jstring error, jstring tail, jstring work, jint code, jboolean cancelled) {
    setup::FailureCtx c;
    c.step = str(e, step); c.stage = str(e, stage); c.error = str(e, error); c.tail = str(e, tail); c.workdir = str(e, work);
    c.exit_code = code; c.android = true; c.cancelled = cancelled;
    const setup::Failure f = setup::explain_failure(c);
    return to_array(e, {f.kind, f.title(), f.text()});
}

JNI(jstring, layoutProblem)(JNIEnv* e, jclass) { return jstr(e, g_problem); }
JNI(jboolean, feature)(JNIEnv* e, jclass, jstring name) { return setup::feature_available(g_layout, str(e, name)); }
JNI(jstring, payloadFingerprint)(JNIEnv* e, jclass) { return jstr(e, g_layout.android_dir.empty() ? "" : setup::android_payload_fingerprint(g_layout)); }
JNI(jstring, gameDir)(JNIEnv* e, jclass) { return jstr(e, setup::android_game_dir()); }
JNI(jstring, releaseRoot)(JNIEnv* e, jclass) { return jstr(e, g_root); }

// install.ini <-> "key=value" lines (workdir, iso, version, built_fp and the options)
JNI(jstring, loadState)(JNIEnv* e, jclass, jstring file) {
    setup::State s = setup::load_state(str(e, file));
    return jstr(e, "workdir=" + s.workdir + "\niso=" + s.iso + "\nversion=" + s.version + "\nbuilt_fp=" + s.built_fp + "\n" + options_text(s.options));
}

JNI(jboolean, saveState)(JNIEnv* e, jclass, jstring file, jstring text) {
    const std::string t = str(e, text);
    setup::State s;
    s.options = parse_options(t);
    std::istringstream is(t);
    std::string line;
    while (std::getline(is, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "workdir") s.workdir = v;
        else if (k == "iso") s.iso = v;
        else if (k == "version") s.version = v;
        else if (k == "built_fp") s.built_fp = v;
    }
    return setup::save_state(str(e, file), s);
}

// Makes the plan: kind "install" (iso + options), "rebuild" (the game again from the extracted work folder), "extras" (Change extras:
// prev = the installed options; removes what was turned off, adds what was turned on, then the copy). Returns the steps as JSON
// (label, explain, weight, base, optional, build, downloads[{url,file}]).
JNI(jstring, plan)(JNIEnv* e, jclass, jstring jkind, jstring jiso, jstring jwork, jstring jopts, jstring jprev) {
    const std::string kind = str(e, jkind), iso = str(e, jiso), work = str(e, jwork);
    const setup::Options o = parse_options(str(e, jopts)), prev = parse_options(str(e, jprev));
    std::lock_guard<std::mutex> lk(g_mu);
    if (kind == "demo" || kind.compare(0, 9, "demo-fail") == 0) g_plan = demo_plan(work, kind.size() > 10 ? kind.substr(10) : kind == "demo-fail" ? "compile" : "");
    else if (kind == "install") g_plan = setup::plan_install_android(g_layout, iso, work, o);
    else if (kind == "rebuild") g_plan = setup::plan_rebuild_android(g_layout, work);
    else g_plan = setup::plan_extras_android(g_layout, work, prev, o);
    g_prog = setup::Progress();
    g_prog.start_ms = now_ms();
    if (!g_plan.empty()) { g_prog.stage = g_plan[0].label; g_prog.explain = g_plan[0].explain; }
    return jstr(e, plan_json(g_plan));
}

// Moves the progress to step i (stage, explain, counters reset) and, unless it is a download step (done by Java), starts its command.
// Returns "" or why it could not start.
JNI(jstring, startStep)(JNIEnv* e, jclass, jint i) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (i < 0 || i >= (jint)g_plan.size()) return jstr(e, "no such step");
    if (i > 0) g_prog.fraction = std::max(g_prog.fraction, setup::step_done_fraction(g_plan, i - 1));
    g_prog.step = i;
    g_prog.stage = g_plan[(size_t)i].label;
    g_prog.explain = g_plan[(size_t)i].explain;
    g_prog.detail.clear();
    g_prog.last_line.clear();
    g_prog.error.clear();
    g_prog.sub = 0;
    g_prog.compile_i = g_prog.compile_n = 0;
    if (!g_plan[(size_t)i].downloads.empty()) return jstr(e, "");
    if (!g_proc.start(g_plan[(size_t)i].argv, g_plan[(size_t)i].env, "")) return jstr(e, g_proc.error().empty() ? "cannot start the step" : g_proc.error());
    return jstr(e, "");
}

// Every output line that arrived since the last call, fed into the progress model; null when there is none.
JNI(jobjectArray, readLines)(JNIEnv* e, jclass) {
    std::vector<std::string> out;
    std::string line;
    std::lock_guard<std::mutex> lk(g_mu);
    while (out.size() < 2000 && g_proc.read_line(line)) {
        if (!g_plan.empty()) setup::feed_line(g_prog, g_plan, line, now_ms());
        out.push_back(line);
    }
    return out.empty() ? nullptr : to_array(e, out);
}

JNI(jboolean, busy)(JNIEnv*, jclass) { return g_proc.busy(); }
JNI(jint, exitCode)(JNIEnv*, jclass) { return g_proc.exit_code(); }
JNI(void, kill)(JNIEnv*, jclass) { g_proc.kill(); }

// A download step's progress (Java runs downloads with Android's HTTPS stack): the fraction of this step that is done, and what to show.
JNI(void, noteDownload)(JNIEnv* e, jclass, jdouble f, jstring detail) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_prog.step >= (int)g_plan.size()) return;
    const setup::Step& s = g_plan[(size_t)g_prog.step];
    g_prog.fraction = std::max(g_prog.fraction, s.base + s.weight * std::min(1.0, std::max(0.0, (double)f)));
    g_prog.detail = str(e, detail);
}

// {step, fraction, stage, explain, detail, error, last, elapsed, eta}
JNI(jstring, progress)(JNIEnv* e, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    const long long t = now_ms();
    const setup::Progress& p = g_prog;
    std::string j = "{\"step\":" + std::to_string(p.step) + ",\"fraction\":" + std::to_string(p.fraction) + ",\"stage\":" + json_esc(p.stage) +
                    ",\"explain\":" + json_esc(p.explain) + ",\"detail\":" + json_esc(p.detail) + ",\"error\":" + json_esc(p.error) +
                    ",\"last\":" + json_esc(p.last_line) + ",\"elapsed\":" + json_esc(setup::elapsed_text(p, t)) + ",\"eta\":" + json_esc(setup::eta_text(p, t)) +
                    ",\"compile_i\":" + std::to_string(p.compile_i) + ",\"compile_n\":" + std::to_string(p.compile_n) + "}";
    return jstr(e, j);
}

JNI(void, clearError)(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_prog.error.clear();
}

// The command lines of the updater (update.py android-check / android-verify) and the environment of the bundled Python.
JNI(jobjectArray, checkArgv)(JNIEnv* e, jclass, jstring manifest, jstring sig, jstring current) {
    return to_array(e, setup::android_check_argv(g_layout, str(e, manifest), str(e, sig), str(e, current)));
}
JNI(jobjectArray, verifyArgv)(JNIEnv* e, jclass, jstring file, jstring sha, jlong size) {
    return to_array(e, setup::android_verify_argv(g_layout, str(e, file), str(e, sha), (long long)size));
}

// Runs a short tool (the update checks) to the end and returns its output lines; blocking (call from a worker thread). timeout_ms kills it.
JNI(jobjectArray, runTool)(JNIEnv* e, jclass, jobjectArray jargv, jstring jwork, jint timeout_ms) {
    std::vector<std::string> argv;
    for (jsize i = 0, n = e->GetArrayLength(jargv); i < n; i++) {
        jstring s = (jstring)e->GetObjectArrayElement(jargv, i);
        argv.push_back(str(e, s));
        e->DeleteLocalRef(s);
    }
    setup::Proc p;
    std::vector<std::string> out;
    if (!p.start(argv, setup::android_env(g_layout, str(e, jwork)), "")) {
        out.push_back("ERROR cannot run the checker: " + p.error());
        return to_array(e, out);
    }
    const long long t0 = now_ms();
    std::string line;
    while (p.busy()) {
        while (p.read_line(line)) out.push_back(line);
        if (now_ms() - t0 > timeout_ms) { p.kill(); out.push_back("ERROR timed out"); break; }
        usleep(20000);
    }
    while (p.read_line(line)) out.push_back(line);
    out.push_back("EXIT " + std::to_string(p.exit_code()));
    return to_array(e, out);
}

// "UPDATE version=... " -> {available, version, current, error, url, sha256, size}
JNI(jstring, parseUpdate)(JNIEnv* e, jclass, jstring line) {
    setup::UpdateInfo u = setup::parse_update_line(str(e, line));
    return jstr(e, std::string("{\"available\":") + (u.available ? "true" : "false") + ",\"version\":" + json_esc(u.version) + ",\"current\":" + json_esc(u.current) +
                       ",\"error\":" + json_esc(u.error) + ",\"url\":" + json_esc(u.url) + ",\"sha256\":" + json_esc(u.sha256) + ",\"size\":" + std::to_string(u.size) + "}");
}

// The disc image arrives as a file descriptor from Android's picker: the build tools inherit it ("fd:N", setup_from_iso.py).
JNI(void, inheritFd)(JNIEnv*, jclass, jint fd) {
    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0) fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC);
}

// ---- Report an issue (runtime/src/psp_report.cpp, the game's code: scrubbing, zip, system facts; the Java side does the HTTPS POST) ----
// flat string arrays: files = rel, path, rel, path ...; facts = key, value, key, value ...
static void report_product(JNIEnv* e, jstring version, jstring space_dir, jobjectArray files, jobjectArray facts) {
    auto strs = [&](jobjectArray a) {
        std::vector<std::string> v;
        for (jsize i = 0, n = a ? e->GetArrayLength(a) : 0; i < n; i++) {
            jstring s = (jstring)e->GetObjectArrayElement(a, i);
            v.push_back(str(e, s));
            e->DeleteLocalRef(s);
        }
        return v;
    };
    psp_report::Product p;
    p.name = "installer";
    p.version = str(e, version);
    p.space_dir = str(e, space_dir);
    const auto f = strs(files), k = strs(facts);
    for (size_t i = 0; i + 1 < f.size(); i += 2) p.files.push_back({f[i], f[i + 1], 8u << 20});
    for (size_t i = 0; i + 1 < k.size(); i += 2) p.facts.push_back({k[i], k[i + 1]});
    psp_report::set_product(p);
}

// The files a report holds: "name|bytes" lines (the page shows them before anything is sent).
JNI(jobjectArray, reportPreview)(JNIEnv* e, jclass, jstring version, jstring space_dir, jobjectArray files, jobjectArray facts) {
    report_product(e, version, space_dir, files, facts);
    std::vector<std::string> out;
    for (const auto& f : psp_report::preview(false)) out.push_back(f.name + "|" + std::to_string(f.size));
    return to_array(e, out);
}

// Builds the zip (scrubbed logs + report.json with the system facts) into `out`. "" = ok, else the error.
JNI(jstring, reportBuild)(JNIEnv* e, jclass, jstring version, jstring space_dir, jobjectArray files, jobjectArray facts, jstring title, jstring desc, jstring out) {
    report_product(e, version, space_dir, files, facts);
    psp_report::Params p;
    p.title = str(e, title);
    p.description = str(e, desc);
    p.include_dump = false;
    std::string err;
    if (!psp_report::build_zip_file(p, str(e, out), &err)) return jstr(e, err.empty() ? "could not build the report" : err);
    return jstr(e, "");
}

// X-Report-Sig for a zip file: HMAC-SHA256(report_key.h's key, "<time>\n<sha256 of the file>"). The key lives only in report_key.h.
JNI(jstring, reportSign)(JNIEnv* e, jclass, jstring time, jstring zip) {
    std::ifstream f(str(e, zip), std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return jstr(e, psp_report::hmac_sha256_hex(PSPRECOMP_REPORT_KEY, str(e, time) + "\n" + psp_report::sha256_hex(reinterpret_cast<const uint8_t*>(b.data()), b.size())));
}

JNI(jlong, freeSpace)(JNIEnv* e, jclass, jstring path) { return (jlong)setup::free_space(str(e, path)); }
#endif
