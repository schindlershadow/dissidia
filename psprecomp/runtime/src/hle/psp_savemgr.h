// Save-folder manager: find, list, import and export PSP save folders between regions and between this runtime and
// PPSSPP / a PSP memory stick. Pure library (std::filesystem, no runtime dependency): used by the in-game
// "Manage saves" screen (psp_savemgr_ui.cpp) and by tools/savemgr.
//
// A PSP save is a folder `<DISC_ID><suffix>` (ULUS10566GameData00, ULES01505FCard03 ...) with PARAM.SFO and the
// game's encrypted files. The files are the same for every region (key and format are shared); moving a save to
// another region means renaming the folder, rewriting SAVEDATA_DIRECTORY and signing PARAM.SFO again
// (psp_savedata_crypto, the same signature PPSSPP makes). The data files are copied untouched. A real PSP checks
// PARAM.SFO against its own console key, which cannot be made here.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace psp_savemgr {

struct SaveInfo {
    std::string path;       // the save folder
    std::string name;       // folder name, e.g. ULES01505GameData00
    std::string disc;       // ULES01505
    std::string suffix;     // GameData00
    std::string title;      // PARAM.SFO TITLE
    std::string savetitle;  // SAVEDATA_TITLE
    std::string detail;     // SAVEDATA_DETAIL
    time_t mtime = 0;       // PARAM.SFO's modification time
    uint64_t bytes = 0;     // size of the files that travel with the save
};

struct Source {
    std::string label;      // "PPSSPP", "Memory stick (/media/me/PSP)", ...
    std::string path;       // a SAVEDATA folder
};

// "ULUS10566GameData00" -> disc "ULUS10566", suffix "GameData00". False when the name is not a save folder name.
bool split_name(const std::string& folder, std::string* disc, std::string* suffix);
bool valid_disc_id(const std::string& s);          // "ULUS10566"
const char* region_of(const std::string& disc);    // "US" "EU" "JP" "Asia" "?"
const char* kind_of(const std::string& suffix);    // "game data" "friend card" "quest" "replay" "other"
bool is_dissidia012(const std::string& title);     // PARAM.SFO TITLE of a Dissidia 012 save
std::string one_line(const std::string& detail);   // newlines -> spaces

// Reads one save folder (PARAM.SFO must parse and the name must look like a save). `only_dissidia`: false accepts
// any game's save.
bool read_save(const std::string& dir, SaveInfo* out, bool only_dissidia = true);
// The saves directly inside `root`, sorted by folder name.
std::vector<SaveInfo> scan(const std::string& root, bool only_dissidia = true);

// SAVEDATA folders of other installs of the same kind, existing on this machine: PPSSPP (Linux, flatpak, macOS,
// Windows), PSP memory sticks mounted under /media, /run/media, /mnt (two levels) and Windows drive letters.
// `with_saves`: keep only the folders that hold a Dissidia 012 save (import sources); false lists all (export
// destinations). `exclude`: a folder never listed (the runtime's own).
std::vector<Source> detect_sources(bool with_saves, const std::string& exclude = "");

// Copies `s` into `dest_root/<new_disc><suffix>` with PARAM.SFO retargeted and signed again. An existing target is
// refused ("already exists") unless `replace`; then the old folder is moved to `backup_root/<name>.bak[N]`
// (`backup_root` empty = next to it in dest_root). Returns "" on success, else an error text. `dest_name` receives
// the folder name written.
std::string copy_save(const SaveInfo& s, const std::string& dest_root, const std::string& new_disc, bool replace,
                      std::string* dest_name, const std::string& backup_root = "");

// The file names that travel with the save folder `dir` (PARAM.SFO, the icons and the files PARAM.SFO lists) that
// exist on disk, sorted. Empty when PARAM.SFO is unreadable.
std::vector<std::string> save_files(const std::string& dir);

// DISC_ID of an install's disc0/PSP_GAME/PARAM.SFO ("" when unreadable).
std::string installed_disc_id(const std::string& workdir);

}  // namespace psp_savemgr
