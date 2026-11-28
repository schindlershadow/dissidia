// dlc_import.h - Dissidia 012's downloadable content as a mod folder (issue #134).
//
// The DLC of the PSP game (costumes, music, Aerith's unlock key) is a set of NPDRM EDAT files, one per asset, named by the
// 8 hex digits of the asset's name hash, in ms0:/PSP/GAME/<game id>/ (the game id is ULUS10566 on the US disc ...). They are
// installed as an ordinary mod folder so the Mods menu can turn them on and off:
//
//   <mods>/dissidia-dlc/mod.json     {"name": "Dissidia 012 DLC", "kind": "dlc", "enabled": true}
//   <mods>/dissidia-dlc/dlc/<hash>.edat   the files as they were (encrypted or not; the runtime decrypts what it opens)
//
// dlc_mod.cpp makes the game see the files of the folders that are on at ms0:/PSP/GAME/<id>/<hash>.edat. This part has no
// dependency on the runtime (std::filesystem + psp_savemgr's search for PPSSPP), so the installer wizard uses it too.
#pragma once

#include <string>
#include <vector>

namespace dissidia_dlc {

constexpr const char* kModFolder = "dissidia-dlc";   // <mods>/<this>/

struct Source {
    std::string label;             // "PPSSPP - ULUS10566"
    std::string path;              // the folder holding the .edat files
    int files = 0;
    unsigned long long bytes = 0;
};

// The DLC files under `root` (any depth up to 4): regular files named <8 hex digits>.edat, any case. Sorted.
std::vector<std::string> collect(const std::string& root);

// Installs of PPSSPP and PSP memory sticks on this machine that hold Dissidia 012 DLC (PSP/GAME/<Dissidia id>/*.edat).
std::vector<Source> detect();

struct Result {
    bool ok = false;
    int files = 0;
    unsigned long long bytes = 0;
    std::string text;              // one line to show: what was installed, or why not
};

// Copies the DLC files found under `src` into <mods_dir>/dissidia-dlc/dlc/ (names lower-cased), creating or updating the mod
// folder; files that are there already are replaced. The mod's on / off setting is kept.
Result import_folder(const std::string& src, const std::string& mods_dir);

}  // namespace dissidia_dlc
