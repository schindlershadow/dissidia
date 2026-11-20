// Installed texture packs: several can be installed side by side, one is applied (docs: games/dissidia/docs/texture-packs.md).
//
//   <mods>/texpacks/<id>/      one folder per pack (a PPSSPP textures.ini with its PNGs, or <id>.png files), written by the installers
//                              (tools/fetch_textures.py); .source.json there names it ("name") and dates it ("installed", Unix seconds)
//   <mods>/textures/           a pack installed by an older release: listed as one more pack (id "textures") until an installer moves it
//
// Which one is applied: the pack chosen in Settings (settings.ini texture_pack=<id>, texture_pack_at=<when it was chosen>), unless a pack was
// installed after that choice (a fresh install is applied); with no choice, the newest pack. Off is settings.ini hd_textures=0.
// Pure std::filesystem; no game knowledge.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct PspTexPack {
    std::string id;        // folder name under texpacks/ ("textures" for the old location)
    std::string name;      // what the menu shows (.source.json "name", else the id)
    std::string dir;       // its folder
    int64_t installed = 0; // Unix seconds (.source.json "installed", else the folder's time)
};

// The installed packs, oldest first.
std::vector<PspTexPack> psp_texpacks_list(const std::string& mods_dir);

// The pack to apply, given the saved choice (chosen_id may be empty; chosen_at = when it was chosen). nullptr-like: id empty when none.
PspTexPack psp_texpacks_pick(const std::vector<PspTexPack>& packs, const std::string& chosen_id, int64_t chosen_at);

// psp_texpacks_pick with the choice of this run (PSPRECOMP_TEXPACK_ID / PSPRECOMP_TEXPACK_AT, set from settings.ini at start). Its folder,
// or "" when no pack is installed.
std::string psp_texpacks_active_dir(const std::string& mods_dir);
std::string psp_texpacks_active_id(const std::string& mods_dir);

// Deletes an installed pack's folder. false (and `err`) when it is not one of the packs or could not be deleted.
bool psp_texpacks_remove(const std::string& mods_dir, const std::string& id, std::string* err);

// Installs a texture pack archive (the player's own download) as one more pack: a .zip (stored / deflate entries, read in pieces, so a
// pack of several GB needs little memory), or on a PC a .7z / .rar a tool on the PATH unpacks (7z / 7za / 7zz / bsdtar; Windows' tar.exe).
// The folder holding its textures.ini (any depth, the shallowest; __MACOSX skipped) is installed as it is; an archive of only id-named
// PNGs (<16 hex digits>.png) is installed flat. It goes to <mods>/texpacks/<id> (id from `display_name`, e.g. "My Pack v2 (US).zip" ->
// "my-pack-v2-us"; the same name again replaces that pack) with .source.json (name, installed now) and PACK_CREDITS.txt; a pack already
// installed is only replaced once the new one is complete. `progress` (0-100) is updated as it goes (may be null). Runs on the caller's
// thread (it can take minutes: call it from a worker). true: `name_out` = the pack's name; false: `err` says why, for the player.
bool psp_texpacks_install(const std::string& mods_dir, const std::string& archive, const std::string& display_name, std::atomic<int>* progress,
                          std::string* name_out, std::string* err);
// The folder name an archive's file name gets ("My Pack v2 (US).zip" -> "my-pack-v2-us"); tools/fetch_textures.py pack_id() does the same.
std::string psp_texpacks_id_for(const std::string& file_name);
