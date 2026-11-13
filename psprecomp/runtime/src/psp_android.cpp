// Android glue (compiled on Android only): where the game's files live, and the log.
//
// The APK holds only the runtime (libmain.so = core + the player's generated game, built on the PC) and SDL's Java activity. The game's
// data (disc0/, ms0/, mods, voice_jp ...: the same work folder the desktop uses) is copied by the player to the app's external
// storage folder, <ExternalStorage>/Android/data/org.psprecomp.game/files/game, with adb or a file manager. Nothing of the game is in the APK.
#ifdef __ANDROID__
#include <dirent.h>
#include "psp_android.h"

#include <SDL.h>
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

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
__attribute__((constructor(101))) static void early_env() { load_env_file("/sdcard/Android/data/org.psprecomp.game/files/game"); }

// Android Dynamic Performance Framework (API 33, loaded at run time so the app still runs on API 29): a performance hint session tells the
// system how long a frame should take and how long it really took, and it raises the CPU / GPU clocks in time. Without it the GPU governor
// (msm-adreno-tz on Adreno) sits at its lowest clock until a frame is already late: a heavy-but-not-full GPU load, such as ray-traced
// shadows at 90 fps, then misses its rate (84 fps instead of 90 measured on the RP6; pinning the GPU clock gave 88-89).
// Called once per presented frame by the thread that presents (psp_gl_swap). PSPRECOMP_ADPF=0 turns it off.
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
    return std::string(ext ? ext : "/sdcard") + "/game";
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
    bool have = false;
    if (d0) { DIR* dir = opendir(d0); if (dir) { while (dirent* e = readdir(dir)) if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) { have = true; break; } closedir(dir); } }
    if (!have) {
        const std::string msg = std::string("The game's files were not found.\n\nReinstall the game with the installer to copy them to:\n") + wd;
        std::fprintf(stderr, "[ANDROID] ERROR: %s\n", msg.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dissidia Recomp", msg.c_str(), nullptr);
        std::_Exit(1);
    }
}
#endif
