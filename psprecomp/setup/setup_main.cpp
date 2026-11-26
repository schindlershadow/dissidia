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
//   psprecomp_setup --headless ... --extra KEY=VALUE  turn on an extra / give its disc (repeatable; e.g. d1_moves=1 d1_iso=FILE)
//   psprecomp_setup --import-saves SAVEDATA --dir DIR  copy Dissidia 012 saves into an install (an install step; no window)
//   psprecomp_setup --root DIR               use DIR as the release folder instead of this program's folder
//   psprecomp_setup --report "TEXT" [--dir DIR] [--report-step "WHAT FAILED"]   send a "Report an issue" without a window (tests; PSPRECOMP_REPORT_URL picks the service)
//   psprecomp_setup --screenshots OUTDIR     write PPM images of the pages (documentation / look checks) and exit
#define SDL_MAIN_HANDLED   // keep the plain main() on Windows (see SDL_SetMainReady below)
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../runtime/src/hle/psp_savedata_dialog.h"
#include "../runtime/src/hle/psp_savemgr.h"
#include "../runtime/include/psp_report.h"
#include "install.h"
#include "extras.h"
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

// A failure text ("<what>\n\nWhy: ...\n\nWhat to do: ...") as (label, body) rows; a plain text is one row.
std::vector<std::pair<std::string, std::string>> paragraphs(const std::string& t) {
    std::vector<std::pair<std::string, std::string>> v;
    size_t a = 0;
    while (a <= t.size()) {
        size_t b = t.find("\n\n", a);
        std::string p = t.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (!p.empty()) {
            if (p.compare(0, 5, "Why: ") == 0) v.push_back({"Why", p.substr(5)});
            else if (p.compare(0, 12, "What to do: ") == 0) v.push_back({"What to do", p.substr(12)});
            else v.push_back({v.empty() ? "What happened" : "More", p});
        }
        if (b == std::string::npos) break;
        a = b + 2;
    }
    if (v.empty()) v.push_back({"What happened", t});
    return v;
}

std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

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
    std::vector<std::string> exts = {".iso", ".cso"};   // the file endings listed (an extra's file: its ExtraDisc::patterns)
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
                if (std::find(exts.begin(), exts.end(), ext) != exts.end()) {
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
enum class Screen { Welcome, PickInfo, PickIso, PickUndub, Waiting, InstallDir, BrowseDir, TypePath, Options, Build, Done, Failed, Checking, UpdateAvail, Message, Report, ReportText, Interrupted };
// What a file/folder pick is for.
enum class Pick { Iso, Disc, Dir };   // Disc: the second disc image of an extra (pick_extra; setup/extras.cpp)

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
    Screen msg_back = Screen::Welcome;   // where the Message page's OK goes (then back to Welcome for the next message)
    Pick msg_pick = Pick::Iso;           // msg_back PickInfo: the file to choose again

    // native (system) file dialogs; the built-in browser is the fallback
    std::unique_ptr<pfd::open_file> file_dlg;
    std::unique_ptr<pfd::select_folder> dir_dlg;
    Pick pick = Pick::Iso;
    std::string pick_extra;   // Pick::Disc: the key of the extra whose disc is picked
    const setup::Extra* picked_extra() const { return setup::find_extra(pick_extra); }
    Screen before_pick = Screen::Welcome;

    // Every page is drawn by the game's Settings screen renderer. The extras' rows come from the shared list (setup/extras.cpp, also the
    // Android app's), then the desktop's own options.
    enum OptKind { OR_EXTRA, OR_UPDATES, OR_ANDROID, OR_SAVES, OR_STEAM, OR_FOLDER, OR_START, OR_BACK };
    struct OptRow {
        OptKind kind;
        std::string extra;   // OR_EXTRA: its key
        bool operator==(const OptRow& o) const { return kind == o.kind && extra == o.extra; }
    };
    std::string step_note;      // what an optional step (Steam) reported, shown at the end
    bool extras_mode = false;   // the Options page opened from "Change extras" on an installed game: only the extras, applied without a rebuild
    bool extras_run = false;    // the running plan is such an apply
    std::vector<OptRow> option_rows() const {
        std::vector<OptRow> r;
        for (const setup::Extra& e : setup::extras()) {
            if (!e.desktop) continue;
            if (!e.parent.empty() && !(opt.on(e.parent) && (e.feature.empty() || setup::feature_available(layout, e.feature)))) continue;   // a sub-choice, while its switch is on
            r.push_back({OR_EXTRA, e.key});
        }
        r.push_back({OR_ANDROID, ""});
        r.push_back({OR_SAVES, ""});
        r.push_back({OR_STEAM, ""});
        if (!extras_mode) {
            r.push_back({OR_UPDATES, ""});
            r.push_back({OR_FOLDER, ""});
        }
        r.push_back({OR_START, ""});
        r.push_back({OR_BACK, ""});
        return r;
    }
    void go_options(OptRow focus = {OR_START, ""}) {   // the Options page with `focus` selected
        std::vector<OptRow> r = option_rows();
        int i = 0;
        for (size_t k = 0; k < r.size(); k++) if (r[k] == focus) i = (int)k;
        go(Screen::Options, i);
    }

    // "Report an issue" (psp_report.cpp, the game's code): the description is typed with the on-screen keyboard (host keyboard or pad)
    std::string rep_text;                 // what the player typed
    Screen rep_back = Screen::Welcome;    // where Back goes
    bool rep_sent = false;                // a report went out (the status row shows its result)
    bool rep_working = false;
    std::string rep_value;                // the Status row's short value: "Issue #N opened" / "Not sent"
    std::string rep_failed_step;          // the step that failed (Failed page), put into the report
    std::string rep_status;               // last status line
    std::string marker_path;              // exists while an install runs: found at the next start = the last run did not end

    // build
    setup::Proc proc;
    std::vector<setup::Step> plan;
    setup::Progress prog;
    bool building = false;
    std::string fail_msg;
    int64_t last_draw = 0;

    bool installed() const { return setup::game_installed(state.workdir); }

    void go(Screen s, int select = 0) {
        screen = s; sel = select; dirty = true;
        view.rows_changed();
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

    bool native_ok() const { return pfd::settings::available(); }

    MenuRow toggle_row(const std::string& label, const std::string& help, bool on, bool available) {
        MenuRow r = row(label, available ? help : help + " (Not in this release yet.)", available ? (on ? "On" : "Off") : "Not yet");
        r.can_dec = r.can_inc = available;
        return r;
    }

    // An extra's row: On / Off, or its choices (the help text is the selected choice's, e.g. the texture pack's credits).
    MenuRow extra_row(const setup::Extra& e) {
        const bool available = e.feature.empty() || setup::feature_available(layout, e.feature);
        if (e.choices.empty()) return toggle_row(e.title, e.help, opt.on(e.key), available);
        MenuRow r = row(e.title, e.help, "");
        r.can_dec = r.can_inc = available;
        const std::string v = opt.get(e.key);
        for (auto& c : e.choices)
            if (c.value == v) { r.value = c.label; r.help = (e.help.empty() ? "" : e.help + " ") + c.help; }
        return r;
    }

    Page page() {
        Page p;
        switch (screen) {
        case Screen::Welcome: {
            p.title = "DISSIDIA 012 SETUP";
            if (!layout_problem.empty()) {
                p.tab = "Problem";
                p.rows = {row("Quit", layout_problem), row("Report an issue", "Send a report about this problem to the developer.")};
            }
            else if (installed()) {
                p.tab = "Installed";
                p.rows = {row("Play", "Start the game."),
                          row("Change extras", "Turn the HD textures, the Japanese voices or the Android version on or off, or pick another texture pack, without rebuilding the game."),
                          row("Add to Steam", "Adds the game to your Steam library as a non-Steam game. Steam closes for a moment and starts again.", setup::feature_available(layout, "steam") ? "" : "Not yet"),
                          row("Check for updates", "Look for a newer release of the game files (not available yet)."),
                          row("Rebuild the game", "Compile the game again from your extracted files. Use this if the game does not start."),
                          row("Install again", "Pick a game file and install from scratch (your saves are kept)."),
                          row("Report an issue", "Tell the developer what went wrong. You see exactly what is sent before it goes."),
                          row("Quit")};
            } else {
                p.tab = "Welcome";
                p.rows = {row("Install the game",
                              "Builds the game on this computer from your own copy of Dissidia 012 (EU or US, ISO or CSO). "
                              "Nothing from the game is downloaded or shipped with this program. It takes a while."),
                          row("Report an issue", "Tell the developer what went wrong. You see exactly what is sent before it goes."),
                          row("Quit")};
            }
            break;
        }
        case Screen::PickInfo: {
            const std::string how = "A file dialog opens next: browse to the file and open it. The program only reads it; nothing is uploaded.";
            if (const setup::Extra* e = pick == Pick::Disc ? picked_extra() : nullptr) {   // an extra's second disc (setup/extras.cpp)
                p.title = upper(e->disc.page_title);
                p.tab = e->disc.eyebrow;
                p.rows = {row(e->disc.what, e->disc.what_help, e->disc.exts), row(e->disc.valid, e->disc.valid_help, e->disc.valid_short),
                          row("Choose the file", how), row("Back")};
            } else {
                p.title = "YOUR GAME FILE";
                p.tab = "Step 1";
                p.rows = {row("A Dissidia 012 disc image", "The game is built from your own copy of the PSP game Dissidia 012 [duodecim] Final Fantasy, as a disc image file: a .iso or a .cso (compressed ISO) of the whole disc. Nothing from the game is downloaded or shipped with this app.", ".iso or .cso"),
                          row("Valid: EU or US release", "The European release (ULES-01505, also the PSN version) and the US release (ULUS-10566) work. It must be Dissidia 012, not an archive: unpack .zip / .7z first.", "EU / US"),
                          row("Choose the file", how),
                          row("Back")};
            }
            break;
        }
        case Screen::Waiting:
            p.title = pick == Pick::Dir ? "CHOOSE A FOLDER" : pick == Pick::Disc && picked_extra() ? "CHOOSE: " + upper(picked_extra()->disc.page_title) : "CHOOSE YOUR GAME FILE";
            p.tab = "Waiting";
            p.rows = {row("Use the simple browser instead", "A file window opened. Pick the file there, or press here to browse inside this program."),
                      row("Back")};
            break;
        case Screen::PickIso:
        case Screen::PickUndub:
        case Screen::BrowseDir: {
            bool dirs = screen == Screen::BrowseDir;
            p.title = dirs ? "CHOOSE A FOLDER" : screen == Screen::PickUndub && picked_extra() ? "CHOOSE: " + upper(picked_extra()->disc.page_title) : "CHOOSE YOUR GAME FILE";
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
            bool an = setup::feature_available(layout, "android");
            if (!save_sources_scanned) {
                save_sources = psp_savemgr::detect_sources(true);
                save_sources_scanned = true;
            }
            for (OptRow k : option_rows()) {
                switch (k.kind) {
                case OR_EXTRA:
                    if (const setup::Extra* e = setup::find_extra(k.extra)) p.rows.push_back(extra_row(*e));
                    break;
                case OR_UPDATES:
                    p.rows.push_back(toggle_row("Check for updates", "Look for new releases when the app starts (changeable later in the game's settings.ini).", opt.check_updates, true));
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
                case OR_STEAM:
                    p.rows.push_back(toggle_row("Add to Steam", "Adds the game to your Steam library as a non-Steam game, so it starts from Steam (and Big Picture / Steam Deck). Steam closes for a moment and starts again.", opt.steam, setup::feature_available(layout, "steam")));
                    break;
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
            p.tab = pc;   // shown in the header after the title
            int64_t t = now_ms();
            std::string stage = prog.stage.empty() ? "Starting..." : prog.stage;
            std::string steps = plan.size() > 1 ? " (" + std::to_string(prog.step + 1) + "/" + std::to_string(plan.size()) + ")" : "";
            p.rows = {row(stage + steps, "", pc),
                      row(prog.detail.empty() ? "Working..." : prog.detail, "", setup::elapsed_text(prog, t)),
                      row(setup::eta_text(prog, t), ""),
                      row("Cancel")};
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
            for (const auto& pa : paragraphs(msg_text)) if (paragraphs(msg_text).size() > 1) p.rows.push_back(row(pa.first, pa.second));
            p.rows.push_back(row("OK", paragraphs(msg_text).size() > 1 ? std::string("Close this.") : msg_text));
            break;
        case Screen::Done:
            p.title = "ALL DONE";
            p.tab = "Finished";
            p.rows = {row("Play now", "Start the game.")};
            if (setup::android_build_exists(cur_workdir()))
                p.rows.push_back(row("Install on an Android device", "The Android version is built: " + cur_workdir() + "/android/dissidia.apk. This installs it on a device connected with USB or wireless debugging (adb in your PATH) and copies the game files to it. Or install the APK by hand and copy the folders (see docs/android.md)."));
            p.rows.push_back(row("Quit", (shortcut_note.empty() ? std::string() : shortcut_note + ". ") + "The game can also be started with this program any time."));
            break;
        case Screen::Interrupted:
            p.title = "LAST RUN STOPPED";
            p.tab = "Report";
            p.rows = {row("Report an issue", "The last install did not finish: the program was closed or crashed while it ran. Send the install log to the developer so it can be fixed."),
                      row("Continue")};
            break;
        case Screen::Report: {
            p.title = "REPORT AN ISSUE";
            p.tab = "Send";
            const std::string d = rep_text.empty() ? std::string("(not written yet)") : shorten_left(rep_text, 34);
            p.rows = {row("What went wrong", "Describe the problem in a few words (what you did, what you expected, what happened). Select to type it with the keyboard.", d),
                      row("Files that will be sent", report_files_help()),
                      row("Send the report", "Sends the report to the developer's issue tracker on GitHub, then opens the new issue in your browser. Nothing is sent before you choose this."),
                      row("Status", rep_status.empty() ? std::string("Nothing is sent until you choose Send the report.") : rep_status, rep_working ? "Sending..." : rep_sent ? rep_value : ""),
                      row("Back")};
            if (!rep_working && !rep_sent) p.rows[3].value = "";
            break;
        }
        case Screen::ReportText:
            break;
        case Screen::Failed:
            p.title = "NOT FINISHED";
            p.tab = "Stopped";
            for (const auto& pa : paragraphs(fail_msg)) p.rows.push_back(row(pa.first, pa.second, pa.first == "What happened" ? fail_title : ""));
            p.rows.push_back(row("Try again", "Runs the steps again; what is already done is kept."));
            p.rows.push_back(
                      row("Report an issue", "Send the log and what failed to the developer. You see what is sent first."));
            if (can_open_log()) {
                p.rows.push_back(row("Open setup.log", "The log of this install: every step's output and why it stopped. " + log_path));
                p.rows.push_back(row("Show setup.log in its folder", "Opens the install folder with setup.log selected, e.g. to send the file. " + log_path));
            }
            p.rows.push_back(row("Back to the start"));
            p.rows.push_back(row("Quit"));
            break;
        }
        return p;
    }

    // ---- Report an issue ----
    static std::string release_version(const std::string& root) {
        std::ifstream f(fs::path(root) / "release.json");
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t at = s.find("\"version\"");
        if (at == std::string::npos) return "dev";
        at = s.find('"', s.find(':', at) + 1);
        const size_t e = at == std::string::npos ? at : s.find('"', at + 1);
        return e == std::string::npos ? "dev" : s.substr(at + 1, e - at - 1);
    }
    // Names the files of this install for the report (the same list the preview shows).
    void report_configure() {
        psp_report::Product pr;
        pr.name = "installer";
        pr.version = release_version(layout.root);
        const std::string wd = cur_workdir();
        const std::string logp = !log_path.empty() ? log_path : (fs::path(wd) / "setup.log").string();
        pr.files.push_back({"setup.log", logp, 8u << 20});
        if (!wd.empty()) {
            for (const char* n : {"last_run.log", "prev_run.log", "run.log"}) pr.files.push_back({n, (fs::path(wd) / n).string(), 8u << 20});
            std::error_code ec;
            for (auto it = fs::directory_iterator(wd, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
                const std::string n = it->path().filename().string(), l = lower(n);
                if (l.rfind("crash", 0) == 0 || l.rfind("hs_err", 0) == 0) pr.files.push_back({n, it->path().string(), 1u << 20});
            }
        }
        pr.save_dir = (fs::path(marker_path).parent_path() / "reports").string();
        pr.space_dir = wd.empty() ? setup::home_dir() : wd;
        std::error_code ec;
        if (!fs::exists(pr.space_dir, ec)) pr.space_dir = setup::home_dir();
        pr.facts.push_back({"installer_screen", std::to_string((int)screen)});
#ifdef _WIN32
        pr.facts.push_back({"installer_platform", "windows"});
#else
        pr.facts.push_back({"installer_platform", "linux"});
#endif
        pr.facts.push_back({"extras", opt.get("hd_textures").empty() ? "" : "hd_textures=" + opt.get("hd_textures")});
        psp_report::set_product(pr);
    }
    std::string report_files_help() {
        report_configure();
        std::string s = "Sent: your description, the installer's version, OS, CPU, memory and free space, which step failed, and ";
        for (const auto& f : psp_report::preview(false)) {
            if (f.name == "report.json") continue;
            s += f.name + ", ";
        }
        s += "(your user name and home folder are replaced by ~). Never sent: your disc image, cloud.ini, saves.";
        return s;
    }
    void begin_report(Screen back) {
        rep_back = back;
        rep_sent = rep_working = false;
        rep_status.clear();
        go(Screen::Report, 0);
    }
    void send_report() {
        if (rep_working) return;
        if (rep_text.empty() && rep_failed_step.empty()) { rep_status = "Write a few words about what went wrong first (What went wrong)."; dirty = true; return; }
        report_configure();
        psp_report::Params p;
        p.description = rep_text;
        if (!rep_failed_step.empty()) p.description += (rep_text.empty() ? "" : "\n\n") + std::string("Failed: ") + rep_failed_step;
        p.title = "Installer: " + (rep_text.empty() ? rep_failed_step : rep_text.substr(0, 70));
        p.include_dump = false;
        if (!rep_failed_step.empty()) p.extra.push_back({"failure", rep_failed_step});
        if (!prog.stage.empty()) p.extra.push_back({"last_step", prog.stage});
        if (!prog.last_line.empty()) p.extra.push_back({"last_line", prog.last_line});
        if (!psp_report::start(p)) { rep_status = "A report is already being sent."; dirty = true; return; }
        rep_working = true;
        rep_sent = false;
        rep_status = "Collecting the report...";
        dirty = true;
    }
    void pump_report() {
        if (!rep_working) return;
        const psp_report::Status st = psp_report::status();
        if (st.text != rep_status && st.state == psp_report::Status::Working) { rep_status = st.text; dirty = true; }
        if (!psp_report::take_finished()) return;
        rep_working = false;
        rep_sent = true;
        rep_status = st.text;
        rep_value = st.state == psp_report::Status::Done ? st.text : "Not sent";
        sel = 3;   // the Status row, whose help says what happened (and where the zip was saved when it did not go)
        if (st.state == psp_report::Status::Done) {
            std::fprintf(stderr, "[REPORT] %s %s\n", st.text.c_str(), st.issue_url.c_str());
            const char* no = std::getenv("PSPRECOMP_REPORT_NO_OPEN");
            if (!(no && no[0] == '1')) SDL_OpenURL(st.issue_url.c_str());
            rep_status = st.text + ". Your browser opens it: " + st.issue_url;
        }
        log_line("report: " + rep_status);
        dirty = true;
    }
    void marker(bool on) {
        if (marker_path.empty()) return;
        if (on) { if (FILE* f = std::fopen(marker_path.c_str(), "wb")) { std::fputs("install running\n", f); std::fclose(f); } }
        else { std::error_code ec; fs::remove(marker_path, ec); }
    }

    bool can_open_log() const {   // a desktop with a setup.log to show (Android: the app's storage, not reachable from here)
        std::error_code ec;
        return !log_path.empty() && std::filesystem::is_regular_file(log_path, ec);
    }
    std::string cur_workdir() const { return !state.workdir.empty() ? state.workdir : workdir; }
    bool is_browser() const { return screen == Screen::PickIso || screen == Screen::PickUndub || screen == Screen::BrowseDir; }
    int row_count() { return is_browser() ? (int)br.list.size() : (int)page().rows.size(); }

    // ---- actions ----
    void start_plan(bool rebuild, bool update = false, bool android_install = false, bool extras = false) {
        extras_run = extras;
        step_note.clear();
        rebuild_only = rebuild || update || android_install || extras;
        updating = update;
        android_inst = android_install;
        const setup::Options& o = (rebuild || update) ? state.options : opt;
        if (extras) {}   // plan already built by start_extras()
        else
        if (android_install) plan = setup::plan_android_install(layout, cur_workdir());
        else if (update) plan = setup::plan_update(layout, state.workdir, o, upd);
        else plan = rebuild ? setup::plan_rebuild(layout, state.workdir, o) : setup::plan_install(layout, iso, workdir, o);
        log_open((rebuild || update || extras) ? state.workdir : workdir);
        rep_failed_step.clear();
        marker(true);
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
    std::string log_path;   // setup.log of this run (the failure page opens it)
    void log_open(const std::string& dir) {
        if (logf) { std::fclose(logf); logf = nullptr; }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        log_path = (std::filesystem::path(dir) / "setup.log").string();
        logf = std::fopen(log_path.c_str(), "ab");
        if (logf) { std::fprintf(logf, "\n===== psprecomp_setup run =====\n"); std::fflush(logf); }
    }
    std::deque<std::string> tail_lines;   // the last output lines (the failure reason looks at them)
    void log_line(const std::string& l) {
        tail_lines.push_back(l);
        if (tail_lines.size() > 40) tail_lines.pop_front();
        if (logf) { std::fprintf(logf, "%s\n", l.c_str()); std::fflush(logf); }
    }

    void fail(const std::string& msg, int code = 0) {
        log_line("FAILED: " + msg);
        building = false;
        marker(false);
        setup::FailureCtx fc;
        fc.step = prog.step >= 0 && prog.step < (int)plan.size() ? plan[(size_t)prog.step].label : "";
        fc.stage = prog.stage;
        fc.error = prog.error.empty() ? msg : prog.error;
        for (const std::string& t : tail_lines) fc.tail += t + "\n";
        fc.workdir = cur_workdir();
        fc.exit_code = code;
        const setup::Failure f = setup::explain_failure(fc);
        log_line("Reason (" + f.kind + "): " + f.why);
        fail_msg = f.text();
        fail_kind = f.kind;
        fail_title = f.title();
        rep_failed_step = (prog.stage.empty() ? std::string() : prog.stage + ": ") + msg;
        go(Screen::Failed, paragraphs(fail_msg).size() > 1 ? 1 : 0);   // on the "Why" row
    }
    std::string fail_kind, fail_title;

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
        if (step_over) code = proc.exit_code();
        if (step_over) {
            const bool opt_step = prog.step < (int)plan.size() && plan[(size_t)prog.step].optional;
            if (code != 0 || !prog.error.empty()) {
                if (opt_step) {   // e.g. no Steam installed: told at the end, the install itself is fine
                    std::string why = prog.error.empty() ? prog.last_line : prog.error;
                    step_note = "Not added to Steam" + (why.empty() ? std::string(".") : ": " + why);
                    log_line("optional step failed: " + step_note);
                    prog.error.clear();
                } else {
                    fail(prog.error.empty() ? "A step failed (exit code " + std::to_string(code) + "); see setup.log in the install folder." : prog.error + ".", code);
                    return;
                }
            } else if (opt_step) step_note = "The game was added to your Steam library.";
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
                marker(false);
                if (!rebuild_only) {
                    state.workdir = workdir;
                    state.iso = iso;
                    state.version = "dev";
                    state.options = opt;
                    state.options.clear_actions();   // an added texture pack is done: the switch is off again next time
                    write_settings();
                }
                if (extras_run) { state.options = opt; state.options.clear_discs(); state.options.steam = false; opt.steam = false; }
                setup::save_state(state_file, state);
                if (!rebuild_only) {
                    std::string where, serr;
                    if (setup::create_shortcut(workdir, layout.root, &where, &serr)) shortcut_note = "A menu entry was added: " + where;
                    else shortcut_note = "No menu entry (" + serr + ")";
                    if (!step_note.empty()) shortcut_note += ". " + step_note;
                    if (!auto_disc_note.empty()) shortcut_note += ". " + auto_disc_note;
                }
                if (extras_run) {
                    extras_mode = false;
                    msg_title = "DONE";
                    msg_text = (step_note.empty() ? std::string("The extras are in place.") : step_note);
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
        std::string cur = k == Pick::Disc ? (picked_extra() ? opt.get(picked_extra()->disc.key) : "") : iso;
        if (!cur.empty() && cur.compare(0, 3, "fd:") != 0) return fs::path(cur).parent_path();
        fs::path dl = fs::path(setup::home_dir()) / "Downloads";
        return fs::is_directory(dl, ec) ? dl : fs::path(setup::home_dir());
    }

    void begin_pick(Pick k, Screen back, bool explained = false) {
        pick = k;
        before_pick = back;
        if ((k == Pick::Iso || k == Pick::Disc) && !explained) { go(Screen::PickInfo, 0); return; }   // first say what to pick and what is valid
        if (native_ok()) {
            std::string start = pick_start(k).string();
            if (k == Pick::Dir) dir_dlg = std::make_unique<pfd::select_folder>("Choose the install folder", start);
            else {
                const setup::Extra* x = k == Pick::Disc ? picked_extra() : nullptr;
                std::string pat;   // e.g. "*.zip *.ZIP *.7z *.7Z": the extra's file types (a disc image by default)
                for (const std::string& ext : x ? x->disc.patterns : std::vector<std::string>{".iso", ".cso"}) {
                    std::string up = ext;
                    for (char& ch : up) ch = (char)std::toupper((unsigned char)ch);
                    pat += (pat.empty() ? "*" : " *") + ext + " *" + up;
                }
                file_dlg = std::make_unique<pfd::open_file>(x ? "Choose: " + x->disc.page_title : std::string("Choose your Dissidia 012 disc image"), start,
                                                            std::vector<std::string>{x ? x->disc.filter_name : std::string("PSP disc images"), pat, "All files", "*"});
            }
            go(Screen::Waiting);
        } else {
            open_browser();
        }
    }

    void open_browser() {
        file_dlg.reset();
        dir_dlg.reset();
        br.exts = pick == Pick::Disc && picked_extra() ? picked_extra()->disc.patterns : std::vector<std::string>{".iso", ".cso"};
        br.open(pick_start(pick), pick == Pick::Dir);
        go(pick == Pick::Dir ? Screen::BrowseDir : pick == Pick::Disc ? Screen::PickUndub : Screen::PickIso);
    }

    void picked(const std::string& path) {   // a chosen file (or folder when pick == Dir)
        std::error_code ec;
        if (pick == Pick::Dir) {
            workdir = path;
            if (before_pick == Screen::Options) go(Screen::Options, 0); else go(Screen::InstallDir);
        } else if (pick == Pick::Disc) {
            const setup::Extra* e = picked_extra();
            const std::string why = e && e->disc.check ? e->disc.check(path) : "";   // the right game, before anything is installed with it
            if (!why.empty()) {
                if (e) opt.set(e->disc.key, "");
                msg_title = e && e->disc.patterns.front() == ".iso" ? "WRONG DISC" : "WRONG FILE";
                msg_text = why;
                msg_back = Screen::PickInfo;   // OK: choose that file again (msg_pick, pick_extra)
                msg_pick = pick;
                go(Screen::Message);
                return;
            }
            if (e) opt.set(e->disc.key, path);
            go_options();
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

    void begin_steam() {   // only the Steam shortcut, for the installed game
        if (!setup::feature_available(layout, "steam")) { msg_title = "STEAM"; msg_text = "This release cannot add the game to Steam yet."; go(Screen::Message); return; }
        workdir = state.workdir;
        extras_mode = true;   // (the end page then returns to the start page)
        plan = setup::plan_steam(layout, workdir);
        start_plan(false, false, false, true);
    }

    void begin_extras() {
        extras_mode = true;
        workdir = state.workdir;
        opt = state.options;
        opt.clear_discs();   // a saved path (or Android descriptor) from the install is no longer valid
        go_options(setup::extras().empty() ? OptRow{OR_START, ""} : OptRow{OR_EXTRA, setup::extras()[0].key});
    }

    // Applies the extras that differ from what is installed: no rebuild.
    void start_extras() {
        const setup::Options& prev = state.options;
        setup::Options o = opt;
        if (prev.android && setup::android_build_exists(workdir)) o.android = false;   // its APK is already built
        plan = setup::plan_extras(layout, workdir, prev, o);   // removes what was turned off, adds what was turned on (setup/extras.cpp)
        if (plan.empty()) {
            state.options = opt; state.options.clear_discs();
            setup::save_state(state_file, state);
            extras_mode = false;
            msg_title = "EXTRAS";
            msg_text = "Nothing needed to be added: the extras you chose are already in place.";
            go(Screen::Message);
            return;
        }
        start_plan(false, false, false, true);
    }

    // The extras that need a second disc: use one found next to the first image the player picked (id / voices checked), else ask.
    std::vector<std::string> discs_still_needed() {
        const std::string ref = !iso.empty() ? iso : state.iso;
        auto need = setup::extras_needing_disc(layout, false, opt, extras_mode ? &state.options : nullptr);
        while (!need.empty()) {
            const setup::Extra* e = setup::find_extra(need[0]);
            const std::string f = e ? setup::find_extra_disc(e->key, ref) : "";
            if (f.empty()) break;
            opt.set(e->disc.key, f);
            std::fprintf(stderr, "Using %s from the same folder as your game file for: %s\n", f.c_str(), e->title.c_str());
            auto_disc_note += (auto_disc_note.empty() ? "" : "\n") + std::string("Used ") + fs::path(f).filename().string() + " (same folder) for " + e->title + ".";
            need = setup::extras_needing_disc(layout, false, opt, extras_mode ? &state.options : nullptr);
        }
        return need;
    }
    std::string auto_disc_note;   // which extra discs were found next to the game file (shown on the finished page)

    // Free space before anything starts: the whole install (or the extras) must fit; else say how much is missing and why.
    bool space_ok() {
        const std::string dir = extras_mode ? cur_workdir() : workdir;
        const std::string why = setup::space_problem(dir, setup::needed_bytes(false, dir, opt, extras_mode), false);
        if (why.empty()) return true;
        msg_title = "NOT ENOUGH SPACE";
        msg_text = why;
        msg_back = Screen::Options;
        go(Screen::Message, 1);   // on the "Why" row
        return false;
    }

    void start_install_or_ask() {
        auto_disc_note.clear();
        auto need = discs_still_needed();   // extras built from a second disc image (undub, Dissidia 1) need it first
        if (!need.empty()) { pick_extra = need[0]; begin_pick(Pick::Disc, Screen::Options); return; }
        if (!space_ok()) return;
        if (extras_mode) { start_extras(); return; }
        std::error_code ec;
        fs::create_directories(workdir, ec);
        if (ec) { fail("Cannot create " + workdir + ": " + ec.message()); return; }
        start_plan(false);
    }

    void toggle_option(OptRow k, int dir = 1) {
        switch (k.kind) {
        case OR_EXTRA:
            if (const setup::Extra* e = setup::find_extra(k.extra)) {
                if (!e->feature.empty() && !setup::feature_available(layout, e->feature)) break;
                if (e->choices.empty()) {
                    opt.set(e->key, opt.on(e->key) ? "0" : "1");
                    if (!opt.on(e->key) && !e->disc.key.empty()) opt.set(e->disc.key, "");   // its disc is picked again when turned on
                } else {   // the next / previous choice
                    size_t i = 0, n = e->choices.size();
                    while (i < n && e->choices[i].value != opt.get(e->key)) i++;
                    i = i >= n ? 0 : (i + n + (dir < 0 ? n - 1 : 1)) % n;
                    opt.set(e->key, e->choices[i].value);
                }
            }
            break;
        case OR_UPDATES: opt.check_updates = !opt.check_updates; break;
        case OR_ANDROID: if (setup::feature_available(layout, "android")) opt.android = !opt.android; break;
        case OR_STEAM: if (setup::feature_available(layout, "steam")) opt.steam = !opt.steam; break;
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
        return i >= 0 && i < (int)r.size() ? r[(size_t)i] : OptRow{OR_BACK, ""};
    }

    void confirm() {
        switch (screen) {
        case Screen::Welcome:
            if (!layout_problem.empty()) { if (sel == 1) begin_report(Screen::Welcome); else quit = true; break; }
            if (installed()) {
                switch (sel) {
                case 0: play(); break;
                case 1: begin_extras(); break;
                case 2: begin_steam(); break;
                case 3: begin_check(true); break;
                case 4: opt = state.options; start_plan(true); break;
                case 5: begin_pick(Pick::Iso, Screen::Welcome); break;
                case 6: begin_report(Screen::Welcome); break;
                default: quit = true;
                }
            } else if (sel == 0) {
                if (workdir.empty()) workdir = setup::default_install_dir();
                begin_pick(Pick::Iso, Screen::Welcome);
            } else if (sel == 1) begin_report(Screen::Welcome);
            else quit = true;
            break;
        case Screen::PickInfo:
            if (sel == 2) begin_pick(pick, before_pick, true);
            else if (sel == 3) go(before_pick);
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
            if (sel == 0) go_options();
            else if (sel == 1) begin_pick(Pick::Dir, Screen::InstallDir);
            else if (sel == 2) { workdir = setup::default_install_dir(); dirty = true; sel = 0; }
            else begin_pick(Pick::Iso, Screen::Welcome);
            break;
        case Screen::Options: {
            OptRow k = option_at(sel);
            if (k.kind == OR_START) start_install_or_ask();
            else if (k.kind == OR_BACK) cancel();
            else if (k.kind == OR_FOLDER) { begin_pick(Pick::Dir, Screen::Options); }
            else toggle_option(k);
            break;
        }
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
        case Screen::Message: {
            const Screen back = msg_back;
            msg_back = Screen::Welcome;
            if (back == Screen::PickInfo) begin_pick(msg_pick, Screen::Options); else go(back);
            break;
        }
        case Screen::Done:
            if (sel == 0) play();
            else if (sel == 1 && setup::android_build_exists(cur_workdir())) start_plan(false, false, true);
            else quit = true;
            break;
        case Screen::Failed: {
            const int np = (int)paragraphs(fail_msg).size();   // the reason rows come first
            if (sel < np) break;
            sel -= np;
            const int extra = can_open_log() ? 2 : 0;   // rows 2 and 3: open setup.log / show it in its folder (row 1: Report an issue)
            if (sel == 0) { if (!plan.empty()) start_plan(rebuild_only && !updating && !android_inst, updating, android_inst); else go(Screen::Welcome); }
            else if (sel == 1) begin_report(Screen::Failed);
            else if (sel <= extra + 1) {
                if (logf) std::fflush(logf);
                std::string err;
                if (!setup::open_on_desktop(log_path, sel == 3, &err)) {
                    msg_title = "SETUP LOG";
                    msg_text = "Could not open it (" + err + "). The file is " + log_path;
                    msg_back = Screen::Failed;
                    go(Screen::Message);
                }
            }
            else if (sel == extra + 2) go(Screen::Welcome);
            else quit = true;
            break;
        }
        case Screen::Interrupted:
            if (sel == 0) begin_report(Screen::Welcome); else go(Screen::Welcome);
            break;
        case Screen::Report:
            if (sel == 0) {
                osk.begin(u"What went wrong", std::u16string(rep_text.begin(), rep_text.end()), 300, true);
                psp_sddialog::osk_set_active(true);
                go(Screen::ReportText);
            } else if (sel == 2) send_report();
            else if (sel == 4) { if (!rep_working) go(rep_back); }
            break;
        case Screen::ReportText:
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
            go_options();
            break;
        case Screen::PickInfo:
            go(before_pick);
            break;
        case Screen::BrowseDir:
            go(Screen::InstallDir);
            break;
        case Screen::InstallDir:
            go(Screen::Welcome);
            break;
        case Screen::Options:
            if (extras_mode) { extras_mode = false; go(Screen::Welcome); } else go(Screen::InstallDir);
            break;
        case Screen::Welcome:
        case Screen::Done:
            quit = true;
            break;
        case Screen::Failed:
        case Screen::Message: {
            const Screen back = msg_back;
            msg_back = Screen::Welcome;
            if (back == Screen::PickInfo) begin_pick(msg_pick, Screen::Options); else go(back);
            break;
        }
        case Screen::UpdateAvail:
            if (launch_mode) play(); else go(Screen::Welcome);
            break;
        case Screen::Report:
            if (!rep_working) go(rep_back);
            break;
        case Screen::Interrupted:
            go(Screen::Welcome);
            break;
        default:
            break;
        }
    }

    void adjust(int dir) {
        if (screen == Screen::Options) {
            OptRow k = option_at(sel);
            if (k.kind != OR_START && k.kind != OR_BACK && k.kind != OR_FOLDER) toggle_option(k, dir);
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
            if (screen != Screen::TypePath && screen != Screen::ReportText && screen != Screen::Build) {
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
                if (std::abs(extra) > 2 && screen != Screen::TypePath && screen != Screen::ReportText && screen != Screen::Build) {
                    int n = row_count();
                    sel = n ? std::max(0, std::min(n - 1, sel + extra)) : 0;
                    dirty = true;
                }
                return;
            }
            if (screen == Screen::TypePath || screen == Screen::ReportText) return;
            tap(dx, dy);
        }
    }

    // ---- mouse (desktop): hover picks the row under the pointer (and shows a hand), ONE left click acts at once (on press), right click goes back ----
    // Hit-test against what the settings renderer drew (its geometry is in the same 480x272 design units).
    // Returns 0 = nothing, 1 = the Back / Close button, 2 = a row (row_idx = its index in the page / browser list).
    int hit(float dx, float dy, const psp_sddialog::SetGeom::Row** row, int* row_idx) {
        const psp_sddialog::SetGeom& G = view.geom();
        if (G.back.w > 0 && G.back.has(dx, dy)) return 1;
        int top = 0;
        if (is_browser()) {   // page() lists only the visible window of the browser's entries
            int total = (int)br.list.size();
            top = std::max(0, std::min(sel - MENU_ROWS / 2, total - MENU_ROWS));
        }
        for (const auto& r : G.rows) {
            if (!r.r.has(dx, dy)) continue;
            *row = &r; *row_idx = top + r.idx;
            return 2;
        }
        return 0;
    }

    void tap(float dx, float dy) {
        const psp_sddialog::SetGeom::Row* r = nullptr;
        int idx = 0;
        int h = hit(dx, dy, &r, &idx);
        if (h == 1) { cancel(); return; }
        if (h != 2) return;
        sel = idx;
        dirty = true;
        if (r->dec.w > 0 && r->dec.has(dx, dy)) adjust(-1);
        else if (r->inc.w > 0 && r->inc.has(dx, dy)) adjust(+1);
        else confirm();
    }

    SDL_Cursor* cur_hand = nullptr;
    SDL_Cursor* cur_arrow = nullptr;
    bool cur_is_hand = false;
    void set_hand(bool on) {
        if (on == cur_is_hand) return;
        cur_is_hand = on;
        if (!cur_hand) { cur_hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND); cur_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW); }
        if (SDL_Cursor* c = on ? cur_hand : cur_arrow) SDL_SetCursor(c);
    }

    bool mouse_to_design(int mx, int my, float& dx, float& dy) {
        // SDL already maps mouse events into the renderer's logical (960x544) space (SDL_RenderSetLogicalSize): any window size / HiDPI
        // scale / letterbox arrives as canvas pixels. (Mapping again with SDL_RenderWindowToLogical would be off at every size but 960x544.)
        dx = (float)mx / (float)SCALE;
        dy = (float)my / (float)SCALE;
        return true;
    }

    void mouse_event(const SDL_Event& ev) {
        if (ev.type == SDL_MOUSEMOTION ? ev.motion.which == SDL_TOUCH_MOUSEID : ev.type == SDL_MOUSEWHEEL ? ev.wheel.which == SDL_TOUCH_MOUSEID : ev.button.which == SDL_TOUCH_MOUSEID) return;   // touch has its own path
        if (ev.type == SDL_MOUSEWHEEL) {
            if (screen == Screen::TypePath || screen == Screen::ReportText) return;
            int n = row_count();
            int step = ev.wheel.y > 0 ? -1 : ev.wheel.y < 0 ? 1 : 0;
            if (n && step) { sel = std::max(0, std::min(n - 1, sel + step)); dirty = true; }
            return;
        }
        if (screen == Screen::TypePath || screen == Screen::ReportText) {   // the on-screen keyboard: click its keys (or type on the host keyboard)
            if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_RIGHT) {
                psp_sddialog::osk_set_active(false);
                if (screen == Screen::ReportText) go(Screen::Report, 0); else go(Screen::BrowseDir);
                return;
            }
            if (ev.type == SDL_MOUSEWHEEL) return;
            float kx, ky;
            const bool mv = ev.type == SDL_MOUSEMOTION;
            mouse_to_design(mv ? ev.motion.x : ev.button.x, mv ? ev.motion.y : ev.button.y, kx, ky);
            const bool click = ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT;
            if (!mv && !click) return;
            const bool over = osk.pointer(kx, ky, click);
            if (mv) set_hand(over);
            dirty = true;
            return;
        }
        float dx, dy;
        const int mx = ev.type == SDL_MOUSEMOTION ? ev.motion.x : ev.button.x, my = ev.type == SDL_MOUSEMOTION ? ev.motion.y : ev.button.y;
        mouse_to_design(mx, my, dx, dy);
        const psp_sddialog::SetGeom::Row* r = nullptr;
        int idx = 0;
        int h = hit(dx, dy, &r, &idx);
        if (ev.type == SDL_MOUSEMOTION) {
            bool hand = h == 1;
            if (h == 2) {
                Page pg = page();
                int pi = is_browser() ? r->idx : idx;   // page() rows are the visible window for the browser
                bool dead = pi >= 0 && pi < (int)pg.rows.size() && pg.rows[pi].disabled;
                hand = !dead && !(screen == Screen::Build && idx < 3);   // the progress / info lines are not buttons
                if (idx != sel && !dead) { sel = idx; dirty = true; }
            }
            set_hand(hand);
        } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
            if (ev.button.button == SDL_BUTTON_LEFT) tap(dx, dy);
            else if (ev.button.button == SDL_BUTTON_RIGHT && screen != Screen::Welcome) cancel();
        }
    }

    void update_input() {
        in.update(pad);
        if (screen == Screen::ReportText) {
            auto r = osk.update(in.pad_held, now_ms());
            dirty = true;
            if (r == psp_sddialog::Result::Selected) {
                psp_sddialog::osk_set_active(false);
                rep_text = utf8_of(osk.text());
                go(Screen::Report, 2);   // on to Send
            }
            return;
        }
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

    static std::string utf8_of(const std::u16string& t) {
        std::string u8;
        for (char16_t c : t) {
            if (c < 0x80) u8 += (char)c;
            else if (c < 0x800) { u8 += (char)(0xC0 | (c >> 6)); u8 += (char)(0x80 | (c & 0x3F)); }
            else { u8 += (char)(0xE0 | (c >> 12)); u8 += (char)(0x80 | ((c >> 6) & 0x3F)); u8 += (char)(0x80 | (c & 0x3F)); }
        }
        return u8;
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
        // no sidebar: the page's status (a percentage, the folder being browsed, "Built") goes into the header after the title
        sc.crumbs.push_back(nice(pg.title));
        if (!pg.tab.empty() && screen != Screen::Options) sc.crumbs.push_back(is_browser() ? shorten_left(pg.tab, 40) : pg.tab);
        sc.hide_side = true;
        sc.cats.push_back({"Setup", 7});
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
            if (progress) r.help = (prog.stage.empty() ? std::string("Working") : prog.stage) + "\n" + (prog.explain.empty() ? std::string("Cancelling stops the install; starting it again continues where it stopped.") : prog.explain);   // the same text whichever row is selected
            if (m.value == "On" || m.value == "Off") { r.kind = SetKind::Toggle; r.on = m.value == "On"; }
            else if (m.can_dec || m.can_inc) r.kind = SetKind::Choice;
            else if (progress && r.id == 0) { r.kind = SetKind::Progress; r.progress = (float)prog.fraction; }
            else if (progress && r.id < 3) r.kind = SetKind::Info;
            else if (m.value.empty()) r.kind = SetKind::Action;
            else r.kind = SetKind::Info;
            sc.rows.push_back(r);
        }
        sc.sel = std::min(vis, std::max(0, (int)pg.rows.size() - 1));
        sc.in_sub = screen != Screen::Welcome;   // Back goes up one step
        sc.device = 1;
        sc.confirm_cross = true;
        return sc;
    }

    void render_page() {
        canvas.clear(0xFF000000);
        if (screen == Screen::TypePath || screen == Screen::ReportText) {
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

int run_headless(const setup::Layout& l, const std::string& iso, const std::string& dir, const std::string& saves,
                 const std::vector<std::pair<std::string, std::string>>& extras) {
    std::string prob = l.problem();
    if (!prob.empty()) { std::fprintf(stderr, "error: %s\n", prob.c_str()); return 2; }
    std::error_code ec;
    fs::create_directories(dir, ec);
    setup::Options o;
    o.import_saves = saves;
    for (const auto& kv : extras) o.set(kv.first, kv.second);
    auto plan = setup::plan_install(l, iso, dir, o);
    setup::Progress prog;
    std::deque<std::string> tail;
    auto why_failed = [&](size_t i, int code, const std::string& err) {   // the plain-words reason (the same as the window's failure page)
        setup::FailureCtx fc;
        fc.step = plan[i].label; fc.stage = prog.stage; fc.error = err; fc.workdir = dir; fc.exit_code = code;
        for (const std::string& t : tail) fc.tail += t + "\n";
        std::fprintf(stderr, "\n%s\n", setup::explain_failure(fc).text().c_str());
    };
    if (const std::string sp = setup::space_problem(dir, setup::needed_bytes(false, dir, o, false), false); !sp.empty()) {
        std::fprintf(stderr, "error: %s\n", sp.c_str());
        return 1;
    }
    for (size_t i = 0; i < plan.size(); i++) {
        prog.step = (int)i;
        std::printf("== %s\n", plan[i].label.c_str());
        setup::Proc p;
        if (!p.start(plan[i].argv, plan[i].env, "")) { std::fprintf(stderr, "error: %s\n", p.error().c_str()); why_failed(i, 0, "Could not start: " + p.error()); return 1; }
        while (p.busy()) {
            std::string line;
            while (p.read_line(line)) {
                setup::feed_line(prog, plan, line, SDL_GetTicks64());
                std::printf("%s\n", line.c_str());
                tail.push_back(line);
                if (tail.size() > 40) tail.pop_front();
            }
            SDL_Delay(20);
        }
        if (p.exit_code() != 0 || !prog.error.empty()) { std::fprintf(stderr, "error: step '%s' failed (%d) %s\n", plan[i].label.c_str(), p.exit_code(), prog.error.c_str()); why_failed(i, p.exit_code(), prog.error); return 1; }
        prog.fraction = std::max(prog.fraction, setup::step_done_fraction(plan, (int)i));
        std::printf("== %s done (%d%%)\n", plan[i].label.c_str(), (int)(prog.fraction * 100));
    }
    std::printf("installed: %s\n", setup::game_binary(dir).c_str());
    return setup::game_installed(dir) ? 0 : 1;
}

// --report TEXT [--dir DIR]: the Report an issue upload without a window (tests, support). Prints the result; exit 0 when the issue was opened.
int run_report_cli(const setup::Layout& l, const std::string& dir, const std::string& text, const std::string& step) {
    psp_report::Product pr;
    pr.name = "installer";
    pr.version = App::release_version(l.root);
    pr.files.push_back({"setup.log", (fs::path(dir.empty() ? "." : dir) / "setup.log").string(), 8u << 20});
    pr.space_dir = dir.empty() ? "." : dir;
    pr.save_dir = (fs::path(dir.empty() ? "." : dir) / "reports").string();
    psp_report::set_product(pr);
    psp_report::Params p;
    p.description = text + (step.empty() ? "" : "\n\nFailed: " + step);
    p.title = "Installer: " + text.substr(0, 70);
    p.include_dump = false;
    if (!step.empty()) p.extra.push_back({"failure", step});
    for (const auto& f : psp_report::preview(false)) std::printf("file: %s (%zu bytes)\n", f.name.c_str(), f.size);
    if (!psp_report::start(p)) return 1;
    while (!psp_report::take_finished()) SDL_Delay(50);
    const psp_report::Status st = psp_report::status();
    std::printf("%s\n%s\n%s\n", st.state == psp_report::Status::Done ? "DONE" : "FAILED", st.text.c_str(), st.issue_url.c_str());
    return st.state == psp_report::Status::Done ? 0 : 1;
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


int main(int argc, char** argv) {
    attach_parent_console();
    std::string root, iso, dir, play_dir, shots, import_from, saves_from;
    bool autostart = false;
    bool headless = false, play = false, launch = false;
    std::string unpack_to, report_text, report_step;
    std::vector<std::pair<std::string, std::string>> extras;
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
        else if (a == "--autostart") autostart = true;   // test aid: with --iso and --dir, open the window straight on the install (as if Start installing was chosen)
        else if (a == "--extra" && i + 1 < argc) {
            const std::string kv = argv[++i];
            const size_t eq = kv.find('=');
            if (eq == std::string::npos || eq == 0) { std::fprintf(stderr, "--extra needs KEY=VALUE\n"); return 2; }
            extras.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
        }
        else if (a == "--report") next(report_text);
        else if (a == "--report-step") next(report_step);
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
    setup::Layout layout = setup::find_layout(root);

    char* pref = SDL_GetPrefPath("psprecomp", "setup");
    std::string state_file = std::string(pref ? pref : "./") + "install.ini";
    const std::string pref2 = pref ? pref : "./";
    if (pref) SDL_free(pref);
    setup::State state = setup::load_state(state_file);

    if (!report_text.empty()) return run_report_cli(layout, dir, report_text, report_step);
    if (headless) {
        if (iso.empty() || dir.empty()) { std::fprintf(stderr, "--headless needs --iso FILE and --dir DIR\n"); return 2; }
        return run_headless(layout, iso, dir, saves_from, extras);
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
    app.marker_path = pref2 + "install.running";
    if (shots.empty() && fs::exists(app.marker_path)) {   // the last install never reached its end (killed, crashed, power loss)
        app.marker(false);
        if (app.layout_problem.empty()) app.go(Screen::Interrupted);
    }

    if (!shots.empty()) {   // look check without a window
        std::error_code ec;
        fs::create_directories(shots + "/game", ec);
        if (FILE* f = std::fopen((shots + "/game/features.ini").c_str(), "wb")) { std::fputs("hd_textures=1\njp_audio=1\nd1_moves=1\nandroid=1\nsteam=1\n", f); std::fclose(f); }
        app.layout.game_dir = shots + "/game";
        app.workdir = "/home/player/Games/Dissidia012";
        app.iso = "/home/player/Downloads/Dissidia 012 (Europe).iso";
        app.opt.set("hd_textures", "1");
        app.br.open(setup::home_dir(), false);
        int n = 0;
        auto shot = [&](const char* name) { for (int f = 0; f < 90; f++) { app.render_page(); if (!app.view.animating()) break; SDL_Delay(16); } app.render_page(); write_ppm(app.canvas, shots + "/" + name + ".ppm"); n++; };
        app.layout_problem.clear();
        app.go(Screen::Welcome); shot("1_welcome");
        app.pick = Pick::Iso; app.go(Screen::Waiting); shot("2_waiting");
        app.go(Screen::PickIso); shot("2b_pick_iso");
        app.pick = Pick::Iso; app.go(Screen::PickInfo, 0); shot("2c_pick_info");
        app.pick = Pick::Disc; app.pick_extra = "jp_audio"; app.go(Screen::PickInfo, 0); shot("2d_pick_info_undub");
        app.pick_extra = "d1_moves"; app.go(Screen::PickInfo, 0); shot("2e_pick_info_d1");
        app.msg_title = "WRONG DISC";   // a refused pick (setup::d1_disc_problem's words for a Dissidia 012 image)
        app.msg_text = "This is Dissidia 012, DISSIDIA duodecim FINAL FANTASY (ULUS10566). The moves come from the first game: pick your "
                       "Dissidia Final Fantasy (2008, USA, ULUS-10437) disc image instead.";
        app.go(Screen::Message, 0); shot("2f_refused_d1");
        app.pick = Pick::Iso;
        app.go(Screen::InstallDir); shot("3_install_dir");
        app.go(Screen::Options, 0); shot("4_options");
        app.opt.set("texture_add", "1"); app.go(Screen::Options, 1); shot("4b_options_pack");
        app.extras_mode = true; app.go(Screen::Options, 0); shot("4c_options_extras"); app.extras_mode = false;
        app.opt.set("hd_textures", "0"); app.go(Screen::Options, 0); shot("4d_options_nopack");
        app.opt.set("hd_textures", "1"); app.opt.set("texture_add", "0");
        setup::Options o; o.android = true; o.set("hd_textures", "1"); o.set("jp_audio", "1"); o.set("undub_iso", "/x.iso");
        app.plan = setup::plan_install(app.layout, app.iso, app.workdir, o);
        app.prog.step = 1; app.prog.stage = "Compiling the game"; app.prog.fraction = 0.42;
        app.prog.explain = "Compiles about 470 source files with the bundled compiler. This is the long part.";
        app.prog.detail = "Compiled 183 of 474: batch_0165.cpp";
        app.prog.start_ms = (long long)now_ms() - 7 * 60 * 1000; app.prog.compile_i = 183; app.prog.compile_n = 474; app.prog.compile_start_ms = (long long)now_ms() - 4 * 60 * 1000;
        app.go(Screen::Build, 3); shot("5_build");
        app.go(Screen::Done); shot("6_done");
        app.fail_msg = "A step failed (exit code 1); see setup.log in the install folder.";
        app.log_path = (std::filesystem::path(shots) / "setup.log").string();   // a log to offer (the rows need the file)
        { std::ofstream(app.log_path) << "example\n"; }
        app.go(Screen::Failed, 1); shot("7_failed");
        {   // the reasons the installer gives (the same text a real failure shows)
            setup::FailureCtx fc; fc.step = "Building the game"; fc.stage = "Compiling the game"; fc.workdir = app.workdir;
            fc.error = "compiling failed:\n/work/output/generated/batch_0000.cpp:1:1: error: CacheCheckFailed";
            { auto ff = setup::explain_failure(fc); app.fail_msg = ff.text(); app.fail_title = ff.title(); } app.go(Screen::Failed, 0); shot("7b_failed_space");
            fc.step = "Reading your game file"; fc.stage = ""; fc.error = "not a 2048-byte-sector ISO 9660 image (no 'CD001' at sector 16)";
            { auto ff = setup::explain_failure(fc); app.fail_msg = ff.text(); app.fail_title = ff.title(); } app.go(Screen::Failed, 0); shot("7c_failed_iso");
            fc.step = "Building the game"; fc.stage = "Compiling the game"; fc.error = "compiling failed: generated/batch_0070.cpp:1234:5: error: boom"; fc.tail = "";
            { auto ff = setup::explain_failure(fc); app.fail_msg = ff.text(); app.fail_title = ff.title(); } app.go(Screen::Failed, 0); shot("7d_failed_compile");
            fc.error = "A step failed (exit code -1)."; fc.exit_code = -1;
            { auto ff = setup::explain_failure(fc); app.fail_msg = ff.text(); app.fail_title = ff.title(); } app.go(Screen::Failed, 0); shot("7e_failed_memory");
            app.msg_title = "NOT ENOUGH SPACE";
            app.msg_text = setup::space_problem(app.workdir, 1LL << 60, false);
            app.go(Screen::Message, 0); shot("7f_not_enough_space");
        }
        { std::error_code ec; std::filesystem::remove(app.log_path, ec); }
        app.osk.begin(u"Folder path", u"/home/player", 200, true);
        app.go(Screen::TypePath); shot("8_type_path");
        app.rep_text = "The compile step stops at 40 percent and the window closes";
        app.rep_failed_step = "Compiling the game: A step failed (exit code 1)";
        app.go(Screen::Report, 2); shot("9_report");
        app.rep_sent = true; app.rep_value = "Issue #144 opened"; app.rep_status = "Issue #144 opened. Your browser opens it: https://github.com/schindlershadow/dissidia/issues/144";
        app.go(Screen::Report, 3); shot("9b_report_sent");
        app.osk.begin(u"What went wrong", u"The compile step stops", 300, true);
        app.go(Screen::ReportText); shot("9c_report_text");
        app.go(Screen::Interrupted); shot("9d_interrupted");
        std::printf("%d screenshots in %s\n", n, shots.c_str());
        SDL_Quit();
        return 0;
    }

    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");   // the click that focuses the window also acts
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
    SDL_StartTextInput();
    if (autostart && !iso.empty() && !dir.empty()) { app.iso = iso; app.workdir = dir; app.start_install_or_ask(); }
    if (app.installed() && app.layout_problem.empty()) {
        if (setup::update_check_enabled(app.state.workdir)) app.begin_check(false);
        else if (launch) app.play();
    }

    while (!app.quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                if (app.building) { app.proc.kill(); app.marker(false); }
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
            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEWHEEL:
                app.mouse_event(ev);
                break;
            case SDL_TEXTINPUT:
                if (app.screen == Screen::TypePath || app.screen == Screen::ReportText)
                    for (const unsigned char* c = (const unsigned char*)ev.text.text; *c; c++)
                        if (*c >= 0x20 && *c < 0x7F) psp_sddialog::osk_push_key(*c);
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_AC_BACK) {   // Android's back button
                    if (app.building) { app.sel = 3; app.confirm(); } else app.cancel();
                } else if (ev.key.keysym.sym == SDLK_F11 || (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                    bool fs_on = SDL_GetWindowFlags(app.win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(app.win, fs_on ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                } else if (app.screen == Screen::TypePath || app.screen == Screen::ReportText) {
                    if (ev.key.keysym.sym == SDLK_BACKSPACE) psp_sddialog::osk_push_key(8);
                    else if (ev.key.keysym.sym == SDLK_RETURN) psp_sddialog::osk_push_key(13);
                    else if (ev.key.keysym.sym == SDLK_ESCAPE) {
                        psp_sddialog::osk_set_active(false);
                        if (app.screen == Screen::ReportText) app.go(Screen::Report, 0); else app.go(Screen::BrowseDir);
                    }
                }
                break;
            case SDL_WINDOWEVENT:
                app.dirty = true;
                break;
            }
        }
        app.poll_native();
        app.pump_report();
        if (app.checking) app.pump_check();
        if (app.building) app.pump_build();
        app.update_input();
        int64_t t = now_ms();
        if (app.dirty || ((app.building || app.rep_working) && t - app.last_draw > 250)) {
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
