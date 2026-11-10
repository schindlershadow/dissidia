// savemgr: list, export and import Dissidia 012 save folders between regions.
//
// A PSP save is a folder `<DISC_ID><suffix>` (ULUS10566GameData00, ULES01505FCard03 ...) holding PARAM.SFO and the
// game's encrypted files. The files are the same for every region (the key and the format are shared), so moving a
// save to another region means giving the folder the other disc ID and keeping PARAM.SFO consistent:
// SAVEDATA_DIRECTORY is rewritten and PARAM.SFO is signed again (psp_savedata_crypto, same as PPSSPP's UpdateHash).
// The encrypted files are copied untouched. A real PSP checks PARAM.SFO against its own console key, which this
// cannot produce: use a tool such as Savegame Manager Deluxe to sign for that console.
//
//   savemgr list   [--from DIR]
//   savemgr export [--from DIR] --to-dir DIR [--id ULES01505] [NAME...]
//   savemgr import --from DIR [--workdir DIR | --to DIR] [--id ULUS10566] [--force] [NAME...]
//
// The logic is runtime/src/hle/psp_savemgr.cpp, shared with the game's in-game "Manage saves" screen.
// Build (from psprecomp/): g++ -O2 -std=c++17 -Iruntime/src/hle tools/savemgr/savemgr.cpp runtime/src/hle/psp_savemgr.cpp runtime/src/hle/psp_savedata_crypto.cpp -o savemgr

#include "psp_savemgr.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace psp_savemgr;

namespace {

[[noreturn]] void usage() {
    std::fputs(
        "savemgr: move Dissidia 012 saves between regions\n"
        "  savemgr list   [--from DIR]\n"
        "  savemgr export [--from DIR] --to-dir DIR [--id ULES01505] [NAME...]\n"
        "  savemgr import --from DIR [--workdir DIR | --to DIR] [--id ULUS10566] [--force] [NAME...]\n"
        "DIR is a SAVEDATA folder (a PSP memory stick's PSP/SAVEDATA, PPSSPP's, or <workdir>/ms0/PSP/SAVEDATA) or a\n"
        "folder holding save folders. --workdir (default $PWD) gives the install: its ms0/PSP/SAVEDATA is the default\n"
        "list source and import target, and its disc ID the default --id. NAME limits the saves (a folder name,\n"
        "e.g. ULES01505GameData00, or its suffix GameData00); none = all Dissidia 012 saves in DIR.\n"
        "The encrypted data is copied untouched; the folder gets the new disc ID and PARAM.SFO is signed again.\n", stderr);
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) usage();
    const std::string cmd = argv[1];
    std::string from, to_dir, id, workdir = ".";
    bool force = false;
    std::vector<std::string> names;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&](std::string& dst) { if (i + 1 >= argc) usage(); dst = argv[++i]; };
        if (a == "--from") val(from);
        else if (a == "--to-dir" || a == "--to") val(to_dir);
        else if (a == "--id") val(id);
        else if (a == "--workdir") val(workdir);
        else if (a == "--force") force = true;
        else if (!a.empty() && a[0] == '-') usage();
        else names.push_back(a);
    }
    if (cmd != "list" && cmd != "export" && cmd != "import") usage();
    if (!id.empty() && !valid_disc_id(id)) { std::fprintf(stderr, "savemgr: --id must look like ULUS10566\n"); return 2; }
    const fs::path ms = fs::path(workdir) / "ms0" / "PSP" / "SAVEDATA";
    if (from.empty()) {
        if (cmd == "import") { std::fprintf(stderr, "savemgr: import needs --from\n"); return 2; }
        from = ms.string();
    }
    // a folder that holds a save directly (has PARAM.SFO) is taken as one save
    std::vector<SaveInfo> saves;
    std::error_code ec;
    if (fs::is_regular_file(fs::path(from) / "PARAM.SFO", ec)) {
        SaveInfo s;
        if (!read_save(from, &s, false)) { std::fprintf(stderr, "savemgr: %s is not named like a save folder (ULUS10566GameData00) or its PARAM.SFO is not valid\n", from.c_str()); return 1; }
        saves.push_back(std::move(s));
    } else {
        if (!fs::is_directory(from, ec)) { std::fprintf(stderr, "savemgr: %s is not a folder\n", from.c_str()); return 1; }
        saves = scan(from);
    }
    if (!names.empty()) {
        std::vector<SaveInfo> keep;
        for (auto& s : saves)
            for (auto& n : names) if (s.name == n || s.suffix == n) { keep.push_back(s); break; }
        saves.swap(keep);
    }
    if (cmd == "list") {
        std::printf("%-28s %-7s %-12s %s\n", "FOLDER", "REGION", "KIND", "DETAILS");
        for (auto& s : saves)
            std::printf("%-28s %-7s %-12s %s\n", s.name.c_str(), region_of(s.disc), kind_of(s.suffix), one_line(s.detail).c_str());
        std::printf("%zu Dissidia 012 save(s) in %s\n", saves.size(), from.c_str());
        return 0;
    }
    if (saves.empty()) { std::fprintf(stderr, "savemgr: no matching Dissidia 012 saves in %s\n", from.c_str()); return 1; }
    fs::path dest_root;
    std::string target = id;
    if (cmd == "import") {
        dest_root = to_dir.empty() ? ms : fs::path(to_dir);
        if (target.empty()) target = installed_disc_id(workdir);
        if (target.empty()) { std::fprintf(stderr, "savemgr: cannot tell the installed game's disc ID (no %s/disc0/PSP_GAME/PARAM.SFO): pass --workdir or --id\n", workdir.c_str()); return 1; }
    } else {
        if (to_dir.empty()) { std::fprintf(stderr, "savemgr: export needs --to-dir\n"); return 2; }
        dest_root = to_dir;
    }
    int failed = 0;
    for (auto& s : saves) {
        const std::string new_disc = target.empty() ? s.disc : target;
        std::string dn;
        std::string err = copy_save(s, dest_root.string(), new_disc, force, &dn);
        if (err.rfind("already exists", 0) == 0) err = "already exists (use --force to replace; the old one is kept as " + dn + ".bak)";
        if (err.empty()) std::printf("%s -> %s\n", s.name.c_str(), (dest_root / dn).string().c_str());
        else { std::fprintf(stderr, "%s: %s\n", s.name.c_str(), err.c_str()); failed++; }
    }
    return failed ? 1 : 0;
}
