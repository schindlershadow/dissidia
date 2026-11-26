// The extras of an install (HD textures, Japanese voices, Bartz's Dissidia 1 moves, ...): ONE list for every installer. The desktop
// wizard (setup_main.cpp) and the Android setup app (android/setup/java, through setup/android_engine.cpp) build their Options and
// Change extras pages, their "pick the second disc" pages, install.ini and their install steps from it. A new extra is one entry in
// extras.cpp (plus its tool in games/<id>/tools and a line in game/features.ini); both installers show and apply it without further
// changes. Mark it desktop = false or android = false only when it cannot work there (it then stays off on that platform).
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "proc.h"

namespace setup {

struct Layout;
struct Options;
struct Step;

// Where the steps run: the desktop wizard or the setup app on the phone.
struct ExtraCtx {
    const Layout& l;
    std::string workdir;
    bool android = false;
    // Python + games/<id>/tools/<script> + args, with the platform's environment (the bundled Python's on Android).
    Step tool(const std::string& label, const std::string& explain, const std::string& script, const std::vector<std::string>& args, double weight) const;
    // Fetches files (url, local file): on Android the app downloads them itself (the bundled Python has no DNS / CA store there),
    // on the desktop tools/download.py does. A file that is already there is kept (a retry continues).
    Step download(const std::string& label, const std::string& explain, const std::vector<std::pair<std::string, std::string>>& files, double weight) const;
    std::string game_files() const;   // where the game reads its files (Android: Android/media/<package>/game; desktop: the work folder)
};

struct ExtraChoice {
    std::string value, label, help;
};

// A second file the extra is built from: a disc image, or a texture pack archive (picked before the install / Change extras applies it).
struct ExtraDisc {
    std::string key;            // where its path is kept in Options (empty: no disc)
    std::string page_title;     // e.g. "The undub file"
    std::string eyebrow;        // e.g. "Japanese voices"
    std::string what, what_help;     // "A second disc image", what it is used for
    std::string valid, valid_help;   // "Valid: ...", which releases work
    std::string exts = ".iso or .cso";
    std::string valid_short;    // the desktop page's value column, e.g. "US"
    std::vector<std::string> patterns = {".iso", ".cso"};   // the file endings the pickers list (lower case; both installers)
    std::string filter_name = "PSP disc images";            // the desktop file dialog's filter name
    // Asked only while the extra has this value (one of its choices; e.g. the texture pack's "file"). Empty: whenever it is on.
    std::string when;
    // Is the picked file the right disc? "" = yes, else what is wrong, in words for the player (both installers show it and ask again).
    // `path` may be "fd:N" (Android picker; the descriptor's offset is not moved).
    std::function<std::string(const std::string& path)> check;
};

struct Extra {
    std::string key;            // install.ini / Options key
    std::string feature;        // game/features.ini line that enables it in a release ("" = always available)
    std::string title, help;
    std::string android_help;   // appended to help in the Android app (e.g. download size and WiFi)
    std::string icon;           // the Android app's icon name: texture, voice, sword, sliders, disc, download, crystal
    std::vector<ExtraChoice> choices;   // empty: an on / off switch ("1" / "0")
    std::string def = "0";      // default value
    std::string parent;         // shown (and used) only while this switch is on (e.g. the texture pack under HD textures)
    ExtraDisc disc;
    bool desktop = true, android = true;
    long long need_bytes = 0;   // free space it needs while it is applied (an estimate; the installers check it before they start)
    // An action rather than a state (e.g. add a texture pack): asks for its file every time it is turned on, and is shown off again
    // after it ran (Options::clear_discs resets it, as the installers do once a run is done).
    bool action = false;
    // Change extras: Once = applied when turned on (with a disc: whenever a disc was picked); its files stay when turned off.
    // Redo = applied again when it or one of its sub-choices changed or its files are gone; removed when turned off.
    enum Reapply { Once, Redo } reapply = Once;
    // Appends the install steps (called only while it is on, available and, when it has a disc, a disc was picked).
    std::function<void(const ExtraCtx&, const Options&, std::vector<Step>&)> steps;
    std::function<bool(const ExtraCtx&)> present;   // Redo: are its files in place?
    std::function<void(const ExtraCtx&)> remove;    // Redo: delete its files (turned off, or about to be redone)
};

const std::vector<Extra>& extras();
const Extra* find_extra(const std::string& key);

// The extras' steps of an install (prev == nullptr) or of Change extras on an installed game (prev = what is installed): removes what
// was turned off or has to be redone, then appends the steps of what has to be added. Order = the list's order.
void plan_extra_steps(std::vector<Step>& p, const ExtraCtx& ctx, const Options& now, const Options* prev);

// The extras that still need their disc picked before `now` can be applied (prev as above): their keys, in order.
std::vector<std::string> extras_needing_disc(const Layout& l, bool android, const Options& now, const Options* prev);

// Looks for the disc an extra needs (a .iso / .cso that passes its check: right game id, right voices) in the folder of the first
// image the player picked, `near_file`. "" when there is none (or the extra's file is not a disc image); then the player is asked.
std::string find_extra_disc(const std::string& key, const std::string& near_file);

// The check of an extra's disc (ExtraDisc::check); "" when it has none or the disc is right.
std::string extra_disc_problem(const std::string& key, const std::string& path);

// The list as JSON for the Android app (title, help, icon, choices, parent, default, available, disc page texts).
std::string extras_json(const Layout& l, bool android);

}  // namespace setup
