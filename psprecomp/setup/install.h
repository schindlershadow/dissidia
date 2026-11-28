// What the setup wizard does, without any UI: where the pieces of a release are, the command lines of the install steps,
// how their output maps to a progress bar, the remembered install location, and starting the game.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "proc.h"

namespace setup {

// A release unpacks to one folder (flat layout; the wizard sits in its root):
//   psprecomp_setup[.exe]        this program
//   bin/psprecomp[.exe]          the recompiler
//   toolchain/                   the portable compiler bundle (toolchain/build_bundle.py), incl. bin/pspdecrypt
//   sdk/                         lib/libpsprecomp_core.a, headers, CMakeLists.txt, toolchain.json
//   game/                        games/dissidia: game.toml, overlay_*.toml, analysis/, tools/*.py
//   python/python.exe            embedded Python (Windows releases; Linux uses python3 from PATH)
struct Layout {
    std::string self;   // this program (the import-saves step runs it again)
    std::string root, python, setup_script, build_script, update_script, recompiler, toolchain, sdk, game_dir;
    std::string pspdecrypt, zig, android_dir, lib_dir;   // Android only (find_layout_android): the tools in the app's native-library folder, the Android part
    // Empty string when everything needed is there, else a message naming what is missing.
    std::string problem() const;
};
Layout find_layout(const std::string& root);   // root: the release folder (default: where the wizard runs from)

struct Step {
    std::string label;                  // shown on the progress page
    std::string explain;                // what is going on, in plain words
    std::vector<std::string> argv;
    EnvList env;
    std::vector<std::pair<std::string, std::string>> downloads;   // (url, file) fetched by the app itself instead of running argv (Android: the bundled Python has no DNS / CA store)
    bool optional = false;              // a failure is reported in the end note but does not stop the install
    bool is_build = false;              // output carries "[psp-build] ..." progress lines
    double weight = 0;                  // share of the whole install (all steps sum to 1)
    double base = 0;                    // fraction done when this step starts
};

// What the player chose besides the game file and folder. The extras (HD textures, Japanese voices, ...) are generic: their list is
// setup/extras.cpp, shared with the Android setup app; the rest are the desktop wizard's own options.
struct Options {
    bool check_updates = true;
    bool android = false;               // also build the Android version (desktop)
    bool steam = false;                 // add the game to the Steam library (desktop; Steam is closed and reopened for a moment)
    std::string import_saves;           // a SAVEDATA folder to copy the player's Dissidia 012 saves from (empty = none; desktop)
    std::string import_dlc;             // a PSP/GAME/<id> folder to copy Dissidia 012's DLC (.edat files) from, installed as a mod (empty = none; desktop)
    std::map<std::string, std::string> extras;   // extra key -> value ("1" / "0" or a choice); a second disc's path under its disc key
    std::string get(const std::string& key) const;   // the value, else the extra's default ("" for an unknown key)
    bool on(const std::string& key) const { return get(key) == "1"; }
    void set(const std::string& key, const std::string& value) { extras[key] = value; }
    void clear_discs();                 // forget the picked discs (a path or Android descriptor is only valid once); resets actions too
    void clear_actions();               // after an install: actions (add a texture pack) are done, their file is forgotten
};
// Optional features a release can offer: game/features.ini has one "name=1" line per implemented feature
// (hd_textures, jp_audio, d1_moves, android). A missing file or line = not in this release yet (the wizard shows the option but
// does not let it be turned on).
bool feature_available(const Layout& l, const std::string& name);

// A PSP disc image's (.iso or .cso) PSP_GAME/PARAM.SFO: DISC_ID (e.g. "ULUS10437") and TITLE. `path` may be "fd:N" (Android picker);
// the descriptor's file offset is not moved. false = not readable as an ISO 9660 PSP disc image.
bool read_disc_id(const std::string& path, std::string* id, std::string* title);
// The Bartz moves need Dissidia Final Fantasy (2008), USA (ULUS-10437). "" when `path` is that disc, else what is wrong
// with it, in words for the player (e.g. a Dissidia 012 image picked by mistake).
std::string d1_disc_problem(const std::string& path);
// The Japanese voices need a Dissidia 012 image whose voices are Japanese (an undub, or the Japanese release; .iso or .cso).
// "" when `path` is one, else what is wrong with it (another game, the first Dissidia, or a regular English 012).
std::string undub_disc_problem(const std::string& path);
// A texture pack archive the player picked (Add your own texture pack): "" when it is a .zip holding a textures.ini (in any
// folder) or id-named PNGs, or a .7z / .rar a tool on this PC can unpack (fetch_textures.py checks those once unpacked); else what is wrong.
// `path` may be "fd:N" (Android picker; read with pread, the offset is not moved).
std::string texture_pack_problem(const std::string& path);
bool archive_tool_available();   // 7z / 7za / 7zz / bsdtar (Windows: also tar.exe) on the PATH; never on Android

// Opens `path` with the desktop's default program (show_in_folder = false), or the folder that holds it with the file
// selected where the platform can (Windows Explorer /select, macOS Finder -R; Linux opens the folder). Not on Android.
bool open_on_desktop(const std::string& path, bool show_in_folder, std::string* error);

std::vector<Step> plan_install(const Layout& l, const std::string& iso, const std::string& workdir, const Options& o);
// The game build only (rebuild / repair): analysis, recompile, compile (+ the Android build when chosen).
// Change extras on an installed game (prev = what is installed, o = the new choice; extras.h plan_extra_steps), plus saves, the Android
// build and Steam as chosen: no rebuild.
std::vector<Step> plan_extras(const Layout& l, const std::string& workdir, const Options& prev, const Options& o);
// Adds the installed game to the Steam library as a non-Steam shortcut (steam_shortcut.py; the launcher is the program itself with --launch).
std::vector<Step> plan_steam(const Layout& l, const std::string& workdir);
std::vector<Step> plan_rebuild(const Layout& l, const std::string& workdir, const Options& o);

// Installs the Android build (<workdir>/android/dissidia.apk) on a connected device with adb and copies the game's files to it
// (game/tools/android_install.py).
std::vector<Step> plan_android_install(const Layout& l, const std::string& workdir);
bool android_build_exists(const std::string& workdir);

#ifdef __ANDROID__
// The setup app: the release folder is the unpacked payload (files/release); the executables are the app's lib*.so (lib_dir).
Layout find_layout_android(const std::string& root, const std::string& lib_dir);
// Read the disc image, translate + compile the game ON this device with the bundled zig (psp_build.py --android), copy the game's
// files to Android/media/<package>/game. The built APK is installed by the wizard afterwards (Java PackageInstaller).
std::vector<Step> plan_install_android(const Layout& l, const std::string& iso, const std::string& workdir, const Options& o);
constexpr const char* kGamePackage = "org.psprecomp.game";
std::string android_game_dir();
EnvList android_env(const Layout& l, const std::string& workdir);   // PYTHONHOME etc. for the bundled Python
// update.py android-check / android-verify (see games/dissidia/tools/update.py)
std::vector<std::string> android_check_argv(const Layout& l, const std::string& manifest, const std::string& sig, const std::string& current);
std::vector<std::string> android_verify_argv(const Layout& l, const std::string& file, const std::string& sha256, long long size);
constexpr const char* kManifestUrl = "https://github.com/schindlershadow/dissidia/releases/latest/download/manifest.json";
std::string android_payload_fingerprint(const Layout& l);   // android/build_fingerprint.txt of the unpacked payload ("" if absent)
// The game build again (translate + compile + copy) from the already extracted work folder, after the app was updated.
// Only the extras of `o` on a built game, then the copy to the game's folder (no rebuild).
std::vector<Step> plan_extras_android(const Layout& l, const std::string& workdir, const Options& prev, const Options& o);
std::vector<Step> plan_rebuild_android(const Layout& l, const std::string& workdir);   // where the game app reads its files: /sdcard/Android/media/org.psprecomp.game/game
#endif

// ---- updates (game/tools/update.py; see docs/installer.md "Updater") ----
struct UpdateInfo {
    bool available = false;
    std::string version, current, error, url, sha256;
    long long size = 0;
    bool rebuild = false, relink = false;
};
// argv of "update.py check" / the parse of its one output line.
std::vector<std::string> update_check_argv(const Layout& l);
UpdateInfo parse_update_line(const std::string& line);
// Download + install the new release files, then (when the generated code or the core changed) build the game again.
std::vector<Step> plan_update(const Layout& l, const std::string& workdir, const Options& o, const UpdateInfo& u);
// settings.ini "check_updates=0" in the work folder, or PSPRECOMP_UPDATE_CHECK=0, turns the automatic check off.
bool update_check_enabled(const std::string& workdir);

struct Progress {
    int step = 0;                       // index into the plan
    int sub = 0;                        // psp_build.py phase (0..4) within the build step
    double fraction = 0;                // overall 0..1
    std::string stage;                  // e.g. "Compiling the game"
    std::string explain;                // what the stage does
    std::string detail;                 // e.g. "Compiling: 183 of 474 files"
    std::string last_line;
    std::string error;                  // set by an "[psp-build] ERROR" line
    long long start_ms = 0;             // when the install started
    int compile_i = 0, compile_n = 0;   // ninja's [i/n] counter
    long long compile_start_ms = 0;
};
// Feeds one output line of plan[step] into the progress model (now_ms: any monotonic clock).
void feed_line(Progress& p, const std::vector<Step>& plan, const std::string& line, long long now_ms);
// Fraction of the whole install done when step `i` has just finished.
double step_done_fraction(const std::vector<Step>& plan, int i);
// "about 12 min left" / "estimating..." and "3:05" style strings for the progress page.
std::string eta_text(const Progress& p, long long now_ms);
std::string elapsed_text(const Progress& p, long long now_ms);

// ---- remembered install (per user: SDL pref dir, passed in so this file needs no SDL) ----
struct State {
    std::string workdir, iso, version, built_fp;   // built_fp: Android, the build fingerprint of the payload the game was last built with
    Options options;
};
State load_state(const std::string& file);
bool save_state(const std::string& file, const State& s);
bool game_installed(const std::string& workdir);          // build/psprecomp_runtime exists
std::string game_binary(const std::string& workdir);

// ---- starting the game ----
EnvList game_env(const std::string& workdir);
bool launch_game(const std::string& workdir, std::string* error);

// A menu entry that starts the game: ~/.local/share/applications/dissidia-012.desktop (GNOME/KDE launchers) on Linux,
// a Start Menu shortcut on Windows. It runs this program with --launch, so updates are checked first. `where` gets the path.
// `root` = the release folder (where DissidiaRecomp[.exe] and game/icon live): the shortcut points at the program there, which is
// not the running one when the single-file download unpacked itself. Empty = the running program's own folder.
bool create_shortcut(const std::string& workdir, const std::string& root, std::string* where, std::string* error);

// default install folder suggestion (~/Games/Dissidia012 or %USERPROFILE%\Games\Dissidia012)
std::string default_install_dir();
std::string home_dir();
// Free bytes on the volume of `path` (nearest existing parent); -1 if unknown.
long long free_space(const std::string& path);
constexpr long long kNeededBytes = 6LL << 30;             // desktop: extracted disc (~1.7 GB) + generated sources + objects, with slack
constexpr long long kNeededBytesAndroid = 7LL << 29;      // phone: 3.5 GB peak (one disc copy 1.7 + objects 0.4 + generated sources 0.2 + zig cache 0.4 + apk/link 0.2 + slack), see needed_bytes

// How much free space an install needs: the game build (desktop kNeededBytes, phone kNeededBytesAndroid; what a retry already has in
// `workdir` - the extracted disc, the compiled objects - is subtracted) plus every extra turned on in `o` (Extra::need_bytes).
// `extras_only`: Change extras on an installed game (no build).
// Are the game's disc files still there (the phone keeps the only copy in game_dir/disc0; an older install has them in workdir/disc0)? When
// not (the player deleted the game folder), a rebuild / extras change needs the game file (.iso / .cso) again.
bool disc_files_present(const std::string& workdir, const std::string& game_dir);
long long needed_bytes(bool android, const std::string& workdir, const Options& o, bool extras_only);
// "" when `workdir`'s drive has room for `need`, else the failure text (explain_failure's "space") to show BEFORE the install
// starts. PSPRECOMP_SKIP_SPACE_CHECK=1 turns the check off. Unknown free space (-1) passes.
std::string space_problem(const std::string& workdir, long long need, bool android);

// What went wrong, in words for the player: which step, why, what to do. Used by both installers (the phone through JNI).
struct FailureCtx {
    std::string step;       // the plan step's label ("Reading your game file", "Building the game on this device", ...)
    std::string stage;      // the progress stage when it stopped ("Compiling the game on this device", ...); may be empty
    std::string error;      // the tool's own error line ("compiling failed: ...", "not enough free disk space ...")
    std::string tail;       // the last lines of output (newline separated)
    std::string workdir;
    int exit_code = 0;      // -1 = the process was killed (not by us)
    bool android = false;   // the phone's setup app (Fast build, close apps hints)
    bool cancelled = false;
};
struct Failure {
    std::string kind;       // space, memory, iso, damaged, version, download, compile, link, translate, pack, device, cancelled, other
    std::string where, why, todo;
    std::string title() const;  // a few words for the page ("Not enough space", "Out of memory", ...)
    std::string text() const;   // "<where>\n\nWhy: <why>\n\nWhat to do: <todo>" (what the failure pages show)
};
Failure explain_failure(const FailureCtx& c);
std::string human_gb(long long bytes);   // "1.3 GB" / "640 MB"

}  // namespace setup
