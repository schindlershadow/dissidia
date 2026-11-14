// What the setup wizard does, without any UI: where the pieces of a release are, the command lines of the install steps,
// how their output maps to a progress bar, the remembered install location, and starting the game.
#pragma once
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
    bool is_build = false;              // output carries "[psp-build] ..." progress lines
    double weight = 0;                  // share of the whole install (all steps sum to 1)
    double base = 0;                    // fraction done when this step starts
};

// What the player chose besides the game file and folder.
struct Options {
    bool check_updates = true;
    bool hd_textures = true;            // pull an HD texture pack (which one: texture_pack)
    std::string texture_pack = "liink"; // "liink" = LiinkPK's hand-retouched pack, "ai" = Niosa's D012FFRT + our AI upscales
    bool jp_audio = false;              // undub (Japanese voices); needs undub_iso
    bool android = false;               // also build the Android version
    std::string undub_iso;
    std::string import_saves;           // a SAVEDATA folder to copy the player's Dissidia 012 saves from (empty = none)
};
// Names and credits of the two texture packs the wizard offers (shown on the options page and in the download step).
const char* texture_pack_title(const std::string& pack);
const char* texture_pack_credit(const std::string& pack);
// Optional features a release can offer: game/features.ini has one "name=1" line per implemented feature
// (hd_textures, jp_audio, android). A missing file or line = not in this release yet (the wizard shows the option but
// does not let it be turned on).
bool feature_available(const Layout& l, const std::string& name);

std::vector<Step> plan_install(const Layout& l, const std::string& iso, const std::string& workdir, const Options& o);
// The game build only (rebuild / repair): analysis, recompile, compile (+ the Android build when chosen).
// Only the extras of `o` (HD textures unless skip_textures, Japanese voices, saves, the Android build) on an installed game: no rebuild.
std::vector<Step> plan_extras(const Layout& l, const std::string& workdir, const Options& o, bool skip_textures);
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
std::vector<Step> plan_extras_android(const Layout& l, const std::string& workdir, const Options& o, bool skip_textures);
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
constexpr long long kNeededBytes = 6LL << 30;             // extracted disc (~1.6 GB) + generated sources + objects, with slack

}  // namespace setup
