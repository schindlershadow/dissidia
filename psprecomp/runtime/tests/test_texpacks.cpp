// psp_texpacks: several installed texture packs, one applied (runtime/include/psp_texpacks.h).
#include "psp_texpacks.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <zlib.h>

namespace fs = std::filesystem;
#ifdef _WIN32
static void setenv(const char* k, const char* v, int) { _putenv_s(k, v); }
#endif
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void pack(const fs::path& d, const char* name, long long installed) {
    fs::create_directories(d);
    std::ofstream(d / "textures.ini") << "[options]\n";
    if (name) std::ofstream(d / ".source.json") << "{\"pack\": \"file\", \"name\": \"" << name << "\", \"installed\": " << installed << "}";
}


// A .zip with the given (name, content) files; deflate = raw-deflate them (method 8), else stored.
static void write_zip(const fs::path& zp, const std::vector<std::pair<std::string, std::string>>& files, bool packed) {
    std::string out, cd;
    auto u16 = [](std::string& o, uint32_t v) { o += char(v & 0xFF); o += char(v >> 8 & 0xFF); };
    auto u32 = [&](std::string& o, uint32_t v) { u16(o, v & 0xFFFF); u16(o, v >> 16); };
    for (const auto& f : files) {
        std::string data = f.second;
        if (packed) {
            z_stream z{};
            deflateInit2(&z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            std::string buf(compressBound(uLong(f.second.size())) + 64, '\0');
            z.next_in = (Bytef*)f.second.data(); z.avail_in = uInt(f.second.size());
            z.next_out = (Bytef*)&buf[0]; z.avail_out = uInt(buf.size());
            deflate(&z, Z_FINISH);
            buf.resize(z.total_out);
            deflateEnd(&z);
            data = buf;
        }
        const uint32_t crc = uint32_t(crc32(0, (const Bytef*)f.second.data(), uInt(f.second.size())));
        const uint32_t at = uint32_t(out.size()), m = packed ? 8 : 0;
        u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, m); u16(out, 0); u16(out, 0x21);
        u32(out, crc); u32(out, uint32_t(data.size())); u32(out, uint32_t(f.second.size())); u16(out, uint32_t(f.first.size())); u16(out, 0);
        out += f.first; out += data;
        u32(cd, 0x02014b50); u16(cd, 20); u16(cd, 20); u16(cd, 0); u16(cd, m); u16(cd, 0); u16(cd, 0x21);
        u32(cd, crc); u32(cd, uint32_t(data.size())); u32(cd, uint32_t(f.second.size())); u16(cd, uint32_t(f.first.size())); u16(cd, 0); u16(cd, 0);
        u16(cd, 0); u16(cd, 0); u32(cd, 0); u32(cd, at); cd += f.first;
    }
    const uint32_t cd_at = uint32_t(out.size());
    out += cd;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, uint32_t(files.size())); u16(out, uint32_t(files.size()));
    u32(out, uint32_t(cd.size())); u32(out, cd_at); u16(out, 0);
    std::ofstream(zp, std::ios::binary) << out;
}
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }

int main() {
    const fs::path m = fs::temp_directory_path() / "psp_texpacks_test" / "mods";
    fs::remove_all(m.parent_path());
    CHECK(psp_texpacks_list(m.string()).empty());
    CHECK(psp_texpacks_pick({}, "x", 0).id.empty());
    pack(m / "texpacks" / "recomp-hd", "Dissidia Recomp HD", 1000);
    pack(m / "texpacks" / "my-pack", "My \\\"Pack\\\" v2", 2000);
    pack(m / "texpacks" / ".new", "half installed", 3000);          // an install in progress: not listed
    fs::create_directories(m / "texpacks" / "empty");                // no pack files: not listed
    fs::create_directories(m / "textures");                          // an older release's pack: listed in place
    std::ofstream(m / "textures" / "0123456789abcdef.png") << "png";

    auto v = psp_texpacks_list(m.string());
    CHECK(v.size() == 3);
    bool legacy = false;
    for (const auto& p : v) if (p.id == "textures") legacy = p.name == "Installed pack" && p.installed > 2000;   // its folder time: now
    CHECK(legacy);
    CHECK(v[0].id == "recomp-hd" && v[0].name == "Dissidia Recomp HD" && v[0].installed == 1000);
    CHECK(v[1].id == "my-pack" && v[1].name == "My \"Pack\" v2");

    CHECK(psp_texpacks_pick(v, "", 0).id == "textures");             // no choice: the newest
    CHECK(psp_texpacks_pick(v, "recomp-hd", 9e9).id == "recomp-hd"); // chosen after every install: kept
    CHECK(psp_texpacks_pick(v, "recomp-hd", 1500).id == "textures"); // a pack installed after the choice: the new one
    CHECK(psp_texpacks_pick(v, "gone", 9e9).id == "textures");       // the chosen pack was deleted: the newest

    setenv("PSPRECOMP_TEXPACK_ID", "my-pack", 1);
    setenv("PSPRECOMP_TEXPACK_AT", "99999999999", 1);
    CHECK(psp_texpacks_active_id(m.string()) == "my-pack");
    CHECK(psp_texpacks_active_dir(m.string()) == (m / "texpacks" / "my-pack").string());

    std::string err;
    CHECK(psp_texpacks_remove(m.string(), "my-pack", &err) && !fs::exists(m / "texpacks" / "my-pack"));
    CHECK(!psp_texpacks_remove(m.string(), "my-pack", &err) && !err.empty());
    CHECK(!psp_texpacks_remove(m.string(), "..", &err));             // only listed packs
    CHECK(psp_texpacks_active_id(m.string()) == "textures");         // the chosen one is gone: the newest

    {   // installing an archive (Settings > Graphics > Install a Texture Pack)
        const fs::path t = m.parent_path() / "in";
        fs::create_directories(t);
        const std::string big(300000, 'p');   // a "PNG" larger than one read buffer
        for (bool deflate : {false, true}) {
            const fs::path zp = t / (deflate ? "My Pack v2 (US).zip" : "stored.zip");
            write_zip(zp, {{"My Pack v2/", ""}, {"My Pack v2/readme.txt", "outside"}, {"My Pack v2/ULUS10566/textures.ini", "[options]\n"},
                           {"My Pack v2/ULUS10566/chars/a.png", big}, {"My Pack v2/ULUS10566/deeper/textures.ini", "no"},
                           {"__MACOSX/My Pack v2/ULUS10566/._a.png", "junk"}, {"My Pack v2/ULUS10566/../../evil.png", "x"}}, deflate);
            std::atomic<int> prog{-1};
            std::string name, err;
            CHECK(psp_texpacks_install(m.string(), zp.string(), zp.filename().string(), &prog, &name, &err));
            const fs::path d = m / "texpacks" / (deflate ? "my-pack-v2-us" : "stored");
            CHECK(err.empty() && prog.load() == 100 && name == (deflate ? "My Pack v2 (US)" : "stored"));
            CHECK(slurp(d / "textures.ini") == "[options]\n" && slurp(d / "chars" / "a.png") == big);
            CHECK(fs::exists(d / "deeper" / "textures.ini") && !fs::exists(d / "readme.txt") && !fs::exists(m / "evil.png"));
            CHECK(fs::exists(d / ".source.json") && fs::exists(d / "PACK_CREDITS.txt"));
        }
        CHECK(psp_texpacks_active_id(m.string()) != "");   // listed
        bool found = false;
        for (const auto& p : psp_texpacks_list(m.string())) if (p.id == "my-pack-v2-us") found = p.name == "My Pack v2 (US)";
        CHECK(found);
        write_zip(t / "ids.zip", {{"up/textures/0123456789ABCDEF.png", "x"}, {"up/README.md", "y"}}, true);
        std::string name, err;
        CHECK(psp_texpacks_install(m.string(), (t / "ids.zip").string(), "", nullptr, &name, &err));
        CHECK(fs::exists(m / "texpacks" / "ids" / "0123456789abcdef.png") && !fs::exists(m / "texpacks" / "ids" / "README.md"));
        // the same name again replaces it; a bad archive leaves the installed pack alone
        write_zip(t / "ids.zip", {{"0000000000000001.png", "z"}}, false);
        CHECK(psp_texpacks_install(m.string(), (t / "ids.zip").string(), "ids.zip", nullptr, &name, &err));
        CHECK(fs::exists(m / "texpacks" / "ids" / "0000000000000001.png") && !fs::exists(m / "texpacks" / "ids" / "0123456789abcdef.png"));
        write_zip(t / "photos.zip", {{"cat.png", "x"}}, false);
        CHECK(!psp_texpacks_install(m.string(), (t / "photos.zip").string(), "ids.zip", nullptr, &name, &err) && err.find("textures.ini") != std::string::npos);
        CHECK(fs::exists(m / "texpacks" / "ids" / "0000000000000001.png") && !fs::exists(m / "texpacks" / ".new-ids"));
        std::ofstream(t / "x.bin") << "hello";
        CHECK(!psp_texpacks_install(m.string(), (t / "x.bin").string(), "", nullptr, &name, &err) && err.find("not a .zip") != std::string::npos);
        CHECK(psp_texpacks_id_for("My Pack v2 (US).zip") == "my-pack-v2-us");
        CHECK(psp_texpacks_id_for("recomp-hd.zip") == "my-pack" && psp_texpacks_id_for("C:\\dl\\Pack.RAR") == "pack");
    }
    fs::remove_all(m.parent_path());
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("texpacks: all checks passed\n");
    return 0;
}
