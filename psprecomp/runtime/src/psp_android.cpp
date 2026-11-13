// Android glue (compiled on Android only): where the game's files live, and the log.
//
// The APK holds only the runtime (libmain.so = core + the player's generated game, built on the PC) and SDL's Java activity. The game's
// data (disc0/, ms0/, mods, voice_jp ...: the same work folder the desktop uses) is copied by the player to the app's external
// storage folder, <ExternalStorage>/Android/data/org.psprecomp.game/files/game, with adb or a file manager. Nothing of the game is in the APK.
#ifdef __ANDROID__
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include "psp_android.h"

#include <SDL.h>
#include <android/log.h>
#include <jni.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <sys/stat.h>

#include <cerrno>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace {
int g_pipe[2] = {-1, -1};
FILE* g_logfile = nullptr;

void* log_pump(void*) {
    char buf[1024];
    std::string line;
    ssize_t n;
    while ((n = read(g_pipe[0], buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                __android_log_write(ANDROID_LOG_INFO, "psprecomp", line.c_str());
                if (g_logfile) { std::fwrite(line.data(), 1, line.size(), g_logfile); std::fputc('\n', g_logfile); }
                line.clear();
            } else line += buf[i];
        }
        if (g_logfile) std::fflush(g_logfile);
    }
    return nullptr;
}
}  // namespace

// <workdir>/env.txt: one KEY=VALUE per line (# comments) becomes an environment variable: the Android way to set PSPRECOMP_* flags.
// Loaded from a very early constructor too, because several flags are read by static initialisers before SDL_main runs.
static void load_env_file(const std::string& wd) {
    if (FILE* ef = std::fopen((wd + "/env.txt").c_str(), "r")) {
        char l[512];
        while (std::fgets(l, sizeof l, ef)) {
            std::string t(l);
            while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
            size_t eq = t.find('=');
            if (t.empty() || t[0] == '#' || eq == std::string::npos) continue;
            setenv(t.substr(0, eq).c_str(), t.substr(eq + 1).c_str(), 1);
        }
        std::fclose(ef);
    }
}
// Where the game folder is. Normally <external>/Android/data/<package>/files/game (copied there by adb or the PC installer). A build made
// ON the phone (Termux, docs/android.md) cannot write there: since Android 11 no other app may touch Android/data, so it puts the folder in
// Android/media/<package>/game, which both Termux and this app can use. The first one that holds a game (disc0) wins; the Android/data one
// when neither does (a fresh install: the folder is created there).
static bool has_game(const std::string& dir) {
    struct stat st;
    return stat((dir + "/disc0").c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
static std::string pick_workdir(const std::string& data_game) {
    const std::string from = "/Android/data/";
    const size_t at = data_game.find(from);
    if (at == std::string::npos || has_game(data_game)) return data_game;
    const std::string media = data_game.substr(0, at) + "/Android/media/" + data_game.substr(at + from.size());
    // .../Android/media/<package>/files/game -> .../Android/media/<package>/game (no "files" level: it is the app's own media folder)
    const size_t files = media.rfind("/files/game");
    const std::string media_game = files == std::string::npos ? media : media.substr(0, files) + "/game";
    return has_game(media_game) ? media_game : data_game;
}
__attribute__((constructor(101))) static void early_env() { load_env_file(pick_workdir("/sdcard/Android/data/org.psprecomp.game/files/game")); }

// Android Dynamic Performance Framework (API 33, loaded at run time so the app still runs on API 29): a performance hint session tells the
// system how long a frame should take and how long it really took, and it raises the CPU / GPU clocks in time. Without it the GPU governor
// (msm-adreno-tz on Adreno) sits at its lowest clock until a frame is already late: a heavy-but-not-full GPU load, such as ray-traced
// shadows at 90 fps, then misses its rate (84 fps instead of 90 measured on the RP6; pinning the GPU clock gave 88-89).
// Called once per presented frame by the thread that presents (psp_gl_swap). PSPRECOMP_ADPF=0 turns it off.
int psp_android_display_hdr_peak_nits() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject act = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !act) return -1;
    int nits = -1;
    jclass ac = env->GetObjectClass(act);
    jmethodID gwm = ac ? env->GetMethodID(ac, "getWindowManager", "()Landroid/view/WindowManager;") : nullptr;
    jobject wm = gwm ? env->CallObjectMethod(act, gwm) : nullptr;
    jclass wmc = wm ? env->GetObjectClass(wm) : nullptr;
    jmethodID gdd = wmc ? env->GetMethodID(wmc, "getDefaultDisplay", "()Landroid/view/Display;") : nullptr;
    jobject disp = gdd ? env->CallObjectMethod(wm, gdd) : nullptr;
    jclass dc = disp ? env->GetObjectClass(disp) : nullptr;
    jmethodID ghc = dc ? env->GetMethodID(dc, "getHdrCapabilities", "()Landroid/view/Display$HdrCapabilities;") : nullptr;
    jobject hc = ghc ? env->CallObjectMethod(disp, ghc) : nullptr;
    if (hc) {
        jclass hcc = env->GetObjectClass(hc);
        jmethodID gml = hcc ? env->GetMethodID(hcc, "getDesiredMaxLuminance", "()F") : nullptr;
        if (gml) nits = static_cast<int>(env->CallFloatMethod(hc, gml));
        if (hcc) env->DeleteLocalRef(hcc);
        env->DeleteLocalRef(hc);
    }
    if (env->ExceptionCheck()) { env->ExceptionClear(); nits = -1; }
    if (dc) env->DeleteLocalRef(dc);
    if (disp) env->DeleteLocalRef(disp);
    if (wmc) env->DeleteLocalRef(wmc);
    if (wm) env->DeleteLocalRef(wm);
    if (ac) env->DeleteLocalRef(ac);
    env->DeleteLocalRef(act);
    return nits;
}

void psp_android_frame_hint() {
    typedef void* (*PFN_mgr)();
    typedef void* (*PFN_create)(void*, const int32_t*, size_t, int64_t);
    typedef int (*PFN_report)(void*, int64_t);
    static PFN_report report = nullptr;
    static void* session = nullptr;
    static bool tried = false;
    static int64_t last = 0, target = 0;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t now = int64_t(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
    if (!tried) {
        tried = true;
        const char* off = std::getenv("PSPRECOMP_ADPF");
        const char* f = std::getenv("PSPRECOMP_FPS");
        const int fps = f && std::atoi(f) >= 30 ? std::atoi(f) : 30;
        target = 1000000000ll / fps;
        void* lib = (off && off[0] == '0') ? nullptr : dlopen("libandroid.so", RTLD_NOW);
        auto get = lib ? reinterpret_cast<PFN_mgr>(dlsym(lib, "APerformanceHint_getManager")) : nullptr;
        auto create = lib ? reinterpret_cast<PFN_create>(dlsym(lib, "APerformanceHint_createSession")) : nullptr;
        report = lib ? reinterpret_cast<PFN_report>(dlsym(lib, "APerformanceHint_reportActualWorkDuration")) : nullptr;
        void* mgr = get ? get() : nullptr;
        const int32_t tid = int32_t(gettid());
        if (mgr && create && report) session = create(mgr, &tid, 1, target);
        std::fprintf(stderr, "[ANDROID] performance hint session: %s (target %.1f ms)\n", session ? "on" : "unavailable", target / 1e6);
    }
    if (session && last && now - last < 2 * target) report(session, now - last);   // slower than twice the target = a menu or a load: no hint
    last = now;
}

std::string psp_android_workdir() {
    const char* ext = SDL_AndroidGetExternalStoragePath();
    return pick_workdir(std::string(ext ? ext : "/sdcard") + "/game");
}


// Developer side-load for a user that adb cannot write to (a secondary Android user: the shell's storage is always user 0). With the system
// property debug.psprecomp.import_port set (adb shell setprop) and `adb reverse tcp:<port> tcp:<port>` to a host script (tools/android_sideload.py),
// the app fetches the game's files itself while it runs as that user. Stream: "F <size> <relative path>\n" + bytes per file, "E\n" at the end.
static bool read_line(int fd, std::string& out) {
    out.clear();
    char c;
    while (true) { const ssize_t n = recv(fd, &c, 1, 0); if (n <= 0) return false; if (c == '\n') return true; out.push_back(c); if (out.size() > 4096) return false; }
}
static bool mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') { const std::string d = path.substr(0, i); if (mkdir(d.c_str(), 0775) != 0 && errno != EEXIST) return false; }
    return true;
}
static bool import_from_host(const std::string& wd) {
    char val[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.psprecomp.import_port", val);
    const int port = std::atoi(val);
    if (port <= 0) return false;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port); inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) { std::fprintf(stderr, "[ANDROID] side-load: nothing listening on 127.0.0.1:%d\n", port); if (fd >= 0) close(fd); return false; }
    std::fprintf(stderr, "[ANDROID] side-load: receiving the game's files into %s\n", wd.c_str());
    mkdirs(wd);
    if (FILE* mk = std::fopen((wd + "/.importing").c_str(), "w")) std::fclose(mk);   // removed when the transfer is complete: a leftover marker means a partial copy
    std::string line; std::vector<char> buf(1 << 20);
    long long total = 0, next_log = 100ll << 20; int files = 0;
    bool ok = false;
    while (read_line(fd, line)) {
        if (line == "E") { ok = true; break; }
        long long size = 0; int off = 0;
        if (std::sscanf(line.c_str(), "F %lld %n", &size, &off) < 1 || off <= 0) break;
        const std::string rel = line.substr((size_t)off);
        if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos) { std::fprintf(stderr, "[ANDROID] side-load: refused path %s\n", rel.c_str()); break; }
        const std::string full = wd + "/" + rel;
        mkdirs(full.substr(0, full.rfind('/')));
        FILE* f = std::fopen(full.c_str(), "wb");
        long long left = size;
        while (left > 0) {
            const ssize_t n = recv(fd, buf.data(), (size_t)std::min<long long>(left, (long long)buf.size()), 0);
            if (n <= 0) { left = -1; break; }
            if (f) std::fwrite(buf.data(), 1, (size_t)n, f);
            left -= n; total += n;
            if (total >= next_log) { std::fprintf(stderr, "[ANDROID] side-load: %lld MB, %d files\n", total >> 20, files); next_log += 100ll << 20; }
        }
        if (f) std::fclose(f);
        if (left != 0) break;
        ++files;
    }
    close(fd);
    if (ok) std::remove((wd + "/.importing").c_str());
    std::fprintf(stderr, "[ANDROID] side-load: %s (%d files, %lld MB)\n", ok ? "done" : "FAILED", files, total >> 20);
    return ok;
}

void psp_android_init() {
    const std::string wd = psp_android_workdir();
    // stderr -> logcat (tag psprecomp) and <workdir>/last_run.log, like the desktop launcher's log
    g_logfile = std::fopen((wd + "/last_run.log").c_str(), "w");
    if (pipe(g_pipe) == 0) {
        dup2(g_pipe[1], 2);
        dup2(g_pipe[1], 1);
        setvbuf(stderr, nullptr, _IONBF, 0);
        pthread_t t;
        pthread_create(&t, nullptr, log_pump, nullptr);
        pthread_detach(t);
    }
    if (chdir(wd.c_str()) != 0) std::fprintf(stderr, "[ANDROID] cannot enter %s (copy the game's work folder there)\n", wd.c_str());
    load_env_file(wd);
    setenv("PSPRECOMP_DISC0", (wd + "/disc0").c_str(), 0);
    setenv("PSPRECOMP_MS0", (wd + "/ms0").c_str(), 0);
    setenv("PSPRECOMP_VK_CACHE_DIR", (wd + "/vk_cache").c_str(), 0);   // precompiled SPIR-V for the Vulkan backend (src/vk/psp_vk_glsl.h)
    if (const char* in = SDL_AndroidGetInternalStoragePath())   // custom GPU drivers (psp_gpu_driver.h): dlopen refuses a library on the sd card
        setenv("PSPRECOMP_GPU_DRIVER_DIR", (std::string(in) + "/gpu_drivers").c_str(), 0);
    setenv("PSPRECOMP_FULLSCREEN", "1", 0);
    setenv("PSPRECOMP_VSYNC", "1", 0);
    std::fprintf(stderr, "[ANDROID] work folder %s\n", wd.c_str());
    // No game data (a user that never had the files copied, a wrong folder): say so, instead of a black screen.
    const char* d0 = std::getenv("PSPRECOMP_DISC0");
    auto has_files = [&]() { bool h = false; DIR* dir = d0 ? opendir(d0) : nullptr; if (dir) { while (dirent* e = readdir(dir)) if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) { h = true; break; } closedir(dir); } return h; };
    const bool partial = access((wd + "/.importing").c_str(), F_OK) == 0;   // an earlier side-load was cut short
    bool have = has_files() && !partial;
    if (!have && import_from_host(wd)) have = has_files();
    if (!have) {
        const std::string msg = std::string("The game's files were not found.\n\nReinstall the game with the installer to copy them to:\n") + wd;
        std::fprintf(stderr, "[ANDROID] ERROR: %s\n", msg.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dissidia Recomp", msg.c_str(), nullptr);
        std::_Exit(1);
    }
}
#endif
