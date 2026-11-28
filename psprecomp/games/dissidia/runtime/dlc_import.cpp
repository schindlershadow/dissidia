// dlc_import.cpp - see dlc_import.h.
#include "dlc_import.h"

#include "../../../runtime/src/hle/psp_savemgr.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace dissidia_dlc {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

bool is_dlc_name(const std::string& n) {   // 8 hex digits + ".edat"
    if (n.size() != 13 || lower(n.substr(8)) != ".edat") return false;
    for (int i = 0; i < 8; i++)
        if (!std::isxdigit((unsigned char)n[size_t(i)])) return false;
    return true;
}

void walk(const fs::path& dir, int depth, std::vector<std::string>* out) {
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        const std::string name = it->path().filename().u8string();
        if (it->is_regular_file(e2)) {
            if (is_dlc_name(name)) out->push_back(it->path().u8string());
        } else if (depth < 4 && it->is_directory(e2) && !it->is_symlink(e2) && !name.empty() && name[0] != '.' && name != "__MACOSX") {
            walk(it->path(), depth + 1, out);
        }
    }
}

// The game folders Dissidia 012 and its DLC use on a PSP (language.bin names ULUS10566 first; the others are the
// other regions and the PSN release).
const char* const kIds[] = {"ULUS10566", "ULES01505", "ULJM05681", "NPJH50377", "NPUH10110", "ULUS10437"};

std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

std::vector<std::string> collect(const std::string& root) {
    std::vector<std::string> out;
    walk(fs::u8path(root), 0, &out);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Source> detect() {
    std::vector<Source> out;
    std::set<std::string> seen;
    for (const psp_savemgr::Source& s : psp_savemgr::detect_sources(false)) {
        const fs::path game = fs::u8path(s.path).parent_path() / "GAME";   // .../PSP/SAVEDATA -> .../PSP/GAME
        std::error_code ec;
        for (const char* id : kIds) {
            // the folder name is matched without case (a PPSSPP install on a case-sensitive disk may spell it either way)
            for (auto it = fs::directory_iterator(game, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
                std::error_code e2;
                if (!it->is_directory(e2) || lower(it->path().filename().u8string()) != lower(id)) continue;
                const std::vector<std::string> files = collect(it->path().u8string());
                if (files.empty()) continue;
                std::error_code e3;
                const std::string canon = fs::weakly_canonical(it->path(), e3).u8string();
                if (!seen.insert(canon).second) continue;
                Source d;
                d.label = s.label + " - " + it->path().filename().u8string();
                d.path = it->path().u8string();
                d.files = int(files.size());
                for (const std::string& f : files) { std::error_code e4; d.bytes += fs::file_size(fs::u8path(f), e4); }
                out.push_back(std::move(d));
            }
            ec.clear();
        }
    }
    return out;
}

Result import_folder(const std::string& src, const std::string& mods_dir) {
    Result r;
    std::error_code ec;
    if (src.empty() || !fs::is_directory(fs::u8path(src), ec)) { r.text = "Not installed: that is not a folder."; return r; }
    const std::vector<std::string> files = collect(src);
    if (files.empty()) {
        r.text = "Not installed: no DLC files found. Dissidia 012's DLC is a folder of files named like eac7bf53.edat (PSP/GAME/ULUS10566 on a memory stick or in PPSSPP).";
        return r;
    }
    const fs::path mod = fs::u8path(mods_dir) / kModFolder, dir = mod / "dlc";
    fs::create_directories(dir, ec);
    if (ec) { r.text = "Not installed: cannot create " + dir.u8string() + ": " + ec.message(); return r; }
    int done = 0;
    for (const std::string& f : files) {
        const fs::path from = fs::u8path(f);
        const fs::path to = dir / fs::u8path(lower(from.filename().u8string()));
        const fs::path tmp = dir / fs::u8path(".part");
        fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::rename(tmp, to, ec);
        if (ec) { fs::remove(tmp, ec); r.text = "Not installed: cannot copy " + from.filename().u8string() + ": " + ec.message(); return r; }
        std::error_code e2;
        r.bytes += fs::file_size(to, e2);
        ++done;
    }
    // everything now in the folder (earlier imports stay)
    int total = 0;
    unsigned long long total_bytes = 0;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || !is_dlc_name(it->path().filename().u8string())) continue;
        ++total;
        total_bytes += fs::file_size(it->path(), e2);
    }
    // mod.json: keep what is there (the on / off setting), else write it
    const fs::path js = mod / "mod.json";
    if (!fs::exists(js, ec)) {
        std::ofstream o(js, std::ios::binary | std::ios::trunc);
        o << "{\n  \"name\": \"Dissidia 012 DLC\",\n  \"display\": \"Dissidia 012 DLC\",\n  \"kind\": \"dlc\",\n  \"enabled\": true\n}\n";
        if (!o) { r.text = "Not installed: cannot write " + js.u8string(); return r; }
    }
    {
        std::ofstream o(mod / "status.txt", std::ios::binary | std::ios::trunc);
        o << total << " DLC file" << (total == 1 ? "" : "s") << " (" << (total_bytes >> 20) << " MB)\n";
    }
    r.ok = true;
    r.files = done;
    r.text = "Installed " + std::to_string(done) + " DLC file" + (done == 1 ? "" : "s") + " (" + std::to_string(r.bytes >> 20) + " MB) as the mod Dissidia 012 DLC. Takes effect the next time the game starts.";
    return r;
}

}  // namespace dissidia_dlc
