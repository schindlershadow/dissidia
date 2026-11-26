// dlc_mod.cpp - Dissidia 012's downloadable content as a mod (issue #134): the game's view of it, and Mods > Install DLC.
//
// The game opens each DLC asset as ms0:/PSP/GAME/<game id>/<8 hex digits of the name hash>.edat (an NPDRM EDAT; the HLE decrypts
// it when the game sets the key, psp_npdrm). The files live in mod folders (dlc_import.h): while such a folder is on, the game
// finds its files at those paths whatever the folder name of its disc region; when every DLC folder is off (Settings > Mods) the
// game finds none. Files a player copied into ms0/PSP/GAME/<id>/ by hand (a PPSSPP card, an uppercase F6207479.EDAT) are found by
// their name without regard to case, as long as no DLC mod folder exists. Which folders are on is read once, at the first look-up:
// like every mod, a change applies at the next start.
#include "dlc_import.h"

#include "hle/psp_hle_io.h"
#include "psp_settings.h"
#include "../../../runtime/src/hle/psp_savearchive.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

std::string dissidia_mods_dir();   // hooks_io.cpp

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

struct DlcState {
    bool built = false;
    bool any_folder = false;                       // a mod folder of kind "dlc" exists (on or off)
    std::map<std::string, std::string> files;      // lower-case file name -> host path, of the folders that are on
};
std::mutex g_mu;
DlcState g_st;

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool json_has(const std::string& js, const char* key, const char* value) {   // "key": "value"
    const size_t k = js.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) return false;
    size_t v = js.find(':', k);
    if (v == std::string::npos) return false;
    ++v;
    while (v < js.size() && std::isspace((unsigned char)js[v])) ++v;
    return js.compare(v, std::char_traits<char>::length(value) + 2, std::string("\"") + value + "\"") == 0;
}
bool json_off(const std::string& js) {
    const size_t k = js.find("\"enabled\"");
    if (k == std::string::npos) return false;
    size_t v = js.find(':', k);
    if (v == std::string::npos) return false;
    ++v;
    while (v < js.size() && std::isspace((unsigned char)js[v])) ++v;
    return js.compare(v, 5, "false") == 0;
}

void build_state() {
    g_st = DlcState();
    g_st.built = true;
    const fs::path mods = fs::u8path(dissidia_mods_dir());
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (auto it = fs::directory_iterator(mods, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        const std::string n = it->path().filename().u8string();
        if (n.empty() || n[0] == '.' || n[0] == '_' || !it->is_directory(e2)) continue;
        dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const fs::path& d : dirs) {
        const std::string js = read_text(d / "mod.json");
        if (js.empty() || !json_has(js, "kind", "dlc")) continue;
        g_st.any_folder = true;
        if (json_off(js)) { std::fprintf(stderr, "[DLC] mod folder %s: off\n", d.filename().u8string().c_str()); continue; }
        int n = 0;
        for (auto it = fs::directory_iterator(d / "dlc", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            g_st.files.emplace(lower(it->path().filename().u8string()), it->path().u8string());   // the first folder wins
            ++n;
        }
        ec.clear();
        std::fprintf(stderr, "[DLC] mod folder %s: on, %d file(s)\n", d.filename().u8string().c_str(), n);
    }
}

bool has_game_dir(const std::string& host, const std::string& ms0) {   // <ms0>/PSP/GAME/<x>/<file>
    if (host.size() <= ms0.size() + 10 || host.compare(0, ms0.size(), ms0) != 0) return false;
    const std::string rest = lower(host.substr(ms0.size()));
    return rest.rfind("/psp/game/", 0) == 0 && rest.find('/', 10) != std::string::npos;
}

// The path the game's open of `host` (made from ms0:/PSP/GAME/<id>/<hash>.edat) should use, or "" to keep it.
std::string remap_ms_path(const std::string& host) {
    if (host.size() < 6 || lower(host.substr(host.size() - 5)) != ".edat") return "";
    std::string ms0 = psp_io_ms0_host_path();
    while (ms0.size() > 1 && ms0.back() == '/') ms0.pop_back();
    if (!has_game_dir(host, ms0)) return "";
    const size_t slash = host.find_last_of('/');
    const std::string name = lower(host.substr(slash + 1));
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st.built) build_state();
    const auto it = g_st.files.find(name);
    if (it != g_st.files.end()) return it->second;
    if (g_st.any_folder) {
        // a DLC mod exists but not this file: only what the player copied by hand stays visible while the mod is on
        if (g_st.files.empty()) return ms0 + "/PSP/GAME/.dlc_off/" + name;   // every DLC folder is off: none of the game's DLC
    }
    std::error_code ec;
    if (fs::exists(fs::u8path(host), ec)) return "";
    // the game asks for a lower-case name; PPSSPP's card has F6207479.EDAT
    for (auto d = fs::directory_iterator(fs::u8path(host.substr(0, slash)), ec); !ec && d != fs::directory_iterator(); d.increment(ec))
        if (lower(d->path().filename().u8string()) == name) return d->path().u8string();
    // the DLC folder of another region (ULES01505 with a US game): any folder under PSP/GAME that holds the file, by name without case
    ec.clear();
    for (auto g = fs::directory_iterator(fs::u8path(ms0 + "/PSP/GAME"), ec); !ec && g != fs::directory_iterator(); g.increment(ec)) {
        std::error_code e2;
        if (!g->is_directory(e2)) continue;
        for (auto d = fs::directory_iterator(g->path(), e2); !e2 && d != fs::directory_iterator(); d.increment(e2))
            if (lower(d->path().filename().u8string()) == name) return d->path().u8string();
    }
    return "";
}

// ---- Mods > Install DLC (from a folder / from a .zip) ---------------------------------------------------------------

struct Prog {
    std::mutex mu;
    std::atomic<bool> busy{false};
    unsigned revision = 0;
    std::string text;
    void set(const std::string& t) { std::lock_guard<std::mutex> lk(mu); text = t; ++revision; }
} g_prog;

std::string import_any(const std::string& src, const std::string& mods) {
    std::error_code ec;
    std::string folder = src, tmp;
    if (fs::is_regular_file(fs::u8path(src), ec)) {
        if (!psp_savearchive::looks_like_archive(fs::u8path(src).filename().u8string())) return "Not installed: choose a folder or a .zip file.";
        tmp = mods + "/.dlc_unpack";
        fs::remove_all(fs::u8path(tmp), ec);
        g_prog.set("Unpacking the archive ...");
        const std::string err = psp_savearchive::extract(src, tmp);
        if (!err.empty()) { fs::remove_all(fs::u8path(tmp), ec); return "Not installed: " + err; }
        folder = tmp;
    }
    g_prog.set("Copying the DLC files ...");
    const dissidia_dlc::Result r = dissidia_dlc::import_folder(folder, mods);
    if (!tmp.empty()) fs::remove_all(fs::u8path(tmp), ec);
    return r.text;
}

bool start_import(const std::string& path) {
    bool expected = false;
    if (!g_prog.busy.compare_exchange_strong(expected, true)) return false;
    const std::string mods = dissidia_mods_dir();
    g_prog.set("Reading the DLC ...");
    std::thread([path, mods]() {
        std::string r;
        try { r = import_any(path, mods); }
        catch (const std::exception& e) { r = std::string("Not installed: ") + e.what(); }
        g_prog.set(r);
        g_prog.busy.store(false);
    }).detach();
    return true;
}

void import_status(unsigned* revision, bool* busy, std::string* text) {
    std::lock_guard<std::mutex> lk(g_prog.mu);
    if (revision) *revision = g_prog.revision;
    if (busy) *busy = g_prog.busy.load();
    if (text) *text = g_prog.text;
}

}  // namespace

// hooks_io.cpp: the ms0 look-up of the game's open / stat
std::string dissidia_dlc_remap_ms(const std::string& host) { return remap_ms_path(host); }

// hooks_main.cpp: Settings > Mods > Install DLC (folder / zip)
void dissidia_dlc_register_installers() {
    static const PspModInstaller folder = {
        "Install DLC from a Folder",
        "Install the DLC\nChoose the folder that holds Dissidia 012's DLC files (PSP/GAME/ULUS10566 in a PPSSPP install or on a PSP memory "
        "stick: files named like eac7bf53.edat). They are copied into the game and listed below as the mod Dissidia 012 DLC, where it can "
        "be turned off and on. Takes effect the next time the game starts.",
        "Choose the folder with the DLC files",
        "",
        start_import,
        import_status,
        1,
    };
    static const PspModInstaller zip = {
        "Install DLC from a Zip",
        "Install the DLC\nChoose a .zip of Dissidia 012's DLC files (the PSP/GAME/ULUS10566 folder of a PPSSPP install or memory stick, "
        "files named like eac7bf53.edat). They are copied into the game and listed below as the mod Dissidia 012 DLC, where it can be turned "
        "off and on. Takes effect the next time the game starts.",
        "Choose a .zip of the DLC",
        ".zip",
        start_import,
        import_status,
        2,
    };
    psp_settings_add_mod_installer(folder);
    psp_settings_add_mod_installer(zip);
    // PSPRECOMP_DLC_IMPORT=<folder or .zip> (tests, scripts): install it at start the way the menu does, print the result and quit
    if (const char* p = std::getenv("PSPRECOMP_DLC_IMPORT")) {
        if (p[0] && start_import(p)) {
            while (g_prog.busy.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::string t;
            import_status(nullptr, nullptr, &t);
            std::fprintf(stderr, "[DLC_IMPORT] result: %s\n", t.c_str());
            std::fflush(stderr);
            std::_Exit(t.rfind("Installed", 0) == 0 ? 0 : 1);
        }
    }
}
