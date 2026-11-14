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
#ifndef __ANDROID__
#define SDL_MAIN_HANDLED   // keep the plain main() on Windows (see SDL_SetMainReady below)
#endif
#include <SDL.h>

#ifdef __ANDROID__
#include <fcntl.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../runtime/src/hle/psp_savedata_dialog.h"
#include "../runtime/src/hle/psp_savemgr.h"
#include "android.h"
#include "install.h"
#include "../runtime/include/psp_app_icon_installer.h"   // the wizard has its own icon, not the game's
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
#ifdef __ANDROID__
        if (cur == "/storage/emulated/0") parent = "/storage";   // /storage/emulated itself cannot be listed
#endif
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
#ifdef __ANDROID__
            if (cur == "/storage") {   // the volumes: internal storage ("emulated/0") and any SD card (named by its volume id)
                if (name == "self") continue;
                Entry e; e.dir = true;
                if (name == "emulated") { e.name = "Internal storage/"; e.path = "/storage/emulated/0"; }
                else { e.name = "SD card " + name + "/"; e.path = it->path().string(); }
                dirs.push_back(e);
                continue;
            }
#endif
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
enum class Screen { Welcome, PickIso, PickUndub, Waiting, InstallDir, BrowseDir, TypePath, Options, Build, Done, Failed, Checking, UpdateAvail, Message, Permission, AndroidInstall, Download };
// What a file/folder pick is for.
enum class Pick { Iso, Undub, Dir };

struct App {
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture* tex = nullptr;
    SDL_GameController* pad = nullptr;
    Canvas canvas{SCALE};
    psp_sddialog::SettingsView view;   // the same screen as the game's Settings menu (sidebar, list, description panel)
    bool view_opened = false;
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
    std::string iso, workdir, iso_name;
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

    // Every page is drawn by the game's Settings screen renderer; the Options page has two categories (sidebar): L / R or a tap on one switch.
    int opt_tab = 0;
    enum OptRow { OR_UPDATES, OR_HD, OR_JP, OR_ANDROID, OR_SAVES, OR_FOLDER, OR_START, OR_BACK };
    bool extras_mode = false;   // the Options page opened from "Change extras" on an installed game: only the extras, applied without a rebuild
    bool extras_run = false;    // the running plan is such an apply
    bool extras_skip_tex = false;
    const std::vector<std::string>& option_tabs() const {
        static const std::vector<std::string> both = {"Extras", "General"}, one = {"Extras"};
        return extras_mode ? one : both;
    }
    std::vector<OptRow> option_rows() const {
        std::vector<OptRow> r;
        if (opt_tab == 0) {
            r = {OR_HD, OR_JP};
#ifndef __ANDROID__
            r.push_back(OR_ANDROID);
            r.push_back(OR_SAVES);
#endif
            r.push_back(OR_START);
            r.push_back(OR_BACK);
        } else {
            r = {OR_UPDATES};
#ifndef __ANDROID__
            r.push_back(OR_FOLDER);
#endif
            r.push_back(OR_START);
            r.push_back(OR_BACK);
        }
        return r;
    }
    void go_options(OptRow focus = OR_START) {   // the Options page on the Extras tab with `focus` selected
        opt_tab = 0;
        std::vector<OptRow> r = option_rows();
        int i = 0;
        for (size_t k = 0; k < r.size(); k++) if (r[k] == focus) i = (int)k;
        go(Screen::Options, i);
    }
    void switch_tab(int dir) {
        const int n = (int)option_tabs().size();
        opt_tab = (opt_tab + dir + n) % n;
        sel = 0;
        view.page_changed(dir);
        view.rows_changed();
        dirty = true;
    }

    // build
    setup::Proc proc;
    std::vector<setup::Step> plan;
    setup::Progress prog;
    bool building = false;
    std::string fail_msg;
    int64_t last_draw = 0;

#ifdef __ANDROID__
    bool installed() const { return setup::android_build_exists(state.workdir); }
    std::string install_note;   // the last result of the package installer
    // updates (Android): the app downloads manifest.json + .sig + the APK itself (android.h download), update.py android-check / android-verify check them
    int ustage = 0;   // 0 idle | 1 manifest | 2 signature | 3 check running | 4 downloading the APK | 5 verifying the APK
    std::string ufiles, manifest_url;   // <files>/update
    bool verify_ok = false;   // update.py android-verify printed OK (lines and the process end arrive in different frames)
    int64_t ustart = 0;
    bool need_rebuild() const {   // the app was updated since the game was built: the translation / core may differ
        return installed() && !layout.android_dir.empty() && setup::android_payload_fingerprint(layout) != state.built_fp;
    }
#else
    bool installed() const { return setup::game_installed(state.workdir); }
#endif

    void go(Screen s, int select = 0) {
        screen = s; sel = select; dirty = true;
        view.rows_changed();
#ifdef __ANDROID__
        if (s == Screen::TypePath) SDL_StartTextInput(); else SDL_StopTextInput();   // the soft keyboard only for typing a path
#endif
    }

    // ---- page content ----
    struct Page {
        std::string title, tab;
        std::vector<MenuRow> rows;
        std::vector<std::string> tabs;   // more than one: drawn as the tab strip, tab_active the shown one
        int tab_active = 0;
    };

    static MenuRow row(const std::string& label, const std::string& help = "", const std::string& value = "") {
        MenuRow r; r.label = label; r.help = help; r.value = value; return r;
    }

#ifdef __ANDROID__
    bool native_ok() const { return false; }   // no system file dialog: the built-in browser
#else
    bool native_ok() const { return pfd::settings::available(); }
#endif

    MenuRow toggle_row(const std::string& label, const std::string& help, bool on, bool available) {
        MenuRow r = row(label, available ? help : help + " (Not in this release yet.)", available ? (on ? "On" : "Off") : "Not yet");
        r.can_dec = r.can_inc = available;
        return r;
    }

    // HD textures: Off / LiinkPK's hand-retouched pack (first) / the AI upscaled pack. The help text credits the pack that is selected.
    MenuRow tex_row(bool available) {
        MenuRow r = row("HD textures", "", "Not yet");
        if (!available) { r.help = "Higher resolution textures for the whole game. (Not in this release yet.)"; return r; }
        r.can_dec = r.can_inc = true;
        if (!opt.hd_textures) {
            r.value = "Off";
            r.help = "No HD textures: the game looks like the PSP original. Left/Right picks a pack: LiinkPK's hand-retouched HD Project (about 0.8 GB) or the AI upscaled pack (about 1.3 GB).";
        } else if (opt.texture_pack == "ai") {
            r.value = "AI upscaled";
            r.help = std::string("D012FFRT plus our own AI upscales of the other textures (about 1.3 GB). ") + setup::texture_pack_credit("ai");
        } else {
            r.value = "LiinkPK's HD Project";
            r.help = std::string("Hand-retouched HD textures, redrawn faces and text (about 0.8 GB). Still a work in progress, so some textures stay original. ") + setup::texture_pack_credit("liink");
        }
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
            }
#ifdef __ANDROID__
            else if (installed()) {
                p.tab = need_rebuild() ? "Update" : "Built";
                if (need_rebuild())
                    p.rows.push_back(row("Update the game", "This app was updated since the game was built. Builds the game again from the files already on this device (no game file needed), "
                                                            "then installs it. About 5 minutes."));
                p.rows.push_back(row("Install the game", "The game is built. This installs it as an app (Android asks you to confirm)."));
                p.rows.push_back(row("Change extras", "Turn the HD textures or the Japanese voices on or off, or pick another texture pack, without building the game again."));
                p.rows.push_back(row("Build it again", "Pick your game file and build from scratch (your saves are kept)."));
                p.rows.push_back(row("Check for updates", "Looks for a newer version of this app (it contacts github.com; nothing about you or your game is sent)."));
                p.rows.push_back(row("Quit"));
            } else {
                p.tab = "Welcome";
                p.rows = {row("Build the game", "Builds the game on this device from your own copy of Dissidia 012 (EU or US, ISO or CSO) and installs it. "
                                                "Nothing from the game is downloaded or shipped with this app. It takes about 10 to 20 minutes."),
                          row("Check for updates", "Looks for a newer version of this app (it contacts github.com; nothing about you or your game is sent)."),
                          row("Quit")};
            }
#else
            else if (installed()) {
                p.tab = "Installed";
                p.rows = {row("Play", "Start the game."),
                          row("Change extras", "Turn the HD textures, the Japanese voices or the Android version on or off, or pick another texture pack, without rebuilding the game."),
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
#endif
            break;
        }
        case Screen::Permission: {
            p.title = "ALLOW ACCESS";
            p.tab = "Files";
#ifdef __ANDROID__
            bool ok = setup::android::has_all_files_access();
            p.rows = {row("Allow access to files", "To give the game its files (Android keeps them in a shared folder), this app needs \"all files access\". "
                                                   "Android opens a settings page: switch it on for this app, then come back.", ok ? "Allowed" : "Needed"),
                      row("Continue", iso.empty() ? "Pick your game file." : "Start building."), row("Back")};
#endif
            break;
        }
        case Screen::Download: {
            p.title = "UPDATING";
            p.tab = upd.version;
#ifdef __ANDROID__
            std::string st = setup::android::download_status();
            double f = 0;
            std::string line = ustage == 5 ? "Checking the download..." : "Downloading the update...";
            if (st.compare(0, 8, "working:") == 0) {
                size_t c2 = st.find(':', 8);
                double done = std::atof(st.substr(8, c2 - 8).c_str()), total = c2 == std::string::npos ? 0 : std::atof(st.substr(c2 + 1).c_str());
                if (total <= 0) total = (double)upd.size;
                f = total > 0 ? std::min(1.0, done / total) : 0;
            } else if (st == "ok") f = 1;
            std::string bar = "[";
            for (int i = 0; i < 20; i++) bar += i < (int)(f * 20 + 0.5) ? '#' : '-';
            bar += "]";
            p.rows = {row(line, "Android then asks you to confirm the installation of the new version. The game and your saves are not touched.", bar), row("Cancel")};
#endif
            break;
        }
        case Screen::AndroidInstall: {
            p.title = "INSTALLING THE GAME";
            p.tab = "Android";
#ifdef __ANDROID__
            std::string st = setup::android::install_status();
            std::string line = st.empty() || st == "working" ? "Waiting for Android..." : st == "ok" ? "Installed" : st;
            p.rows = {row(line, st == "ok" ? "Open \"Dissidia 012\" from your app list. The first start prepares graphics for a moment."
                                           : "Android shows a confirmation: tap Install. If it asks for permission to install apps from this app, allow it and press Install again."),
                      row("Install again", "Ask Android to install the built game again."), row("Back")};
#endif
            break;
        }
        case Screen::Waiting:
            p.title = pick == Pick::Dir ? "CHOOSE A FOLDER" : pick == Pick::Undub ? "CHOOSE THE UNDUB FILE" : "CHOOSE YOUR GAME FILE";
            p.tab = "Waiting";
#ifdef __ANDROID__
            p.rows = {row("Choose the game file", "Opens Android's file picker. Pick your Dissidia 012 disc image (.iso or .cso) from the SD card, Download or anywhere else."),
                      row("Use the simple browser instead", "Browse the storage inside this app. Needs the \"all files\" permission."),
                      row("Back")};
#else
            p.rows = {row("Use the simple browser instead", "A file window opened. Pick the file there, or press here to browse inside this program."),
                      row("Back")};
#endif
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
            p.tab = option_tabs()[(size_t)opt_tab];
            p.tabs = option_tabs();
            p.tab_active = opt_tab;
            bool tex = setup::feature_available(layout, "hd_textures"), jp = setup::feature_available(layout, "jp_audio"),
                 an = setup::feature_available(layout, "android");
            if (!save_sources_scanned) {
#ifdef __ANDROID__
                save_sources.clear();
#else
                save_sources = psp_savemgr::detect_sources(true);
#endif
                save_sources_scanned = true;
            }
#ifdef __ANDROID__
            const std::string tex_note = " The download goes over your connection (WiFi is best) and needs about 3 GB of free space while it unpacks.";
#else
            const std::string tex_note;
#endif
            for (OptRow k : option_rows()) {
                switch (k) {
                case OR_UPDATES:
                    p.rows.push_back(toggle_row("Check for updates", "Look for new releases when the app starts (changeable later in the game's settings.ini).", opt.check_updates, true));
                    break;
                case OR_HD:
                    p.rows.push_back(tex_row(tex));
                    if (!tex_note.empty() && !p.rows.back().disabled) p.rows.back().help += tex_note;
                    break;
                case OR_JP:
                    p.rows.push_back(toggle_row("Japanese voices (undub)", "Adds the Japanese voices and movies from a second disc image (an undub ISO) that you pick next.", opt.jp_audio, jp));
                    break;
                case OR_ANDROID:
                    p.rows.push_back(toggle_row("Android version", "Also builds the Android version (an APK for phones and handhelds, 64-bit ARM). Needs a one-time download of Google's Android NDK (about 0.7 GB; by turning this on you accept its license, developer.android.com/studio/terms) and some extra minutes. Afterwards you can install it on a connected device from the last page.", opt.android, an));
                    break;
                case OR_SAVES: {
                    MenuRow sv = row("Import saves", "", "None found");
                    if (!save_sources.empty()) {
                        sv.help = "Copies your Dissidia 012 saves from PPSSPP or a memory stick into the game, whatever their region. The originals are not changed. You can also do this later in the game (Triangle in the save list).";
                        sv.value = "Off";
                        for (auto& src : save_sources)
                            if (src.path == opt.import_saves) sv.value = src.label + " (" + std::to_string(psp_savemgr::scan(src.path).size()) + ")";
                        sv.can_dec = sv.can_inc = true;
                    } else sv.help = "No PPSSPP or memory stick saves of Dissidia 012 were found. You can import saves later in the game (Triangle in the save list).";
                    p.rows.push_back(sv);
                    break;
                }
                case OR_FOLDER:
                    p.rows.push_back(row("Install folder", "The game's files and your saves go here. Press Confirm to pick another folder.", shorten_left(workdir, 34)));
                    break;
                case OR_START:
                    if (extras_mode) p.rows.push_back(row("Apply changes", "Downloads or adds the extras you turned on and removes the HD textures if you turned them off. The game is not rebuilt. Turning the Japanese voices off keeps the files already added."));
                    else p.rows.push_back(row("Start installing", "Reads " + shorten_left(iso, 40) + " and builds the game in " + shorten_left(workdir, 34) +
                                                                ". You can cancel and continue later."));
                    break;
                case OR_BACK:
                    p.rows.push_back(row("Back"));
                    break;
                }
            }
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
#ifdef __ANDROID__
            help += " Android asks you to confirm the installation. If the update changes how the game is built, you are offered to build it again afterwards (no game file needed).";
#else
            help += upd.rebuild ? " The game has to be built again afterwards, as at installation." :
                    upd.relink ? " The game is relinked afterwards (a short step)." : " Only program files change.";
#endif
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
#ifdef __ANDROID__
            p.rows = {row("Install the game", "The game is built and its files are in place. This installs the app: Android asks you to confirm."), row("Quit")};
            break;
#endif
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
    void start_plan(bool rebuild, bool update = false, bool android_install = false, bool extras = false) {
        extras_run = extras;
        rebuild_only = rebuild || update || android_install || extras;
        updating = update;
        android_inst = android_install;
        const setup::Options& o = (rebuild || update) ? state.options : opt;
        if (extras) {}   // plan already built by start_extras()
        else
#ifdef __ANDROID__
        if (!rebuild) plan = setup::plan_install_android(layout, iso, workdir, opt);
        else if (!android_install && !update) plan = setup::plan_rebuild_android(layout, state.workdir);
        else
#endif
        if (android_install) plan = setup::plan_android_install(layout, cur_workdir());
        else if (update) plan = setup::plan_update(layout, state.workdir, o, upd);
        else plan = rebuild ? setup::plan_rebuild(layout, state.workdir, o) : setup::plan_install(layout, iso, workdir, o);
        log_open((rebuild || update || extras) ? state.workdir : workdir);
        prog = setup::Progress();
        prog.stage = plan[0].label;
        prog.explain = plan[0].explain;
        prog.step = 0;
        prog.start_ms = now_ms();
        building = true;
#ifdef __ANDROID__
        setup::android::keep_awake(true);
#endif
        start_step();
        go(Screen::Build, 3);
    }

#ifdef __ANDROID__
    // A step with `downloads` is carried out by the app itself (Android's HTTPS stack; the bundled Python has no DNS / CA store).
    bool dl_active = false;
    size_t dl_i = 0;
    bool dl_step() const { return building && prog.step < (int)plan.size() && !plan[(size_t)prog.step].downloads.empty(); }
    void dl_begin() {   // starts the next missing file (a finished one from an earlier try is kept)
        const auto& d = plan[(size_t)prog.step].downloads;
        std::error_code ec;
        while (dl_i < d.size() && fs::is_regular_file(d[dl_i].second, ec)) dl_i++;
        if (dl_i >= d.size()) { dl_active = false; return; }
        prog.detail = "Starting download " + std::to_string(dl_i + 1) + " of " + std::to_string(d.size()) + "...";
        setup::android::download(d[dl_i].first, d[dl_i].second);
        dl_active = true;
    }
    // returns true when the step's downloads are all done
    bool dl_pump() {
        if (!dl_active) return true;
        const auto& d = plan[(size_t)prog.step].downloads;
        std::string st = setup::android::download_status();
        const setup::Step& stp = plan[(size_t)prog.step];
        if (st.compare(0, 8, "working:") == 0) {
            long long b = 0, t = 0;
            std::sscanf(st.c_str() + 8, "%lld:%lld", &b, &t);
            double f = t > 0 ? (double)b / (double)t : 0;
            prog.detail = "File " + std::to_string(dl_i + 1) + " of " + std::to_string(d.size()) + ": " + human_size((uintmax_t)b) + (t > 0 ? " of " + human_size((uintmax_t)t) : "");
            prog.fraction = std::max(prog.fraction, stp.base + stp.weight * ((double)dl_i + f) / (double)d.size());
            dirty = true;
            return false;
        }
        if (st == "ok") {
            dl_i++;
            dl_begin();
            dirty = true;
            return !dl_active;
        }
        if (st.compare(0, 6, "failed") == 0) {
            dl_active = false;
            fail("The download failed (" + st.substr(st.size() > 8 ? 8 : 0) + "). Check your connection and try again; finished files are kept.");
            return false;
        }
        return false;
    }
#endif

    void start_step() {
#ifdef __ANDROID__
        if (dl_step()) { dl_i = 0; prog.stage = plan[(size_t)prog.step].label; prog.explain = plan[(size_t)prog.step].explain; dl_begin(); return; }
#endif
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
#ifdef __ANDROID__
        setup::android::keep_awake(false);
#endif
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
        bool step_over = building && !proc.busy();
        int code = 0;
#ifdef __ANDROID__
        if (dl_step()) { step_over = dl_pump() && building; code = 0; }
        else
#endif
        if (step_over) code = proc.exit_code();
        if (step_over) {
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
#ifdef __ANDROID__
                setup::android::keep_awake(false);
#endif
                if (!rebuild_only) {
                    state.workdir = workdir;
                    state.iso = iso;
                    state.version = "dev";
                    state.options = opt;
                    write_settings();
                }
#ifdef __ANDROID__
                state.workdir = rebuild_only ? state.workdir : workdir;
                if (!extras_run) state.built_fp = setup::android_payload_fingerprint(layout);
#endif
                if (extras_run) { state.options = opt; state.options.undub_iso.clear(); }
                setup::save_state(state_file, state);
#ifdef __ANDROID__
                if (false)
#endif
                if (!rebuild_only) {
                    std::string where, serr;
                    if (setup::create_shortcut(workdir, layout.root, &where, &serr)) shortcut_note = "A menu entry was added: " + where;
                    else shortcut_note = "No menu entry (" + serr + ")";
                }
                if (extras_run) {
                    extras_mode = false;
                    msg_title = "EXTRAS UPDATED";
                    msg_text = "The extras are in place.";
                    go(Screen::Message);
                } else go(Screen::Done);
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
#ifdef __ANDROID__
        manual_check = manual;
        check_result.clear();
        std::error_code e2;
        ufiles = setup::android::files_dir() + "/update";
        fs::create_directories(ufiles, e2);
        fs::remove(ufiles + "/manifest.json", e2);
        fs::remove(ufiles + "/manifest.json.sig", e2);
        manifest_url = setup::kManifestUrl;
        if (std::ifstream mf{setup::android::files_dir() + "/manifest_url.txt"}) {   // debug / test switch: another manifest (still needs a valid signature)
            std::string u; std::getline(mf, u);
            while (!u.empty() && (u.back() == '\r' || u.back() == ' ')) u.pop_back();
            if (!u.empty()) manifest_url = u;
        }
        setup::android::download(manifest_url, ufiles + "/manifest.json");
        ustage = 1;
        checking = true;
        check_start = now_ms();
        if (manual) go(Screen::Checking);   // the start-up check runs silently
        return;
#endif
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
#ifdef __ANDROID__
        ustage = 0;
        if (!manual_check && screen != Screen::Welcome) return;   // the silent start-up check never interrupts what the player is doing
#endif
        if (u.available) { go(Screen::UpdateAvail); return; }
        if (manual_check) {
            msg_title = "UPDATES";
            msg_text = u.error.empty() ? "You have the newest version." : "Could not check for updates: " + u.error;
            go(Screen::Message);
        } else if (launch_mode) play();
        else go(Screen::Welcome);
    }

#ifdef __ANDROID__
    void update_log(const std::string& l) {   // <files>/update/update.log: what the updater did (the app has no console)
        if (FILE* f = std::fopen((ufiles + "/update.log").c_str(), "ab")) { std::fprintf(f, "%s\n", l.c_str()); std::fclose(f); }
    }

    void pump_check_android() {
        if (!checking || ustage < 1 || ustage > 3) return;
        std::string st = setup::android::download_status();
        auto failed = [&] { return st.compare(0, 6, "failed") == 0; };
        if (now_ms() - check_start > 30000) {
            setup::android::cancel_download();
            if (check_proc.busy()) check_proc.kill();
            setup::UpdateInfo u; u.error = "timed out";
            finish_check(u);
            return;
        }
        if (ustage == 1) {
            if (st == "ok") { setup::android::download(manifest_url + ".sig", ufiles + "/manifest.json.sig"); ustage = 2; }
            else if (failed()) { setup::UpdateInfo u; u.error = st.substr(std::min<size_t>(st.size(), 8)); finish_check(u); }
        } else if (ustage == 2) {
            if (st == "ok" || failed()) {   // a missing signature is judged by update.py (an error when this app carries a key)
                if (!check_proc.start(setup::android_check_argv(layout, ufiles + "/manifest.json", ufiles + "/manifest.json.sig", setup::android::app_version()),
                                      setup::android_env(layout, workdir), "")) { setup::UpdateInfo u; u.error = "cannot run the checker"; finish_check(u); return; }
                ustage = 3;
            }
        } else if (ustage == 3) {
            std::string line;
            while (check_proc.read_line(line))
                if (line.compare(0, 6, "UPDATE") == 0 || line.compare(0, 8, "NOUPDATE") == 0 || line.compare(0, 5, "ERROR") == 0) check_result = line;
            if (!check_proc.busy()) finish_check(setup::parse_update_line(check_result));
        }
    }

    // the update's APK: downloaded by Java, checked by update.py android-verify, installed by the PackageInstaller
    void pump_download_android() {
        if (screen != Screen::Download) return;
        std::string st = setup::android::download_status();
        if (ustage == 4) {
            dirty = true;
            if (st == "ok") {
                update_log("verifying " + ufiles + "/dissidia-setup.apk sha256=" + upd.sha256 + " size=" + std::to_string(upd.size));
                if (!check_proc.start(setup::android_verify_argv(layout, ufiles + "/dissidia-setup.apk", upd.sha256, upd.size), setup::android_env(layout, workdir), "")) {
                    msg_title = "UPDATE FAILED"; msg_text = "Cannot run the checker."; go(Screen::Message); ustage = 0; return;
                }
                verify_ok = false;
                ustage = 5;
            } else if (st.compare(0, 6, "failed") == 0) {
                msg_title = "UPDATE FAILED"; msg_text = "The download failed: " + st.substr(std::min<size_t>(st.size(), 8)); ustage = 0; go(Screen::Message);
            }
        } else if (ustage == 5) {
            std::string line;
            while (check_proc.read_line(line)) { update_log("verify: " + line); if (line == "OK") verify_ok = true; }
            if (!check_proc.busy()) {
                ustage = 0;
                if (verify_ok) { setup::android::install_apk(ufiles + "/dissidia-setup.apk"); go(Screen::AndroidInstall); }
                else { msg_title = "UPDATE FAILED"; msg_text = "The downloaded file does not match what the release lists, so it was deleted. Try again later."; go(Screen::Message); }
            }
        }
    }
#endif

    void pump_check() {
        std::string line;
        while (check_proc.read_line(line)) {
            if (line.compare(0, 6, "UPDATE") == 0 || line.compare(0, 8, "NOUPDATE") == 0 || line.compare(0, 5, "ERROR") == 0) check_result = line;
        }
#ifdef __ANDROID__
        return;   // pump_check_android drives it
#endif
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
        if (!cur.empty() && cur.compare(0, 3, "fd:") != 0) return fs::path(cur).parent_path();
#ifdef __ANDROID__
        return fs::path("/storage/emulated/0");
#endif
        fs::path dl = fs::path(setup::home_dir()) / "Downloads";
        return fs::is_directory(dl, ec) ? dl : fs::path(setup::home_dir());
    }

    void begin_pick(Pick k, Screen back) {
        pick = k;
        before_pick = back;
#ifdef __ANDROID__
        if (k == Pick::Iso || k == Pick::Undub) {   // Android's own file picker; the built-in browser stays as the fallback (the Waiting page)
            setup::android::pick_file();
            go(Screen::Waiting);
            return;
        }
#endif
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
            if (before_pick == Screen::Options) go(Screen::Options, 0); else go(Screen::InstallDir);
        } else if (pick == Pick::Undub) {
            opt.undub_iso = path;
            go_options();
            start_install_or_ask();
        } else {
            iso = path;
#ifdef __ANDROID__
            std::error_code e2;
            fs::create_directories(workdir, e2);   // workdir = <app files>/work (set at start)
            if (setup::android::has_all_files_access()) go_options(); else go(Screen::Permission);
            return;
#endif
            if (workdir.empty()) workdir = !state.workdir.empty() ? state.workdir : setup::default_install_dir();
            go(Screen::InstallDir);
        }
    }

    void poll_native() {
#ifdef __ANDROID__
        if (screen == Screen::Waiting) {
            std::string st = setup::android::pick_status();
            if (st.compare(0, 3, "ok:") == 0) {
                size_t c = st.find(':', 3);
                int fd = std::atoi(st.substr(3, c - 3).c_str());
                int fl = fcntl(fd, F_GETFD);
                if (fl >= 0) fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC);   // the build tools inherit it (setup_from_iso.py "fd:N")
                std::string fdpath = "fd:" + std::to_string(fd);
                setup::android::clear_pick();
                if (pick == Pick::Undub) { picked(fdpath); return; }   // the second disc image of the undub option
                iso = fdpath;
                iso_name = c == std::string::npos ? "game" : st.substr(c + 1);
                picked(iso);
                return;
            }
            if (st == "cancelled" || st.compare(0, 6, "failed") == 0) { setup::android::clear_pick(); dirty = true; }
        }
#endif
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

    void begin_extras() {
        extras_mode = true;
        workdir = state.workdir;
        opt = state.options;
        opt.undub_iso.clear();   // a saved path (or Android descriptor) from the install is no longer valid
        go_options(OR_HD);
    }

    // Applies the extras that differ from what is installed: no rebuild.
    void start_extras() {
        const setup::Options& prev = state.options;
        std::error_code ec;
        const fs::path tex = fs::path(workdir) / "mods" / "textures";
        const bool present = fs::is_regular_file(tex / "textures.ini", ec);
        const bool pack_changed = prev.hd_textures && opt.hd_textures && prev.texture_pack != opt.texture_pack;
        extras_skip_tex = !(opt.hd_textures && (!prev.hd_textures || pack_changed || !present));
        if (!opt.hd_textures || pack_changed) {   // turned off, or another pack: the old files must not stay
            fs::remove_all(tex, ec);
#ifdef __ANDROID__
            fs::remove_all(fs::path(setup::android_game_dir()) / "mods" / "textures", ec);
#endif
        }
        setup::Options o = opt;
        if (prev.jp_audio && o.undub_iso.empty()) o.jp_audio = false;   // already added: nothing to extract
        if (prev.android && setup::android_build_exists(workdir)) o.android = false;   // its APK is already built
#ifdef __ANDROID__
        plan = setup::plan_extras_android(layout, workdir, o, extras_skip_tex);
#else
        plan = setup::plan_extras(layout, workdir, o, extras_skip_tex);
#endif
        if (plan.empty()) {
            state.options = opt; state.options.undub_iso.clear();
            setup::save_state(state_file, state);
            extras_mode = false;
            msg_title = "EXTRAS";
            msg_text = "Nothing needed to be added: the extras you chose are already in place.";
            go(Screen::Message);
            return;
        }
        start_plan(false, false, false, true);
    }

    void start_install_or_ask() {
        if (extras_mode) {
            if (opt.jp_audio && !state.options.jp_audio && opt.undub_iso.empty() && setup::feature_available(layout, "jp_audio")) { begin_pick(Pick::Undub, Screen::Options); return; }
            start_extras();
            return;
        }
        // the undub option needs its disc image first
        if (opt.jp_audio && opt.undub_iso.empty() && setup::feature_available(layout, "jp_audio")) { begin_pick(Pick::Undub, Screen::Options); return; }
        std::error_code ec;
        fs::create_directories(workdir, ec);
        if (ec) { fail("Cannot create " + workdir + ": " + ec.message()); return; }
        start_plan(false);
    }

    void toggle_option(OptRow k, int dir = 1) {
        switch (k) {
        case OR_UPDATES: opt.check_updates = !opt.check_updates; break;
        case OR_HD:   // cycle (Right / select): LiinkPK -> AI upscaled -> Off -> LiinkPK; Left goes the other way
            if (setup::feature_available(layout, "hd_textures")) {
                int cur = !opt.hd_textures ? 2 : opt.texture_pack == "ai" ? 1 : 0;
                cur = (cur + (dir < 0 ? 2 : 1)) % 3;
                opt.hd_textures = cur != 2;
                if (cur != 2) opt.texture_pack = cur == 1 ? "ai" : "liink";
            }
            break;
        case OR_JP:
            if (setup::feature_available(layout, "jp_audio")) { opt.jp_audio = !opt.jp_audio; if (!opt.jp_audio) opt.undub_iso.clear(); }
            break;
        case OR_ANDROID: if (setup::feature_available(layout, "android")) opt.android = !opt.android; break;
        case OR_SAVES: {   // Off -> each detected source -> Off
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

    OptRow option_at(int i) const {
        std::vector<OptRow> r = option_rows();
        return i >= 0 && i < (int)r.size() ? r[(size_t)i] : OR_BACK;
    }

    void confirm() {
        switch (screen) {
        case Screen::Welcome:
            if (!layout_problem.empty()) { quit = true; break; }
#ifdef __ANDROID__
            if (installed()) {
                int i = sel - (need_rebuild() ? 1 : 0);
                if (need_rebuild() && sel == 0) { workdir = state.workdir; start_plan(true); }
                else if (i == 0) { setup::android::install_apk(state.workdir + "/android/dissidia.apk"); go(Screen::AndroidInstall); }
                else if (i == 1) begin_extras();
                else if (i == 2) begin_pick(Pick::Iso, Screen::Welcome);
                else if (i == 3) begin_check(true);
                else quit = true;
            } else if (sel == 0) begin_pick(Pick::Iso, Screen::Welcome);
            else if (sel == 1) begin_check(true);
            else quit = true;
            break;
#endif
            if (installed()) {
                switch (sel) {
                case 0: play(); break;
                case 1: begin_extras(); break;
                case 2: begin_check(true); break;
                case 3: opt = state.options; start_plan(true); break;
                case 4: begin_pick(Pick::Iso, Screen::Welcome); break;
                default: quit = true;
                }
            } else if (sel == 0) {
                if (workdir.empty()) workdir = setup::default_install_dir();
                begin_pick(Pick::Iso, Screen::Welcome);
            } else quit = true;
            break;
        case Screen::Download:
#ifdef __ANDROID__
            setup::android::cancel_download();
            if (check_proc.busy()) check_proc.kill();
            ustage = 0;
            go(Screen::Welcome);
#endif
            break;
        case Screen::Permission:
#ifdef __ANDROID__
            if (sel == 0) {
                if (setup::android::has_all_files_access()) { if (!iso.empty()) { std::error_code e2; fs::create_directories(workdir, e2); go_options(); } else { pick = Pick::Iso; open_browser(); } }
                else setup::android::request_all_files_access();
            } else if (sel == 1) {
                if (setup::android::has_all_files_access()) { if (!iso.empty()) { std::error_code e2; fs::create_directories(workdir, e2); go_options(); } else { pick = Pick::Iso; open_browser(); } }
                else { msg_title = "ALLOW ACCESS"; msg_text = "Access to files is not allowed yet. Press \"Allow access to files\" and switch it on for this app."; go(Screen::Message); }
            } else go(Screen::Welcome);
#endif
            break;
        case Screen::AndroidInstall:
#ifdef __ANDROID__
            if (sel == 1) {
                if (!setup::android::can_install_packages()) setup::android::request_install_permission();
                else setup::android::install_apk(cur_workdir() + "/android/dissidia.apk");
            } else if (sel == 2) go(Screen::Welcome);
#endif
            break;
        case Screen::Waiting:
#ifdef __ANDROID__
            if (sel == 0) setup::android::pick_file();
            else if (sel == 1) {
                if (setup::android::has_all_files_access()) open_browser(); else go(Screen::Permission);
            } else go(before_pick);
            break;
#endif
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
            if (sel == 0) go_options();
            else if (sel == 1) begin_pick(Pick::Dir, Screen::InstallDir);
            else if (sel == 2) { workdir = setup::default_install_dir(); dirty = true; sel = 0; }
            else begin_pick(Pick::Iso, Screen::Welcome);
            break;
        case Screen::Options: {
            OptRow k = option_at(sel);
            if (k == OR_START) start_install_or_ask();
            else if (k == OR_BACK) cancel();
            else if (k == OR_FOLDER) { begin_pick(Pick::Dir, Screen::Options); }
            else toggle_option(k);
            break;
        }
        case Screen::Build:
            if (sel == 3) {
                proc.kill();
#ifdef __ANDROID__
                if (dl_active) { setup::android::cancel_download(); dl_active = false; }
#endif
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
#ifdef __ANDROID__
            if (sel == 0) {
                setup::android::download(upd.url, ufiles + "/dissidia-setup.apk");
                ustage = 4;
                go(Screen::Download);
            } else go(Screen::Welcome);
            break;
#endif
            if (sel == 0) { opt = state.options; start_plan(false, true); }
            else if (launch_mode) play();
            else go(Screen::Welcome);
            break;
        case Screen::Message:
            go(Screen::Welcome);
            break;
        case Screen::Done:
#ifdef __ANDROID__
            if (sel == 0) {
                if (!setup::android::can_install_packages()) {
                    setup::android::request_install_permission();
                    msg_title = "ALLOW INSTALLING";
                    msg_text = "Switch on \"Allow from this source\" for this app, come back, then press Install the game again.";
                    go(Screen::Message);
                } else {
                    setup::android::install_apk(cur_workdir() + "/android/dissidia.apk");
                    go(Screen::AndroidInstall);
                }
            } else quit = true;
            break;
#endif
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
        case Screen::Permission:
        case Screen::AndroidInstall:
            go(Screen::Welcome);
            break;
        case Screen::Download:
            sel = 1; confirm();
            break;
        case Screen::PickUndub:
            go_options();
            break;
        case Screen::BrowseDir:
            go(Screen::InstallDir);
            break;
        case Screen::InstallDir:
            go(Screen::Welcome);
            break;
        case Screen::Options:
#ifdef __ANDROID__
            extras_mode = false;
            go(Screen::Welcome);
#else
            if (extras_mode) { extras_mode = false; go(Screen::Welcome); } else go(Screen::InstallDir);
#endif
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
        if (screen == Screen::Options) {
            OptRow k = option_at(sel);
            if (k != OR_START && k != OR_BACK && k != OR_FOLDER) toggle_option(k, dir);
        }
    }

    // ---- touch (Android): a tap on a row picks it, a tap on the title bar goes back, a vertical drag moves the selection ----
    bool touching = false;
    float touch_x0 = 0, touch_y0 = 0, touch_x = 0, touch_y = 0;   // design units (480x272)
    bool touch_moved = false;
    float drag_accum = 0;
    int64_t touch_t0 = 0;

    bool to_design(float nx, float ny, float& dx, float& dy) {
        int w = 0, h = 0;
        SDL_GetWindowSize(win, &w, &h);
        float lx = 0, ly = 0;
        SDL_RenderWindowToLogical(ren, (int)(nx * (float)w), (int)(ny * (float)h), &lx, &ly);
        dx = lx / (float)SCALE;
        dy = ly / (float)SCALE;
        return true;
    }

    void touch_event(const SDL_Event& ev) {
        float dx, dy;
        to_design(ev.tfinger.x, ev.tfinger.y, dx, dy);
        if (ev.type == SDL_FINGERDOWN) {
            touching = true; touch_moved = false; drag_accum = 0; touch_t0 = now_ms();
            touch_x0 = touch_x = dx; touch_y0 = touch_y = dy;
        } else if (ev.type == SDL_FINGERMOTION && touching) {
            drag_accum += dy - touch_y;
            touch_x = dx; touch_y = dy;
            if (std::abs(dy - touch_y0) > 10 || std::abs(dx - touch_x0) > 10) touch_moved = true;
            if (screen != Screen::TypePath && screen != Screen::Build) {
                const float step = 22;   // design units of drag per row
                int n = row_count();
                while (drag_accum > step) { sel = n ? std::max(0, sel - 1) : 0; drag_accum -= step; dirty = true; }   // finger down = list moves down = earlier rows
                while (drag_accum < -step) { sel = n ? std::min(n - 1, sel + 1) : 0; drag_accum += step; dirty = true; }
            }
        } else if (ev.type == SDL_FINGERUP && touching) {
            touching = false;
            if (touch_moved) {   // a flick keeps the list going: rows = speed (design units per ms) * a factor
                int64_t dt = std::max<int64_t>(1, now_ms() - touch_t0);
                float v = (dy - touch_y0) / (float)dt;
                int extra = std::abs(v) < 0.3f ? 0 : (int)std::max(-120.f, std::min(120.f, -v * 90.f));   // only a quick flick
                if (std::abs(extra) > 2 && screen != Screen::TypePath && screen != Screen::Build) {
                    int n = row_count();
                    sel = n ? std::max(0, std::min(n - 1, sel + extra)) : 0;
                    dirty = true;
                }
                return;
            }
            if (screen == Screen::TypePath) return;
            // hit-test against what the settings renderer drew (its geometry is in the same 480x272 design units)
            const psp_sddialog::SetGeom& G = view.geom();
            if (G.back.w > 0 && G.back.has(dx, dy) && screen != Screen::Welcome) { cancel(); return; }
            for (size_t k = 0; k < G.cats.size(); k++)
                if (G.cats[k].has(dx, dy) && screen == Screen::Options) {
                    const int n = (int)option_tabs().size();
                    int d = (int)k - opt_tab;
                    if (d != 0) { if (d > n / 2) d -= n; else if (d < -n / 2) d += n; switch_tab(d > 0 ? 1 : -1); while (opt_tab != (int)k) switch_tab(d > 0 ? 1 : -1); }
                    return;
                }
            int top = 0;
            if (is_browser()) {   // page() lists only the visible window of the browser's entries
                int total = (int)br.list.size();
                top = std::max(0, std::min(sel - MENU_ROWS / 2, total - MENU_ROWS));
            }
            for (const auto& r : G.rows) {
                if (!r.r.has(dx, dy)) continue;
                sel = top + r.idx;
                dirty = true;
                if (r.dec.w > 0 && r.dec.has(dx, dy)) adjust(-1);
                else if (r.inc.w > 0 && r.inc.has(dx, dy)) adjust(+1);
                else confirm();
                return;
            }
        }
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
        if (screen == Screen::Options) {
            if (p & BTN_LTRIGGER) switch_tab(-1);
            if (p & BTN_RTRIGGER) switch_tab(+1);
        }
        if (is_browser()) {
            if (p & BTN_LTRIGGER) { sel = std::max(0, sel - MENU_ROWS); dirty = true; }
            if (p & BTN_RTRIGGER) { sel = std::min(n - 1, sel + MENU_ROWS); dirty = true; }
        }
    }

    static std::string nice(const std::string& up) {   // "WHERE TO INSTALL" -> "Where to install"
        std::string r;
        for (size_t k = 0; k < up.size(); k++) r += k == 0 ? up[k] : (char)std::tolower((unsigned char)up[k]);
        return r;
    }

    // The page as a SetScene for the settings renderer. Rows: no value = an action; On / Off = a toggle; arrows = a choice; the progress rows are info lines.
    psp_sddialog::SetScene make_scene(const Page& pg, int vis) {
        using psp_sddialog::SetKind;
        psp_sddialog::SetScene sc;
        sc.title = "Setup";
        sc.brand = "Dissidia 012 Setup";
        sc.crumbs.push_back(nice(pg.title));
        if (pg.tabs.size() > 1) {
            for (size_t k = 0; k < pg.tabs.size(); k++) sc.cats.push_back({pg.tabs[k], k == 0 ? 10 : 7});
            sc.cat = pg.tab_active;
        } else {
            sc.cats.push_back({screen == Screen::Build ? "Install" : is_browser() ? "Files" : "Setup", 7});   // the page's own tab text (a percentage, a folder) is too long for the sidebar
        }
        const bool progress = screen == Screen::Build;
        int id = 0;
        for (const MenuRow& m : pg.rows) {
            psp_sddialog::SetRow r;
            r.id = id++ % psp_sddialog::SET_MAX_IDS;
            r.label = m.label;
            r.value = m.value;
            r.disabled = m.disabled;
            r.can_dec = m.can_dec; r.can_inc = m.can_inc;
            r.help = m.help.empty() ? std::string() : m.label + "\n" + m.help;
            if (m.value == "On" || m.value == "Off") { r.kind = SetKind::Toggle; r.on = m.value == "On"; }
            else if (m.can_dec || m.can_inc) r.kind = SetKind::Choice;
            else if (progress && r.id < 3) r.kind = SetKind::Info;
            else if (m.value.empty()) r.kind = SetKind::Action;
            else r.kind = SetKind::Info;
            sc.rows.push_back(r);
        }
        sc.sel = std::min(vis, std::max(0, (int)pg.rows.size() - 1));
        sc.in_sub = screen != Screen::Welcome;   // Back goes up one step
#ifdef __ANDROID__
        sc.device = 2;
#else
        sc.device = 1;
#endif
        sc.confirm_cross = true;
        return sc;
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
        if (!view_opened) { view.set_open(true); view_opened = true; }
        view.render(canvas, make_scene(pg, vis), (double)now_ms());
        if (view.animating()) dirty = true;   // keep drawing while something moves
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
        win = SDL_CreateWindow("Dissidia Recomp: unpacking (first start only)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 520, 70, 0);
        if (win) {
            if (SDL_Surface* ic = SDL_CreateRGBSurfaceWithFormatFrom((void*)k_app_icon_rgba, k_app_icon_w, k_app_icon_h, 32, k_app_icon_w * 4, SDL_PIXELFORMAT_RGBA32)) {
                SDL_SetWindowIcon(win, ic);
                SDL_FreeSurface(ic);
            }
            ren = SDL_CreateRenderer(win, -1, 0);
#ifdef __ANDROID__
            if (ren) SDL_RenderSetLogicalSize(ren, 520, 70);   // the window is the whole screen there
#endif
        }
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
            std::string t = "Dissidia Recomp: unpacking (first start only) " + std::to_string(pct) + "%";
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

#ifdef __ANDROID__
// The setup app: unpack the payload (python stdlib, zig's lib, the NDK sysroot, the Android core + libraries, the game tools) from
// lib/libpayload.so the first time (and after an update of the app), link the Python loader the patched interpreter asks for, find the tools.
static bool android_bootstrap(std::string& root, setup::Layout& layout, std::string& err) {
    namespace fs2 = std::filesystem;
    const std::string lib = setup::android::native_lib_dir(), files = setup::android::files_dir();
    if (lib.empty() || files.empty()) { err = "cannot find the app's folders"; return false; }
    // the interpreter of the bundled Python is /data/data/org.psprecomp.setup/files/ld-musl (patched in at build time): point it at the loader
    std::error_code ec;
    fs2::remove(fs2::path(files) / "ld-musl", ec);
    fs2::create_symlink(fs2::path(lib) / "libmusl.so", fs2::path(files) / "ld-musl", ec);
    setup::Payload pl;
    fs2::path zip = fs2::path(lib) / "libpayload.so";
    if (!fs2::exists(zip, ec)) { err = "this app has no payload (libpayload.so)"; return false; }
    pl.offset = 0;
    pl.size = (uint64_t)fs2::file_size(zip, ec);
    {   // the version is the size plus a checksum of the zip's tail (its central directory lists every file's CRC): a rebuilt payload is a new one
        uint32_t h = 2166136261u;
        if (FILE* f = std::fopen(zip.string().c_str(), "rb")) {
            long tail = (long)std::min<uint64_t>(pl.size, 1 << 16);
            std::fseek(f, -tail, SEEK_END);
            unsigned char b[4096]; size_t n;
            while ((n = std::fread(b, 1, sizeof b, f)) > 0) for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
            std::fclose(f);
        }
        pl.version = std::to_string(pl.size) + "-" + std::to_string(h);
    }
    fs2::path dest = fs2::path(files) / ("release-" + pl.version);
    err = unpack_payload_with_window(zip, pl, dest, false);
    if (!err.empty()) return false;
    for (auto& e : fs2::directory_iterator(files, ec))   // an older unpack is not needed any more
        if (e.path().filename().string().rfind("release-", 0) == 0 && e.path() != dest) fs2::remove_all(e.path(), ec);
    root = dest.string();
    layout = setup::find_layout_android(root, lib);
    return true;
}
#endif

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
#ifdef __ANDROID__
    setup::Layout android_layout;
    {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
        std::string err;
        if (!android_bootstrap(root, android_layout, err)) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dissidia Recomp Setup", ("Could not prepare the app:\n" + err).c_str(), nullptr);
            return 1;
        }
    }
#else
    SDL_SetMainReady();
#endif
#ifndef __ANDROID__
    // Single-file release: the release folder is appended to this program (payload.h); unpack it once, then run from there.
    if (root.empty() || !unpack_to.empty()) {
        setup::Payload pl;
        std::filesystem::path self = setup::self_exe();
        if (!self.empty() && setup::find_payload(self, pl)) {
            std::filesystem::path dest = unpack_to.empty() ? setup::data_dir() / ("v" + pl.version) : std::filesystem::path(unpack_to);
            std::string err = unpack_payload_with_window(self, pl, dest, headless || !unpack_to.empty());
            if (!err.empty()) {
                std::fprintf(stderr, "error: %s\n", err.c_str());
                if (unpack_to.empty()) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dissidia Recomp", ("Could not unpack the setup files:\n" + err).c_str(), nullptr);
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
#endif
#ifdef __ANDROID__
    setup::Layout layout = android_layout;
#else
    setup::Layout layout = setup::find_layout(root);
#endif

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
#ifdef __ANDROID__
    if (false) {
#else
    if (shots.empty() && !state.workdir.empty() && setup::game_installed(state.workdir)) {
#endif
        std::string where, serr;   // installs from before the menu entry existed get one at the next start
        setup::create_shortcut(state.workdir, layout.root, &where, &serr);
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
#ifdef __ANDROID__
    app.workdir = setup::android::files_dir() + "/work";
    app.layout_problem = layout.problem();
    app.iso.clear();   // a descriptor number ("fd:N") means nothing after a restart: the picker is asked again
#endif

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
        auto shot = [&](const char* name) { for (int f = 0; f < 90; f++) { app.render_page(); if (!app.view.animating()) break; SDL_Delay(16); } app.render_page(); write_ppm(app.canvas, shots + "/" + name + ".ppm"); n++; };
        app.layout_problem.clear();
        app.go(Screen::Welcome); shot("1_welcome");
        app.pick = Pick::Iso; app.go(Screen::Waiting); shot("2_waiting");
        app.go(Screen::PickIso); shot("2b_pick_iso");
        app.go(Screen::InstallDir); shot("3_install_dir");
        app.opt_tab = 0; app.go(Screen::Options, 0); shot("4_options");
        app.go(Screen::Options, 2); shot("4b_options_scrolled");
        app.opt_tab = 1; app.go(Screen::Options, 0); shot("4c_options_general");
        app.opt_tab = 0;
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

    app.win = SDL_CreateWindow("Dissidia Recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480 * SCALE, 272 * SCALE,
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
#ifndef __ANDROID__
    SDL_StartTextInput();
#endif
#ifdef __ANDROID__
    if (app.layout_problem.empty() && app.state.options.check_updates) app.begin_check(false);   // silent; an update shows its page only from the start screen
    if (false) {
#else
    if (app.installed() && app.layout_problem.empty()) {
#endif
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
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
            case SDL_FINGERUP:
                app.touch_event(ev);
                break;
            case SDL_TEXTINPUT:
                if (app.screen == Screen::TypePath)
                    for (const unsigned char* c = (const unsigned char*)ev.text.text; *c; c++)
                        if (*c >= 0x20 && *c < 0x7F) psp_sddialog::osk_push_key(*c);
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_AC_BACK) {   // Android's back button
                    if (app.building) { app.sel = 3; app.confirm(); } else app.cancel();
                } else if (ev.key.keysym.sym == SDLK_F11 || (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
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
#ifdef __ANDROID__
        app.pump_check_android();
        app.pump_download_android();
#endif
        if (app.screen == Screen::AndroidInstall && now_ms() - app.last_draw > 400) app.dirty = true;   // the installer's answer arrives from Java
        if (app.checking) app.pump_check();
        if (app.building) app.pump_build();
        app.update_input();
        int64_t t = now_ms();
        if (app.dirty || (app.building && t - app.last_draw > 250)) {
            app.dirty = false;   // before drawing: the settings view asks for the next frame while it animates
            app.present();
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
