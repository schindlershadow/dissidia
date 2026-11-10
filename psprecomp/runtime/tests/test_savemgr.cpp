// Host test for the save manager (psp_savemgr) and its in-game screen (psp_savemgr_ui): synthetic PPSSPP-style
// source folders, the import / export flows driven with pad presses, replace-with-backup, shared region IDs, a
// typed path, and (when present) the real PPSSPP save folder, read-only.
#include "psp_savemgr.h"
#include "psp_savemgr_ui.h"
#include "psp_savedata_crypto.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using namespace psp_sddialog;
using Screen = ManagerDialog::Screen;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using Bytes = std::vector<uint8_t>;
static Bytes slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return Bytes(std::istreambuf_iterator<char>(f), {}); }
static void spit(const fs::path& p, const Bytes& b) { std::ofstream f(p, std::ios::binary); f.write((const char*)b.data(), (std::streamsize)b.size()); }

struct E { const char* key; uint16_t fmt; uint32_t max; Bytes v; };
static Bytes make_sfo(const std::vector<E>& es) {
    Bytes keys; std::vector<uint32_t> kofs;
    for (auto& e : es) { kofs.push_back((uint32_t)keys.size()); for (const char* c = e.key; *c; ++c) keys.push_back((uint8_t)*c); keys.push_back(0); }
    uint32_t kt = 20 + 16 * (uint32_t)es.size(), dt = (kt + (uint32_t)keys.size() + 3) & ~3u;
    Bytes out(dt + 0, 0);
    auto w32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) out[o + i] = uint8_t(v >> (8 * i)); };
    auto w16 = [&](size_t o, uint16_t v) { out[o] = uint8_t(v); out[o + 1] = uint8_t(v >> 8); };
    std::memcpy(out.data(), "\0PSF", 4); w32(4, 0x101); w32(8, kt); w32(12, dt); w32(16, (uint32_t)es.size());
    uint32_t doff = 0;
    for (size_t i = 0; i < es.size(); ++i) {
        w16(20 + 16 * i, (uint16_t)kofs[i]); w16(22 + 16 * i, es[i].fmt); w32(24 + 16 * i, (uint32_t)es[i].v.size());
        w32(28 + 16 * i, es[i].max); w32(32 + 16 * i, doff); doff += es[i].max;
    }
    std::memcpy(out.data() + kt, keys.data(), keys.size());
    for (auto& e : es) { Bytes d = e.v; d.resize(e.max, 0); out.insert(out.end(), d.begin(), d.end()); }
    while (out.size() % 16) out.push_back(0);
    return out;
}
static Bytes str(const std::string& s) { Bytes b(s.begin(), s.end()); b.push_back(0); return b; }

static void make_save(const fs::path& root, const std::string& name, const std::string& title, const std::string& detail,
                      const std::string& file, uint8_t seed) {
    fs::path d = root / name;
    fs::create_directories(d);
    Bytes fl(3168, 0);
    std::memcpy(fl.data(), file.c_str(), file.size());
    Bytes params(128, 0); params[0] = 0x41;
    Bytes sfo = make_sfo({{"CATEGORY", 0x204, 4, str("MS")}, {"SAVEDATA_DETAIL", 0x204, 1024, str(detail)},
                          {"SAVEDATA_DIRECTORY", 0x204, 64, str(name)}, {"SAVEDATA_FILE_LIST", 4, 3168, fl},
                          {"SAVEDATA_PARAMS", 4, 128, params}, {"SAVEDATA_TITLE", 0x204, 128, str("Slot")},
                          {"TITLE", 0x204, 128, str(title)}});
    // sign it the way the runtime does (so a re-sign of an unchanged file must be a no-op)
    {
        auto g32 = [&](size_t o) { return uint32_t(sfo[o] | sfo[o + 1] << 8 | sfo[o + 2] << 16 | uint32_t(sfo[o + 3]) << 24); };
        auto g16 = [&](size_t o) { return uint16_t(sfo[o] | sfo[o + 1] << 8); };
        uint32_t kt = g32(8), dt = g32(12), n = g32(16);
        for (uint32_t i = 0; i < n; ++i)
            if (std::strcmp((const char*)&sfo[kt + g16(20 + 16 * i)], "SAVEDATA_PARAMS") == 0)
                psp_sdcrypto::update_sfo_hash(sfo, dt + g32(20 + 16 * i + 12), psp_sdcrypto::crypt_mode_from_params_flag(0x41));
    }
    spit(d / "PARAM.SFO", sfo);
    Bytes data(300);
    for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(seed * 7 + i);
    spit(d / file, data);
    spit(d / "ICON0.PNG", Bytes{1, 2, 3});
    spit(d / "stray.sync-conflict.BIN", Bytes{9});
}

// ---- driving the dialog ----
struct Drv {
    ManagerDialog d;
    int64_t t = 1000;
    void tick(uint32_t b) { d.update(b, t); t += 100; }
    void tap(uint32_t b) { tick(b); tick(0); }
};
constexpr uint32_t UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80, CROSS = 0x4000, CIRCLE = 0x2000;
static void down(Drv& v, int n) { for (int i = 0; i < n; ++i) v.tap(DOWN); }

static std::string existing_in(const std::string& root, const std::string& suffix) {
    for (const char* id : {"ULUS10566", "ULES01505"})
        if (fs::is_directory(fs::path(root) / (std::string(id) + suffix))) return std::string(id) + suffix;
    return "";
}

static ManagerConfig make_cfg(const fs::path& saves, const fs::path& bak, std::vector<psp_savemgr::Source> src_imp,
                              std::vector<psp_savemgr::Source> src_exp) {
    ManagerConfig c;
    c.saves_root = saves.string(); c.backup_root = bak.string(); c.disc_id = "ULES01505";
    c.existing = [r = saves.string()](const std::string& s) { return existing_in(r, s); };
    c.detect = [=](bool with_saves) { return with_saves ? src_imp : src_exp; };
    return c;
}

static bool resign_is_noop(const fs::path& sfo_path) {
    Bytes sfo = slurp(sfo_path), orig = sfo;
    // locate SAVEDATA_PARAMS through the SFO index
    auto g32 = [&](size_t o) { return uint32_t(sfo[o] | sfo[o + 1] << 8 | sfo[o + 2] << 16 | uint32_t(sfo[o + 3]) << 24); };
    auto g16 = [&](size_t o) { return uint16_t(sfo[o] | sfo[o + 1] << 8); };
    uint32_t kt = g32(8), dt = g32(12), n = g32(16);
    for (uint32_t i = 0; i < n; ++i)
        if (std::strcmp((const char*)&sfo[kt + g16(20 + 16 * i)], "SAVEDATA_PARAMS") == 0) {
            size_t off = dt + g32(20 + 16 * i + 12);
            int mode = psp_sdcrypto::crypt_mode_from_params_flag(sfo[off]);
            if (mode <= 0) return true;
            return psp_sdcrypto::update_sfo_hash(sfo, off, mode) == 0 && sfo == orig;
        }
    return false;
}

static void test_synthetic(const fs::path& T) {
    const fs::path src = T / "ppsspp", saves = T / "game" / "SAVEDATA", bak = T / "game" / "SAVEDATA_BACKUP", stick = T / "stick" / "PSP" / "SAVEDATA";
    fs::create_directories(saves); fs::create_directories(stick);
    make_save(src, "ULES01505GameData00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Ender\nLevel 99", "DISSIDIA.BIN", 1);
    make_save(src, "ULUS10566GameData00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Old", "DISSIDIA.BIN", 2);
    make_save(src, "ULES01505FCard00", "DISSIDIA 012 FINAL FANTASY", "friend", "FCARD.BIN", 3);
    make_save(src, "ULJM05500", "DISSIDIA FINAL FANTASY", "first game", "X.BIN", 4);
    make_save(src, "UCUS98711_DATA01", "Some Other Game", "x", "Y.BIN", 5);

    auto found = psp_savemgr::scan(src.string());
    CHECK(found.size() == 3);                                   // only Dissidia 012
    CHECK(psp_savemgr::scan(src.string(), false).size() == 5);
    CHECK(found[0].name == "ULES01505FCard00" && found[0].bytes > 300);
    CHECK(std::string(psp_savemgr::region_of("ULUS10566")) == "US");

    // ---- import: one per slot, this region first ----
    Drv v;
    v.d.begin(make_cfg(saves, bak, {{"PPSSPP", src.string()}}, {{"Stick", stick.parent_path().parent_path().string()}, {"Stick2", stick.string()}}));
    v.tick(0);                                           // arms the dialog
    CHECK(v.d.screen() == Screen::Main);
    v.tap(CROSS);                                        // Import saves
    CHECK(v.d.screen() == Screen::PickSource);
    v.tap(CROSS);                                        // PPSSPP
    CHECK(v.d.screen() == Screen::PickSaves);
    if (const char* dir = std::getenv("SAVEMGR_SHOTS")) {
        Canvas k(2);
        v.d.render(k);
        FILE* f = std::fopen((std::string(dir) + "/pick_saves.ppm").c_str(), "wb");
        if (f) {
            std::fprintf(f, "P6\n%d %d\n255\n", k.w, k.h);
            for (uint32_t px : k.px) { unsigned char rgb[3] = {(unsigned char)px, (unsigned char)(px >> 8), (unsigned char)(px >> 16)}; std::fwrite(rgb, 1, 3, f); }
            std::fclose(f);
        }
    }
    v.tap(CROSS);                                        // "All saves" -> already the default -> clear
    v.tap(CROSS);                                        // ... and again -> default (ULES one of GameData00 + FCard00)
    down(v, 4); 
    CHECK(v.d.selected() == 4);                          // Import selected
    v.tap(CROSS);
    CHECK(v.d.screen() == Screen::Result);
    CHECK(v.d.changed());
    CHECK(fs::is_directory(saves / "ULES01505GameData00") && fs::is_directory(saves / "ULES01505FCard00"));
    CHECK(!fs::exists(saves / "ULUS10566GameData00"));
    CHECK(slurp(saves / "ULES01505GameData00" / "DISSIDIA.BIN") == slurp(src / "ULES01505GameData00" / "DISSIDIA.BIN"));
    CHECK(!fs::exists(saves / "ULES01505GameData00" / "stray.sync-conflict.BIN"));
    CHECK(slurp(saves / "ULES01505FCard00" / "PARAM.SFO") == slurp(src / "ULES01505FCard00" / "PARAM.SFO"));   // same name: byte-identical
    // ---- importing again: conflicts -> Confirm; Skip leaves things alone ----
    v.tap(CROSS);                                        // OK -> Main
    CHECK(v.d.screen() == Screen::Main);
    v.tap(CROSS); v.tap(CROSS);
    down(v, 4); v.tap(CROSS);
    CHECK(v.d.screen() == Screen::Confirm);
    down(v, 1); v.tap(CROSS);                            // Skip existing
    CHECK(v.d.screen() == Screen::Result && !fs::exists(bak));
    // ---- ... and Replace keeps the old copy ----
    v.tap(CROSS); v.tap(CROSS); v.tap(CROSS);
    down(v, 4); v.tap(CROSS);
    CHECK(v.d.screen() == Screen::Confirm);
    v.tap(CROSS);                                        // Replace them
    CHECK(v.d.screen() == Screen::Result);
    CHECK(fs::is_directory(bak / "ULES01505GameData00.bak") && fs::is_directory(bak / "ULES01505FCard00.bak"));
    CHECK(!fs::exists(saves / "ULES01505GameData00.bak"));   // never inside the folder the game lists

    // ---- a save already stored under the other region's ID is replaced in place ----
    fs::remove_all(saves); fs::remove_all(bak); fs::create_directories(saves);
    psp_savemgr::SaveInfo us;
    CHECK(psp_savemgr::read_save((src / "ULUS10566GameData00").string(), &us));
    std::string dn;
    CHECK(psp_savemgr::copy_save(us, saves.string(), "ULUS10566", false, &dn).empty());
    CHECK(resign_is_noop(saves / "ULUS10566GameData00" / "PARAM.SFO"));
    Drv w;
    w.d.begin(make_cfg(saves, bak, {{"PPSSPP", src.string()}}, {}));
    w.tick(0); w.tap(CROSS); w.tap(CROSS);
    down(w, 4); w.tap(CROSS);
    CHECK(w.d.screen() == Screen::Confirm);
    w.tap(CROSS);
    CHECK(fs::is_directory(saves / "ULUS10566GameData00") && !fs::exists(saves / "ULES01505GameData00"));
    CHECK(slurp(saves / "ULUS10566GameData00" / "DISSIDIA.BIN") == slurp(src / "ULES01505GameData00" / "DISSIDIA.BIN"));
    CHECK(fs::is_directory(bak / "ULUS10566GameData00.bak"));
    CHECK(resign_is_noop(saves / "ULUS10566GameData00" / "PARAM.SFO"));   // retargeted + signed again, consistent
    {
        Bytes sfo = slurp(saves / "ULUS10566GameData00" / "PARAM.SFO");
        std::string s((const char*)sfo.data(), sfo.size());
        CHECK(s.find("ULUS10566GameData00") != std::string::npos);
    }
    CHECK(fs::exists(saves / "ULES01505FCard00"));

    // ---- export, renaming to this game's ID ----
    const fs::path out = T / "stick" / "PSP" / "SAVEDATA";
    Drv x;
    x.d.begin(make_cfg(saves, bak, {}, {{"Stick", out.string()}}));
    x.tick(0);
    down(x, 1); x.tap(CROSS);                            // Export saves
    CHECK(x.d.screen() == Screen::PickDest);
    x.tap(CROSS);                                        // Stick
    CHECK(x.d.screen() == Screen::PickSaves);
    down(x, 3);                                          // all, save1, save2, then "Folder names"
    x.tap(RIGHT);                                        // keep region -> use the game's ID
    down(x, 1);
    x.tap(CROSS);
    CHECK(x.d.screen() == Screen::Result);
    CHECK(fs::is_directory(out / "ULES01505GameData00") && fs::is_directory(out / "ULES01505FCard00"));
    CHECK(resign_is_noop(out / "ULES01505GameData00" / "PARAM.SFO"));
    CHECK(!x.d.changed());

    // ---- a typed path ----
    Drv y;
    y.d.begin(make_cfg(T / "game2", bak, {}, {}));
    fs::create_directories(T / "game2");
    y.tick(0); y.tap(CROSS);
    CHECK(y.d.screen() == Screen::PickSource);
    y.tap(CROSS);                                        // only "Type a path..." is on the list
    CHECK(y.d.screen() == Screen::TypePath);
    for (char c : src.string()) osk_push_key((uint32_t)(unsigned char)c);
    osk_push_key(13);
    y.tick(0); y.tick(0);
    CHECK(y.d.screen() == Screen::PickSaves);
    // a wrong path comes back with a note
    Drv z;
    z.d.begin(make_cfg(T / "game2", bak, {}, {}));
    z.tick(0); z.tap(CROSS); z.tap(CROSS);
    for (char c : std::string("/nonexistent/place")) osk_push_key((uint32_t)(unsigned char)c);
    osk_push_key(13);
    z.tick(0); z.tick(0);
    CHECK(z.d.screen() == Screen::PickSource);

    // ---- rendering does not crash on every screen ----
    Canvas c(1);
    x.d.render(c); y.d.render(c); z.d.render(c); v.d.render(c);
    if (const char* dir = std::getenv("SAVEMGR_SHOTS")) {   // PPM dumps of a few screens, for a visual check
        auto dump = [&](const char* name, const ManagerDialog& d) {
            Canvas k(2);
            d.render(k);
            FILE* f = std::fopen((std::string(dir) + "/" + name + ".ppm").c_str(), "wb");
            if (!f) return;
            std::fprintf(f, "P6\n%d %d\n255\n", k.w, k.h);
            for (uint32_t px : k.px) { unsigned char rgb[3] = {(unsigned char)px, (unsigned char)(px >> 8), (unsigned char)(px >> 16)}; std::fwrite(rgb, 1, 3, f); }
            std::fclose(f);
        };
        dump("export_saves", x.d);
        dump("typed_path", y.d);
        dump("pick_source_note", z.d);
        dump("import_result", v.d);
    }
}

static void test_real(const fs::path& T) {
    const char* e = std::getenv("PSPRECOMP_TEST_PPSSPP_SAVES");
    std::string root = e ? e : std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.config/ppsspp/PSP/SAVEDATA";
    if (!fs::is_directory(root)) { std::printf("real PPSSPP save folder %s not present: skipped\n", root.c_str()); return; }
    auto before = psp_savemgr::scan(root);
    for (auto& b : before) {   // info: how each save is encrypted (PARAM.SFO's SAVEDATA_PARAMS flag) and its data file sizes
        Bytes sfo = slurp(fs::path(b.path) / "PARAM.SFO");
        auto g32 = [&](size_t o) { return uint32_t(sfo[o] | sfo[o + 1] << 8 | sfo[o + 2] << 16 | uint32_t(sfo[o + 3]) << 24); };
        auto g16 = [&](size_t o) { return uint16_t(sfo[o] | sfo[o + 1] << 8); };
        uint32_t kt = g32(8), dt = g32(12), n = g32(16);
        for (uint32_t i = 0; i < n; ++i)
            if (std::strcmp((const char*)&sfo[kt + g16(20 + 16 * i)], "SAVEDATA_PARAMS") == 0)
                std::printf("  %-22s params flag 0x%02x, PARAM.SFO %zu bytes, %llu bytes of files\n", b.name.c_str(),
                            sfo[dt + g32(20 + 16 * i + 12)], sfo.size(), (unsigned long long)b.bytes);
    }
    std::printf("real PPSSPP folder: %zu Dissidia 012 save(s):", before.size());
    for (auto& s : before) std::printf(" %s", s.name.c_str());
    std::printf("\n");
    CHECK(!before.empty());
    // snapshot of everything read-only we care about
    auto snap = [&] {
        std::string r;
        for (auto& s : psp_savemgr::scan(root))
            for (auto& f : fs::directory_iterator(s.path))
                r += f.path().string() + ":" + std::to_string(f.file_size()) + ":" + std::to_string(f.last_write_time().time_since_epoch().count()) + ";";
        return r;
    };
    const std::string snap0 = snap();

    const fs::path saves = T / "real" / "SAVEDATA", bak = T / "real" / "BAK";
    fs::create_directories(saves);
    Drv v;
    v.d.begin(make_cfg(saves, bak, {{"PPSSPP", root}}, {}));
    v.tick(0); v.tap(CROSS); v.tap(CROSS);
    CHECK(v.d.screen() == Screen::PickSaves);
    // start row = 1 + number of saves (import)
    const int start = 1 + (int)before.size() - 0;   // rows: all, saves..., start
    (void)start;
    // go to the Start row by pressing Down until Import selected is under the cursor: Start is the second-to-last row
    for (int i = 0; i < (int)before.size() + 1; ++i) v.tap(DOWN);
    v.tap(CROSS);
    CHECK(v.d.screen() == Screen::Result);
    int ok = 0;
    for (auto& s : psp_savemgr::scan(saves.string())) {
        ++ok;
        std::printf("  imported %s (%s)\n", s.name.c_str(), psp_savemgr::one_line(s.detail).c_str());
        CHECK(s.disc == "ULES01505");
        // same region, same name: the copy is byte-identical (a PSP's own signature is not touched)
        CHECK(slurp(fs::path(s.path) / "PARAM.SFO") == slurp(fs::path(root) / s.name / "PARAM.SFO"));
        // data files are copied byte for byte from the source with the same suffix and region
        for (auto& b : before)
            if (b.suffix == s.suffix && b.disc == s.disc)
                for (auto& f : fs::directory_iterator(b.path)) {
                    std::string n = f.path().filename().string();
                    if (n == "PARAM.SFO" || !fs::exists(fs::path(s.path) / n)) continue;
                    CHECK(slurp(f.path()) == slurp(fs::path(s.path) / n));
                }
    }
    CHECK(ok > 0);
    CHECK(fs::is_directory(saves / "ULES01505GameData00"));   // the user's EU game data came across
    CHECK(snap() == snap0);                                   // the PPSSPP folder was not touched
    std::printf("real PPSSPP import: %d save(s) imported into a scratch folder\n", ok);
    // and back out, renamed to the other region
    const fs::path out = T / "real" / "OUT";
    Drv x;
    ManagerConfig cfg = make_cfg(saves, bak, {}, {{"Scratch", out.string()}});
    cfg.disc_id = "ULUS10566";
    x.d.begin(cfg);
    x.tick(0); down(x, 1); x.tap(CROSS); x.tap(CROSS);
    CHECK(x.d.screen() == Screen::PickSaves);
    for (int i = 0; i < ok + 1; ++i) x.tap(DOWN);
    x.tap(RIGHT);
    x.tap(DOWN);
    x.tap(CROSS);
    CHECK(x.d.screen() == Screen::Result);
    CHECK(fs::is_directory(out / "ULUS10566GameData00"));
    CHECK(resign_is_noop(out / "ULUS10566GameData00" / "PARAM.SFO"));
}

int main() {
    char tmpl[] = "/tmp/savemgrtestXXXXXX";
    if (!mkdtemp(tmpl)) return 2;
    const fs::path T = tmpl;
    test_synthetic(T / "syn");
    test_real(T);
    if (!std::getenv("KEEP")) fs::remove_all(T);
    else std::printf("kept %s\n", tmpl);
    if (g_fail) { std::printf("test_savemgr: %d FAILURES\n", g_fail); return 1; }
    std::printf("test_savemgr: all passed\n");
    return 0;
}
