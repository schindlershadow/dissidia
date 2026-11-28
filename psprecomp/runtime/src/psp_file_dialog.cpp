// The operating system's file / folder picker: see include/psp_file_dialog.h.
#include "psp_file_dialog.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
#include <SDL.h>
#include <jni.h>
#else
// The installer's file dialogs (Windows IFileDialog, zenity / kdialog / matedialog / qarma on Linux, osascript on macOS); used
// asynchronously (ready(0) every frame).
#include "../../setup/third_party/portable-file-dialogs.h"
#endif

namespace fs = std::filesystem;

namespace psp_filedlg {
namespace {

std::mutex g_mu;
State g_state = State::Idle;
std::string g_path, g_error, g_scratch;
Kind g_kind = Kind::ImportFolder;
unsigned g_generation = 0;   // an abandoned dialog's late answer is ignored

void finish(unsigned gen, State s, const std::string& path, const std::string& err) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (gen != g_generation || g_state != State::Running) return;
    g_state = s;
    g_path = path;
    g_error = err;
    std::fprintf(stderr, "[FILEDLG] %s%s%s\n", s == State::Done ? "picked " : s == State::Cancelled ? "cancelled" : "failed: ",
                 s == State::Done ? path.c_str() : "", s == State::Failed ? err.c_str() : "");
}

#if defined(__ANDROID__)
// ------------------------------------------------------------------------------------------------------------- Android
// org.psprecomp.game.FilePicker (android/game_java): static methods, loaded through the activity's class loader (FindClass from a
// native thread only sees the system classes).
// A failed lookup is retried (at most once a second, 10 times): an early call can come before SDL has the activity. The cause is logged.
std::string java_exception_text(JNIEnv* env) {
    jthrowable t = env->ExceptionOccurred();
    if (!t) return "";
    env->ExceptionClear();
    std::string r = "a Java exception";
    jclass tc = env->GetObjectClass(t);
    jmethodID ts = tc ? env->GetMethodID(tc, "toString", "()Ljava/lang/String;") : nullptr;
    if (ts) {
        jstring m = static_cast<jstring>(env->CallObjectMethod(t, ts));
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (m) { const char* c = env->GetStringUTFChars(m, nullptr); if (c) { r = c; env->ReleaseStringUTFChars(m, c); } env->DeleteLocalRef(m); }
    } else env->ExceptionClear();
    if (tc) env->DeleteLocalRef(tc);
    env->DeleteLocalRef(t);
    return r;
}
jclass picker_class(JNIEnv* env) {
    static jclass cls = nullptr;
    static int fails = 0;
    static uint32_t last_ms = 0;
    if (cls || fails >= 10) return cls;
    const uint32_t now = SDL_GetTicks();
    if (fails && now - last_ms < 1000) return nullptr;
    last_ms = now;
    if (env->ExceptionCheck()) env->ExceptionClear();   // a pending exception from other code would make every call below fail
    std::string why;
    jobject act = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!act) why = "SDL has no activity yet";
    else {
        jclass ac = env->GetObjectClass(act);
        jmethodID gcl = env->GetMethodID(ac, "getClassLoader", "()Ljava/lang/ClassLoader;");
        jobject loader = gcl ? env->CallObjectMethod(act, gcl) : nullptr;
        if (env->ExceptionCheck() || !loader) why = "getClassLoader: " + java_exception_text(env);
        else {
            jclass lc = env->FindClass("java/lang/ClassLoader");
            jmethodID load = lc ? env->GetMethodID(lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
            jstring n = env->NewStringUTF("org.psprecomp.game.FilePicker");
            jobject c = load ? env->CallObjectMethod(loader, load, n) : nullptr;
            if (env->ExceptionCheck()) { why = java_exception_text(env); c = nullptr; }
            else if (!c) why = "loadClass returned null";
            if (c) { cls = static_cast<jclass>(env->NewGlobalRef(c)); env->DeleteLocalRef(c); }
            env->DeleteLocalRef(n);
            if (lc) env->DeleteLocalRef(lc);
            env->DeleteLocalRef(loader);
        }
        env->DeleteLocalRef(ac);
        env->DeleteLocalRef(act);
    }
    if (cls) {
        if (fails) std::fprintf(stderr, "[FILEDLG] FilePicker found on try %d\n", fails + 1);
        return cls;
    }
    ++fails;
    std::fprintf(stderr, "[FILEDLG] no FilePicker (try %d of 10): %s%s\n", fails, why.c_str(),
                 why.find("ClassNotFound") != std::string::npos ? " (this APK was built from an Android part without android/game_java: an older build)" : "");
    return nullptr;
}
std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    env->DeleteLocalRef(s);
    return r;
}
// Calls a static String method (String...) of FilePicker; "" on any Java exception (its text in *err).
std::string call_str(const char* method, const std::vector<std::string>& args, std::string* err) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass c = env ? picker_class(env) : nullptr;
    if (!c) { if (err) *err = "no document picker"; return ""; }
    std::string sig = "(";
    for (size_t i = 0; i < args.size(); ++i) sig += "Ljava/lang/String;";
    sig += ")Ljava/lang/String;";
    jmethodID m = env->GetStaticMethodID(c, method, sig.c_str());
    if (!m) { env->ExceptionClear(); if (err) *err = std::string("FilePicker.") + method + " is missing"; return ""; }
    std::vector<jvalue> v(args.size());
    for (size_t i = 0; i < args.size(); ++i) v[i].l = env->NewStringUTF(args[i].c_str());
    jstring r = static_cast<jstring>(env->CallStaticObjectMethodA(c, m, v.data()));
    for (auto& x : v) env->DeleteLocalRef(x.l);
    if (env->ExceptionCheck()) { env->ExceptionClear(); if (err) *err = "the document picker failed"; return ""; }
    return jstr(env, r);
}
std::string g_uri;   // the picked document (exports: written by commit)
#else
// ------------------------------------------------------------------------------------------------------------- desktop (pfd)
std::unique_ptr<pfd::select_folder> g_folder;
std::unique_ptr<pfd::open_file> g_open;
std::unique_ptr<pfd::save_file> g_save;
unsigned g_pfd_gen = 0;
bool desktop_available() {
    static const bool ok = [] {
        if (const char* f = std::getenv("PSPRECOMP_FILE_DIALOG"); f && !std::strcmp(f, "none")) return false;
#if !defined(_WIN32) && !defined(__APPLE__)
        const char* desk = std::getenv("XDG_CURRENT_DESKTOP");
        if (std::getenv("GAMESCOPE_WAYLAND_DISPLAY") || (desk && std::strstr(desk, "gamescope"))) return false;   // Steam Deck game mode shows no other windows
        if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) return false;
#endif
        return pfd::settings::available();
    }();
    return ok;
}
void desktop_drop() { g_folder.reset(); g_open.reset(); g_save.reset(); }
#endif

}  // namespace

bool available() { return backend()[0] != 0; }

const char* backend() {
#if defined(__ANDROID__)
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());   // not cached here: picker_class retries a failed lookup
    return env && picker_class(env) ? "Android" : "";
#else
    return desktop_available() ? "system" : "";
#endif
}

void set_scratch_dir(const std::string& dir) { std::lock_guard<std::mutex> lk(g_mu); g_scratch = dir; }

bool start(Kind kind, const std::string& title, const std::string& start_dir, const std::string& default_name) {
    if (!available()) return false;
    unsigned gen;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_state == State::Running) return false;
        g_state = State::Running;
        g_path.clear(); g_error.clear();
        g_kind = kind;
        gen = ++g_generation;
    }
    std::fprintf(stderr, "[FILEDLG] %s: %s\n", backend(), title.c_str());
#if defined(__ANDROID__)
    (void)start_dir;
    std::string err;
    const char* what = kind == Kind::ImportFolder ? "import_folder" : (kind == Kind::ImportArchive || kind == Kind::ImportPatch) ? "import_file" : kind == Kind::ExportFolder ? "export_folder" : "export_zip";
    const std::string r = call_str("start", {what, title, default_name}, &err);
    if (!err.empty() || r != "ok") { finish(gen, State::Failed, "", err.empty() ? r : err); }
#else
    {
        std::lock_guard<std::mutex> lk(g_mu);
        desktop_drop();
        g_pfd_gen = gen;
        std::string dir = start_dir;
#ifdef _WIN32
        if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') dir += '\\';
#else
        if (!dir.empty() && dir.back() != '/') dir += '/';   // zenity opens a folder given with its slash, else its parent with it selected
#endif
        switch (kind) {
        case Kind::ImportFolder: case Kind::ExportFolder: g_folder = std::make_unique<pfd::select_folder>(title, dir); break;
        case Kind::ImportArchive:
            g_open = std::make_unique<pfd::open_file>(title, dir, std::vector<std::string>{
                "Save archives (.zip .7z .rar .tar.gz)", "*.zip *.ZIP *.7z *.7Z *.rar *.RAR *.tar *.gz *.tgz *.xz *.txz *.bz2", "All files", "*"});
            break;
        case Kind::ImportPatch:
            g_open = std::make_unique<pfd::open_file>(title, dir, std::vector<std::string>{
                "ISO patches (.xdelta .vcdiff)", "*.xdelta *.XDELTA *.vcdiff *.VCDIFF *.xd3 *.delta", "All files", "*"});
            break;
        case Kind::ExportZip:
            g_save = std::make_unique<pfd::save_file>(title, dir.empty() ? default_name : dir + default_name,
                                                      std::vector<std::string>{"Zip archive", "*.zip"}, pfd::opt::force_overwrite);
            break;
        }
    }
#endif
    return true;
}

State poll(std::string* path, std::string* error) {
#if defined(__ANDROID__)
    // FilePicker.poll(): "" while open, "!cancel", "!error <text>", else the picked document's URI.
    bool running;
    unsigned gen;
    Kind kind;
    std::string scratch;
    { std::lock_guard<std::mutex> lk(g_mu); running = g_state == State::Running; gen = g_generation; kind = g_kind; scratch = g_scratch; }
    if (running) {
        std::string err;
        const std::string r = call_str("poll", {}, &err);
        if (!err.empty()) finish(gen, State::Failed, "", err);
        else if (r == "!cancel") finish(gen, State::Cancelled, "", "");
        else if (r.rfind("!error", 0) == 0) finish(gen, State::Failed, "", r.size() > 7 ? r.substr(7) : "the document picker failed");
        else if (!r.empty()) {
            g_uri = r;
            std::error_code ec;
            const fs::path s = fs::u8path(scratch.empty() ? "/data/local/tmp" : scratch);
            if (kind == Kind::ImportFolder || kind == Kind::ImportArchive || kind == Kind::ImportPatch) {
                const fs::path dst = s / (kind == Kind::ImportFolder ? "picked" : kind == Kind::ImportPatch ? "picked.patch" : "picked.archive");
                fs::remove_all(dst, ec);
                fs::create_directories(s, ec);
                const std::string e = call_str(kind == Kind::ImportFolder ? "copyTreeIn" : "copyFileIn", {r, dst.u8string()}, &err);
                if (!err.empty() || !e.empty()) finish(gen, State::Failed, "", err.empty() ? e : err);
                else finish(gen, State::Done, dst.u8string(), "");
            } else {
                const fs::path dst = s / (kind == Kind::ExportFolder ? "export" : "export.zip");
                fs::remove_all(dst, ec);
                fs::create_directories(kind == Kind::ExportFolder ? dst : s, ec);
                finish(gen, State::Done, dst.u8string(), "");
            }
        }
    }
#else
    {   // the dialog runs in pfd's own process / thread: look whether it has answered
        std::string picked;
        bool answered = false;
        unsigned gen = 0;
        Kind kind = Kind::ImportFolder;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            gen = g_pfd_gen;
            kind = g_kind;
            if (g_folder && g_folder->ready(0)) { picked = g_folder->result(); answered = true; }
            else if (g_open && g_open->ready(0)) { auto r = g_open->result(); if (!r.empty()) picked = r[0]; answered = true; }
            else if (g_save && g_save->ready(0)) { picked = g_save->result(); answered = true; }
            if (answered) desktop_drop();
        }
        if (answered) {
            if (picked.empty()) finish(gen, State::Cancelled, "", "");
            else {
                if (kind == Kind::ExportZip && (picked.size() < 4 || (picked.compare(picked.size() - 4, 4, ".zip") != 0 && picked.compare(picked.size() - 4, 4, ".ZIP") != 0))) picked += ".zip";
                finish(gen, State::Done, picked, "");
            }
        }
    }
#endif
    std::lock_guard<std::mutex> lk(g_mu);
    const State s = g_state;
    if (s == State::Done || s == State::Cancelled || s == State::Failed) {
        if (path) *path = g_path;
        if (error) *error = g_error;
        g_state = State::Idle;
    }
    return s;
}

void abandon() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_state != State::Running) return;
    ++g_generation;
    g_state = State::Idle;
#if !defined(__ANDROID__)
    if (g_folder) g_folder->kill();
    if (g_open) g_open->kill();
    if (g_save) g_save->kill();
    desktop_drop();
#endif
}

std::string commit() {
#if defined(__ANDROID__)
    Kind kind;
    std::string scratch;
    { std::lock_guard<std::mutex> lk(g_mu); kind = g_kind; scratch = g_scratch; }
    if (g_uri.empty()) return "nothing was picked";
    const fs::path s = fs::u8path(scratch);
    std::string err;
    const std::string r = kind == Kind::ExportFolder ? call_str("copyTreeOut", {(s / "export").u8string(), g_uri}, &err)
                                                     : call_str("copyFileOut", {(s / "export.zip").u8string(), g_uri}, &err);
    std::error_code ec;
    fs::remove_all(s / "export", ec);
    fs::remove(s / "export.zip", ec);
    return err.empty() ? r : err;
#else
    return "";
#endif
}

}  // namespace psp_filedlg
