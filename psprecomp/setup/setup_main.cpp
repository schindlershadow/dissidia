// psprecomp_setup: the installer / launcher wizard.
//
// A small SDL2 program (Linux and Windows) that drives the install of a release on the player's machine: pick the game file,
// pick a folder, then run the steps in install.cpp (extract the disc, translate the game, compile it with the bundled
// toolchain) while showing progress, and finally start the game. It draws with the runtime's own menu renderer
// (psp_sddialog::render_menu / KeyboardDialog from src/hle/psp_savedata_dialog.cpp: the settings-menu look, here in its
// generic theme because the game's art is only available after the disc has been read) and is operated with the keyboard or a
// game controller. See docs/installer.md.
//
//   psprecomp_setup                          the wizard
//   psprecomp_setup --play [DIR]             start the installed game (the remembered install, or DIR) and exit
//   psprecomp_setup --launch                 the shortcut mode: check for an update (unless turned off), then start the game; a window
//                                            only matters when an update is found (or something fails)
//   psprecomp_setup --headless --iso FILE --dir DIR   the install steps without a window (prints the output; for scripts/tests)
//   psprecomp_setup --headless ... --saves SAVEDATA   also import the saves of a SAVEDATA folder
//   psprecomp_setup --import-saves SAVEDATA --dir DIR  copy Dissidia 012 saves into an install (an install step; no window)
//   psprecomp_setup --root DIR               use DIR as the release folder instead of this program's folder
//   psprecomp_setup --screenshots OUTDIR     write PPM images of the pages (documentation / look checks) and exit
#define SDL_MAIN_HANDLED   // keep the plain main() on Windows (see SDL_SetMainReady below)
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../runtime/src/hle/psp_savedata_dialog.h"
#include "../runtime/src/hle/psp_savemgr.h"
#include "install.h"
#include "../runtime/include/psp_app_icon_dissidia.h"
#include "proc.h"
#include "payload.h"
#include "third_party/portable-file-dialogs.h"

namespace fs = std::filesystem;
using psp_sddialog::Canvas;
using psp_sddialog::MenuRow;

namespace {

constexpr int SCALE = 2;   // canvas scale: 960x544 window by default

// PSP pad bits (what the dialog code speaks).
using namespace psp_sddialog;
constexpr uint32_t BTN_START = 0x0008, BTN_TRIANGLE = 0x1000, BTN_SQUARE = 0x8000;

int64_t now_ms() { return (int64_t)SDL_GetTicks64(); }

// -------------------------------------------------------------------------------------------------------------- input
struct Input {
    uint32_t held = 0, prev = 0, pad_held = 0;   // pad_held: controller only (the on-screen keyboard gets typed text from the keyboard)
    int64_t next_repeat = 0;
    uint32_t pressed = 0;   // newly pressed this frame, plus d-pad repeats

    void update(SDL_GameController* pad) {
        uint32_t kb = 0, pd = 0;
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        if (k[SDL_SCANCODE_UP]) kb |= BTN_UP;
        if (k[SDL_SCANCODE_DOWN]) kb |= BTN_DOWN;
        if (k[SDL_SCANCODE_LEFT]) kb |= BTN_LEFT;
        if (k[SDL_SCANCODE_RIGHT]) kb |= BTN_RIGHT;
        if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER] || k[SDL_SCANCODE_SPACE]) kb |= BTN_CROSS;
        if (k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_BACKSPACE]) kb |= BTN_CIRCLE;
        if (k[SDL_SCANCODE_PAGEUP]) kb |= BTN_LTRIGGER;
        if (k[SDL_SCANCODE_PAGEDOWN]) kb |= BTN_RTRIGGER;
        if (pad) {
            auto bt = [&](SDL_GameControllerButton x) { return SDL_GameControllerGetButton(pad, x) != 0; };
            if (bt(SDL_CONTROLLER_BUTTON_DPAD_UP)) pd |= BTN_UP;
            if (bt(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) pd |= BTN_DOWN;
            if (bt(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) pd |= BTN_LEFT;
            if (bt(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) pd |= BTN_RIGHT;
            if (bt(SDL_CONTROLLER_BUTTON_A)) pd |= BTN_CROSS;
            if (bt(SDL_CONTROLLER_BUTTON_B)) pd |= BTN_CIRCLE;
            if (bt(SDL_CONTROLLER_BUTTON_Y)) pd |= BTN_TRIANGLE;
            if (bt(SDL_CONTROLLER_BUTTON_X)) pd |= BTN_SQUARE;
            if (bt(SDL_CONTROLLER_BUTTON_START)) pd |= BTN_START;
            if (bt(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) pd |= BTN_LTRIGGER;
            if (bt(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) pd |= BTN_RTRIGGER;
            const int dz = 16000;
            int ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
            int lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
            if (ly < -dz) pd |= BTN_UP;
            if (ly > dz) pd |= BTN_DOWN;
            if (lx < -dz) pd |= BTN_LEFT;
            if (lx > dz) pd |= BTN_RIGHT;
        }
        pad_held = pd;
        uint32_t b = kb | pd;
        held = b;
        pressed = b & ~prev;
        const uint32_t dirs = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;
        int64_t t = now_ms();
        if (pressed & dirs) next_repeat = t + 380;
        else if ((b & dirs) && t >= next_repeat) { pressed |= b & dirs; next_repeat = t + 70; }
        prev = b;
    }
};

// ----------------------------------------------------------------------------------------------------- file browser
struct Entry {
    std::string name, path, info;
    bool dir = false, up = false, use_here = false, type_path = false;
};

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string human_size(uintmax_t n) {
    char b[32];
    if (n >= (1u << 30)) std::snprintf(b, sizeof b, "%.1f GB", n / double(1u << 30));
    else if (n >= (1u << 20)) std::snprintf(b, sizeof b, "%.0f MB", n / double(1u << 20));
    else std::snprintf(b, sizeof b, "%ju KB", n >> 10);
    return b;
}

std::string shorten_left(const std::string& s, size_t n) {
    return s.size() <= n ? s : "..." + s.substr(s.size() - (n - 3));
}

struct Browser {
    bool want_dir = false;
    fs::path cur;
    std::vector<Entry> list;
    int sel = 0;

    void open(const fs::path& start, bool dirs_only) {
        want_dir = dirs_only;
        std::error_code ec;
        fs::path p = start;
        while (!p.empty() && !fs::is_directory(p, ec)) {
            fs::path parent = p.parent_path();
            if (parent == p) break;
            p = parent;
        }
        cur = p.empty() ? fs::path(setup::home_dir()) : p;
        refresh();
    }

    void refresh() {
        list.clear();
        sel = 0;
        std::error_code ec;
        if (want_dir) {
            Entry u; u.use_here = true; u.name = "Use this folder"; u.path = cur.string();
            list.push_back(u);
            Entry t; t.type_path = true; t.name = "Type a folder path...";
            list.push_back(t);
        }
        fs::path parent = cur.parent_path();
        if (!cur.empty() && parent != cur) { Entry u; u.up = true; u.dir = true; u.name = ".. (up)"; u.path = parent.string(); list.push_back(u); }
#ifdef _WIN32
        else {   // at a drive root: offer the other drives
            for (char d = 'A'; d <= 'Z'; d++) {
                std::string root = std::string(1, d) + ":\\";
                if (fs::exists(root, ec)) { Entry e; e.dir = true; e.name = root; e.path = root; list.push_back(e); }
            }
        }
#endif
        std::vector<Entry> dirs, files;
        for (fs::directory_iterator it(cur, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            std::error_code e2;
            if (it->is_directory(e2)) {
                Entry e; e.dir = true; e.name = name + "/"; e.path = it->path().string();
                dirs.push_back(e);
            } else if (!want_dir && it->is_regular_file(e2)) {
                std::string ext = lower(it->path().extension().string());
                if (ext == ".iso" || ext == ".cso") {
                    Entry e; e.name = name; e.path = it->path().string();
                    e.info = human_size(it->file_size(e2));
                    files.push_back(e);
                }
            }
        }
        auto byname = [](const Entry& a, const Entry& b) { return lower(a.name) < lower(b.name); };
        std::sort(dirs.begin(), dirs.end(), byname);
        std::sort(files.begin(), files.end(), byname);
        list.insert(list.end(), files.begin(), files.end());
        list.insert(list.end(), dirs.begin(), dirs.end());
    }
};

// -------------------------------------------------------------------------------------------------------------- app
enum class Screen { Welcome, PickIso, PickUndub, Waiting, InstallDir, BrowseDir, TypePath, Options, Build, Done, Failed, Checking, UpdateAvail, Message };
// What a file/folder pick is for.
enum class Pick { Iso, Undub, Dir };

struct App {
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture* tex = nullptr;
    SDL_GameController* pad = nullptr;
    Canvas canvas{SCALE};
    Input in;
    bool quit = false, dirty = true;

    setup::Layout layout;
    setup::State state;
    std::string state_file;
    std::string layout_problem;

    Screen screen = Screen::Welcome;
    int sel = 0;
    Browser br;
    KeyboardDialog osk;
    std::string iso, workdir;
    setup::Options opt;
    std::vector<psp_savemgr::Source> save_sources;   // PPSSPP / memory sticks that hold Dissidia 012 saves (found when Options opens)
    bool save_sources_scanned = false;
    bool rebuild_only = false;
    bool android_inst = false;   // the running plan is the Android device install
    bool updating = false;

    // update check (game/tools/update.py check)
    setup::Proc check_proc;
    setup::UpdateInfo upd;
    bool checking = false, manual_check = false, launch_mode = false;
    std::string shortcut_note;
    int64_t check_start = 0;
    std::string check_result;
    std::string msg_title, msg_text;

    // native (system) file dialogs; the built-in browser is the fallback
    std::unique_ptr<pfd::open_file> file_dlg;
    std::unique_ptr<pfd::select_folder> dir_dlg;
    Pick pick = Pick::Iso;
    Screen before_pick = Screen::Welcome;

    // build
    setup::Proc proc;
    std::vector<setup::Step> plan;
    setup::Progress prog;
    bool building = false;
    std::string fail_msg;
    int64_t last_draw = 0;

    bool installed() const { return setup::game_installed(state.workdir); }

    void go(Screen s, int select = 0) { screen = s; sel = select; dirty = true; }

    // ---- page content ----
    struct Page {
        std::string title, tab;
        std::vector<MenuRow> rows;
    };

    static MenuRow row(const std::string& label, const std::string& help = "", const std::string& value = "") {
        MenuRow r; r.label = label; r.help = help; r.value = value; return r;
    }

    bool native_ok() const { return pfd::settings::available(); }

    MenuRow toggle_row(const std::string& label, const std::string& help, bool on, bool available) {
        MenuRow r = row(label, available ? help : help + " (Not in this release yet.)", available ? (on ? "On" : "Off") : "Not yet");
        r.can_dec = r.can_inc = available;
        return r;
    }

    Page page() {
        Page p;
        switch (screen) {
        case Screen::Welcome: {
            p.title = "DISSIDIA 012 SETUP";
            if (!layout_problem.empty()) {
                p.tab = "Problem";
                p.rows = {row("Quit", layout_problem)};
            } else if (installed()) {
                p.tab = "Installed";
                p.rows = {row("Play", "Start the game."),
                          row("Check for updates", "Look for a newer release of the game files (not available yet)."),
                          row("Rebuild the game", "Compile the game again from your extracted files. Use this if the game does not start."),
                          row("Install again", "Pick a game file and install from scratch (your saves are kept)."),
                          row("Quit")};
            } else {
                p.tab = "Welcome";
                p.rows = {row("Install the game",
                              "Builds the game on this computer from your own copy of Dissidia 012 (EU or US, ISO or CSO). "
                              "Nothing from the game is downloaded or shipped with this program. It takes a while."),
                          row("Quit")};
            }
            break;
        }
        case Screen::Waiting:
            p.title = pick == Pick::Dir ? "CHOOSE A FOLDER" : pick == Pick::Undub ? "CHOOSE THE UNDUB FILE" : "CHOOSE YOUR GAME FILE";
            p.tab = "Waiting";
            p.rows = {row("Use the simple browser instead", "A file window opened. Pick the file there, or press here to browse inside this program."),
                      row("Back")};
            break;
        case Screen::PickIso:
        case Screen::PickUndub:
        case Screen::BrowseDir: {
            bool dirs = screen == Screen::BrowseDir;
            p.title = dirs ? "CHOOSE A FOLDER" : screen == Screen::PickUndub ? "CHOOSE THE UNDUB FILE" : "CHOOSE YOUR GAME FILE";
            p.tab = shorten_left(br.cur.string(), 36);
            int total = (int)br.list.size();
            int top = std::max(0, std::min(sel - MENU_ROWS / 2, total - MENU_ROWS));
            for (int i = top; i < std::min(total, top + MENU_ROWS); i++) {
                const Entry& e = br.list[(size_t)i];
                MenuRow r = row(e.name, e.use_here ? e.path : e.up ? "Go up one folder" : e.dir ? "Open this folder" :
                                       e.type_path ? "Type the folder's path with the keyboard" : e.path, e.info);
                p.rows.push_back(r);
            }
            if (p.rows.empty()) p.rows.push_back(row(dirs ? "(empty)" : "No .iso or .cso files here", "Go up with Back, or open another folder."));
            break;
        }
        case Screen::TypePath:
            break;   // drawn by the keyboard dialog
        case Screen::InstallDir: {
            p.title = "WHERE TO INSTALL";
            p.tab = "Folder";
            long long free = setup::free_space(workdir);
            std::string help = "The game's files and your saves go in this folder.";
            if (free >= 0) help += " Free space there: " + human_size((uintmax_t)free) + "; about " + human_size((uintmax_t)setup::kNeededBytes) + " are needed.";
            if (free >= 0 && free < setup::kNeededBytes) help += " NOT ENOUGH FREE SPACE: pick another folder.";
            MenuRow r0 = row("Install here", help, shorten_left(workdir, 34));
            p.rows = {r0, row("Choose another folder...", "Pick a different place for the installation. Now: " + workdir),
                      row("Use the suggested folder", "Go back to " + setup::default_install_dir()), row("Back", "Pick a different game file.")};
            break;
        }
        case Screen::Options: {
            p.title = "OPTIONS";
            p.tab = "Options";
            bool tex = setup::feature_available(layout, "hd_textures"), jp = setup::feature_available(layout, "jp_audio"),
                 an = setup::feature_available(layout, "android");
            MenuRow u = toggle_row("Check for updates", "Look for new releases when the game starts (changeable later in the game's settings.ini).", opt.check_updates, true);
            std::string jpHelp = "Adds the Japanese voices and movies from a second disc image (an undub ISO) that you pick next.";
            if (!save_sources_scanned) { save_sources = psp_savemgr::detect_sources(true); save_sources_scanned = true; }
            MenuRow sv = row("Import saves", "", "None found");
            if (!save_sources.empty()) {
                sv.help = "Copies your Dissidia 012 saves from PPSSPP or a memory stick into the game, whatever their region. The originals are not changed. You can also do this later in the game (Triangle in the save list).";
                sv.value = "Off";
                for (auto& src : save_sources)
                    if (src.path == opt.import_saves) sv.value = src.label + " (" + std::to_string(psp_savemgr::scan(src.path).size()) + ")";
                sv.can_dec = sv.can_inc = true;
            } else sv.help = "No PPSSPP or memory stick saves of Dissidia 012 were found. You can import saves later in the game (Triangle in the save list).";
            p.rows = {u,
                      toggle_row("HD texture pack", "Download the community HD texture pack for the game.", opt.hd_textures, tex),
                      toggle_row("Japanese voices (undub)", jpHelp, opt.jp_audio, jp),
                      toggle_row("Android version", "Also builds the Android version (an APK for phones and handhelds, 64-bit ARM). Needs a one-time download of Google's Android NDK (about 0.7 GB; by turning this on you accept its license, developer.android.com/studio/terms) and some extra minutes. Afterwards you can install it on a connected device from the last page.", opt.android, an),
                      sv,
                      row("Start installing", "Reads " + shorten_left(iso, 40) + " and builds the game in " + shorten_left(workdir, 34) +
                                                  ". You can cancel and continue later."),
                      row("Back")};
            break;
        }
        case Screen::Build: {
            p.title = android_inst ? "ON YOUR DEVICE" : updating ? "UPDATING" : rebuild_only ? "REBUILDING" : "INSTALLING";
            char pc[16];
            std::snprintf(pc, sizeof pc, "%d%%", (int)(prog.fraction * 100 + 0.5));
            std::string bar = "[";
            int fill = (int)(prog.fraction * 20 + 0.5);
            for (int i = 0; i < 20; i++) bar += i < fill ? '#' : '-';
            bar += "]";
            p.tab = pc;
            int64_t t = now_ms();
            std::string stage = prog.stage.empty() ? "Starting..." : prog.stage;
            std::string steps = plan.size() > 1 ? " (" + std::to_string(prog.step + 1) + "/" + std::to_string(plan.size()) + ")" : "";
            p.rows = {row(stage + steps, "", bar),
                      row(prog.detail.empty() ? "Working..." : prog.detail, "", setup::elapsed_text(prog, t)),
                      row(setup::eta_text(prog, t), ""),
                      row("Cancel", (prog.explain.empty() ? "" : prog.explain + " ") + "Cancel stops the installation; starting it again continues where it stopped.")};
            if (sel > 3) sel = 3;
            break;
        }
        case Screen::Checking:
            p.title = "CHECKING FOR UPDATES";
            p.tab = "Please wait";
            p.rows = {row("Looking for a newer version...", "Contacts the release page. This does not send anything about you or your game."),
                      row(launch_mode ? "Skip and play" : "Skip")};
            break;
        case Screen::UpdateAvail: {
            p.title = "UPDATE AVAILABLE";
            p.tab = upd.version;
            std::string help = "Version " + upd.version + " is available (you have " + upd.current + "). Download size: " +
                               human_size((uintmax_t)upd.size) + ".";
            help += upd.rebuild ? " The game has to be built again afterwards, as at installation." :
                    upd.relink ? " The game is relinked afterwards (a short step)." : " Only program files change.";
            p.rows = {row("Update now", help), row(launch_mode ? "Not now: play" : "Not now", "You can update later from the start screen.")};
            break;
        }
        case Screen::Message:
            p.title = msg_title;
            p.tab = "Info";
            p.rows = {row("OK", msg_text)};
            break;
        case Screen::Done:
            p.title = "ALL DONE";
            p.tab = "Finished";
            p.rows = {row("Play now", "Start the game.")};
            if (setup::android_build_exists(cur_workdir()))
                p.rows.push_back(row("Install on an Android device", "The Android version is built: " + cur_workdir() + "/android/dissidia.apk. This installs it on a device connected with USB or wireless debugging (adb in your PATH) and copies the game files to it. Or install the APK by hand and copy the folders (see docs/android.md)."));
            p.rows.push_back(row("Quit", (shortcut_note.empty() ? std::string() : shortcut_note + ". ") + "The game can also be started with this program any time."));
            break;
        case Screen::Failed:
            p.title = "NOT FINISHED";
            p.tab = "Stopped";
            p.rows = {row("Try again", fail_msg + " Trying again continues where it stopped."), row("Back to the start"), row("Quit")};
            break;
        }
        return p;
    }

    std::string cur_workdir() const { return !state.workdir.empty() ? state.workdir : workdir; }
    bool is_browser() const { return screen == Screen::PickIso || screen == Screen::PickUndub || screen == Screen::BrowseDir; }
    int row_count() { return is_browser() ? (int)br.list.size() : (int)page().rows.size(); }

    // ---- actions ----
    void start_plan(bool rebuild, bool update = false, bool android_install = false) {
        rebuild_only = rebuild || update || android_install;
        updating = update;
        android_inst = android_install;
        const setup::Options& o = (rebuild || update) ? state.options : opt;
        if (android_install) plan = setup::plan_android_install(layout, cur_workdir());
        else if (update) plan = setup::plan_update(layout, state.workdir, o, upd);
        else plan = rebuild ? setup::plan_rebuild(layout, state.workdir, o) : setup::plan_install(layout, iso, workdir, o);
        log_open((rebuild || update) ? state.workdir : workdir);
        prog = setup::Progress();
        prog.stage = plan[0].label;
        prog.explain = plan[0].explain;
        prog.step = 0;
        prog.start_ms = now_ms();
        building = true;
        start_step();
        go(Screen::Build, 3);
    }

    void start_step() {
        if (!proc.start(plan[(size_t)prog.step].argv, plan[(size_t)prog.step].env, "")) {
            fail("Could not start: " + proc.error());
            return;
        }
    }

    // setup.log in the install folder: every build line plus the failure reason (the wizard has no console on Windows)
    FILE* logf = nullptr;
    void log_open(const std::string& dir) {
        if (logf) { std::fclose(logf); logf = nullptr; }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        logf = std::fopen((std::filesystem::path(dir) / "setup.log").string().c_str(), "ab");
        if (logf) { std::fprintf(logf, "\n===== psprecomp_setup run =====\n"); std::fflush(logf); }
    }
    void log_line(const std::string& l) {
        if (logf) { std::fprintf(logf, "%s\n", l.c_str()); std::fflush(logf); }
    }

    void fail(const std::string& msg) {
        log_line("FAILED: " + msg);
        building = false;
        fail_msg = msg;
        go(Screen::Failed);
    }

    void pump_build() {
        std::string line;
        while (proc.read_line(line)) {
            std::fprintf(stderr, "%s\n", line.c_str());
            log_line(line);
            setup::feed_line(prog, plan, line, now_ms());
            dirty = true;
        }
        if (building && !proc.busy()) {
            int code = proc.exit_code();
            if (code != 0 || !prog.error.empty()) {
                fail(prog.error.empty() ? "A step failed (exit code " + std::to_string(code) + "); see setup.log in the install folder." : prog.error + ".");
                return;
            }
            prog.fraction = std::max(prog.fraction, setup::step_done_fraction(plan, prog.step));
            if (prog.step + 1 < (int)plan.size()) {
                prog.step++;
                prog.stage = plan[(size_t)prog.step].label;
                prog.explain = plan[(size_t)prog.step].explain;
                prog.detail.clear();
                prog.last_line.clear();
                prog.sub = 0;
                prog.compile_i = prog.compile_n = 0;
                start_step();
            } else {
                building = false;
                if (!rebuild_only) {
                    state.workdir = workdir;
                    state.iso = iso;
                    state.version = "dev";
                    state.options = opt;
                    write_settings();
                }
                setup::save_state(state_file, state);
                if (!rebuild_only) {
                    std::string where, serr;
                    if (setup::create_shortcut(workdir, &where, &serr)) shortcut_note = "A menu entry was added: " + where;
                    else shortcut_note = "No menu entry (" + serr + ")";
                }
                go(Screen::Done);
            }
            dirty = true;
        }
    }

    void write_settings() {
        // settings.ini is the game's own file (key=value); only add the update option if the player has no entry yet.
        fs::path f = fs::path(workdir) / "settings.ini";
        std::string content;
        if (FILE* in = std::fopen(f.string().c_str(), "rb")) {
            char b[4096]; size_t n;
            while ((n = std::fread(b, 1, sizeof b, in)) > 0) content.append(b, n);
            std::fclose(in);
        }
        if (content.find("check_updates=") != std::string::npos) return;
        if (!content.empty() && content.back() != '\n') content += '\n';
        content += std::string("check_updates=") + (opt.check_updates ? "1" : "0") + "\n";
        if (FILE* out = std::fopen(f.string().c_str(), "wb")) { std::fwrite(content.data(), 1, content.size(), out); std::fclose(out); }
    }

    // ---- update check ----
    void begin_check(bool manual) {
        if (!layout_problem.empty()) { go(Screen::Welcome); return; }
        manual_check = manual;
        check_result.clear();
        if (!check_proc.start(setup::update_check_argv(layout), {{"PYTHONUNBUFFERED", "1"}}, "")) {
            finish_check(setup::UpdateInfo());
            return;
        }
        checking = true;
        check_start = now_ms();
        go(Screen::Checking);
    }

    void finish_check(const setup::UpdateInfo& u) {
        checking = false;
        upd = u;
        if (u.available) { go(Screen::UpdateAvail); return; }
        if (manual_check) {
            msg_title = "UPDATES";
            msg_text = u.error.empty() ? "You have the newest version." : "Could not check for updates: " + u.error;
            go(Screen::Message);
        } else if (launch_mode) play();
        else go(Screen::Welcome);
    }

    void pump_check() {
        std::string line;
        while (check_proc.read_line(line)) {
            if (line.compare(0, 6, "UPDATE") == 0 || line.compare(0, 8, "NOUPDATE") == 0 || line.compare(0, 5, "ERROR") == 0) check_result = line;
        }
        if (checking && (!check_proc.busy() || now_ms() - check_start > 15000)) {
            if (check_proc.busy()) { check_proc.kill(); check_result = "ERROR timed out"; }
            finish_check(setup::parse_update_line(check_result));
        }
    }

    void play() {
        std::string err;
        if (!setup::launch_game(state.workdir.empty() ? workdir : state.workdir, &err)) {
            fail(err);
            return;
        }
        quit = true;
    }

    // ---- picking files and folders: the system dialog when there is one, else (or on request) the built-in browser ----
    fs::path pick_start(Pick k) {
        std::error_code ec;
        if (k == Pick::Dir) return fs::path(workdir).parent_path();
        std::string cur = k == Pick::Undub ? opt.undub_iso : iso;
        if (!cur.empty()) return fs::path(cur).parent_path();
        fs::path dl = fs::path(setup::home_dir()) / "Downloads";
        return fs::is_directory(dl, ec) ? dl : fs::path(setup::home_dir());
    }

    void begin_pick(Pick k, Screen back) {
        pick = k;
        before_pick = back;
        if (native_ok()) {
            std::string start = pick_start(k).string();
            if (k == Pick::Dir) dir_dlg = std::make_unique<pfd::select_folder>("Choose the install folder", start);
            else file_dlg = std::make_unique<pfd::open_file>(k == Pick::Undub ? "Choose the undub disc image" : "Choose your Dissidia 012 disc image",
                                                              start, std::vector<std::string>{"PSP disc images", "*.iso *.ISO *.cso *.CSO", "All files", "*"});
            go(Screen::Waiting);
        } else {
            open_browser();
        }
    }

    void open_browser() {
        file_dlg.reset();
        dir_dlg.reset();
        br.open(pick_start(pick), pick == Pick::Dir);
        go(pick == Pick::Dir ? Screen::BrowseDir : pick == Pick::Undub ? Screen::PickUndub : Screen::PickIso);
    }

    void picked(const std::string& path) {   // a chosen file (or folder when pick == Dir)
        std::error_code ec;
        if (pick == Pick::Dir) {
            workdir = path;
            go(Screen::InstallDir);
        } else if (pick == Pick::Undub) {
            opt.undub_iso = path;
            go(Screen::Options, 5);
            start_install_or_ask();
        } else {
            iso = path;
            if (workdir.empty()) workdir = !state.workdir.empty() ? state.workdir : setup::default_install_dir();
            go(Screen::InstallDir);
        }
    }

    void poll_native() {
        if (file_dlg && file_dlg->ready(0)) {
            auto r = file_dlg->result();
            file_dlg.reset();
            if (r.empty()) go(before_pick); else picked(r[0]);
        } else if (dir_dlg && dir_dlg->ready(0)) {
            std::string r = dir_dlg->result();
            dir_dlg.reset();
            if (r.empty()) go(before_pick); else picked(r);
        }
    }

    void start_install_or_ask() {
        // the undub option needs its disc image first
        if (opt.jp_audio && opt.undub_iso.empty() && setup::feature_available(layout, "jp_audio")) { begin_pick(Pick::Undub, Screen::Options); return; }
        std::error_code ec;
        fs::create_directories(workdir, ec);
        if (ec) { fail("Cannot create " + workdir + ": " + ec.message()); return; }
        start_plan(false);
    }

    void toggle_option(int row_idx) {
        switch (row_idx) {
        case 0: opt.check_updates = !opt.check_updates; break;
        case 1: if (setup::feature_available(layout, "hd_textures")) opt.hd_textures = !opt.hd_textures; break;
        case 2:
            if (setup::feature_available(layout, "jp_audio")) { opt.jp_audio = !opt.jp_audio; if (!opt.jp_audio) opt.undub_iso.clear(); }
            break;
        case 3: if (setup::feature_available(layout, "android")) opt.android = !opt.android; break;
        case 4: {   // Off -> each detected source -> Off
            size_t i = 0;
            if (!opt.import_saves.empty()) { while (i < save_sources.size() && save_sources[i].path != opt.import_saves) i++; i++; }
            opt.import_saves = i < save_sources.size() ? save_sources[i].path : "";
            if (save_sources.empty()) opt.import_saves.clear();
            break;
        }
        default: break;
        }
        dirty = true;
    }

    void confirm() {
        switch (screen) {
        case Screen::Welcome:
            if (!layout_problem.empty()) { quit = true; break; }
            if (installed()) {
                switch (sel) {
                case 0: play(); break;
                case 1: begin_check(true); break;
                case 2: opt = state.options; start_plan(true); break;
                case 3: begin_pick(Pick::Iso, Screen::Welcome); break;
                default: quit = true;
                }
            } else if (sel == 0) {
                if (workdir.empty()) workdir = setup::default_install_dir();
                begin_pick(Pick::Iso, Screen::Welcome);
            } else quit = true;
            break;
        case Screen::Waiting:
            if (sel == 0) open_browser();
            else { file_dlg.reset(); dir_dlg.reset(); go(before_pick); }
            break;
        case Screen::PickIso:
        case Screen::PickUndub:
        case Screen::BrowseDir: {
            if (br.list.empty()) break;
            Entry e = br.list[(size_t)sel];
            if (e.type_path) {
                osk.begin(u"Folder path", u"", 200, true);
                psp_sddialog::osk_set_active(true);
                go(Screen::TypePath);
            } else if (e.use_here) {
                picked(e.path);
            } else if (e.dir) {
                br.cur = e.path;
                br.refresh();
                sel = 0;
                dirty = true;
            } else {
                picked(e.path);
            }
            break;
        }
        case Screen::InstallDir:
            if (sel == 0) go(Screen::Options, 5);
            else if (sel == 1) begin_pick(Pick::Dir, Screen::InstallDir);
            else if (sel == 2) { workdir = setup::default_install_dir(); dirty = true; sel = 0; }
            else begin_pick(Pick::Iso, Screen::Welcome);
            break;
        case Screen::Options:
            if (sel <= 4) toggle_option(sel);
            else if (sel == 5) start_install_or_ask();
            else go(Screen::InstallDir);
            break;
        case Screen::Build:
            if (sel == 3) {
                proc.kill();
                building = false;
                fail("The installation was cancelled.");
            }
            break;
        case Screen::Checking:
            if (checking) check_proc.kill();
            checking = false;
            if (launch_mode) play(); else go(Screen::Welcome);
            break;
        case Screen::UpdateAvail:
            if (sel == 0) { opt = state.options; start_plan(false, true); }
            else if (launch_mode) play();
            else go(Screen::Welcome);
            break;
        case Screen::Message:
            go(Screen::Welcome);
            break;
        case Screen::Done:
            if (sel == 0) play();
            else if (sel == 1 && setup::android_build_exists(cur_workdir())) start_plan(false, false, true);
            else quit = true;
            break;
        case Screen::Failed:
            if (sel == 0) { if (!plan.empty()) start_plan(rebuild_only && !updating && !android_inst, updating, android_inst); else go(Screen::Welcome); }
            else if (sel == 1) go(Screen::Welcome);
            else quit = true;
            break;
        case Screen::TypePath:
            break;
        }
    }

    void cancel() {
        switch (screen) {
        case Screen::Waiting:
            file_dlg.reset(); dir_dlg.reset();
            go(before_pick);
            break;
        case Screen::PickIso:
            go(Screen::Welcome);
            break;
        case Screen::PickUndub:
            go(Screen::Options, 5);
            break;
        case Screen::BrowseDir:
            go(Screen::InstallDir);
            break;
        case Screen::InstallDir:
            go(Screen::Welcome);
            break;
        case Screen::Options:
            go(Screen::InstallDir);
            break;
        case Screen::Welcome:
        case Screen::Done:
            quit = true;
            break;
        case Screen::Failed:
        case Screen::Message:
            go(Screen::Welcome);
            break;
        case Screen::UpdateAvail:
            if (launch_mode) play(); else go(Screen::Welcome);
            break;
        default:
            break;
        }
    }

    void adjust(int dir) {
        (void)dir;
        if (screen == Screen::Options && sel <= 4) toggle_option(sel);
    }

    void update_input() {
        in.update(pad);
        if (screen == Screen::TypePath) {
            auto r = osk.update(in.pad_held, now_ms());
            dirty = true;
            if (r == psp_sddialog::Result::Selected) {
                psp_sddialog::osk_set_active(false);
                std::string u8;
                for (char16_t c : osk.text()) {   // BMP only: enough for paths typed on a keyboard
                    if (c < 0x80) u8 += (char)c;
                    else if (c < 0x800) { u8 += (char)(0xC0 | (c >> 6)); u8 += (char)(0x80 | (c & 0x3F)); }
                    else { u8 += (char)(0xE0 | (c >> 12)); u8 += (char)(0x80 | ((c >> 6) & 0x3F)); u8 += (char)(0x80 | (c & 0x3F)); }
                }
                std::error_code ec;
                if (!u8.empty() && fs::is_directory(u8, ec)) { br.open(u8, true); go(Screen::BrowseDir); }
                else if (!u8.empty()) { workdir = u8; go(Screen::InstallDir); }
                else go(Screen::BrowseDir);
            }
            return;
        }
        uint32_t p = in.pressed;
        if (!p) return;
        int n = row_count();
        if (p & BTN_UP) { sel = n ? (sel + n - 1) % n : 0; dirty = true; }
        if (p & BTN_DOWN) { sel = n ? (sel + 1) % n : 0; dirty = true; }
        if (p & BTN_LEFT) adjust(-1);
        if (p & BTN_RIGHT) adjust(+1);
        if (p & BTN_CROSS) confirm();
        if (p & BTN_CIRCLE) cancel();
        if (is_browser()) {
            if (p & BTN_LTRIGGER) { sel = std::max(0, sel - MENU_ROWS); dirty = true; }
            if (p & BTN_RTRIGGER) { sel = std::min(n - 1, sel + MENU_ROWS); dirty = true; }
        }
    }

    void render_page() {
        canvas.clear(0xFF000000);
        if (screen == Screen::TypePath) {
            osk.render(canvas);
            return;
        }
        Page pg = page();
        int vis = sel;
        if (is_browser()) {
            int total = (int)br.list.size();
            int top = std::max(0, std::min(sel - MENU_ROWS / 2, total - MENU_ROWS));
            vis = sel - top;
        }
        render_menu(canvas, pg.title, pg.tab, pg.rows, std::min(vis, std::max(0, (int)pg.rows.size() - 1)), true);
    }

    void present() {
        render_page();
        SDL_UpdateTexture(tex, nullptr, canvas.px.data(), canvas.w * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
    }
};

void write_ppm(const Canvas& c, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", c.w, c.h);
    std::vector<unsigned char> row((size_t)c.w * 3);
    for (int y = 0; y < c.h; y++) {
        for (int x = 0; x < c.w; x++) {
            uint32_t v = c.px[(size_t)y * c.w + x];
            row[(size_t)x * 3] = v & 0xFF; row[(size_t)x * 3 + 1] = (v >> 8) & 0xFF; row[(size_t)x * 3 + 2] = (v >> 16) & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

int run_headless(const setup::Layout& l, const std::string& iso, const std::string& dir, const std::string& saves) {
    std::string prob = l.problem();
    if (!prob.empty()) { std::fprintf(stderr, "error: %s\n", prob.c_str()); return 2; }
    std::error_code ec;
    fs::create_directories(dir, ec);
    setup::Options o;
    o.import_saves = saves;
    auto plan = setup::plan_install(l, iso, dir, o);
    setup::Progress prog;
    for (size_t i = 0; i < plan.size(); i++) {
        prog.step = (int)i;
        std::printf("== %s\n", plan[i].label.c_str());
        setup::Proc p;
        if (!p.start(plan[i].argv, plan[i].env, "")) { std::fprintf(stderr, "error: %s\n", p.error().c_str()); return 1; }
        while (p.busy()) {
            std::string line;
            while (p.read_line(line)) {
                setup::feed_line(prog, plan, line, SDL_GetTicks64());
                std::printf("%s\n", line.c_str());
            }
            SDL_Delay(20);
        }
        if (p.exit_code() != 0 || !prog.error.empty()) { std::fprintf(stderr, "error: step '%s' failed (%d) %s\n", plan[i].label.c_str(), p.exit_code(), prog.error.c_str()); return 1; }
        prog.fraction = std::max(prog.fraction, setup::step_done_fraction(plan, (int)i));
        std::printf("== %s done (%d%%)\n", plan[i].label.c_str(), (int)(prog.fraction * 100));
    }
    std::printf("installed: %s\n", setup::game_binary(dir).c_str());
    return setup::game_installed(dir) ? 0 : 1;
}

}  // namespace

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
// The wizard is a GUI-subsystem program (no console window when started from Explorer). When started from a terminal its
// --headless / --play output still goes there: attach to the parent console.
static void attach_parent_console() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}
#else
static void attach_parent_console() {}
#endif

// --import-saves FROM --dir DIR: copy the Dissidia 012 saves of FROM (a SAVEDATA folder) into DIR/ms0/PSP/SAVEDATA under
// the installed game's disc ID. Run by the install plan as a step; no window.
static int import_saves_cli(const std::string& from, const std::string& workdir) {
    namespace sm = psp_savemgr;
    const std::string target = sm::installed_disc_id(workdir);
    if (target.empty()) { std::fprintf(stderr, "error: cannot tell the installed game's disc ID in %s\n", workdir.c_str()); return 1; }
    const std::string dest = (fs::path(workdir) / "ms0" / "PSP" / "SAVEDATA").string();
    std::error_code ec;
    fs::create_directories(dest, ec);
    auto saves = sm::scan(from);
    std::printf("%zu save(s) in %s\n", saves.size(), from.c_str());
    int failed = 0;
    for (auto& s : saves) {
        std::string dn;
        std::string err = sm::copy_save(s, dest, target, false, &dn);
        if (err.rfind("already exists", 0) == 0) { std::printf("%s: already in the game, kept\n", s.name.c_str()); continue; }   // never replace progress made in the new install
        if (err.empty()) std::printf("%s -> %s\n", s.name.c_str(), dn.c_str());
        else { std::fprintf(stderr, "%s: %s\n", s.name.c_str(), err.c_str()); failed++; }
    }
    return failed ? 1 : 0;
}

// Unpacks the bundled release, with a small window that only shows a progress bar (title carries the text).
static std::string unpack_payload_with_window(const std::filesystem::path& exe, const setup::Payload& pl,
                                              const std::filesystem::path& dest, bool quiet) {
    if (std::filesystem::exists(dest / ".payload-ok")) return "";
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (!quiet && SDL_Init(SDL_INIT_VIDEO) == 0) {
        win = SDL_CreateWindow("psprecomp setup: unpacking (first start only)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 520, 70, 0);
        if (win) ren = SDL_CreateRenderer(win, -1, 0);
    }
    int last = -1;
    std::string err = setup::extract_payload(exe, pl, dest, [&](double f) {
        int pct = (int)(f * 100);
        if (pct == last) return true;
        last = pct;
        if (quiet) { std::fprintf(stderr, "\runpacking %d%%", pct); if (pct == 100) std::fprintf(stderr, "\n"); }
        if (win) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {}
            std::string t = "psprecomp setup: unpacking (first start only) " + std::to_string(pct) + "%";
            SDL_SetWindowTitle(win, t.c_str());
            if (ren) {
                SDL_SetRenderDrawColor(ren, 30, 30, 30, 255);
                SDL_RenderClear(ren);
                SDL_Rect bg{20, 25, 480, 20}, fg{20, 25, (int)(480 * f), 20};
                SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
                SDL_RenderFillRect(ren, &bg);
                SDL_SetRenderDrawColor(ren, 90, 170, 255, 255);
                SDL_RenderFillRect(ren, &fg);
                SDL_RenderPresent(ren);
            }
        }
        return true;
    });
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    return err;
}

int main(int argc, char** argv) {
    attach_parent_console();
    std::string root, iso, dir, play_dir, shots, import_from, saves_from;
    bool headless = false, play = false, launch = false;
    std::string unpack_to;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; };
        if (a == "--root") next(root);
        else if (a == "--iso") next(iso);
        else if (a == "--dir") next(dir);
        else if (a == "--import-saves") next(import_from);
        else if (a == "--saves") next(saves_from);
        else if (a == "--unpack") next(unpack_to);   // single-file release: unpack the bundled release folder there and exit
        else if (a == "--headless") headless = true;
        else if (a == "--launch") launch = true;
        else if (a == "--screenshots") next(shots);
        else if (a == "--play") { play = true; if (i + 1 < argc && argv[i + 1][0] != '-') play_dir = argv[++i]; }
        else { std::fprintf(stderr, "unknown option %s\n(see the header of setup_main.cpp)\n", a.c_str()); return 2; }
    }
    if (!import_from.empty()) {
        if (dir.empty()) { std::fprintf(stderr, "--import-saves needs --dir DIR\n"); return 2; }
        return import_saves_cli(import_from, dir);
    }
    SDL_SetMainReady();
    // Single-file release: the release folder is appended to this program (payload.h); unpack it once, then run from there.
    if (root.empty() || !unpack_to.empty()) {
        setup::Payload pl;
        std::filesystem::path self = setup::self_exe();
        if (!self.empty() && setup::find_payload(self, pl)) {
            std::filesystem::path dest = unpack_to.empty() ? setup::data_dir() / ("v" + pl.version) : std::filesystem::path(unpack_to);
            std::string err = unpack_payload_with_window(self, pl, dest, headless || !unpack_to.empty());
            if (!err.empty()) {
                std::fprintf(stderr, "error: %s\n", err.c_str());
                if (unpack_to.empty()) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "psprecomp setup", ("Could not unpack the setup files:\n" + err).c_str(), nullptr);
                return 1;
            }
            if (!unpack_to.empty()) { std::printf("unpacked to %s\n", dest.string().c_str()); return 0; }
            root = dest.string();
        } else if (!unpack_to.empty()) { std::fprintf(stderr, "this program has no bundled release (use the single-file download)\n"); return 2; }
    }
    if (root.empty()) {
        char* base = SDL_GetBasePath();
        root = base ? base : ".";
        SDL_free(base);
        while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    }
    setup::Layout layout = setup::find_layout(root);

    char* pref = SDL_GetPrefPath("psprecomp", "setup");
    std::string state_file = std::string(pref ? pref : "./") + "install.ini";
    if (pref) SDL_free(pref);
    setup::State state = setup::load_state(state_file);

    if (headless) {
        if (iso.empty() || dir.empty()) { std::fprintf(stderr, "--headless needs --iso FILE and --dir DIR\n"); return 2; }
        return run_headless(layout, iso, dir, saves_from);
    }
    if (play) {
        std::string wd = play_dir.empty() ? state.workdir : play_dir;
        std::string err;
        if (wd.empty() || !setup::game_installed(wd)) { std::fprintf(stderr, "no installed game found%s\n", wd.empty() ? "" : (" in " + wd).c_str()); return 1; }
        return setup::launch_game(wd, &err) ? 0 : (std::fprintf(stderr, "%s\n", err.c_str()), 1);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (shots.empty() && !state.workdir.empty() && setup::game_installed(state.workdir)) {
        std::string where, serr;   // installs from before the menu entry existed get one at the next start
        setup::create_shortcut(state.workdir, &where, &serr);
    }
    App app;
    app.layout = layout;
    app.layout_problem = layout.problem();
    app.state = state;
    app.state_file = state_file;
    app.workdir = state.workdir;
    app.iso = state.iso;
    app.opt = state.options;
    app.launch_mode = launch;

    if (!shots.empty()) {   // look check without a window
        std::error_code ec;
        fs::create_directories(shots + "/game", ec);
        if (FILE* f = std::fopen((shots + "/game/features.ini").c_str(), "wb")) { std::fputs("hd_textures=1\njp_audio=1\nandroid=1\n", f); std::fclose(f); }
        app.layout.game_dir = shots + "/game";
        app.workdir = "/home/player/Games/Dissidia012";
        app.iso = "/home/player/Downloads/Dissidia 012 (Europe).iso";
        app.opt.hd_textures = true;
        app.br.open(setup::home_dir(), false);
        int n = 0;
        auto shot = [&](const char* name) { app.render_page(); write_ppm(app.canvas, shots + "/" + name + ".ppm"); n++; };
        app.layout_problem.clear();
        app.go(Screen::Welcome); shot("1_welcome");
        app.pick = Pick::Iso; app.go(Screen::Waiting); shot("2_waiting");
        app.go(Screen::PickIso); shot("2b_pick_iso");
        app.go(Screen::InstallDir); shot("3_install_dir");
        app.go(Screen::Options, 1); shot("4_options");
        app.go(Screen::Options, 5); shot("4b_options_scrolled");
        setup::Options o; o.android = true; o.hd_textures = true; o.jp_audio = true; o.undub_iso = "/x.iso";
        app.plan = setup::plan_install(app.layout, app.iso, app.workdir, o);
        app.prog.step = 1; app.prog.stage = "Compiling the game"; app.prog.fraction = 0.42;
        app.prog.explain = "Compiles about 470 source files with the bundled compiler. This is the long part.";
        app.prog.detail = "Compiled 183 of 474: batch_0165.cpp";
        app.prog.start_ms = (long long)now_ms() - 7 * 60 * 1000; app.prog.compile_i = 183; app.prog.compile_n = 474; app.prog.compile_start_ms = (long long)now_ms() - 4 * 60 * 1000;
        app.go(Screen::Build, 3); shot("5_build");
        app.go(Screen::Done); shot("6_done");
        app.fail_msg = "The installation was cancelled.";
        app.go(Screen::Failed); shot("7_failed");
        app.osk.begin(u"Folder path", u"/home/player", 200, true);
        app.go(Screen::TypePath); shot("8_type_path");
        std::printf("%d screenshots in %s\n", n, shots.c_str());
        SDL_Quit();
        return 0;
    }

    app.win = SDL_CreateWindow("Dissidia 012 Setup", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480 * SCALE, 272 * SCALE,
                               SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!app.win) { std::fprintf(stderr, "cannot create a window: %s\n", SDL_GetError()); return 1; }
    if (SDL_Surface* ic = SDL_CreateRGBSurfaceWithFormatFrom((void*)k_app_icon_rgba, k_app_icon_w, k_app_icon_h, 32, k_app_icon_w * 4, SDL_PIXELFORMAT_RGBA32)) {
        SDL_SetWindowIcon(app.win, ic);
        SDL_FreeSurface(ic);
    }
    app.ren = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!app.ren) app.ren = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_SOFTWARE);
    if (!app.ren) { std::fprintf(stderr, "cannot create a renderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_RenderSetLogicalSize(app.ren, 480 * SCALE, 272 * SCALE);
    app.tex = SDL_CreateTexture(app.ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, app.canvas.w, app.canvas.h);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && (app.pad = SDL_GameControllerOpen(i))) break;
    SDL_StartTextInput();
    if (app.installed() && app.layout_problem.empty()) {
        if (setup::update_check_enabled(app.state.workdir)) app.begin_check(false);
        else if (launch) app.play();
    }

    while (!app.quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                if (app.building) app.proc.kill();
                app.quit = true;
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (!app.pad) app.pad = SDL_GameControllerOpen(ev.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (app.pad && ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(app.pad))) {
                    SDL_GameControllerClose(app.pad);
                    app.pad = nullptr;
                }
                break;
            case SDL_TEXTINPUT:
                if (app.screen == Screen::TypePath)
                    for (const unsigned char* c = (const unsigned char*)ev.text.text; *c; c++)
                        if (*c >= 0x20 && *c < 0x7F) psp_sddialog::osk_push_key(*c);
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_F11 || (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                    bool fs_on = SDL_GetWindowFlags(app.win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(app.win, fs_on ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                } else if (app.screen == Screen::TypePath) {
                    if (ev.key.keysym.sym == SDLK_BACKSPACE) psp_sddialog::osk_push_key(8);
                    else if (ev.key.keysym.sym == SDLK_RETURN) psp_sddialog::osk_push_key(13);
                    else if (ev.key.keysym.sym == SDLK_ESCAPE) { psp_sddialog::osk_set_active(false); app.go(Screen::BrowseDir); }
                }
                break;
            case SDL_WINDOWEVENT:
                app.dirty = true;
                break;
            }
        }
        app.poll_native();
        if (app.checking) app.pump_check();
        if (app.building) app.pump_build();
        app.update_input();
        int64_t t = now_ms();
        if (app.dirty || (app.building && t - app.last_draw > 250)) {
            app.present();
            app.dirty = false;
            app.last_draw = t;
        }
        SDL_Delay(8);
    }
    if (app.building) app.proc.kill();
    if (app.tex) SDL_DestroyTexture(app.tex);
    if (app.ren) SDL_DestroyRenderer(app.ren);
    if (app.win) SDL_DestroyWindow(app.win);
    SDL_Quit();
    return 0;
}

#ifdef _WIN32
// GUI subsystem entry (SDL_MAIN_HANDLED keeps our own main()).
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
