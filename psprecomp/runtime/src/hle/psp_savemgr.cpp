#include "psp_savemgr.h"

#include "psp_savedata_crypto.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace psp_savemgr {
namespace {

fs::path P(const std::string& s) { return fs::u8path(s); }
std::string S(const fs::path& p) { return p.u8string(); }

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
size_t strnlen_(const char* p, size_t n) { size_t i = 0; while (i < n && p[i]) i++; return i; }

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
            s.key = std::string(reinterpret_cast<const char*>(&d[ko]), strnlen_(reinterpret_cast<const char*>(&d[ko]), d.size() - ko));
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
        return std::string(p, strnlen_(p, s->max));
    }
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

std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(uint8_t(c))); return s; }

// The data files PARAM.SFO lists, plus the well-known extras a PSP folder keeps.
std::set<std::string> files_to_copy(const Sfo& sfo) {
    std::set<std::string> f = {"PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "ICON1.PNG", "PIC0.PNG", "PIC1.PNG", "SND0.AT3"};
    if (const SfoEntry* fl = sfo.find("SAVEDATA_FILE_LIST")) {
        const uint8_t* p = &sfo.d[sfo.datatab + fl->off];
        for (uint32_t o = 0; o + 32 <= fl->max; o += 32) {   // {char name[13]; u8 hash[16]; u8 pad[3]}
            if (!p[o]) continue;
            f.insert(std::string(reinterpret_cast<const char*>(p + o), strnlen_(reinterpret_cast<const char*>(p + o), 13)));
        }
    }
    return f;
}

bool is_dir(const fs::path& p) { std::error_code ec; return fs::is_directory(p, ec); }

}  // namespace

bool split_name(const std::string& n, std::string* disc, std::string* suffix) {
    if (n.size() < 9) return false;
    for (int i = 0; i < 4; i++) if (!std::isupper(uint8_t(n[i]))) return false;
    for (int i = 4; i < 9; i++) if (!std::isdigit(uint8_t(n[i]))) return false;
    if (disc) *disc = n.substr(0, 9);
    if (suffix) *suffix = n.substr(9);
    return true;
}
bool valid_disc_id(const std::string& s) { return s.size() == 9 && split_name(s, nullptr, nullptr); }

const char* region_of(const std::string& disc) {
    const std::string p = disc.substr(0, 4);
    if (p == "ULUS" || p == "NPUH" || p == "UCUS") return "US";
    if (p == "ULES" || p == "NPEH" || p == "UCES") return "EU";
    if (p == "ULJM" || p == "NPJH" || p == "ULJS" || p == "UCJS") return "JP";
    if (p == "ULAS" || p == "NPHH" || p == "UCAS") return "Asia";
    return "?";
}
const char* kind_of(const std::string& suffix) {
    if (suffix.rfind("GameData", 0) == 0) return "game data";
    if (suffix.rfind("FCard", 0) == 0) return "friend card";
    if (suffix.rfind("Quest", 0) == 0) return "quest";
    if (suffix.rfind("Replay", 0) == 0) return "replay";
    return "other";
}
bool is_dissidia012(const std::string& title) {
    const std::string t = lower(title);
    return t.find("dissidia") != std::string::npos && (t.find("012") != std::string::npos || t.find("duodecim") != std::string::npos);
}
std::string one_line(const std::string& detail) {
    std::string r;
    for (char c : detail) r += (c == '\n' || c == '\r') ? ' ' : c;
    while (r.find("  ") != std::string::npos) r.replace(r.find("  "), 2, " ");
    return r;
}

bool read_save(const std::string& dir, SaveInfo* out, bool only_dissidia) {
    const fs::path d = P(dir);
    SaveInfo s;
    std::string n = S(d.filename());
    if (n.empty()) n = S(d.parent_path().filename());   // a trailing slash
    if (!split_name(n, &s.disc, &s.suffix)) return false;
    s.name = n;
    s.path = dir;
    std::error_code ec;
    const fs::path sfop = d / "PARAM.SFO";
    if (!fs::is_regular_file(sfop, ec)) return false;
    Sfo sfo;
    sfo.d = read_file(sfop);
    if (!sfo.parse()) return false;
    s.title = sfo.str("TITLE");
    s.savetitle = sfo.str("SAVEDATA_TITLE");
    s.detail = sfo.str("SAVEDATA_DETAIL");
    if (only_dissidia && !is_dissidia012(s.title)) return false;
    struct stat st;
    if (::stat(sfop.string().c_str(), &st) == 0) s.mtime = st.st_mtime;
    for (const std::string& f : files_to_copy(sfo)) {
        const auto sz = fs::file_size(d / f, ec);
        if (!ec) s.bytes += sz;
        ec.clear();
    }
    *out = std::move(s);
    return true;
}

std::vector<SaveInfo> scan(const std::string& root, bool only_dissidia) {
    std::vector<SaveInfo> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(P(root), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        SaveInfo s;
        if (read_save(S(it->path()), &s, only_dissidia)) out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const SaveInfo& a, const SaveInfo& b) { return a.name < b.name; });
    return out;
}

std::vector<Source> detect_sources(bool with_saves, const std::string& exclude) {
    std::vector<Source> cand;
    auto add = [&](const std::string& label, const fs::path& p) { cand.push_back({label, S(p)}); };
    auto env = [](const char* k) -> std::string { const char* v = std::getenv(k); return v ? v : ""; };
    // direct children of `dir` (one directory level), as PSP/SAVEDATA candidates
    auto children = [](const fs::path& dir) {
        std::vector<fs::path> r;
        std::error_code ec;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->is_directory(ec)) r.push_back(it->path());
        std::sort(r.begin(), r.end());
        return r;
    };
#ifdef _WIN32
    const std::string home = env("USERPROFILE"), appdata = env("APPDATA");
    if (!home.empty()) add("PPSSPP", P(home) / "Documents" / "PPSSPP" / "PSP" / "SAVEDATA");
    if (!home.empty()) add("PPSSPP", P(home) / "Documents" / "PPSSPP" / "memstick" / "PSP" / "SAVEDATA");
    if (!appdata.empty()) add("PPSSPP", P(appdata) / "PPSSPP" / "PSP" / "SAVEDATA");
    const DWORD drives = GetLogicalDrives();
    for (int i = 2; i < 26; i++)   // C: and up (A:/B: are floppies and can stall)
        if (drives & (1u << i)) {
            std::string root(1, char('A' + i));
            root += ":\\";
            add("Memory stick (" + root.substr(0, 2) + ")", P(root) / "PSP" / "SAVEDATA");
        }
#else
    const std::string home = env("HOME");
    const std::string xdg = env("XDG_CONFIG_HOME");
    if (!xdg.empty()) add("PPSSPP", P(xdg) / "ppsspp" / "PSP" / "SAVEDATA");
    if (!home.empty()) {
        add("PPSSPP", P(home) / ".config" / "ppsspp" / "PSP" / "SAVEDATA");
        add("PPSSPP (Flatpak)", P(home) / ".var" / "app" / "org.ppsspp.PPSSPP" / "config" / "ppsspp" / "PSP" / "SAVEDATA");
        add("PPSSPP", P(home) / "Library" / "Application Support" / "PPSSPP" / "PSP" / "SAVEDATA");
        add("PPSSPP", P(home) / "Documents" / "PPSSPP" / "PSP" / "SAVEDATA");
        add("PPSSPP (Android backup)", P(home) / "PSP" / "SAVEDATA");
    }
    const std::string user = env("USER");
    std::vector<fs::path> mounts = {P("/media/" + user), P("/run/media/" + user), P("/run/media"), P("/media")};
    for (const fs::path& m : mounts)
        for (const fs::path& c : children(m)) {
            add("Memory stick (" + S(c) + ")", c / "PSP" / "SAVEDATA");
            for (const fs::path& c2 : children(c)) add("Memory stick (" + S(c2) + ")", c2 / "PSP" / "SAVEDATA");
        }
    for (const fs::path& c : children(P("/mnt"))) {
        add("Disk (" + S(c) + ")", c / "PSP" / "SAVEDATA");
        for (const fs::path& c2 : children(c)) add("Disk (" + S(c2) + ")", c2 / "PSP" / "SAVEDATA");
    }
#endif
    std::vector<Source> out;
    std::set<std::string> seen;
    std::error_code ec;
    const std::string ex = exclude.empty() ? "" : S(fs::weakly_canonical(P(exclude), ec));
    for (auto& c : cand) {
        if (!is_dir(P(c.path))) continue;
        ec.clear();
        const std::string canon = S(fs::weakly_canonical(P(c.path), ec));
        if (!seen.insert(canon).second || (!ex.empty() && canon == ex)) continue;
        if (with_saves && scan(c.path).empty()) continue;
        out.push_back(c);
    }
    return out;
}

std::string copy_save(const SaveInfo& s, const std::string& dest_root_s, const std::string& new_disc, bool replace,
                      std::string* dest_name, const std::string& backup_root_s) {
    const fs::path dest_root = P(dest_root_s);
    const std::string name = new_disc + s.suffix;
    if (dest_name) *dest_name = name;
    const fs::path dest = dest_root / P(name);
    std::error_code ec;
    if (fs::exists(dest, ec)) {
        if (!replace) return "already exists (the old one is kept as " + name + ".bak when replaced)";
        const fs::path broot = backup_root_s.empty() ? dest_root : P(backup_root_s);
        fs::create_directories(broot, ec);
        fs::path bak = broot / P(name + ".bak");
        for (int i = 1; fs::exists(bak, ec); i++) bak = broot / P(name + ".bak" + std::to_string(i));
        fs::rename(dest, bak, ec);
        if (ec) return "cannot move the old save aside: " + ec.message();
    }
    fs::create_directories(dest, ec);
    if (ec) return "cannot create " + S(dest) + ": " + ec.message();
    const fs::path src = P(s.path);
    Sfo sfo;
    sfo.d = read_file(src / "PARAM.SFO");
    if (!sfo.parse()) return "PARAM.SFO is not valid";
    const Sfo orig = sfo;
    if (sfo.find("SAVEDATA_DIRECTORY") && !sfo.set_str("SAVEDATA_DIRECTORY", name)) return "PARAM.SFO has no room for the new folder name";
    // Sign again (the hash covers PARAM.SFO, including SAVEDATA_DIRECTORY). A copy that keeps its name stays
    // byte-identical, whatever signed it (a PSP's own console key included).
    const SfoEntry* sp = sfo.find("SAVEDATA_PARAMS");
    Bytes sfo_out = sfo.d;
    if (name != s.name && sp && sp->max == 128 && sfo_out.size() % 16 == 0) {
        const size_t off = sfo.datatab + sp->off;
        const int mode = psp_sdcrypto::crypt_mode_from_params_flag(sfo_out[off]);
        if (mode > 0 && psp_sdcrypto::update_sfo_hash(sfo_out, off, mode) != 0) return "could not sign PARAM.SFO";
    }
    for (const std::string& f : files_to_copy(orig)) {
        const fs::path sf = src / P(f);
        if (!fs::is_regular_file(sf, ec)) continue;
        if (f == "PARAM.SFO") { if (!write_file(dest / "PARAM.SFO", sfo_out)) return "cannot write PARAM.SFO"; continue; }
        fs::copy_file(sf, dest / P(f), fs::copy_options::overwrite_existing, ec);
        if (ec) return "cannot copy " + f + ": " + ec.message();
    }
    return "";
}

std::string installed_disc_id(const std::string& workdir) {
    Sfo s;
    s.d = read_file(P(workdir) / "disc0" / "PSP_GAME" / "PARAM.SFO");
    if (!s.parse()) return "";
    return s.str("DISC_ID");
}

}  // namespace psp_savemgr
