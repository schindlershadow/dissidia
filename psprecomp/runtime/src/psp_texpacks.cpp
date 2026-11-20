#include "psp_texpacks.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <zlib.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string read_small(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    if (f) s << f.rdbuf();
    std::string t = s.str();
    if (t.size() > 65536) t.resize(65536);
    return t;
}

// The value of a top-level "key": "string" / "key": number in .source.json (written by tools/fetch_textures.py; flat, no nesting).
bool json_field(const std::string& j, const char* key, std::string* str, int64_t* num) {
    const std::string k = std::string("\"") + key + "\"";
    size_t at = j.find(k);
    if (at == std::string::npos) return false;
    at = j.find(':', at + k.size());
    if (at == std::string::npos) return false;
    at++;
    while (at < j.size() && std::isspace(static_cast<unsigned char>(j[at]))) at++;
    if (at >= j.size()) return false;
    if (j[at] == '"') {
        std::string v;
        for (size_t i = at + 1; i < j.size() && j[i] != '"'; i++) {
            if (j[i] == '\\' && i + 1 < j.size()) i++;
            v += j[i];
        }
        if (str) *str = v;
        return true;
    }
    if (num) *num = std::strtoll(j.c_str() + at, nullptr, 10);
    return true;
}

// A folder that holds a texture pack: a textures.ini, a .source.json, or at least one <16 hex digits>.png.
bool is_pack(const fs::path& d) {
    std::error_code ec;
    if (fs::is_regular_file(d / "textures.ini", ec) || fs::is_regular_file(d / ".source.json", ec)) return true;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.size() == 20 && n.compare(16, 4, ".png") == 0 &&
            std::all_of(n.begin(), n.begin() + 16, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
            return true;
    }
    return false;
}

PspTexPack describe(const fs::path& d, const std::string& id) {
    PspTexPack p;
    p.id = id;
    p.dir = d.string();
    p.name = id == "textures" ? "Installed pack" : id;
    const std::string j = read_small(d / ".source.json");
    std::string name;
    int64_t when = 0;
    if (json_field(j, "name", &name, nullptr) && !name.empty()) p.name = name;
    else if (json_field(j, "archive", &name, nullptr) && !name.empty()) p.name = name;   // packs installed before names were written
    else {
        std::string kind;
        if (json_field(j, "pack", &kind, nullptr) && kind == "ai") p.name = "Dissidia Recomp HD";
    }
    if (json_field(j, "installed", nullptr, &when) && when > 0) p.installed = when;
    else {
        std::error_code ec;
        const auto t = fs::last_write_time(d, ec);
        if (!ec)
            p.installed = std::chrono::duration_cast<std::chrono::seconds>(
                              t - fs::file_time_type::clock::now() + std::chrono::system_clock::now().time_since_epoch()).count();
    }
    return p;
}

int64_t env_i64(const char* k) {
    const char* e = std::getenv(k);
    return e && e[0] ? std::strtoll(e, nullptr, 10) : 0;
}

}  // namespace

std::vector<PspTexPack> psp_texpacks_list(const std::string& mods_dir) {
    std::vector<PspTexPack> v;
    if (mods_dir.empty()) return v;
    std::error_code ec;
    const fs::path root = fs::u8path(mods_dir);
    for (fs::directory_iterator it(root / "texpacks", ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const std::string id = it->path().filename().string();
        if (id.empty() || id[0] == '.' || !it->is_directory(e2) || !is_pack(it->path())) continue;   // .new / .unpack-*: an install in progress
        v.push_back(describe(it->path(), id));
    }
    if (fs::is_directory(root / "textures", ec) && is_pack(root / "textures")) v.push_back(describe(root / "textures", "textures"));
    std::sort(v.begin(), v.end(), [](const PspTexPack& a, const PspTexPack& b) {
        return a.installed != b.installed ? a.installed < b.installed : a.id < b.id;
    });
    return v;
}

PspTexPack psp_texpacks_pick(const std::vector<PspTexPack>& packs, const std::string& chosen_id, int64_t chosen_at) {
    if (packs.empty()) return {};
    const PspTexPack& newest = packs.back();
    for (const PspTexPack& p : packs)
        if (!chosen_id.empty() && p.id == chosen_id)
            return newest.installed > chosen_at && newest.id != p.id ? newest : p;   // installed after the choice: the new one
    return newest;
}

std::string psp_texpacks_active_id(const std::string& mods_dir) {
    const char* id = std::getenv("PSPRECOMP_TEXPACK_ID");
    return psp_texpacks_pick(psp_texpacks_list(mods_dir), id ? id : "", env_i64("PSPRECOMP_TEXPACK_AT")).id;
}

std::string psp_texpacks_active_dir(const std::string& mods_dir) {
    const char* id = std::getenv("PSPRECOMP_TEXPACK_ID");
    return psp_texpacks_pick(psp_texpacks_list(mods_dir), id ? id : "", env_i64("PSPRECOMP_TEXPACK_AT")).dir;
}

bool psp_texpacks_remove(const std::string& mods_dir, const std::string& id, std::string* err) {
    for (const PspTexPack& p : psp_texpacks_list(mods_dir)) {
        if (p.id != id) continue;
        std::error_code ec;
        fs::remove_all(fs::u8path(p.dir), ec);
        if (ec) {
            if (err) *err = ec.message();
            return false;
        }
        return true;
    }
    if (err) *err = "no such texture pack";
    return false;
}

// ---- installing an archive ------------------------------------------------------------------------------------------------------------
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool is_id_png(const std::string& base) {
    if (base.size() != 20 || lower(base.substr(16)) != ".png") return false;
    return std::all_of(base.begin(), base.begin() + 16, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}
std::string json_str(const std::string& v) {
    std::string o = "\"";
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) o += ' ';
        else o += char(c);
    }
    return o + "\"";
}
int64_t now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The archive's entries that make up the pack: (name in the archive, path inside the pack). Empty when it holds no pack.
std::vector<std::pair<std::string, std::string>> pack_files(const std::vector<std::string>& names) {
    std::string root;
    bool have_root = false;
    size_t depth = 0;
    for (const std::string& n : names) {   // the shallowest folder with a textures.ini
        if (n.find("__MACOSX/") != std::string::npos) continue;
        const size_t sl = n.find_last_of('/');
        const std::string base = sl == std::string::npos ? n : n.substr(sl + 1);
        if (lower(base) != "textures.ini") continue;
        const size_t d = size_t(std::count(n.begin(), n.end(), '/'));
        if (!have_root || d < depth) { root = n.substr(0, n.size() - base.size()); depth = d; have_root = true; }
    }
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& n : names) {
        if (n.empty() || n.back() == '/' || n[0] == '/' || n.find("__MACOSX/") != std::string::npos) continue;
        bool bad = false;   // nothing outside the pack's folder ("..", a drive letter)
        size_t i = 0;
        while (i <= n.size()) {
            size_t j = n.find('/', i);
            if (j == std::string::npos) j = n.size();
            const std::string part = n.substr(i, j - i);
            if (part == ".." || part.find(':') != std::string::npos) bad = true;
            i = j + 1;
        }
        if (bad) continue;
        if (have_root) {
            if (n.compare(0, root.size(), root) == 0) out.push_back({n, n.substr(root.size())});
        } else {
            const size_t sl = n.find_last_of('/');
            const std::string base = sl == std::string::npos ? n : n.substr(sl + 1);
            if (is_id_png(base)) out.push_back({n, lower(base)});
        }
    }
    return out;
}

const char* kNoPack = "the archive has no textures.ini (and no PNG files named by texture ids): it is not a texture pack the game can use";

// ---- .zip, read in place
struct ZipEntry {
    std::string name;
    uint16_t method = 0, flags = 0;
    uint64_t csize = 0, usize = 0, local = 0;
};
uint16_t le16(const unsigned char* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const unsigned char* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
uint64_t le64(const unsigned char* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }

struct File {
    std::FILE* f = nullptr;
    explicit File(const std::string& p, const char* mode) {
#ifdef _WIN32
        f = _wfopen(fs::u8path(p).wstring().c_str(), std::wstring(mode, mode + std::strlen(mode)).c_str());
#else
        f = std::fopen(p.c_str(), mode);
#endif
    }
    ~File() { if (f) std::fclose(f); }
    bool seek(uint64_t o) {
#ifdef _WIN32
        return _fseeki64(f, int64_t(o), SEEK_SET) == 0;
#else
        return fseeko(f, off_t(o), SEEK_SET) == 0;
#endif
    }
    bool read(void* b, size_t n) { return std::fread(b, 1, n, f) == n; }
};

std::string zip_list(File& z, uint64_t size, std::vector<ZipEntry>* out) {
    const uint64_t tail = std::min<uint64_t>(size, 65557);
    const size_t tail_n = static_cast<size_t>(tail);
    std::vector<unsigned char> t(tail_n);
    if (!z.seek(size - tail) || !z.read(t.data(), t.size())) return "the .zip could not be read";
    int64_t eocd = -1;
    for (int64_t i = int64_t(tail) - 22; i >= 0; i--)
        if (le32(&t[size_t(i)]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) return "this is not a .zip file, or it is damaged";
    uint64_t count = le16(&t[size_t(eocd) + 10]), cd_size = le32(&t[size_t(eocd) + 12]), cd_off = le32(&t[size_t(eocd) + 16]);
    if ((cd_off == 0xFFFFFFFFu || count == 0xFFFF) && eocd >= 20 && le32(&t[size_t(eocd) - 20]) == 0x07064b50) {   // Zip64
        const uint64_t z64 = le64(&t[size_t(eocd) - 20 + 8]);
        unsigned char r[56];
        if (!z.seek(z64) || !z.read(r, sizeof r) || le32(r) != 0x06064b50) return "the .zip (Zip64) could not be read";
        count = le64(r + 32);
        cd_size = le64(r + 40);
        cd_off = le64(r + 48);
    }
    if (cd_off + cd_size > size || count > 2000000) return "the .zip is damaged";
    std::vector<unsigned char> cd(static_cast<size_t>(cd_size));
    if (!z.seek(cd_off) || !z.read(cd.data(), cd.size())) return "the .zip could not be read";
    size_t p = 0;
    for (uint64_t k = 0; k < count; k++) {
        if (p + 46 > cd.size() || le32(&cd[p]) != 0x02014b50) return "the .zip is damaged";
        ZipEntry e;
        e.flags = le16(&cd[p + 8]);
        e.method = le16(&cd[p + 10]);
        e.csize = le32(&cd[p + 20]);
        e.usize = le32(&cd[p + 24]);
        const size_t nl = le16(&cd[p + 28]), xl = le16(&cd[p + 30]), cl = le16(&cd[p + 32]);
        e.local = le32(&cd[p + 42]);
        if (p + 46 + nl + xl + cl > cd.size()) return "the .zip is damaged";
        e.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nl);
        for (char& c : e.name) if (c == '\\') c = '/';
        for (size_t x = p + 46 + nl; x + 4 <= p + 46 + nl + xl;) {   // Zip64 sizes / offset
            const uint16_t id = le16(&cd[x]), len = le16(&cd[x + 2]);
            if (id == 1) {
                size_t q = x + 4;
                if (e.usize == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.usize = le64(&cd[q]); q += 8; }
                if (e.csize == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.csize = le64(&cd[q]); q += 8; }
                if (e.local == 0xFFFFFFFFu && q + 8 <= x + 4 + len) e.local = le64(&cd[q]);
            }
            x += 4 + len;
        }
        out->push_back(e);
        p += 46 + nl + xl + cl;
    }
    return "";
}

std::string zip_extract(File& z, const ZipEntry& e, const fs::path& to) {
    unsigned char lh[30];
    if (!z.seek(e.local) || !z.read(lh, sizeof lh) || le32(lh) != 0x04034b50) return "the .zip is damaged";
    if (!z.seek(e.local + 30 + le16(lh + 26) + le16(lh + 28))) return "the .zip is damaged";
    if (e.flags & 1) return "the .zip is password protected";
    if (e.method != 0 && e.method != 8) return "the .zip uses a compression the game can not unpack (only stored / deflate): unpack it and zip it again";
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    File out(to.u8string(), "wb");
    if (!out.f) return "could not write " + to.u8string();
    std::vector<unsigned char> in(1 << 16), buf(1 << 16);
    uint64_t left = e.csize;
    if (e.method == 0) {
        while (left) {
            const size_t n = size_t(std::min<uint64_t>(left, in.size()));
            if (!z.read(in.data(), n) || std::fwrite(in.data(), 1, n, out.f) != n) return "could not copy " + e.name;
            left -= n;
        }
        return "";
    }
    z_stream s{};
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return "zlib failed";
    int r = Z_OK;
    while (r != Z_STREAM_END) {
        if (s.avail_in == 0) {
            if (!left) break;
            const size_t n = size_t(std::min<uint64_t>(left, in.size()));
            if (!z.read(in.data(), n)) { inflateEnd(&s); return "the .zip is damaged"; }
            left -= n;
            s.next_in = in.data();
            s.avail_in = uInt(n);
        }
        s.next_out = buf.data();
        s.avail_out = uInt(buf.size());
        r = inflate(&s, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) { inflateEnd(&s); return "the .zip is damaged (" + e.name + ")"; }
        const size_t got = buf.size() - s.avail_out;
        if (got && std::fwrite(buf.data(), 1, got, out.f) != got) { inflateEnd(&s); return "could not write " + to.u8string(); }
    }
    inflateEnd(&s);
    return r == Z_STREAM_END ? "" : "the .zip is damaged (" + e.name + ")";
}

// ---- .7z / .rar on a PC: a tool unpacks it into a scratch folder
std::string find_tool(const char* name) {
    const char* path = std::getenv("PATH");
    if (!path) return "";
#ifdef _WIN32
    const char sep = ';';
    const std::string exe = ".exe";
#else
    const char sep = ':';
    const std::string exe;
#endif
    std::string s = path;
    for (size_t i = 0; i <= s.size();) {
        size_t j = s.find(sep, i);
        if (j == std::string::npos) j = s.size();
        if (j > i) {
            std::error_code ec;
            const fs::path c = fs::u8path(s.substr(i, j - i)) / (std::string(name) + exe);
            if (fs::is_regular_file(c, ec)) return c.u8string();
        }
        i = j + 1;
    }
    return "";
}
std::string quote(const std::string& a) {
#ifdef _WIN32
    return "\"" + a + "\"";
#else
    std::string o = "'";
    for (char c : a) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
#endif
}
std::string run_tool(const std::string& archive, const fs::path& dest) {
#ifdef __ANDROID__
    (void)archive; (void)dest;
    return "a .7z or .rar pack can not be unpacked on Android: unpack it on a PC, zip the folder again and choose the .zip";
#else
    std::string cmd;
    for (const char* t : {"7z", "7za", "7zz"}) {
        const std::string exe = find_tool(t);
        if (!exe.empty()) { cmd = quote(exe) + " x -y -bd " + quote("-o" + dest.u8string()) + " " + quote(archive); break; }
    }
    if (cmd.empty()) {
        std::string exe = find_tool("bsdtar");
#ifdef _WIN32
        if (exe.empty()) exe = find_tool("tar");   // Windows 10+: bsdtar, reads .7z and .rar
#endif
        if (!exe.empty()) cmd = quote(exe) + " -x -f " + quote(archive) + " -C " + quote(dest.u8string());
    }
    if (cmd.empty()) return "a .7z or .rar pack needs 7-Zip (7-zip.org) to be unpacked: install it, or unpack the pack, zip the folder again and choose the .zip";
#ifdef _WIN32
    cmd = "\"" + cmd + " >NUL 2>&1\"";
#else
    cmd += " >/dev/null 2>&1";
#endif
    const int rc = std::system(cmd.c_str());
    return rc == 0 ? "" : "the archive could not be unpacked (the unpacking tool failed)";
#endif
}

}  // namespace

std::string psp_texpacks_id_for(const std::string& file_name) {
    std::string base = file_name;
    const size_t sl = base.find_last_of("/\\");
    if (sl != std::string::npos) base = base.substr(sl + 1);
    for (const char* ext : {".zip", ".7z", ".rar"}) {
        const size_t n = std::strlen(ext);
        if (base.size() > n && lower(base.substr(base.size() - n)) == ext) { base.resize(base.size() - n); break; }
    }
    std::string id;
    for (char c : lower(base)) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) id += c;
        else if (!id.empty() && id.back() != '-') id += '-';
    }
    if (id.size() > 48) id.resize(48);
    while (!id.empty() && id.back() == '-') id.pop_back();
    return id.empty() || id == "recomp-hd" || id == "textures" ? "my-pack" : id;
}

bool psp_texpacks_install(const std::string& mods_dir, const std::string& archive, const std::string& display_name, std::atomic<int>* progress,
                          std::string* name_out, std::string* err) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    auto set = [&](int p) { if (progress) progress->store(p); };
    if (mods_dir.empty()) return fail("no mods folder");
    std::string shown = display_name.empty() ? fs::u8path(archive).filename().u8string() : display_name;
    std::string name = shown;
    for (const char* ext : {".zip", ".7z", ".rar", ".ZIP", ".7Z", ".RAR"}) {
        const size_t n = std::strlen(ext);
        if (name.size() > n && name.compare(name.size() - n, n, ext) == 0) { name.resize(name.size() - n); break; }
    }
    const std::string id = psp_texpacks_id_for(shown);
    const fs::path packs = fs::u8path(mods_dir) / "texpacks";
    const fs::path tmp = packs / (".new-" + id), dest = packs / id;
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) return fail("could not create " + tmp.u8string() + ": " + ec.message());
    set(0);
    File a(archive, "rb");
    if (!a.f) { fs::remove_all(tmp, ec); return fail("the file could not be opened"); }
    unsigned char head[8] = {};
    const bool head_ok = a.read(head, sizeof head);
    const bool zip = head_ok && head[0] == 'P' && head[1] == 'K';
    const bool sevenz = head_ok && std::memcmp(head, "7z\xBC\xAF\x27\x1C", 6) == 0, rar = head_ok && std::memcmp(head, "Rar!", 4) == 0;
    size_t files = 0;
    std::string why;
    if (zip) {
        std::vector<ZipEntry> entries;
        why = zip_list(a, uint64_t(fs::file_size(fs::u8path(archive), ec)), &entries);
        if (why.empty()) {
            std::vector<std::string> names;
            for (const ZipEntry& e : entries) names.push_back(e.name);
            const auto want = pack_files(names);
            if (want.empty()) why = kNoPack;
            std::map<std::string, const ZipEntry*> by_name;
            for (const ZipEntry& e : entries) by_name[e.name] = &e;
            uint64_t total = 0, done = 0;
            for (const auto& w : want) total += by_name[w.first]->usize;
            for (const auto& w : want) {
                if (!why.empty()) break;
                const ZipEntry& e = *by_name[w.first];
                why = zip_extract(a, e, tmp / fs::u8path(w.second));
                done += e.usize;
                files++;
                set(int(total ? done * 99 / total : 99));
            }
        }
    } else if (sevenz || rar) {
        std::fclose(a.f);
        a.f = nullptr;
        const fs::path raw = packs / (".unpack-" + id);
        fs::remove_all(raw, ec);
        fs::create_directories(raw, ec);
        set(5);
        why = run_tool(archive, raw);
        if (why.empty()) {
            std::vector<std::string> names;
            for (fs::recursive_directory_iterator it(raw, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec)) names.push_back(fs::relative(it->path(), raw, ec).generic_u8string());
            const auto want = pack_files(names);
            if (want.empty()) why = kNoPack;
            for (const auto& w : want) {
                const fs::path to = tmp / fs::u8path(w.second);
                fs::create_directories(to.parent_path(), ec);
                fs::rename(raw / fs::u8path(w.first), to, ec);
                if (ec) { why = "could not move " + w.first + ": " + ec.message(); break; }
                files++;
            }
        }
        fs::remove_all(raw, ec);
    } else {
        why = "this is not a .zip, .7z or .rar file";
    }
    if (!why.empty()) { fs::remove_all(tmp, ec); return fail(why); }
    {
        std::ofstream j(tmp / ".source.json", std::ios::binary);
        j << "{\"pack\": \"file\", \"name\": " << json_str(name) << ", \"installed\": " << now_s() << ", \"archive\": " << json_str(shown)
          << ", \"files\": " << files << ", \"by\": \"settings\"}";
        std::ofstream c(tmp / "PACK_CREDITS.txt", std::ios::binary);
        c << "HD textures: your own texture pack, installed from " << shown << ".\n"
          << "Its own credits and license, if it came with any, are in this folder. The artwork belongs to Square Enix.\n";
    }
    fs::remove_all(dest, ec);   // the same pack again: replaced, now that the new copy is complete
    fs::rename(tmp, dest, ec);
    if (ec) { fs::remove_all(tmp, ec); return fail("could not put the pack in place: " + ec.message()); }
    set(100);
    if (name_out) *name_out = name;
    return true;
}

bool psp_texpacks_install_parts(const std::string& mods_dir, const std::string& id, const std::string& name, const std::string& kind,
                                const std::string& credits, const std::vector<PspTexPackPart>& parts, std::atomic<int>* progress,
                                int pct_from, int pct_to, std::string* err) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    const fs::path packs = fs::u8path(mods_dir) / "texpacks";
    const fs::path tmp = packs / (".new-" + id), dest = packs / id;
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) return fail("could not create " + tmp.u8string() + ": " + ec.message());
    struct Job { File* f; const ZipEntry* e; fs::path to; };
    std::vector<std::unique_ptr<File>> files;
    std::vector<std::vector<ZipEntry>> lists(parts.size());
    std::vector<Job> jobs;
    uint64_t total = 0;
    for (size_t k = 0; k < parts.size(); k++) {
        const PspTexPackPart& part = parts[k];
        files.push_back(std::make_unique<File>(part.zip, "rb"));
        File& z = *files.back();
        if (!z.f) { fs::remove_all(tmp, ec); return fail("could not open " + part.zip); }
        const std::string why = zip_list(z, uint64_t(fs::file_size(fs::u8path(part.zip), ec)), &lists[k]);
        if (!why.empty()) { fs::remove_all(tmp, ec); return fail(why + " (" + fs::u8path(part.zip).filename().u8string() + ")"); }
        std::map<std::string, const ZipEntry*> by_name;
        std::vector<std::string> names;
        for (const ZipEntry& e : lists[k]) { by_name[e.name] = &e; names.push_back(e.name); }
        std::vector<std::pair<std::string, std::string>> want;
        if (!part.ids_flat) {
            for (const auto& w : pack_files(names)) {
                const std::string low = lower(w.second);
                bool skip = w.second.empty() || w.second[0] == '.' || low.compare(0, 8, ".github/") == 0;   // repository files
                for (const std::string& suf : part.skip_suffixes)
                    if (low.size() >= suf.size() && low.compare(low.size() - suf.size(), suf.size(), suf) == 0) skip = true;
                if (!skip) want.push_back(w);
            }
        } else {
            for (const std::string& n : names) {
                const size_t sl = n.find_last_of('/');
                const std::string base = sl == std::string::npos ? n : n.substr(sl + 1);
                if (is_id_png(base) && ("/" + n).find("/textures/") != std::string::npos && n.find("..") == std::string::npos)
                    want.push_back({n, lower(base)});
            }
        }
        for (const auto& w : want) {
            jobs.push_back({&z, by_name[w.first], tmp / fs::u8path(w.second)});
            total += by_name[w.first]->usize;
        }
    }
    uint64_t done = 0;
    for (const Job& j : jobs) {
        const std::string why = zip_extract(*j.f, *j.e, j.to);
        if (!why.empty()) { fs::remove_all(tmp, ec); return fail(why); }
        done += j.e->usize;
        if (progress) progress->store(pct_from + int(total ? done * uint64_t(pct_to - pct_from) / total : 0));
    }
    files.clear();
    if (!fs::is_regular_file(tmp / "textures.ini", ec)) {
        bool ids = false;
        for (fs::directory_iterator it(tmp, ec), end; !ec && it != end && !ids; it.increment(ec)) ids = is_id_png(it->path().filename().u8string());
        if (!ids) { fs::remove_all(tmp, ec); return fail(kNoPack); }
    }
    {
        std::ofstream j(tmp / ".source.json", std::ios::binary);
        j << "{\"pack\": " << json_str(kind) << ", \"name\": " << json_str(name) << ", \"installed\": " << now_s() << ", \"files\": " << jobs.size()
          << ", \"by\": \"settings\"}";
        std::ofstream c(tmp / "PACK_CREDITS.txt", std::ios::binary);
        c << credits;
    }
    fs::remove_all(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec) { fs::remove_all(tmp, ec); return fail("could not put the pack in place: " + ec.message()); }
    if (progress) progress->store(pct_to);
    return true;
}
