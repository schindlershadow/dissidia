// Android glue (compiled on Android only): where the game's files live, and the log.
//
// The APK holds only the runtime (libmain.so = core + the player's generated game, built on the PC) and SDL's Java activity. The game's
// data (disc0/, ms0/, mods, voice_jp ...: the same work folder the desktop uses) is copied by the player to the app's external
// storage folder, <ExternalStorage>/Android/data/org.psprecomp.game/files/game, with adb or a file manager. Nothing of the game is in the APK.
#ifdef __ANDROID__
#include "psp_android.h"

#include <SDL.h>
#include <android/log.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    setenv("PSPRECOMP_DISC0", (wd + "/disc0").c_str(), 0);
    setenv("PSPRECOMP_MS0", (wd + "/ms0").c_str(), 0);
    setenv("PSPRECOMP_FULLSCREEN", "1", 0);
    setenv("PSPRECOMP_VSYNC", "1", 0);
    std::fprintf(stderr, "[ANDROID] work folder %s\n", wd.c_str());
}
#endif
