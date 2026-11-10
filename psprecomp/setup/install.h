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
    std::string root, python, setup_script, build_script, recompiler, toolchain, sdk, game_dir;
    // Empty string when everything needed is there, else a message naming what is missing.
    std::string problem() const;
};
Layout find_layout(const std::string& root);   // root: the release folder (default: where the wizard runs from)

struct Step {
    std::string label;                  // shown on the progress page
    std::string explain;                // what is going on, in plain words
    std::vector<std::string> argv;
    EnvList env;
    bool is_build = false;              // output carries "[psp-build] ..." progress lines
    double weight = 0;                  // share of the whole install (all steps sum to 1)
    double base = 0;                    // fraction done when this step starts
};

// What the player chose besides the game file and folder.
struct Options {
    bool check_updates = true;
    bool hd_textures = false;           // pull the HD texture pack
    bool jp_audio = false;              // undub (Japanese voices); needs undub_iso
    bool android = false;               // also build the Android version
    std::string undub_iso;
};
// Optional features a release can offer: game/features.ini has one "name=1" line per implemented feature
// (hd_textures, jp_audio, android). A missing file or line = not in this release yet (the wizard shows the option but
// does not let it be turned on).
bool feature_available(const Layout& l, const std::string& name);

std::vector<Step> plan_install(const Layout& l, const std::string& iso, const std::string& workdir, const Options& o);
// The game build only (rebuild / repair): analysis, recompile, compile (+ the Android build when chosen).
std::vector<Step> plan_rebuild(const Layout& l, const std::string& workdir, const Options& o);

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
    std::string workdir, iso, version;
    Options options;
};
State load_state(const std::string& file);
bool save_state(const std::string& file, const State& s);
bool game_installed(const std::string& workdir);          // build/psprecomp_runtime exists
std::string game_binary(const std::string& workdir);

// ---- starting the game ----
EnvList game_env(const std::string& workdir);
bool launch_game(const std::string& workdir, std::string* error);

// default install folder suggestion (~/Games/Dissidia012 or %USERPROFILE%\Games\Dissidia012)
std::string default_install_dir();
std::string home_dir();
// Free bytes on the volume of `path` (nearest existing parent); -1 if unknown.
long long free_space(const std::string& path);
constexpr long long kNeededBytes = 6LL << 30;             // extracted disc (~1.6 GB) + generated sources + objects, with slack

}  // namespace setup
