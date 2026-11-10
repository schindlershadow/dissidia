#include "install.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace setup {

#ifdef _WIN32
static const char* EXE = ".exe";
#else
static const char* EXE = "";
#endif

static std::string join(const std::string& a, const std::string& b) { return (fs::path(a) / b).string(); }
static bool is_file(const std::string& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
static bool is_dir(const std::string& p) { std::error_code ec; return fs::is_directory(p, ec); }

static std::string find_on_path(const char* name) {
    const char* path = std::getenv("PATH");
    if (!path) return "";
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string s = path;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(sep, i);
        if (j == std::string::npos) j = s.size();
        std::string dir = s.substr(i, j - i);
        if (!dir.empty()) {
            std::string cand = join(dir, std::string(name) + EXE);
            if (is_file(cand)) return cand;
        }
        i = j + 1;
    }
    return "";
}

Layout find_layout(const std::string& root) {
    Layout l;
    l.root = root;
    l.recompiler = join(root, std::string("bin/psprecomp") + EXE);
    l.self = join(root, std::string("psprecomp_setup") + EXE);
    l.toolchain = join(root, "toolchain");
    l.sdk = join(root, "sdk");
    l.game_dir = join(root, "game");
    l.setup_script = join(root, "game/tools/setup_from_iso.py");
    l.build_script = join(root, "game/tools/psp_build.py");
    l.update_script = join(root, "game/tools/update.py");
    std::string embedded = join(root, std::string("python/python") + EXE);
    if (is_file(embedded)) l.python = embedded;
    else {
        l.python = find_on_path("python3");
        if (l.python.empty()) l.python = find_on_path("python");
    }
    return l;
}

std::string Layout::problem() const {
    if (python.empty()) return "Python 3 was not found (install python3; Windows releases include it)";
    const std::pair<const std::string*, const char*> need[] = {
        {&recompiler, "bin/psprecomp"}, {&setup_script, "game/tools/setup_from_iso.py"},
        {&build_script, "game/tools/psp_build.py"}};
    for (auto& n : need)
        if (!is_file(*n.first)) return std::string("this release is incomplete: ") + n.second + " is missing";
    if (!is_file(join(sdk, "lib/libpsprecomp_core.a")) && !is_file(join(sdk, "lib/libpsprecomp_core.lib")))
        return "this release is incomplete: sdk/lib/libpsprecomp_core.a is missing";
    if (!is_file(join(toolchain, "toolchain.cmake"))) return "this release is incomplete: toolchain/ is missing";
    return "";
}

bool feature_available(const Layout& l, const std::string& name) {
    std::ifstream f(join(l.game_dir, "features.ini"));
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == name + "=1") return true;
    }
    return false;
}

static const char* kExplainBuild[5] = {
    "Checks your game files against the analysis data that ships with this program, so the translation matches your copy exactly.",
    "Translates the game's PSP machine code into C++ on this computer (about 9,000 functions). The result stays on your PC and is never shared.",
    "Does the same for the game's battle, menu and extra modules.",
    "Sets up the compiler for the project.",
    "Compiles about 470 source files with the bundled compiler. This is the long part: a couple of minutes on a fast PC, up to half an hour on a slow one. "
    "Cancelling is safe; starting again continues where it stopped."};

static Step build_step(const Layout& l, const std::string& workdir, bool android) {
    Step b;
    b.label = android ? "Building the Android version" : "Building the game";
    b.explain = android ? "Builds the Android version of the game from the same translation, for copying to your phone or handheld."
                        : "";
    b.is_build = true;
    b.argv = {l.python, l.build_script, workdir, "--sdk", l.sdk, "--toolchain", l.toolchain, "--psprecomp", l.recompiler,
              "--game", l.game_dir, "--progress"};
    if (android) b.argv.push_back("--android");
    b.env = {{"PYTHONUNBUFFERED", "1"}};
    return b;
}

static void finish_plan(std::vector<Step>& plan) {
    double total = 0;
    for (auto& s : plan) total += s.weight;
    double at = 0;
    for (auto& s : plan) { s.weight /= total; s.base = at; at += s.weight; }
}

std::vector<Step> plan_rebuild(const Layout& l, const std::string& workdir, const Options& o) {
    std::vector<Step> p;
    Step b = build_step(l, workdir, false);
    b.weight = 0.84;
    p.push_back(b);
    if (o.android && feature_available(l, "android")) {
        Step a = build_step(l, workdir, true);
        a.weight = 0.60;
        p.push_back(a);
    }
    finish_plan(p);
    return p;
}

std::vector<Step> plan_install(const Layout& l, const std::string& iso, const std::string& workdir, const Options& o) {
    Step x;
    x.label = "Reading your game file";
    x.explain = "Copies the game's files out of the disc image and decrypts the main program. Nothing is uploaded anywhere.";
    x.argv = {l.python, l.setup_script, iso, workdir};
    x.env = {{"PSPRECOMP_TOOLCHAIN", l.toolchain}, {"PYTHONUNBUFFERED", "1"}};
    x.weight = 0.08;
    std::vector<Step> p{x};
    for (auto& s : plan_rebuild(l, workdir, o)) p.push_back(s);   // weights are renormalised below
    // plan_rebuild() normalised its own steps; restore their relative sizes
    for (size_t i = 1; i < p.size(); i++) p[i].weight = i == 1 ? 0.84 : 0.60;
    if (o.hd_textures && feature_available(l, "hd_textures")) {
        Step t;
        t.label = "Downloading the HD texture pack";
        t.explain = "Fetches the community HD texture pack into the game's mods folder.";
        t.argv = {l.python, join(l.game_dir, "tools/fetch_textures.py"), workdir};
        t.env = {{"PYTHONUNBUFFERED", "1"}};
        t.weight = 0.06;
        p.push_back(t);
    }
    if (o.jp_audio && feature_available(l, "jp_audio") && !o.undub_iso.empty()) {
        Step j;
        j.label = "Extracting the Japanese voices";
        j.explain = "Reads the voice and movie files out of the undub disc image you picked and adds them as an option in the game.";
        j.argv = {l.python, join(l.game_dir, "tools/jp_audio.py"), "--iso", o.undub_iso, "--work", workdir};
        j.env = {{"PYTHONUNBUFFERED", "1"}};
        j.weight = 0.05;
        p.push_back(j);
    }
    if (!o.import_saves.empty()) {
        Step v;
        v.label = "Importing your saves";
        v.explain = "Copies your Dissidia 012 saves into the game's save folder (from another region or from PPSSPP) and signs them for this game. The originals are not changed.";
        v.argv = {l.self, "--import-saves", o.import_saves, "--dir", workdir};
        v.weight = 0.01;
        p.push_back(v);
    }
    finish_plan(p);
    return p;
}

std::vector<Step> plan_android_install(const Layout& l, const std::string& workdir) {
    Step x;
    x.label = "Installing on your device";
    x.explain = "Installs the app on the phone or handheld that is connected with USB or wireless debugging (adb), then copies the game files (about 2 GB) to it. Needs adb (Android platform-tools) in your PATH.";
    x.argv = {l.python, join(l.game_dir, "tools/android_install.py"), workdir};
    x.env = {{"PYTHONUNBUFFERED", "1"}};
    x.weight = 1;
    std::vector<Step> p{x};
    finish_plan(p);
    return p;
}

bool android_build_exists(const std::string& workdir) { return is_file(join(workdir, "android/dissidia.apk")); }

std::vector<std::string> update_check_argv(const Layout& l) {
    std::vector<std::string> a = {l.python, l.update_script, "check", "--root", l.root};
    if (const char* m = std::getenv("PSPRECOMP_MANIFEST_URL")) { a.push_back("--manifest"); a.push_back(m); }
    return a;
}

UpdateInfo parse_update_line(const std::string& line) {
    UpdateInfo u;
    auto field = [&](const std::string& key) -> std::string {
        size_t p = line.find(" " + key + "=");
        if (p == std::string::npos) return "";
        p += key.size() + 2;
        size_t e = line.find(' ', p);
        return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };
    if (line.compare(0, 6, "UPDATE") == 0) {
        u.available = true;
        u.version = field("version");
        u.current = field("current");
        u.size = std::atoll(field("size").c_str());
        u.rebuild = field("rebuild") == "1";
        u.relink = field("relink") == "1";
    } else if (line.compare(0, 5, "ERROR") == 0) {
        u.error = line.size() > 6 ? line.substr(6) : "unknown error";
    }
    return u;
}

std::vector<Step> plan_update(const Layout& l, const std::string& workdir, const Options& o, const UpdateInfo& u) {
    Step d;
    d.label = "Downloading the update";
    d.explain = "Downloads the new release, checks it and replaces the program files. Your game files and saves are not touched.";
    d.argv = {l.python, l.update_script, "apply", "--root", l.root};
    if (const char* m = std::getenv("PSPRECOMP_MANIFEST_URL")) { d.argv.push_back("--manifest"); d.argv.push_back(m); }
    d.env = {{"PYTHONUNBUFFERED", "1"}};
    d.weight = u.rebuild ? 0.10 : 0.60;
    std::vector<Step> p{d};
    if (u.rebuild || u.relink) {
        Step b = build_step(l, workdir, false);
        b.explain = "";
        b.weight = u.rebuild ? 0.90 : 0.40;
        p.push_back(b);
    }
    (void)o;
    finish_plan(p);
    return p;
}

bool update_check_enabled(const std::string& workdir) {
    if (const char* e = std::getenv("PSPRECOMP_UPDATE_CHECK")) if (e[0] == '0') return false;
    std::ifstream f(join(workdir, "settings.ini"));
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == "check_updates=0") return false;
    }
    return true;
}

double step_done_fraction(const std::vector<Step>& plan, int i) {
    if (i < 0) return 0;
    if (i >= (int)plan.size()) return 1;
    return plan[(size_t)i].base + plan[(size_t)i].weight;
}

// psp-build STEP boundaries inside a build step: analysis, recompile, overlays, configure, compile.
static const double kBuildStart[] = {0.00, 0.03, 0.10, 0.15, 0.17, 1.00};

void feed_line(Progress& p, const std::vector<Step>& plan, const std::string& line, long long now_ms) {
    p.last_line = line.size() > 200 ? line.substr(0, 200) : line;
    const Step& st = plan[(size_t)std::min<int>(p.step, (int)plan.size() - 1)];
    if (p.start_ms == 0) p.start_ms = now_ms;
    double base = st.base, span = st.weight;
    if (!st.is_build) {
        p.stage = st.label;
        p.explain = st.explain;
        p.detail = line.size() > 60 ? line.substr(0, 57) + "..." : line;
        p.fraction = std::max(p.fraction, base);
        int pc = 0;
        if (std::sscanf(line.c_str(), "[update] PROGRESS %d", &pc) == 1) {   // update.py's download
            p.detail = "Downloading: " + std::to_string(pc) + "%";
            p.fraction = std::max(p.fraction, base + span * std::min(100, std::max(0, pc)) / 100.0 * 0.8);
        } else if (line.compare(0, 9, "[update] ") == 0) {
            if (line.compare(9, 5, "ERROR") == 0) p.error = line.substr(15);
            else p.detail = line.substr(9, 56);
        }
        return;
    }
    // ninja's own counter "[183/474] Building CXX object .../batch_0165.cpp.o"
    if (!line.empty() && line[0] == '[' && std::isdigit((unsigned char)line[1])) {
        int i = 0, n = 0;
        if (std::sscanf(line.c_str(), "[%d/%d]", &i, &n) == 2 && n > 0) {
            if (p.compile_n == 0 || i < p.compile_i) p.compile_start_ms = now_ms;
            p.compile_i = i; p.compile_n = n;
            size_t sl = line.rfind('/');
            std::string file = sl == std::string::npos ? line : line.substr(sl + 1);
            if (file.size() > 3 && file.compare(file.size() - 2, 2, ".o") == 0) file.erase(file.size() - 2);
            p.detail = "Compiled " + std::to_string(i) + " of " + std::to_string(n) + ": " + file;
            return;
        }
    }
    const std::string tagp = "[psp-build] ";
    if (line.compare(0, tagp.size(), tagp) != 0) return;
    std::istringstream is(line.substr(tagp.size()));
    std::string kind;
    is >> kind;
    if (kind == "STEP") {
        int i = 0, n = 0; char slash = 0;
        is >> i >> slash >> n;
        std::string rest;
        std::getline(is, rest);
        while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        p.sub = std::max(1, std::min(i, 5)) - 1;
        p.stage = (rest.empty() ? st.label : rest);
        p.explain = !st.explain.empty() ? st.explain : kExplainBuild[p.sub];
        p.detail.clear();
        p.fraction = std::max(p.fraction, base + span * kBuildStart[p.sub]);
    } else if (kind == "PROGRESS") {
        int pc = 0;
        is >> pc;
        double lo = kBuildStart[p.sub], hi = kBuildStart[p.sub + 1];
        p.fraction = std::max(p.fraction, base + span * (lo + (hi - lo) * std::min(100, std::max(0, pc)) / 100.0));
    } else if (kind == "ERROR") {
        std::string rest;
        std::getline(is, rest);
        p.error = rest.empty() ? "the build failed" : rest.substr(rest[0] == ' ' ? 1 : 0);
    }
}

static std::string mmss(long long secs) {
    char b[32];
    if (secs >= 3600) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", secs / 3600, (secs / 60) % 60, secs % 60);
    else std::snprintf(b, sizeof b, "%lld:%02lld", secs / 60, secs % 60);
    return b;
}

std::string elapsed_text(const Progress& p, long long now_ms) {
    return p.start_ms ? mmss(std::max(0LL, (now_ms - p.start_ms) / 1000)) + " elapsed" : "";
}

std::string eta_text(const Progress& p, long long now_ms) {
    double remaining = -1;   // seconds
    if (p.compile_n > 20 && p.compile_i > 8 && p.compile_start_ms) {
        // during the compile the object counter is the best clock (later files are not cheaper than earlier ones on average)
        double per = (now_ms - p.compile_start_ms) / 1000.0 / p.compile_i;
        remaining = per * (p.compile_n - p.compile_i);
    } else if (p.start_ms && p.fraction > 0.04) {
        remaining = (now_ms - p.start_ms) / 1000.0 * (1.0 - p.fraction) / p.fraction;
    }
    if (remaining < 0) return "Working out how long this will take...";
    long long m = (long long)(remaining / 60.0 + 0.5);
    if (remaining < 45) return "Less than a minute left";
    if (m < 90) return "About " + std::to_string(m) + " minute" + (m == 1 ? "" : "s") + " left";
    return "About " + std::to_string((m + 30) / 60) + " hours left";
}

State load_state(const std::string& file) {
    State s;
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "workdir") s.workdir = v;
        else if (k == "iso") s.iso = v;
        else if (k == "version") s.version = v;
        else if (k == "check_updates") s.options.check_updates = v != "0";
        else if (k == "hd_textures") s.options.hd_textures = v == "1";
        else if (k == "jp_audio") s.options.jp_audio = v == "1";
        else if (k == "android") s.options.android = v == "1";
        else if (k == "undub_iso") s.options.undub_iso = v;
    }
    return s;
}

bool save_state(const std::string& file, const State& s) {
    std::ofstream f(file, std::ios::trunc);
    if (!f) return false;
    f << "workdir=" << s.workdir << "\niso=" << s.iso << "\nversion=" << s.version << "\n"
      << "check_updates=" << s.options.check_updates << "\nhd_textures=" << s.options.hd_textures << "\njp_audio=" << s.options.jp_audio
      << "\nandroid=" << s.options.android << "\nundub_iso=" << s.options.undub_iso << "\n";
    return (bool)f;
}

std::string game_binary(const std::string& workdir) { return join(workdir, std::string("build/psprecomp_runtime") + EXE); }
bool game_installed(const std::string& workdir) { return !workdir.empty() && is_file(game_binary(workdir)); }

EnvList game_env(const std::string& workdir) {
    // What games/dissidia/tools/run.sh sets for a plain start.
    EnvList e;
    e.push_back({"PSPRECOMP_DISC0", join(workdir, "disc0")});
    e.push_back({"PSPRECOMP_MS0", join(workdir, "ms0")});
    e.push_back({"PSPRECOMP_CROSS_MID", "1"});
    if (!std::getenv("PSPRECOMP_PAD_L2")) e.push_back({"PSPRECOMP_PAD_L2", "R+CROSS"});
    if (!std::getenv("PSPRECOMP_PAD_R2")) e.push_back({"PSPRECOMP_PAD_R2", "R+TRIANGLE"});
    std::string db = join(workdir, "gamecontrollerdb.txt");
    if (is_file(db) && !std::getenv("PSPRECOMP_GAMECONTROLLERDB")) e.push_back({"PSPRECOMP_GAMECONTROLLERDB", db});
    return e;
}

bool launch_game(const std::string& workdir, std::string* error) {
    std::error_code ec;
    fs::create_directories(join(workdir, "ms0/PSP/SAVEDATA"), ec);
    fs::create_directories(join(workdir, "ms0/PSP/GAME"), ec);
    return spawn_detached({game_binary(workdir)}, game_env(workdir), workdir, error);
}

// Path of the running program (the wizard), "" if unknown.
static std::string self_exe() {
#ifdef _WIN32
    char buf[4096];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    return n > 0 && n < sizeof buf ? std::string(buf, n) : std::string();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
#endif
}

bool create_shortcut(const std::string& workdir, std::string* where, std::string* error) {
    const std::string exe = self_exe();
    if (exe.empty()) { if (error) *error = "cannot find the setup program's own path"; return false; }
    std::error_code ec;
    // The game's icon, from the disc metadata the installer kept (ICON0.PNG, 144x80).
    std::string icon = join(workdir, "disc0/PSP_GAME/ICON0.PNG");
    std::string icon_copy = join(workdir, "icon.png");
    if (is_file(icon)) fs::copy_file(icon, icon_copy, fs::copy_options::overwrite_existing, ec);
    const std::string name = "Dissidia 012 Duodecim Final Fantasy";
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (!appdata) { if (error) *error = "APPDATA is not set"; return false; }
    std::string dir = join(appdata, "Microsoft\\Windows\\Start Menu\\Programs");
    std::string lnk = join(dir, name + ".lnk");
    // A .lnk is a binary format; the WScript.Shell COM object writes one (present on every Windows).
    auto q = [](const std::string& x) { std::string r = "'"; for (char c : x) { if (c == '\'') r += "''"; else r += c; } return r + "'"; };
    std::string ps = "$s=(New-Object -ComObject WScript.Shell).CreateShortcut(" + q(lnk) + ");$s.TargetPath=" + q(exe) +
                     ";$s.Arguments='--launch';$s.WorkingDirectory=" + q(workdir) +
                     ";$s.Description=" + q(name) + ";$s.Save()";
    if (std::system(("powershell -NoProfile -NonInteractive -Command \"" + ps + "\"").c_str()) != 0 || !is_file(lnk)) {
        if (error) *error = "could not create " + lnk;
        return false;
    }
    if (where) *where = lnk;
    return true;
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base = xdg && *xdg ? xdg : join(home_dir(), ".local/share");
    std::string dir = join(base, "applications");
    fs::create_directories(dir, ec);
    std::string file = join(dir, "dissidia-012.desktop");
    auto esc = [](const std::string& x) { std::string r = "\""; for (char c : x) { if (c == '"' || c == '\\' || c == '$' || c == '`') r += '\\'; r += c; } return r + "\""; };
    std::ofstream f(file, std::ios::trunc);
    if (!f) { if (error) *error = "cannot write " + file; return false; }
    f << "[Desktop Entry]\nType=Application\nName=" << name << "\nComment=Dissidia 012 native build\n"
      << "Exec=" << esc(exe) << " --launch\n"
      << "Path=" << workdir << "\n"
      << (is_file(icon_copy) ? "Icon=" + icon_copy + "\n" : std::string())
      << "Terminal=false\nCategories=Game;\nStartupWMClass=Dissidia\n";
    f.close();
    fs::permissions(file, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add, ec);
    if (where) *where = file;
    return (bool)f;
#endif
}

std::string home_dir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return h ? h : "";
}

std::string default_install_dir() {
    std::string h = home_dir();
    return h.empty() ? "Dissidia012" : join(join(h, "Games"), "Dissidia012");
}

long long free_space(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::absolute(path, ec);
    while (!p.empty() && !fs::exists(p, ec)) {
        fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    auto sp = fs::space(p, ec);
    return ec ? -1 : (long long)sp.available;
}

}  // namespace setup
