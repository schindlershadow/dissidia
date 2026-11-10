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
// Build (from psprecomp/): g++ -O2 -std=c++17 -Iruntime/src/hle tools/savemgr/savemgr.cpp runtime/src/hle/psp_savedata_crypto.cpp -o savemgr

#include "psp_savedata_crypto.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(f), {});
}
bool write_file(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return bool(f);
}
uint16_t g16(const Bytes& d, size_t o) { return o + 2 <= d.size() ? uint16_t(d[o] | d[o + 1] << 8) : 0; }
uint32_t g32(const Bytes& d, size_t o) { return o + 4 <= d.size() ? uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24) : 0; }
void p32(Bytes& d, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) d[o + i] = uint8_t(v >> (8 * i)); }

// ---- PARAM.SFO (read, and patch one string in place) ----
struct SfoEntry { std::string key; uint16_t fmt; uint32_t len, max, off; };
struct Sfo {
    Bytes d;
    std::vector<SfoEntry> e;
    uint32_t datatab = 0;
    bool parse() {
        if (d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4) != 0) return false;
        const uint32_t kt = g32(d, 8), n = g32(d, 16);
        datatab = g32(d, 12);
        if (n > 256 || 20 + 16 * size_t(n) > d.size() || kt > d.size() || datatab > d.size()) return false;
        for (uint32_t i = 0; i < n; i++) {
            const size_t b = 20 + 16 * size_t(i);
            const size_t ko = kt + g16(d, b);
            if (ko >= d.size()) return false;
            SfoEntry s;
            s.key = reinterpret_cast<const char*>(&d[ko]);   // keys are NUL-terminated inside the key table
            s.fmt = g16(d, b + 2); s.len = g32(d, b + 4); s.max = g32(d, b + 8); s.off = g32(d, b + 12);
            if (size_t(datatab) + s.off + s.max > d.size()) return false;
            e.push_back(s);
        }
        return true;
    }
    const SfoEntry* find(const char* k) const { for (auto& s : e) if (s.key == k) return &s; return nullptr; }
    std::string str(const char* k) const {
        const SfoEntry* s = find(k);
        if (!s) return "";
        const char* p = reinterpret_cast<const char*>(&d[datatab + s->off]);
        return std::string(p, strnlen(p, s->max));
    }
    // Overwrite a string value (UTF-8 formats 0x0204) in place; the table layout does not change.
    bool set_str(const char* k, const std::string& v) {
        const SfoEntry* s = find(k);
        if (!s || v.size() + 1 > s->max) return false;
        uint8_t* p = &d[datatab + s->off];
        std::memset(p, 0, s->max);
        std::memcpy(p, v.data(), v.size());
        const size_t idx = size_t(s - e.data());
        p32(d, 20 + 16 * idx + 4, uint32_t(v.size() + 1));
        return true;
    }
};

// ---- save folder naming ----
struct Id { std::string disc, suffix; };
bool split_name(const std::string& n, Id* out) {
    if (n.size() < 9) return false;
    for (int i = 0; i < 4; i++) if (!std::isupper(uint8_t(n[i]))) return false;
    for (int i = 4; i < 9; i++) if (!std::isdigit(uint8_t(n[i]))) return false;
    out->disc = n.substr(0, 9); out->suffix = n.substr(9);
    return true;
}
const char* region_of(const std::string& disc) {
    const std::string p = disc.substr(0, 4);
    if (p == "ULUS" || p == "NPUH" || p == "UCUS") return "US";
    if (p == "ULES" || p == "NPEH" || p == "UCES") return "EU";
    if (p == "ULJM" || p == "NPJH" || p == "ULJS" || p == "UCJS") return "JP";
    if (p == "ULAS" || p == "NPHH" || p == "UCAS") return "Asia";
    return "?";
}
bool valid_id(const std::string& s) { Id i; return s.size() == 9 && split_name(s, &i); }

struct Save {
    fs::path dir;
    std::string name;
    Id id;
    Sfo sfo;
    std::string title, detail;
};
std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(uint8_t(c))); return s; }

// Dissidia 012's saves: the title is "DISSIDIA 012 FINAL FANTASY" (disc: "DISSIDIA duodecim ..."); the first
// Dissidia's saves say "DISSIDIA FINAL FANTASY" and are left alone.
bool is_dissidia012(const std::string& title) {
    const std::string t = lower(title);
    return t.find("dissidia") != std::string::npos && (t.find("012") != std::string::npos || t.find("duodecim") != std::string::npos);
}

std::vector<Save> scan(const fs::path& root, bool only_dissidia = true) {
    std::vector<Save> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        Save s;
        s.dir = it->path();
        s.name = s.dir.filename().string();
        if (!split_name(s.name, &s.id)) continue;
        const fs::path sfo = s.dir / "PARAM.SFO";
        if (!fs::is_regular_file(sfo, ec)) continue;
        s.sfo.d = read_file(sfo);
        if (!s.sfo.parse()) continue;
        s.title = s.sfo.str("TITLE");
        s.detail = s.sfo.str("SAVEDATA_DETAIL");
        if (only_dissidia && !is_dissidia012(s.title)) continue;
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const Save& a, const Save& b) { return a.name < b.name; });
    return out;
}

const char* kind_of(const std::string& suffix) {
    if (suffix.rfind("GameData", 0) == 0) return "game data";
    if (suffix.rfind("FCard", 0) == 0) return "friend card";
    if (suffix.rfind("Quest", 0) == 0) return "quest";
    if (suffix.rfind("Replay", 0) == 0) return "replay";
    return "other";
}
std::string one_line(const std::string& detail) {
    std::string r;
    for (char c : detail) r += (c == '\n' || c == '\r') ? ' ' : c;
    while (r.find("  ") != std::string::npos) r.replace(r.find("  "), 2, " ");
    return r;
}

// PSP folders keep a few well-known extra files; the encrypted data files are the ones PARAM.SFO lists.
std::set<std::string> files_to_copy(const Save& s) {
    std::set<std::string> f = {"PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "ICON1.PNG", "PIC0.PNG", "PIC1.PNG", "SND0.AT3"};
    if (const SfoEntry* fl = s.sfo.find("SAVEDATA_FILE_LIST")) {
        const uint8_t* p = &s.sfo.d[s.sfo.datatab + fl->off];
        for (uint32_t o = 0; o + 32 <= fl->max; o += 32) {   // {char name[13]; u8 hash[16]; u8 pad[3]}
            if (!p[o]) continue;
            f.insert(std::string(reinterpret_cast<const char*>(p + o), strnlen(reinterpret_cast<const char*>(p + o), 13)));
        }
    }
    return f;
}

// Copy `s` into `dest_root/<new disc><suffix>`, with PARAM.SFO retargeted and signed again.
// Returns an error text, or "" on success.
std::string copy_retargeted(const Save& s, const fs::path& dest_root, const std::string& new_disc, bool force, std::string* dest_name) {
    const std::string name = new_disc + s.id.suffix;
    *dest_name = name;
    const fs::path dest = dest_root / name;
    std::error_code ec;
    if (fs::exists(dest, ec)) {
        if (!force) return "already exists (use --force to replace; the old one is kept as " + name + ".bak)";
        fs::path bak = dest_root / (name + ".bak");
        for (int i = 1; fs::exists(bak, ec); i++) bak = dest_root / (name + ".bak" + std::to_string(i));
        fs::rename(dest, bak, ec);
        if (ec) return "cannot move the old save aside: " + ec.message();
    }
    fs::create_directories(dest, ec);
    if (ec) return "cannot create " + dest.string() + ": " + ec.message();
    Sfo sfo = s.sfo;
    if (sfo.find("SAVEDATA_DIRECTORY") && !sfo.set_str("SAVEDATA_DIRECTORY", name)) return "PARAM.SFO has no room for the new folder name";
    // Sign again (the hash covers PARAM.SFO, including SAVEDATA_DIRECTORY). A copy that keeps its name stays
    // byte-identical, whatever signed it (a PSP's own console key included). The signature made here is
    // PPSSPP's: this runtime and PPSSPP accept it; a real PSP only accepts saves signed with its own key.
    const SfoEntry* sp = sfo.find("SAVEDATA_PARAMS");
    Bytes sfo_out = sfo.d;
    if (name != s.name && sp && sp->max == 128 && sfo_out.size() % 16 == 0) {
        const size_t off = sfo.datatab + sp->off;
        const int mode = psp_sdcrypto::crypt_mode_from_params_flag(sfo_out[off]);
        if (mode > 0 && psp_sdcrypto::update_sfo_hash(sfo_out, off, mode) != 0) return "could not sign PARAM.SFO";
    }
    for (const std::string& f : files_to_copy(s)) {
        const fs::path src = s.dir / f;
        if (!fs::is_regular_file(src, ec)) continue;
        if (f == "PARAM.SFO") { if (!write_file(dest / f, sfo_out)) return "cannot write PARAM.SFO"; continue; }
        fs::copy_file(src, dest / f, fs::copy_options::overwrite_existing, ec);
        if (ec) return "cannot copy " + f + ": " + ec.message();
    }
    return "";
}

std::string installed_disc_id(const fs::path& workdir) {
    const fs::path p = workdir / "disc0" / "PSP_GAME" / "PARAM.SFO";
    Sfo s;
    s.d = read_file(p);
    if (!s.parse()) return "";
    return s.str("DISC_ID");
}

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
    if (!id.empty() && !valid_id(id)) { std::fprintf(stderr, "savemgr: --id must look like ULUS10566\n"); return 2; }
    const fs::path ms = fs::path(workdir) / "ms0" / "PSP" / "SAVEDATA";
    if (from.empty()) {
        if (cmd == "import") { std::fprintf(stderr, "savemgr: import needs --from\n"); return 2; }
        from = ms.string();
    }
    // a folder that holds a save directly (has PARAM.SFO) is taken as one save
    fs::path src_root = from;
    std::vector<Save> saves;
    std::error_code ec;
    if (fs::is_regular_file(src_root / "PARAM.SFO", ec)) {
        Id sid;
        if (!split_name(src_root.filename().string(), &sid)) { std::fprintf(stderr, "savemgr: %s is not named like a save folder (ULUS10566GameData00)\n", from.c_str()); return 1; }
        Save s;
        s.dir = src_root; s.name = src_root.filename().string(); s.id = sid;
        s.sfo.d = read_file(src_root / "PARAM.SFO");
        if (!s.sfo.parse()) { std::fprintf(stderr, "savemgr: %s/PARAM.SFO is not valid\n", from.c_str()); return 1; }
        s.title = s.sfo.str("TITLE"); s.detail = s.sfo.str("SAVEDATA_DETAIL");
        saves.push_back(std::move(s));
    } else {
        if (!fs::is_directory(src_root, ec)) { std::fprintf(stderr, "savemgr: %s is not a folder\n", from.c_str()); return 1; }
        saves = scan(src_root);
    }
    if (!names.empty()) {
        std::vector<Save> keep;
        for (auto& s : saves)
            for (auto& n : names) if (s.name == n || s.id.suffix == n) { keep.push_back(s); break; }
        saves.swap(keep);
    }
    if (cmd == "list") {
        std::printf("%-28s %-7s %-12s %s\n", "FOLDER", "REGION", "KIND", "DETAILS");
        for (auto& s : saves)
            std::printf("%-28s %-7s %-12s %s\n", s.name.c_str(), region_of(s.id.disc), kind_of(s.id.suffix), one_line(s.detail).c_str());
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
        const std::string new_disc = target.empty() ? s.id.disc : target;
        std::string dn;
        const std::string err = copy_retargeted(s, dest_root, new_disc, force, &dn);
        if (err.empty()) std::printf("%s -> %s\n", s.name.c_str(), (dest_root / dn).string().c_str());
        else { std::fprintf(stderr, "%s: %s\n", s.name.c_str(), err.c_str()); failed++; }
    }
    return failed ? 1 : 0;
}
