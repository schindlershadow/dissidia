#include "install.h"
#include "extras.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <cstdint>
#include <cstring>
#include <vector>
#include "third_party/miniz.h"

namespace fs = std::filesystem;

namespace setup {

#ifdef _WIN32
static const char* EXE = ".exe";
#else
static const char* EXE = "";
#endif

static std::string join(const std::string& a, const std::string& b) { return (fs::path(a) / b).string(); }
static bool is_file(const std::string& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
static bool is_dir(const std::string& p) { std::error_code ec; return fs::is_directory(p, ec); }

static std::string find_on_path(const char* name) {
    const char* path = std::getenv("PATH");
    if (!path) return "";
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string s = path;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(sep, i);
        if (j == std::string::npos) j = s.size();
        std::string dir = s.substr(i, j - i);
        if (!dir.empty()) {
            std::string cand = join(dir, std::string(name) + EXE);
            if (is_file(cand)) return cand;
        }
        i = j + 1;
    }
    return "";
}

Layout find_layout(const std::string& root) {
    Layout l;
    l.root = root;
    l.recompiler = join(root, std::string("bin/psprecomp") + EXE);
    l.self = join(root, std::string("DissidiaRecomp") + EXE);
    l.toolchain = join(root, "toolchain");
    l.sdk = join(root, "sdk");
    l.game_dir = join(root, "game");
    l.setup_script = join(root, "game/tools/setup_from_iso.py");
    l.build_script = join(root, "game/tools/psp_build.py");
    l.update_script = join(root, "game/tools/update.py");
    std::string embedded = join(root, std::string("python/python") + EXE);
    if (is_file(embedded)) l.python = embedded;
    else {
        l.python = find_on_path("python3");
        if (l.python.empty()) l.python = find_on_path("python");
    }
    return l;
}

std::string Layout::problem() const {
#ifdef __ANDROID__
    const std::pair<const std::string*, const char*> need_a[] = {
        {&python, "libpython.so"}, {&recompiler, "libpsprecomp.so"}, {&pspdecrypt, "libpspdecrypt.so"}, {&zig, "libzig.so"},
        {&setup_script, "game/tools/setup_from_iso.py"}, {&build_script, "game/tools/psp_build.py"}};
    for (auto& n : need_a)
        if (!is_file(*n.first)) return std::string("this app is incomplete: ") + n.second + " is missing";
    if (!is_file(join(sdk, "lib/libpsprecomp_core.a"))) return "this app is incomplete: the game core (android/sdk) is missing";
    return "";
#endif
    if (python.empty()) return "Python 3 was not found (install python3; Windows releases include it)";
    const std::pair<const std::string*, const char*> need[] = {
        {&recompiler, "bin/psprecomp"}, {&setup_script, "game/tools/setup_from_iso.py"},
        {&build_script, "game/tools/psp_build.py"}};
    for (auto& n : need)
        if (!is_file(*n.first)) return std::string("this release is incomplete: ") + n.second + " is missing";
    if (!is_file(join(sdk, "lib/libpsprecomp_core.a")) && !is_file(join(sdk, "lib/libpsprecomp_core.lib")))
        return "this release is incomplete: sdk/lib/libpsprecomp_core.a is missing";
    if (!is_file(join(toolchain, "toolchain.cmake"))) return "this release is incomplete: toolchain/ is missing";
    return "";
}

// ---- disc identity: which game a picked disc image is (before anything is installed with it) ----
namespace {
// A disc image read in place: plain ISO or CSO (CISO v1, raw deflate blocks; the wizard's undub pick accepts both).
struct DiscReader {
    int fd = -1;               // "fd:N": pread, so the build tools that inherit the descriptor find it where it was
    std::ifstream f;
    bool cso = false;
    uint32_t block = 0, align = 0;
    uint64_t total = 0;
    std::vector<uint32_t> index;
    int64_t cached = -1;
    std::string cache;
    explicit DiscReader(const std::string& path) {
        if (path.compare(0, 3, "fd:") == 0) fd = std::atoi(path.c_str() + 3);
        else f.open(fs::u8path(path), std::ios::binary);
        std::string h;
        if (!ok() || !raw(0, 24, &h) || h.compare(0, 4, "CISO") != 0) return;
        block = (uint32_t)(uint8_t)h[16] | (uint32_t)(uint8_t)h[17] << 8 | (uint32_t)(uint8_t)h[18] << 16 | (uint32_t)(uint8_t)h[19] << 24;
        for (int i = 7; i >= 0; i--) total = total << 8 | (uint8_t)h[8 + i];
        align = (uint8_t)h[21];
        if ((uint8_t)h[20] > 1 || block < 2048 || (block & (block - 1)) || total == 0 || total > (1ull << 40)) { fd = -1; f.close(); return; }
        const uint64_t n = (total + block - 1) / block + 1;
        std::string ix;
        if (!raw(24, (size_t)n * 4, &ix)) { fd = -1; f.close(); return; }
        index.resize((size_t)n);
        for (size_t i = 0; i < n; i++)
            index[i] = (uint32_t)(uint8_t)ix[4 * i] | (uint32_t)(uint8_t)ix[4 * i + 1] << 8 | (uint32_t)(uint8_t)ix[4 * i + 2] << 16 | (uint32_t)(uint8_t)ix[4 * i + 3] << 24;
        cso = true;
    }
    bool ok() const { return fd >= 0 || f.is_open(); }
    bool raw(uint64_t off, size_t n, std::string* out) {
        out->assign(n, '\0');
        if (n == 0) return true;
#ifndef _WIN32
        if (fd >= 0) {
            size_t got = 0;
            while (got < n) {
                const ssize_t r = ::pread(fd, &(*out)[got], n - got, static_cast<off_t>(off + got));
                if (r <= 0) return false;
                got += static_cast<size_t>(r);
            }
            return true;
        }
#endif
        if (!f.is_open()) return false;
        f.clear();
        f.seekg(static_cast<std::streamoff>(off));
        f.read(&(*out)[0], static_cast<std::streamsize>(n));
        return static_cast<size_t>(f.gcount()) == n;
    }
    bool cso_block(uint64_t i) {
        if ((int64_t)i == cached) return true;
        if (i + 1 >= index.size()) return false;
        const uint64_t a = (uint64_t)(index[i] & 0x7FFFFFFFu) << align, b = (uint64_t)(index[i + 1] & 0x7FFFFFFFu) << align;
        if (b < a || b - a > (uint64_t)block * 2 + 64) return false;
        std::string src;
        if (!raw(a, (size_t)(b - a), &src)) return false;
        if (index[i] & 0x80000000u) { src.resize(std::min<size_t>(src.size(), block)); cache.swap(src); }
        else {
            cache.assign(block, '\0');
            const size_t got = tinfl_decompress_mem_to_mem(&cache[0], block, src.data(), src.size(), 0);
            if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
            cache.resize(got);
        }
        cached = (int64_t)i;
        return true;
    }
    bool read(uint64_t off, size_t n, std::string* out) {   // `n` bytes of the disc at `off`
        if (!cso) return raw(off, n, out);
        out->clear();
        if (off + n > total) return false;
        while (out->size() < n) {
            const uint64_t p = off + out->size(), bi = p / block, in = p % block;
            if (!cso_block(bi) || in >= cache.size()) return false;
            out->append(cache, (size_t)in, std::min<size_t>(cache.size() - (size_t)in, n - out->size()));
        }
        return true;
    }
};
uint32_t le32(const std::string& s, size_t o) {
    return o + 4 <= s.size() ? (uint32_t)(uint8_t)s[o] | (uint32_t)(uint8_t)s[o + 1] << 8 | (uint32_t)(uint8_t)s[o + 2] << 16 | (uint32_t)(uint8_t)s[o + 3] << 24 : 0;
}
uint16_t le16(const std::string& s, size_t o) { return o + 2 <= s.size() ? (uint16_t)((uint8_t)s[o] | (uint8_t)s[o + 1] << 8) : 0; }
// an ISO 9660 file or folder by its path ("PSP_GAME/PARAM.SFO"): extent and size
bool iso_find(DiscReader& r, const std::string& path, uint32_t* out_lba, uint32_t* out_size) {
    std::string pvd;
    if (!r.read(16u * 2048u, 2048, &pvd) || pvd.compare(1, 5, "CD001") != 0) return false;
    uint32_t lba = le32(pvd, 156 + 2), size = le32(pvd, 156 + 10);
    size_t from = 0;
    while (from <= path.size()) {
        size_t to = path.find('/', from);
        if (to == std::string::npos) to = path.size();
        const std::string want = path.substr(from, to - from);
        from = to + 1;
        if (size == 0 || size > (1u << 20)) return false;
        std::string d;
        if (!r.read((uint64_t)lba * 2048u, size, &d)) return false;
        bool found = false;
        for (size_t p = 0; p < d.size();) {
            const uint8_t len = (uint8_t)d[p];
            if (len == 0) { p = (p / 2048 + 1) * 2048; continue; }          // records do not cross a sector
            if (p + 33 > d.size() || p + len > d.size()) break;
            const size_t nl = (uint8_t)d[p + 32];
            std::string nm = d.substr(p + 33, std::min(nl, d.size() - p - 33));
            if (size_t semi = nm.find(';'); semi != std::string::npos) nm.resize(semi);
            for (char& ch : nm) ch = (char)std::toupper((unsigned char)ch);
            if (nm == want) { lba = le32(d, p + 2); size = le32(d, p + 10); found = true; break; }
            p += len;
        }
        if (!found) return false;
    }
    *out_lba = lba; *out_size = size;
    return true;
}
bool disc_id(DiscReader& r, std::string* id, std::string* title) {
    id->clear(); title->clear();
    uint32_t lba = 0, size = 0;
    if (!r.ok() || !iso_find(r, "PSP_GAME/PARAM.SFO", &lba, &size) || size < 20 || size > 65536) return false;
    std::string s;
    if (!r.read((uint64_t)lba * 2048u, size, &s) || s.compare(0, 4, std::string("\0PSF", 4)) != 0) return false;
    const uint32_t keys = le32(s, 8), data = le32(s, 12), n = le32(s, 16);
    for (uint32_t i = 0; i < n && 20 + 16 * (i + 1) <= s.size(); i++) {
        const size_t e = 20 + 16 * i;
        const size_t ko = keys + le16(s, e), dofs = data + le32(s, e + 12), dl = le32(s, e + 4);
        if (ko >= s.size() || dofs > s.size() || le16(s, e + 2) == 0x404) continue;   // 0x404: integers, not needed
        const std::string key = s.c_str() + ko;
        std::string v = s.substr(dofs, std::min<size_t>(dl, s.size() - dofs));
        if (size_t z = v.find('\0'); z != std::string::npos) v.resize(z);
        if (key == "DISC_ID") *id = v;
        else if (key == "TITLE") *title = v;
    }
    return !id->empty();
}
std::string lower(std::string s) { for (char& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; }
bool is_012(const std::string& id, const std::string& title) {
    return lower(title).find("duodecim") != std::string::npos || id == "ULUS10566" || id == "ULES01505" || id == "ULJM05814" || id == "NPJH50377";
}
bool is_d1(const std::string& id, const std::string& title) {
    return id == "ULUS10437" || (!is_012(id, title) && lower(title).find("dissidia") != std::string::npos);
}
std::string disc_words(const std::string& id, const std::string& title) { return (title.empty() ? std::string("this disc") : title) + " (" + id + ")"; }
}  // namespace

bool read_disc_id(const std::string& path, std::string* id, std::string* title) {
    DiscReader r(path);
    return disc_id(r, id, title);
}

std::string d1_disc_problem(const std::string& path) {
    std::string id, title;
    DiscReader r(path);
    if (!disc_id(r, &id, &title))
        return "This file is not a PSP disc image that can be read. Pick an uncompressed .iso of Dissidia Final Fantasy (USA); "
               "unpack a .zip / .7z first.";
    if (id == "ULUS10437") return "";
    const std::string what = disc_words(id, title);
    if (is_012(id, title))
        return "This is Dissidia 012, " + what + ". The moves come from the first game: pick your Dissidia Final Fantasy "
               "(2008, USA, ULUS-10437) disc image instead.";
    if (is_d1(id, title))
        return "This is another release of the first Dissidia, " + what + ". The moves are made from the US release only: "
               "pick Dissidia Final Fantasy (USA, ULUS-10437).";
    return "This is not Dissidia Final Fantasy: the disc is " + what + ". Pick Dissidia Final Fantasy (2008, USA, ULUS-10437).";
}

// The English battle voice bank of Bartz (voice/en/battle/voice_p_fiv100_00.scd, package CRC 0x4300D781) as the EU and US
// discs have it: an image whose bank is this one has the English voices, so it is not an undub.
constexpr uint32_t kVoiceProbeCrc = 0x4300D781u, kVoiceProbeEnSize = 302368u, kVoiceProbeEnCrc32 = 0x6EE2B876u;
std::string undub_disc_problem(const std::string& path) {
    std::string id, title;
    DiscReader r(path);
    if (!disc_id(r, &id, &title))
        return "This file is not a PSP disc image that can be read. Pick a .iso or .cso of Dissidia 012 with Japanese voices; "
               "unpack a .zip / .7z first.";
    const std::string what = disc_words(id, title);
    if (!is_012(id, title))
        return (is_d1(id, title) ? "This is the first Dissidia, " : "This is not Dissidia 012: the disc is ") + what +
               ". The Japanese voices come from a Dissidia 012 disc image that has them (an undub or the Japanese release).";
    uint32_t il = 0, is = 0, pl = 0, ps = 0;
    std::string info;
    if (!iso_find(r, "PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN", &il, &is) || !iso_find(r, "PSP_GAME/USRDIR/DATA/PACKAGE.BIN", &pl, &ps) ||
        is < 16 || is > (4u << 20) || !r.read((uint64_t)il * 2048u, is, &info) || info.compare(4, 4, "pack") != 0)
        return "The voice files of this disc image cannot be read (" + what + "). Pick another image of the undub or Japanese release.";
    const uint32_t count = le32(info, 8);
    for (uint32_t i = 0; i < count && 16 + 12 * (i + 1) <= info.size(); i++) {
        if (le32(info, 16 + 12 * i) != kVoiceProbeCrc) continue;
        const uint64_t off = (uint64_t)pl * 2048u + (uint64_t)le32(info, 16 + 12 * i + 4) * 2048u;
        const uint32_t size = le32(info, 16 + 12 * i + 8) & 0x0FFFFFFFu;
        std::string bank;
        if (size == kVoiceProbeEnSize && r.read(off, size, &bank) &&
            mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(bank.data()), bank.size()) == kVoiceProbeEnCrc32)
            return "This disc has the English voices, " + what + ": it is a regular Dissidia 012, like your own. Pick the undub "
                   "(a patched copy with the Japanese voices) or the Japanese release.";
        return "";
    }
    return "";   // no English bank to compare with (a release that names its files differently): not refused here
}

// ---- a texture pack archive (the texture_add extra: Add your own texture pack) ----
namespace {
bool is_texture_id_png(const std::string& base) {   // <16 hex digits>.png: the runtime's id-named textures
    if (base.size() != 20 || lower(base.substr(16)) != ".png") return false;
    for (size_t i = 0; i < 16; i++)
        if (!std::isxdigit(static_cast<unsigned char>(base[i]))) return false;
    return true;
}
size_t pack_read(void* opaque, mz_uint64 ofs, void* buf, size_t n) {
    std::string s;
    if (!static_cast<DiscReader*>(opaque)->raw(ofs, n, &s)) return 0;
    std::memcpy(buf, s.data(), n);
    return n;
}
}  // namespace

// Something on the PATH that unpacks .7z / .rar (fetch_textures.py uses the same list).
bool archive_tool_available() {
#ifdef __ANDROID__
    return false;
#else
    for (const char* t : {"7z", "7za", "7zz", "bsdtar"})
        if (!find_on_path(t).empty()) return true;
#ifdef _WIN32
    return !find_on_path("tar").empty();   // Windows 10+ ships bsdtar as tar.exe: it reads .7z and .rar
#else
    return false;
#endif
#endif
}

std::string texture_pack_problem(const std::string& path) {
    DiscReader r(path);
    std::string head;
    if (!r.ok() || !r.raw(0, 8, &head)) return "The file could not be read.";
    uint64_t size = 0;
#ifndef _WIN32
    struct stat st;
    if (r.fd >= 0) { if (fstat(r.fd, &st) == 0) size = static_cast<uint64_t>(st.st_size); }
    else
#endif
    {
        std::error_code ec;
        size = static_cast<uint64_t>(fs::file_size(fs::u8path(path), ec));
    }
    const bool sevenz = head.compare(0, 6, std::string("7z\xBC\xAF\x27\x1C", 6)) == 0, rar = head.compare(0, 4, "Rar!") == 0;
    if (sevenz || rar) {
#ifdef __ANDROID__
        return std::string("This is a .") + (sevenz ? "7z" : "rar") + " file, which can not be unpacked on a phone. Unpack it on a PC, zip the folder "
               "again and pick the .zip.";
#else
        if (archive_tool_available()) return "";   // checked once it is unpacked (fetch_textures.py)
        return std::string("This is a .") + (sevenz ? "7z" : "rar") + " file. Install 7-Zip (7-zip.org) to unpack it, or unpack it yourself, "
               "zip the folder again and pick the .zip.";
#endif
    }
    if (head.compare(0, 2, "PK") != 0) return "This is not a .zip, .7z or .rar file.";
    mz_zip_archive z{};
    z.m_pRead = pack_read;
    z.m_pIO_opaque = &r;
    if (size == 0 || !mz_zip_reader_init(&z, size, 0)) return "This .zip file is damaged or not a .zip.";
    bool ini = false;
    size_t ids = 0;
    const mz_uint n = mz_zip_reader_get_num_files(&z);
    for (mz_uint i = 0; i < n && !ini; i++) {
        char name[1024];
        if (mz_zip_reader_is_file_a_directory(&z, i) || !mz_zip_reader_get_filename(&z, i, name, sizeof name)) continue;
        std::string f = name;
        const size_t sl = f.find_last_of('/');
        const std::string base = sl == std::string::npos ? f : f.substr(sl + 1);
        if (f.find("__MACOSX/") != std::string::npos) continue;
        if (lower(base) == "textures.ini") ini = true;
        else if (is_texture_id_png(base)) ids++;
    }
    mz_zip_reader_end(&z);
    if (ini || ids) return "";
    return "This .zip has no textures.ini (and no PNG files named by texture ids), so it is not a texture pack the game can use. Texture packs "
           "for PPSSPP (Dissidia 012, ULUS10566 / ULES01505) have one.";
}

bool open_on_desktop(const std::string& path, bool show_in_folder, std::string* error) {
#if defined(__ANDROID__)
    (void)path; (void)show_in_folder;
    if (error) *error = "not available on Android";
    return false;
#elif defined(_WIN32)
    auto wide = [](const std::string& s) {
        std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0), L'\0');
        if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], (int)w.size());
        if (!w.empty() && w.back() == L'\0') w.pop_back();
        return w;
    };
    std::string p = path;
    for (char& ch : p) if (ch == '/') ch = '\\';
    HINSTANCE r;
    if (show_in_folder) {
        const std::wstring args = L"/select,\"" + wide(p) + L"\"";
        r = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        r = ShellExecuteW(nullptr, L"open", wide(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if ((INT_PTR)r <= 32) { if (error) *error = "Windows could not open " + p; return false; }
    return true;
#elif defined(__APPLE__)
    return spawn_detached(show_in_folder ? std::vector<std::string>{"open", "-R", path} : std::vector<std::string>{"open", path}, {}, "", error);
#else
    const std::string target = show_in_folder ? fs::path(path).parent_path().string() : path;
    return spawn_detached({"xdg-open", target}, {}, "", error);
#endif
}

bool feature_available(const Layout& l, const std::string& name) {
    std::ifstream f(join(l.game_dir, "features.ini"));
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == name + "=1") return true;
    }
    return false;
}

static const char* kExplainBuild[5] = {
    "Checks your game files against the analysis data that ships with this program.",
    "Translates the game's PSP code into C++ on this device. Nothing is shared.",
    "Does the same for the battle, menu and extra modules.",
    "Sets up the compiler.",
    "Compiles about 470 source files: the long part. Cancelling is safe; it continues where it stopped."};

static Step build_step(const Layout& l, const std::string& workdir, bool android) {
    Step b;
    b.label = android ? "Building the Android version" : "Building the game";
    b.explain = android ? "Builds the Android version from the same translation."
                        : "";
    b.is_build = true;
    b.argv = {l.python, l.build_script, workdir, "--sdk", l.sdk, "--toolchain", l.toolchain, "--psprecomp", l.recompiler,
              "--game", l.game_dir, "--progress"};
    if (android) b.argv.push_back("--android");
    b.env = {{"PYTHONUNBUFFERED", "1"}};
    return b;
}

static void finish_plan(std::vector<Step>& plan) {
    double total = 0;
    for (auto& s : plan) total += s.weight;
    double at = 0;
    for (auto& s : plan) { s.weight /= total; s.base = at; at += s.weight; }
}

std::vector<Step> plan_rebuild(const Layout& l, const std::string& workdir, const Options& o) {
    std::vector<Step> p;
    Step b = build_step(l, workdir, false);
    b.weight = 0.84;
    p.push_back(b);
    if (o.android && feature_available(l, "android")) {
        Step a = build_step(l, workdir, true);
        a.weight = 0.60;
        p.push_back(a);
    }
    finish_plan(p);
    return p;
}

// The optional extras after the game is built (the shared list in extras.cpp; prev = what is installed for Change extras), then the saves.
static void append_extras(std::vector<Step>& p, const Layout& l, const std::string& workdir, const Options& o, const Options* prev) {
    plan_extra_steps(p, ExtraCtx{l, workdir, false}, o, prev);
    if (!o.import_saves.empty()) {
        Step v;
        v.label = "Importing your saves";
        v.explain = "Copies your saves into the game. The originals are not changed.";
        v.argv = {l.self, "--import-saves", o.import_saves, "--dir", workdir};
        v.weight = 0.01;
        p.push_back(v);
    }
}

std::vector<Step> plan_install(const Layout& l, const std::string& iso, const std::string& workdir, const Options& o) {
    Step x;
    x.label = "Reading your game file";
    x.explain = "Reads the game's files out of your disc image. Nothing is uploaded.";
    x.argv = {l.python, l.setup_script, iso, workdir};
    x.env = {{"PSPRECOMP_TOOLCHAIN", l.toolchain}, {"PYTHONUNBUFFERED", "1"}};
    x.weight = 0.08;
    std::vector<Step> p{x};
    for (auto& s : plan_rebuild(l, workdir, o)) p.push_back(s);   // weights are renormalised below
    // plan_rebuild() normalised its own steps; restore their relative sizes
    for (size_t i = 1; i < p.size(); i++) p[i].weight = i == 1 ? 0.84 : 0.60;
    append_extras(p, l, workdir, o, nullptr);
    if (o.steam && feature_available(l, "steam")) for (auto& st : plan_steam(l, workdir)) { st.weight = 0.03; p.push_back(st); }
    finish_plan(p);
    return p;
}

std::vector<Step> plan_extras(const Layout& l, const std::string& workdir, const Options& prev, const Options& o) {
    std::vector<Step> p;
    append_extras(p, l, workdir, o, &prev);
    if (o.android && feature_available(l, "android")) {
        Step a = build_step(l, workdir, true);
        a.weight = 0.60;
        p.push_back(a);
    }
    for (auto& st : p) if (st.weight <= 0) st.weight = 0.05;
    if (o.steam && feature_available(l, "steam")) for (auto& st : plan_steam(l, workdir)) { st.weight = 0.03; p.push_back(st); }
    finish_plan(p);
    return p;
}

static std::string self_exe();

std::vector<Step> plan_steam(const Layout& l, const std::string& workdir) {
    std::string exe = self_exe();
    if (!l.root.empty() && is_file(join(l.root, std::string("DissidiaRecomp") + EXE))) exe = join(l.root, std::string("DissidiaRecomp") + EXE);
    Step st;
    st.label = "Adding the game to Steam";
    st.explain = "Adds a shortcut to your Steam library. Steam closes for a moment and starts again.";
    st.argv = {l.python, join(l.game_dir, "tools/steam_shortcut.py"), exe, "--name", "Dissidia 012 Duodecim Final Fantasy",
               "--launch-options=--launch", "--yes", "--icon", join(l.game_dir, "icon/icon.png"),
               "--pic1", join(workdir, "disc0/PSP_GAME/PIC1.PNG")};
    st.env = {{"PYTHONUNBUFFERED", "1"}};
    st.weight = 0.03;
    st.optional = true;
    std::vector<Step> p{st};
    finish_plan(p);
    return p;
}

std::vector<Step> plan_android_install(const Layout& l, const std::string& workdir) {
    Step x;
    x.label = "Installing on your device";
    x.explain = "Installs the app over adb and copies the game files (about 2 GB).";
    x.argv = {l.python, join(l.game_dir, "tools/android_install.py"), workdir};
    x.env = {{"PYTHONUNBUFFERED", "1"}};
    x.weight = 1;
    std::vector<Step> p{x};
    finish_plan(p);
    return p;
}

bool android_build_exists(const std::string& workdir) { return is_file(join(workdir, "android/dissidia.apk")); }

#ifdef __ANDROID__
Layout find_layout_android(const std::string& root, const std::string& lib_dir) {
    Layout l;
    l.root = root;
    l.lib_dir = lib_dir;
    l.python = join(lib_dir, "libpython.so");
    l.recompiler = join(lib_dir, "libpsprecomp.so");
    l.pspdecrypt = join(lib_dir, "libpspdecrypt.so");
    l.zig = join(lib_dir, "libzig.so");
    l.android_dir = join(root, "android");
    l.sdk = join(root, "android/sdk");
    l.game_dir = join(root, "game");
    l.setup_script = join(root, "game/tools/setup_from_iso.py");
    l.build_script = join(root, "game/tools/psp_build.py");
    l.update_script = join(root, "game/tools/update.py");
    return l;
}

std::vector<std::string> android_check_argv(const Layout& l, const std::string& manifest, const std::string& sig, const std::string& current) {
    return {l.python, l.update_script, "android-check", "--manifest", manifest, "--sig", sig, "--key", join(l.game_dir, "update_key.pub"), "--current", current};
}

std::vector<std::string> android_verify_argv(const Layout& l, const std::string& file, const std::string& sha256, long long size) {
    return {l.python, l.update_script, "android-verify", "--file", file, "--sha256", sha256, "--size", std::to_string(size)};
}

std::string android_game_dir() { return std::string("/sdcard/Android/media/") + kGamePackage + "/game"; }

EnvList android_env(const Layout& l, const std::string& workdir) {
    return {{"PYTHONUNBUFFERED", "1"}, {"PYTHONDONTWRITEBYTECODE", "1"}, {"PYTHONHOME", join(l.root, "python")},
            {"PSPRECOMP_ANDROID_DEVICE", "1"}, {"PSPRECOMP_ZIG", l.zig}, {"HOME", workdir},
            {"TMPDIR", join(workdir, "tmp")}, {"ZIG_LIB_DIR", join(l.android_dir, "zig-lib")}};
}

std::string android_payload_fingerprint(const Layout& l) {
    std::ifstream f(join(l.android_dir, "build_fingerprint.txt"));
    std::string fp;
    std::getline(f, fp);
    while (!fp.empty() && (fp.back() == '\r' || fp.back() == ' ')) fp.pop_back();
    return fp;
}

std::vector<Step> plan_rebuild_android(const Layout& l, const std::string& workdir) {
    Options none;
    for (const Extra& e : extras()) none.set(e.key, "0");   // the build only
    std::vector<Step> all = plan_install_android(l, "", workdir, none);
    std::vector<Step> p(all.begin() + 1, all.end());   // without "Reading your game file": the disc is already extracted
    for (auto& s : p) { s.base = 0; }
    p[0].weight = 0.88; p[1].weight = 0.12;
    finish_plan(p);
    return p;
}

std::vector<Step> plan_install_android(const Layout& l, const std::string& iso, const std::string& workdir, const Options& o) {
    std::error_code ec;
    fs::create_directories(join(workdir, "tmp"), ec);
    Step x;
    x.label = "Reading your game file";
    x.explain = "Reads the game's files out of your disc image. Nothing is uploaded.";
    x.argv = {l.python, l.setup_script, iso, workdir, "--pspdecrypt", l.pspdecrypt};
    x.env = android_env(l, workdir);
    x.weight = 0.10;
    Step b;
    b.label = "Building the game on this device";
    b.explain = "Translates the game and compiles it here: the long part (10 to 20 minutes). Cancelling is safe.";
    b.is_build = true;
    b.argv = {l.python, l.build_script, workdir, "--android", l.android_dir, "--psprecomp", l.recompiler, "--game", l.game_dir, "--progress"};
    b.env = android_env(l, workdir);
    b.weight = 0.80;
    Step c;
    c.label = "Putting the game files where the game can read them";
    c.explain = "Copies the game's files (about 2 GB) to where the game reads them.";
    c.argv = {l.python, join(l.game_dir, "tools/android_local_install.py"), workdir, "--dest", android_game_dir()};
    c.env = android_env(l, workdir);
    c.weight = 0.10;
    std::vector<Step> p{x};
    // the extras go in before the files are copied to the game's folder
    plan_extra_steps(p, ExtraCtx{l, workdir, true}, o, nullptr);
    p.push_back(b);
    p.push_back(c);
    finish_plan(p);
    return p;
}

std::vector<Step> plan_extras_android(const Layout& l, const std::string& workdir, const Options& prev, const Options& o) {
    std::error_code ec;
    fs::create_directories(join(workdir, "tmp"), ec);
    std::vector<Step> p;
    plan_extra_steps(p, ExtraCtx{l, workdir, true}, o, &prev);
    Step c;
    c.label = "Putting the game files where the game can read them";
    c.explain = "Copies the files to where the game reads them.";
    c.argv = {l.python, join(l.game_dir, "tools/android_local_install.py"), workdir, "--dest", android_game_dir()};
    c.env = android_env(l, workdir);
    c.weight = 0.15;
    p.push_back(c);
    for (auto& st : p) if (st.weight <= 0) st.weight = 0.05;
    finish_plan(p);
    return p;
}
#endif

std::vector<std::string> update_check_argv(const Layout& l) {
    std::vector<std::string> a = {l.python, l.update_script, "check", "--root", l.root};
    if (const char* m = std::getenv("PSPRECOMP_MANIFEST_URL")) { a.push_back("--manifest"); a.push_back(m); }
    return a;
}

UpdateInfo parse_update_line(const std::string& line) {
    UpdateInfo u;
    auto field = [&](const std::string& key) -> std::string {
        size_t p = line.find(" " + key + "=");
        if (p == std::string::npos) return "";
        p += key.size() + 2;
        size_t e = line.find(' ', p);
        return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };
    if (line.compare(0, 6, "UPDATE") == 0) {
        u.available = true;
        u.version = field("version");
        u.current = field("current");
        u.size = std::atoll(field("size").c_str());
        u.rebuild = field("rebuild") == "1";
        u.relink = field("relink") == "1";
        u.url = field("url");
        u.sha256 = field("sha256");
    } else if (line.compare(0, 5, "ERROR") == 0) {
        u.error = line.size() > 6 ? line.substr(6) : "unknown error";
    }
    return u;
}

std::vector<Step> plan_update(const Layout& l, const std::string& workdir, const Options& o, const UpdateInfo& u) {
    Step d;
    d.label = "Downloading the update";
    d.explain = "Downloads the new release and replaces the program files. Your saves are untouched.";
    d.argv = {l.python, l.update_script, "apply", "--root", l.root};
    if (const char* m = std::getenv("PSPRECOMP_MANIFEST_URL")) { d.argv.push_back("--manifest"); d.argv.push_back(m); }
    d.env = {{"PYTHONUNBUFFERED", "1"}};
    d.weight = u.rebuild ? 0.10 : 0.60;
    std::vector<Step> p{d};
    if (u.rebuild || u.relink) {
        Step b = build_step(l, workdir, false);
        b.explain = "";
        b.weight = u.rebuild ? 0.90 : 0.40;
        p.push_back(b);
    }
    (void)o;
    finish_plan(p);
    return p;
}

bool update_check_enabled(const std::string& workdir) {
    if (const char* e = std::getenv("PSPRECOMP_UPDATE_CHECK")) if (e[0] == '0') return false;
    std::ifstream f(join(workdir, "settings.ini"));
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == "check_updates=0") return false;
    }
    return true;
}

double step_done_fraction(const std::vector<Step>& plan, int i) {
    if (i < 0) return 0;
    if (i >= (int)plan.size()) return 1;
    return plan[(size_t)i].base + plan[(size_t)i].weight;
}

// psp-build STEP boundaries inside a build step: analysis, recompile, overlays, configure, compile.
static const double kBuildStart[] = {0.00, 0.03, 0.10, 0.15, 0.17, 1.00};

void feed_line(Progress& p, const std::vector<Step>& plan, const std::string& line, long long now_ms) {
    p.last_line = line.size() > 200 ? line.substr(0, 200) : line;
    const Step& st = plan[(size_t)std::min<int>(p.step, (int)plan.size() - 1)];
    if (p.start_ms == 0) p.start_ms = now_ms;
    double base = st.base, span = st.weight;
    if (!st.is_build) {
        p.stage = st.label;
        p.explain = st.explain;
        p.detail = line.size() > 60 ? line.substr(0, 57) + "..." : line;
        p.fraction = std::max(p.fraction, base);
        int pc = 0;
        if (std::sscanf(line.c_str(), "[update] PROGRESS %d", &pc) == 1) {   // update.py's download
            p.detail = "Downloading: " + std::to_string(pc) + "%";
            p.fraction = std::max(p.fraction, base + span * std::min(100, std::max(0, pc)) / 100.0 * 0.8);
        } else if (line.compare(0, 9, "[update] ") == 0) {
            if (line.compare(9, 5, "ERROR") == 0) p.error = line.substr(15);
            else p.detail = line.substr(9, 56);
        }
        return;
    }
    // ninja's own counter "[183/474] Building CXX object .../batch_0165.cpp.o"
    if (!line.empty() && line[0] == '[' && std::isdigit((unsigned char)line[1])) {
        int i = 0, n = 0;
        if (std::sscanf(line.c_str(), "[%d/%d]", &i, &n) == 2 && n > 0) {
            if (p.compile_n == 0 || i < p.compile_i) p.compile_start_ms = now_ms;
            p.compile_i = i; p.compile_n = n;
            size_t sl = line.rfind('/');
            std::string file = sl == std::string::npos ? line : line.substr(sl + 1);
            if (file.size() > 3 && file.compare(file.size() - 2, 2, ".o") == 0) file.erase(file.size() - 2);
            p.detail = "Compiled " + std::to_string(i) + " of " + std::to_string(n) + ": " + file;
            return;
        }
    }
    const std::string tagp = "[psp-build] ";
    if (line.compare(0, tagp.size(), tagp) != 0) return;
    std::istringstream is(line.substr(tagp.size()));
    std::string kind;
    is >> kind;
    if (kind == "STEP") {
        int i = 0, n = 0; char slash = 0;
        is >> i >> slash >> n;
        std::string rest;
        std::getline(is, rest);
        while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        p.sub = std::max(1, std::min(i, 5)) - 1;
        p.stage = (rest.empty() ? st.label : rest);
        p.explain = !st.explain.empty() ? st.explain : kExplainBuild[p.sub];
        p.detail.clear();
        p.fraction = std::max(p.fraction, base + span * kBuildStart[p.sub]);
    } else if (kind == "PROGRESS") {
        int pc = 0;
        is >> pc;
        double lo = kBuildStart[p.sub], hi = kBuildStart[p.sub + 1];
        p.fraction = std::max(p.fraction, base + span * (lo + (hi - lo) * std::min(100, std::max(0, pc)) / 100.0));
    } else if (kind == "ERROR") {
        std::string rest;
        std::getline(is, rest);
        p.error = rest.empty() ? "the build failed" : rest.substr(rest[0] == ' ' ? 1 : 0);
    }
}

static std::string mmss(long long secs) {
    char b[32];
    if (secs >= 3600) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", secs / 3600, (secs / 60) % 60, secs % 60);
    else std::snprintf(b, sizeof b, "%lld:%02lld", secs / 60, secs % 60);
    return b;
}

std::string elapsed_text(const Progress& p, long long now_ms) {
    return p.start_ms ? mmss(std::max(0LL, (now_ms - p.start_ms) / 1000)) + " elapsed" : "";
}

std::string eta_text(const Progress& p, long long now_ms) {
    double remaining = -1;   // seconds
    if (p.compile_n > 20 && p.compile_i > 8 && p.compile_start_ms) {
        // during the compile the object counter is the best clock (later files are not cheaper than earlier ones on average)
        double per = (now_ms - p.compile_start_ms) / 1000.0 / p.compile_i;
        remaining = per * (p.compile_n - p.compile_i);
    } else if (p.start_ms && p.fraction > 0.04) {
        remaining = (now_ms - p.start_ms) / 1000.0 * (1.0 - p.fraction) / p.fraction;
    }
    if (remaining < 0) return "Working out how long this will take...";
    long long m = (long long)(remaining / 60.0 + 0.5);
    if (remaining < 45) return "Less than a minute left";
    if (m < 90) return "About " + std::to_string(m) + " minute" + (m == 1 ? "" : "s") + " left";
    return "About " + std::to_string((m + 30) / 60) + " hours left";
}

State load_state(const std::string& file) {
    State s;
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "workdir") s.workdir = v;
        else if (k == "iso") s.iso = v;
        else if (k == "version") s.version = v;
        else if (k == "built_fp") s.built_fp = v;
        else if (k == "check_updates") s.options.check_updates = v != "0";
        else if (k == "android") s.options.android = v == "1";
        else if (find_extra(k)) s.options.set(k, v);   // the extras (extras.cpp), whatever they are
        else for (const Extra& e : extras()) if (!e.disc.key.empty() && e.disc.key == k) s.options.set(k, v);
    }
    return s;
}

bool save_state(const std::string& file, const State& s) {
    std::ofstream f(file, std::ios::trunc);
    if (!f) return false;
    f << "workdir=" << s.workdir << "\niso=" << s.iso << "\nversion=" << s.version << "\n"
      << "check_updates=" << s.options.check_updates << "\nandroid=" << s.options.android << "\n";
    for (const Extra& e : extras()) {
        f << e.key << "=" << s.options.get(e.key) << "\n";
        if (!e.disc.key.empty()) f << e.disc.key << "=" << s.options.get(e.disc.key) << "\n";
    }
    f << "built_fp=" << s.built_fp << "\n";
    return (bool)f;
}

std::string game_binary(const std::string& workdir) { return join(workdir, std::string("build/psprecomp_runtime") + EXE); }
bool game_installed(const std::string& workdir) { return !workdir.empty() && is_file(game_binary(workdir)); }

EnvList game_env(const std::string& workdir) {
    // What games/dissidia/tools/run.sh sets for a plain start.
    EnvList e;
    e.push_back({"PSPRECOMP_DISC0", join(workdir, "disc0")});
    e.push_back({"PSPRECOMP_MS0", join(workdir, "ms0")});
    e.push_back({"PSPRECOMP_CROSS_MID", "1"});
    if (!std::getenv("PSPRECOMP_PAD_L2")) e.push_back({"PSPRECOMP_PAD_L2", "R+CROSS"});
    if (!std::getenv("PSPRECOMP_PAD_R2")) e.push_back({"PSPRECOMP_PAD_R2", "R+TRIANGLE"});
    std::string db = join(workdir, "gamecontrollerdb.txt");
    if (is_file(db) && !std::getenv("PSPRECOMP_GAMECONTROLLERDB")) e.push_back({"PSPRECOMP_GAMECONTROLLERDB", db});
    return e;
}

bool launch_game(const std::string& workdir, std::string* error) {
    std::error_code ec;
    fs::create_directories(join(workdir, "ms0/PSP/SAVEDATA"), ec);
    fs::create_directories(join(workdir, "ms0/PSP/GAME"), ec);
    return spawn_detached({game_binary(workdir)}, game_env(workdir), workdir, error, join(workdir, "last_run.log"));
}

// Path of the running program (the wizard), "" if unknown.
static std::string self_exe() {
#ifdef _WIN32
    char buf[4096];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    return n > 0 && n < sizeof buf ? std::string(buf, n) : std::string();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
#endif
}

bool create_shortcut(const std::string& workdir, const std::string& root, std::string* where, std::string* error) {
    std::string exe = self_exe();
    if (!root.empty() && is_file(join(root, std::string("DissidiaRecomp") + EXE))) exe = join(root, std::string("DissidiaRecomp") + EXE);
    if (exe.empty()) { if (error) *error = "cannot find the setup program's own path"; return false; }
    std::error_code ec;
    // The game's icon: the release's own (game/icon/icon.png + icon.ico, next to this program), else the disc's ICON0.PNG (144x80).
    const std::string exe_dir = fs::path(exe).parent_path().string();   // (root when given: the program is there)
    const std::string icon_png = join(exe_dir, "game/icon/icon.png"), icon_ico = join(exe_dir, "game/icon/icon.ico");
    std::string icon = is_file(icon_png) ? icon_png : join(workdir, "disc0/PSP_GAME/ICON0.PNG");
    std::string icon_copy = join(workdir, "icon.png");
    if (is_file(icon)) fs::copy_file(icon, icon_copy, fs::copy_options::overwrite_existing, ec);
    std::string ico_copy = join(workdir, "icon.ico");
    if (is_file(icon_ico)) fs::copy_file(icon_ico, ico_copy, fs::copy_options::overwrite_existing, ec);
    const std::string name = "Dissidia 012 Duodecim Final Fantasy";
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (!appdata) { if (error) *error = "APPDATA is not set"; return false; }
    std::string dir = join(appdata, "Microsoft\\Windows\\Start Menu\\Programs");
    std::string lnk = join(dir, name + ".lnk");
    // A .lnk is a binary format; the WScript.Shell COM object writes one (present on every Windows).
    auto q = [](const std::string& x) { std::string r = "'"; for (char c : x) { if (c == '\'') r += "''"; else r += c; } return r + "'"; };
    std::string ps = "$s=(New-Object -ComObject WScript.Shell).CreateShortcut(" + q(lnk) + ");$s.TargetPath=" + q(exe) +
                     ";$s.Arguments='--launch';$s.WorkingDirectory=" + q(workdir) +
                     ";$s.Description=" + q(name) + (is_file(ico_copy) ? ";$s.IconLocation=" + q(ico_copy) : std::string()) + ";$s.Save()";
    if (std::system(("powershell -NoProfile -NonInteractive -Command \"" + ps + "\"").c_str()) != 0 || !is_file(lnk)) {
        if (error) *error = "could not create " + lnk;
        return false;
    }
    if (where) *where = lnk;
    return true;
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base = xdg && *xdg ? xdg : join(home_dir(), ".local/share");
    std::string dir = join(base, "applications");
    fs::create_directories(dir, ec);
    std::string file = join(dir, "dissidia-012.desktop");
    auto esc = [](const std::string& x) { std::string r = "\""; for (char c : x) { if (c == '"' || c == '\\' || c == '$' || c == '`') r += '\\'; r += c; } return r + "\""; };
    std::ofstream f(file, std::ios::trunc);
    if (!f) { if (error) *error = "cannot write " + file; return false; }
    f << "[Desktop Entry]\nType=Application\nName=" << name << "\nComment=Dissidia 012 native build\n"
      << "Exec=" << esc(exe) << " --launch\n"
      << "Path=" << workdir << "\n"
      << (is_file(icon_copy) ? "Icon=" + icon_copy + "\n" : std::string())
      << "Terminal=false\nCategories=Game;\nStartupWMClass=Dissidia\n";
    f.close();
    fs::permissions(file, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add, ec);
    if (where) *where = file;
    return (bool)f;
#endif
}

std::string home_dir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return h ? h : "";
}

std::string default_install_dir() {
    std::string h = home_dir();
    return h.empty() ? "Dissidia012" : join(join(h, "Games"), "Dissidia012");
}

long long free_space(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::absolute(path, ec);
    while (!p.empty() && !fs::exists(p, ec)) {
        fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    auto sp = fs::space(p, ec);
    return ec ? -1 : (long long)sp.available;
}

}  // namespace setup
