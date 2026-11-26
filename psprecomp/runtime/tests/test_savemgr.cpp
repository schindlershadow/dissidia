// Host test for save management: psp_savemgr (regions, finding saves anywhere below a folder, renamed folders, the Japanese
// title), psp_savearchive (zip / tar.gz in and out, unsafe archives refused), psp_savebackup (automatic backups, grouping,
// daily, prune, legacy .bak), and Settings > Saves (psp_saves_page) driven like the menu does: import from a found source,
// from a folder / archive the (faked) system file dialog returns, a typed path, the in-game browser, export to a folder and
// to a .zip, replace-with-backup, Back up now and restore, and the cloud conflict screen with a fake cloud. Also the real
// PPSSPP save folder, read-only, when it exists (~/.config/ppsspp/PSP/SAVEDATA or PSPRECOMP_TEST_PPSSPP_SAVES).
// DUMP_SAVES=/dir writes PPM pictures of the page as the settings screen draws it.
#include "psp_savemgr.h"
#include "psp_savearchive.h"
#include "psp_savebackup.h"
#include "psp_saves_page.h"
#include "psp_savedata_crypto.h"

#include <functional>
#include <algorithm>
#include <map>
#include <set>
#include <chrono>
#include <ctime>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using psp_sddialog::SetKind;

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

// ---------------------------------------------------------------------------------------------------- library
static std::set<std::string> names_of(const std::vector<psp_savemgr::SaveInfo>& v) {
    std::set<std::string> r;
    for (auto& s : v) r.insert(s.name);
    return r;
}

static void test_library(const fs::path& T) {
    const fs::path src = T / "ppsspp";
    make_save(src, "ULES01505GameData00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Ender\nLevel 99", "DISSIDIA.BIN", 1);
    make_save(src, "ULUS10566GameData00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Old", "DISSIDIA.BIN", 2);
    make_save(src, "ULES01505FCard00", "DISSIDIA 012 FINAL FANTASY", "friend", "FCARD.BIN", 3);
    make_save(src, "ULJM05500", "DISSIDIA FINAL FANTASY", "first game", "X.BIN", 4);
    make_save(src, "UCUS98711_DATA01", "Some Other Game", "x", "Y.BIN", 5);
    // Japanese disc, Japanese title (no "Dissidia" in Latin letters)
    make_save(src, "ULJM05814GameData01", "\xE3\x83\x87\xE3\x82\xA3\xE3\x82\xB7\xE3\x83\x87\xE3\x82\xA3\xE3\x82\xA2 \xE3\x83\x87\xE3\x83\xA5\xE3\x82\xAA\xE3\x83\x87\xE3\x82\xB7\xE3\x83\xA0", "jp", "DISSIDIA.BIN", 6);
    // a JP PSN save whose title is something unexpected: taken by its disc ID
    make_save(src, "NPJH50377GameData02", "???", "jp psn", "DISSIDIA.BIN", 7);

    auto found = psp_savemgr::scan(src.string());
    CHECK(found.size() == 5);
    CHECK(names_of(found).count("ULJM05814GameData01") && names_of(found).count("NPJH50377GameData02"));
    CHECK(!names_of(found).count("ULJM05500"));
    CHECK(psp_savemgr::scan(src.string(), false).size() == 7);
    CHECK(std::string(psp_savemgr::region_of("ULUS10566")) == "US");
    CHECK(std::string(psp_savemgr::region_of("NPJH50377")) == "JP");

    // find_saves: nested PSP/SAVEDATA, a single save folder, a renamed folder (name from SAVEDATA_DIRECTORY)
    const fs::path nest = T / "nest";
    make_save(nest / "stick" / "PSP" / "SAVEDATA", "ULUS10566GameData00", "DISSIDIA 012 FINAL FANTASY", "nested", "DISSIDIA.BIN", 8);
    make_save(nest / "backup 2024", "ULES01505FCard02", "DISSIDIA 012 FINAL FANTASY", "card", "FCARD.BIN", 9);
    fs::rename(nest / "backup 2024" / "ULES01505FCard02", nest / "backup 2024" / "my friend card");
    make_save(nest / ".hidden", "ULES01505Quest00", "DISSIDIA 012 FINAL FANTASY", "hidden", "Q.BIN", 10);
    auto deep = psp_savemgr::find_saves(nest.string());
    CHECK(deep.size() == 2);
    CHECK(names_of(deep).count("ULUS10566GameData00") && names_of(deep).count("ULES01505FCard02"));
    CHECK(psp_savemgr::find_saves((nest / "stick" / "PSP" / "SAVEDATA" / "ULUS10566GameData00").string()).size() == 1);
    CHECK(psp_savemgr::scan((nest / "backup 2024").string()).empty());   // the game itself would not find a renamed folder
    CHECK(psp_savemgr::find_saves(nest.string(), 1).empty());             // depth limit
}

// ---------------------------------------------------------------------------------------------------- archives
static Bytes zip_with_name(const std::string& name, const Bytes& data) {   // one stored entry, for refusal tests
    Bytes z;
    auto p16 = [&](uint16_t v) { z.push_back(uint8_t(v)); z.push_back(uint8_t(v >> 8)); };
    auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) z.push_back(uint8_t(v >> (8 * i))); };
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data) { crc ^= b; for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1))); }
    crc = ~crc;
    p32(0x04034b50); p16(20); p16(0); p16(0); p16(0); p16(0); p32(crc); p32(uint32_t(data.size())); p32(uint32_t(data.size())); p16(uint16_t(name.size())); p16(0);
    z.insert(z.end(), name.begin(), name.end()); z.insert(z.end(), data.begin(), data.end());
    const uint32_t cd = uint32_t(z.size());
    p32(0x02014b50); p16(20); p16(20); p16(0); p16(0); p16(0); p16(0); p32(crc); p32(uint32_t(data.size())); p32(uint32_t(data.size()));
    p16(uint16_t(name.size())); p16(0); p16(0); p16(0); p16(0); p32(0); p32(0);
    z.insert(z.end(), name.begin(), name.end());
    const uint32_t cds = uint32_t(z.size()) - cd;
    p32(0x06054b50); p16(0); p16(0); p16(1); p16(1); p32(cds); p32(cd); p16(0);
    return z;
}

static void test_archives(const fs::path& T) {
    const fs::path src = T / "src";
    make_save(src, "ULUS10566GameData00", "DISSIDIA 012 FINAL FANTASY", "zip me", "DISSIDIA.BIN", 11);
    make_save(src, "ULUS10566FCard01", "DISSIDIA 012 FINAL FANTASY", "card", "FCARD.BIN", 12);
    const fs::path zip = T / "out" / "saves.zip";
    CHECK(psp_savearchive::write_zip(zip.string(), {(src / "ULUS10566GameData00").string(), (src / "ULUS10566FCard01").string()}, "PSP/SAVEDATA").empty());
    CHECK(psp_savearchive::detect(zip.string()) == psp_savearchive::Kind::Zip);
    CHECK(psp_savearchive::extract(zip.string(), (T / "x1").string()).empty());
    auto got = psp_savemgr::find_saves((T / "x1").string());
    CHECK(got.size() == 2);
    CHECK(slurp(T / "x1" / "PSP" / "SAVEDATA" / "ULUS10566GameData00" / "DISSIDIA.BIN") == slurp(src / "ULUS10566GameData00" / "DISSIDIA.BIN"));
    CHECK(slurp(T / "x1" / "PSP" / "SAVEDATA" / "ULUS10566GameData00" / "PARAM.SFO") == slurp(src / "ULUS10566GameData00" / "PARAM.SFO"));
    // what another tool makes (system zip / tar), when present
    if (std::system(("cd '" + src.string() + "' && zip -qr '" + (T / "sys.zip").string() + "' ULUS10566GameData00 2>/dev/null").c_str()) == 0) {
        CHECK(psp_savearchive::extract((T / "sys.zip").string(), (T / "x2").string()).empty());
        CHECK(psp_savemgr::find_saves((T / "x2").string()).size() == 1);
    } else std::printf("zip not installed: system zip case skipped\n");
    if (std::system(("tar -czf '" + (T / "s.tar.gz").string() + "' -C '" + src.string() + "' ULUS10566FCard01 2>/dev/null").c_str()) == 0) {
        CHECK(psp_savearchive::detect((T / "s.tar.gz").string()) == psp_savearchive::Kind::TarGz);
        CHECK(psp_savearchive::extract((T / "s.tar.gz").string(), (T / "x3").string()).empty());
        CHECK(psp_savemgr::find_saves((T / "x3").string()).size() == 1);
    } else std::printf("tar not installed: tar.gz case skipped\n");
    // unsafe names never leave the folder
    spit(T / "evil.zip", zip_with_name("../escape.txt", Bytes{1, 2, 3}));
    CHECK(!psp_savearchive::extract((T / "evil.zip").string(), (T / "x4").string()).empty());
    CHECK(!fs::exists(T / "escape.txt"));
    spit(T / "abs.zip", zip_with_name("/tmp/abs_escape.txt", Bytes{1}));
    CHECK(!psp_savearchive::extract((T / "abs.zip").string(), (T / "x5").string()).empty());
    // damaged
    Bytes bad = slurp(zip);
    {   // a byte inside DISSIDIA.BIN's data (just after its local header): the CRC check catches it
        const std::string n = "ULUS10566GameData00/DISSIDIA.BIN";
        const auto it = std::search(bad.begin(), bad.end(), n.begin(), n.end());
        CHECK(it != bad.end());
        if (it != bad.end()) bad[size_t(it - bad.begin()) + n.size() + 4] ^= 0xFF;
    }
    spit(T / "bad.zip", bad);
    CHECK(!psp_savearchive::extract((T / "bad.zip").string(), (T / "x6").string()).empty());
    // not an archive
    spit(T / "plain.txt", Bytes{'h', 'i'});
    CHECK(psp_savearchive::detect((T / "plain.txt").string()) == psp_savearchive::Kind::None);
    CHECK(!psp_savearchive::extract((T / "plain.txt").string(), (T / "x7").string()).empty());
    CHECK(psp_savearchive::looks_like_archive("Saves.TAR.GZ") && psp_savearchive::looks_like_archive("a.7z") && !psp_savearchive::looks_like_archive("a.bin"));
}

// ---------------------------------------------------------------------------------------------------- backups
static void test_backups(const fs::path& T) {
    const fs::path saves = T / "SAVEDATA", bak = T / "BACKUP";
    make_save(saves, "ULES01505GameData00", "DISSIDIA 012 FINAL FANTASY", "gd", "DISSIDIA.BIN", 20);
    make_save(saves, "ULES01505FCard00", "DISSIDIA 012 FINAL FANTASY", "fc", "FCARD.BIN", 21);
    CHECK(psp_savebackup::list(bak.string()).empty());
    CHECK(psp_savebackup::daily(saves.string(), bak.string()));
    CHECK(!psp_savebackup::daily(saves.string(), bak.string()));   // once a day
    auto l = psp_savebackup::list(bak.string());
    CHECK(l.size() == 1 && l[0].reason == psp_savebackup::kDaily && l[0].saves.size() == 2);
    // two saves replaced by one occasion share one backup
    const Bytes gd = slurp(saves / "ULES01505GameData00" / "DISSIDIA.BIN");
    CHECK(psp_savebackup::set_aside((saves / "ULES01505GameData00").string(), bak.string(), psp_savebackup::kCloudDownload).empty());
    CHECK(psp_savebackup::set_aside((saves / "ULES01505FCard00").string(), bak.string(), psp_savebackup::kCloudDownload).empty());
    CHECK(!fs::exists(saves / "ULES01505GameData00"));
    l = psp_savebackup::list(bak.string());
    CHECK(l.size() == 2);
    const psp_savebackup::Backup* cd = nullptr;
    for (auto& b : l) if (b.reason == psp_savebackup::kCloudDownload) cd = &b;
    CHECK(cd && cd->saves.size() == 2);
    if (cd) CHECK(slurp(fs::path(cd->path) / "ULES01505GameData00" / "DISSIDIA.BIN") == gd);
    CHECK(std::string(psp_savebackup::reason_text(psp_savebackup::kCloudDownload)).find("cloud") != std::string::npos);
    // an older runtime's <name>.bak folder is listed with its real name
    make_save(bak, "ULES01505Quest00", "DISSIDIA 012 FINAL FANTASY", "q", "Q.BIN", 22);
    fs::rename(bak / "ULES01505Quest00", bak / "ULES01505Quest00.bak");
    l = psp_savebackup::list(bak.string());
    bool legacy = false;
    for (auto& b : l) if (b.reason == psp_savebackup::kLegacy) { legacy = true; CHECK(b.saves.size() == 1 && b.saves[0].name == "ULES01505Quest00"); }
    CHECK(legacy);
    // prune keeps the newest automatic ones; manual ones stay
    make_save(saves, "ULES01505GameData00", "DISSIDIA 012 FINAL FANTASY", "gd2", "DISSIDIA.BIN", 23);
    std::string mid;
    CHECK(psp_savebackup::snapshot(saves.string(), bak.string(), psp_savebackup::kManual, {}, &mid).empty() && !mid.empty());
    for (int i = 0; i < 4; ++i) CHECK(psp_savebackup::snapshot(saves.string(), bak.string(), psp_savebackup::kImport).empty());
    psp_savebackup::prune(bak.string(), 2, 1);
    int autos = 0, manual = 0;
    for (auto& b : psp_savebackup::list(bak.string())) {
        if (b.reason == psp_savebackup::kManual) ++manual;
        else if (b.reason != psp_savebackup::kLegacy && b.reason != psp_savebackup::kDaily) ++autos;
    }
    CHECK(autos == 2 && manual == 1);
    CHECK(psp_savebackup::remove(bak.string(), mid).empty());
    CHECK(!psp_savebackup::remove(bak.string(), "../x").empty());
    CHECK(psp_savebackup::fingerprint((saves / "ULES01505GameData00").string()) != psp_savebackup::fingerprint((bak / "ULES01505Quest00.bak").string()));
}

// ---------------------------------------------------------------------------------------------------- the page
struct Fake {
    std::vector<psp_savemgr::Source> imp, exp, places;
    bool dlg = true;
    int starts = 0;
    psp_filedlg::Kind kind = psp_filedlg::Kind::ImportFolder;
    psp_filedlg::State answer = psp_filedlg::State::Running;
    std::string answer_path;
    bool cloud = false, configured = false;
    psp_cloud_svc::Status st;
    int syncs = 0;
    std::vector<std::pair<std::string, psp_cloud::Choice>> resolved;
};

static psp_saves::Env make_env(const fs::path& T, Fake& f) {
    psp_saves::Env e;
    e.saves_root = (T / "game" / "SAVEDATA").string();
    e.backup_root = (T / "game" / "SAVEDATA_BACKUP").string();
    e.scratch_dir = (T / "game" / "tmp").string();
    e.game_dir = (T / "game").string();
    e.disc_id = "ULES01505";
    e.device_name = "test-host";
    e.detect = [&f](bool with) { return with ? f.imp : f.exp; };
    e.places = [&f] { return f.places; };
    e.dlg_available = [&f] { return f.dlg; };
    e.dlg_start = [&f](psp_filedlg::Kind k, const std::string&, const std::string&, const std::string&) { ++f.starts; f.kind = k; return true; };
    e.dlg_poll = [&f](std::string* p, std::string*) { const auto s = f.answer; if (s != psp_filedlg::State::Running) { *p = f.answer_path; f.answer = psp_filedlg::State::Idle; } return s; };
    e.dlg_abandon = [&f] { f.answer = psp_filedlg::State::Idle; };
    e.dlg_commit = [] { return std::string(); };
    if (f.cloud) {
        e.cloud_status = [&f] { return f.st; };
        e.cloud_configured = [&f] { return f.configured; };
        e.cloud_sync = [&f] { ++f.syncs; };
        e.cloud_resolve = [&f](const std::string& s, psp_cloud::Choice c) { f.resolved.push_back({s, c}); };
    }
    fs::create_directories(e.saves_root);
    return e;
}

static int row_of(psp_saves::Page& p, const std::string& prefix, int from = 0) {
    const auto& r = p.rows();
    for (int i = from; i < int(r.size()); ++i) if (r[size_t(i)].label.rfind(prefix, 0) == 0 && r[size_t(i)].kind != SetKind::Header) return i;
    return -1;
}
static bool press(psp_saves::Page& p, const std::string& prefix, int from = 0) {
    const int i = row_of(p, prefix, from);
    if (i < 0) { std::printf("  (no row \"%s\"; rows:", prefix.c_str()); for (auto& r : p.rows()) std::printf(" [%s]", r.label.c_str()); std::printf(")\n"); return false; }
    p.confirm(i);
    p.wait_idle();   // opening Backups reads them on a thread
    return true;
}
static bool answer(psp_saves::Page& p, const std::string& button) {
    psp_saves::Dialog d;
    if (!p.dialog(&d)) { std::printf("  (no dialog for \"%s\")\n", button.c_str()); return false; }
    for (size_t i = 0; i < d.buttons.size(); ++i) if (d.buttons[i] == button) { p.dialog_choose(int(i)); return true; }
    std::printf("  (dialog \"%s\" has no \"%s\")\n", d.title.c_str(), button.c_str());
    return false;
}
static std::string dialog_text(psp_saves::Page& p) { psp_saves::Dialog d; return p.dialog(&d) ? d.title + ": " + d.text : ""; }

static void dump_page(const char* dir, const char* name, psp_saves::Page& p, int sel = -1) {
    psp_sddialog::SettingsView v;
    psp_sddialog::SetScene sc;
    for (const char* n : {"Display", "Graphics", "Image", "Effects", "Network", "Saves", "Cloud", "Mods", "System", "Debug"})
        sc.cats.push_back({n, std::string(n) == "Saves" ? 11 : std::string(n) == "Cloud" ? 6 : std::string(n) == "Mods" ? 10 : std::string(n) == "System" ? 7 : std::string(n) == "Network" ? 5 : std::string(n) == "Debug" ? 9 : n[0] == 'D' ? 0 : n[0] == 'G' ? 1 : n[0] == 'I' ? 2 : 3});
    sc.cat = 5;
    sc.crumbs = {"Saves"};
    for (auto& c : p.crumbs()) sc.crumbs.push_back(c);
    sc.in_sub = p.in_sub();
    sc.rows = p.rows();
    sc.sel = sel >= 0 ? sel : p.sel();
    psp_saves::Dialog d;
    if (p.dialog(&d)) { sc.dlg.active = true; sc.dlg.title = d.title; sc.dlg.text = d.text; sc.dlg.buttons = d.buttons; }
    psp_sddialog::Canvas c(2);
    v.set_open(true);
    double t = 0;
    for (int i = 0; i < 120; ++i) { v.render(c, sc, t); t += 16; }
    CHECK(!v.geom().rows.empty() || sc.rows.empty());
    if (!dir) return;
    const std::string path = std::string(dir) + "/" + name + ".ppm";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", c.w, c.h);
    for (uint32_t px : c.px) { unsigned char rgb[3] = {(unsigned char)(px & 255), (unsigned char)((px >> 8) & 255), (unsigned char)((px >> 16) & 255)}; std::fwrite(rgb, 1, 3, f); }
    std::fclose(f);
}

static void test_page(const fs::path& T) {
    const char* dump = std::getenv("DUMP_SAVES");
    const fs::path ppsspp = T / "ppsspp" / "PSP" / "SAVEDATA";
    make_save(ppsspp, "ULES01505GameData00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Ender  Lv 99", "DISSIDIA.BIN", 31);
    make_save(ppsspp, "ULUS10566GameData00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Old", "DISSIDIA.BIN", 32);
    make_save(ppsspp, "ULUS10566FCard03", "DISSIDIA 012 FINAL FANTASY", "friend", "FCARD.BIN", 33);
    fs::last_write_time(ppsspp / "ULUS10566GameData00" / "PARAM.SFO", fs::last_write_time(ppsspp / "ULES01505GameData00" / "PARAM.SFO") - std::chrono::hours(48));
    Fake f;
    f.imp = {{"Test PPSSPP", ppsspp.string()}};
    f.exp = {{"Test stick", (T / "stick").string()}};
    fs::create_directories(T / "stick" / "PSP" / "SAVEDATA");
    psp_saves::Page p;
    const psp_saves::Env env = make_env(T, f);
    p.set_env(env);
    p.reset();
    p.wait_idle();
    const fs::path saves = env.saves_root, bak = env.backup_root;

    // top level: sections and rows
    CHECK(row_of(p, "From Test PPSSPP") >= 0);
    CHECK(p.rows()[size_t(row_of(p, "From Test PPSSPP"))].value == "3 saves");
    CHECK(row_of(p, "Choose a folder") >= 0 && row_of(p, "Choose an archive") >= 0 && row_of(p, "Export as a .zip") >= 0);
    CHECK(row_of(p, "Back up now") >= 0 && row_of(p, "Restore a backup") >= 0);
    CHECK(row_of(p, "Needs your choice") < 0);
    CHECK(p.rows()[size_t(p.sel())].kind != SetKind::Header);
    dump_page(dump, "saves_top", p);

    // 1. import from the found source: one save per slot (this region's GameData00, the card)
    CHECK(press(p, "From Test PPSSPP"));
    CHECK(p.in_sub() && p.crumbs().size() == 1);
    dump_page(dump, "saves_import_pick", p);
    int on = 0;
    for (auto& r : p.rows()) if (r.kind == SetKind::Toggle && r.label != "All saves") on += r.on;
    CHECK(on == 2);
    CHECK(press(p, "Import 2 saves"));
    CHECK(dialog_text(p).find("Imported 2 saves") != std::string::npos);
    CHECK(answer(p, "OK"));
    CHECK(!p.in_sub());
    CHECK(fs::is_directory(saves / "ULES01505GameData00") && fs::is_directory(saves / "ULES01505FCard03"));
    CHECK(resign_is_noop(saves / "ULES01505FCard03" / "PARAM.SFO"));   // renamed from ULUS: signed again
    CHECK(p.saves_changed() == 1);

    // 2. import again, choosing the US game data: replacing asks, and the old one goes to Backups
    CHECK(press(p, "From Test PPSSPP"));
    {
        const int us = row_of(p, "Game data  US");
        CHECK(us > 0);
        p.confirm(us);   // selecting it unselects the EU one (one per slot)
        int gd = 0;
        for (auto& r : p.rows()) if (r.kind == SetKind::Toggle && r.label.rfind("Game data", 0) == 0) gd += r.on;
        CHECK(gd == 1);
        CHECK(p.rows()[size_t(us)].on);
        CHECK(p.rows()[size_t(us)].help.find("Replaces your Game data") != std::string::npos);
    }
    CHECK(press(p, "Import 2 saves"));
    CHECK(dialog_text(p).find("Replace your saves?") != std::string::npos);
    dump_page(dump, "saves_replace_question", p);
    CHECK(answer(p, "Replace"));
    CHECK(answer(p, "OK"));
    CHECK(slurp(saves / "ULES01505GameData00" / "DISSIDIA.BIN") == slurp(ppsspp / "ULUS10566GameData00" / "DISSIDIA.BIN"));
    {
        auto bl = psp_savebackup::list(bak.string());
        CHECK(bl.size() == 1 && bl[0].reason == psp_savebackup::kImport && bl[0].saves.size() == 2);
    }

    // 3. "Only the new ones" with nothing new: nothing changes
    CHECK(press(p, "From Test PPSSPP"));
    CHECK(press(p, "Import 2 saves"));
    CHECK(answer(p, "Cancel"));   // every one exists: Replace / Cancel only
    CHECK(p.in_sub());
    CHECK(p.back());
    CHECK(!p.in_sub());

    // 4. an archive picked in the (fake) system file window
    const fs::path zsrc = T / "zsrc";
    make_save(zsrc, "ULJM05814Quest00", "DISSIDIA 012 FINAL FANTASY", "jp quest", "QUEST.BIN", 34);
    CHECK(psp_savearchive::write_zip((T / "shared.zip").string(), {(zsrc / "ULJM05814Quest00").string()}).empty());
    CHECK(press(p, "Choose an archive"));
    CHECK(f.starts == 1 && f.kind == psp_filedlg::Kind::ImportArchive && p.native_dialog_open());
    CHECK(dialog_text(p).find("file window") != std::string::npos);
    dump_page(dump, "saves_file_window", p);
    f.answer = psp_filedlg::State::Done;
    f.answer_path = (T / "shared.zip").string();
    p.tick(0);
    CHECK(!p.native_dialog_open());
    CHECK(p.in_sub() && row_of(p, "Quest 00  JP") > 0);
    CHECK(press(p, "Import 1 save"));
    CHECK(answer(p, "OK"));
    CHECK(fs::is_directory(saves / "ULES01505Quest00"));
    CHECK(!fs::exists(fs::path(env.scratch_dir) / "unpacked"));   // the unpacked copy is gone once the level closed

    // 5. cancelling the file window
    CHECK(press(p, "Choose a folder"));
    f.answer = psp_filedlg::State::Cancelled;
    p.tick(0);
    CHECK(!p.native_dialog_open() && !p.dialog(nullptr) && !p.in_sub());
    // ... and the Cancel button while it is open
    CHECK(press(p, "Choose a folder"));
    CHECK(answer(p, "Cancel"));
    CHECK(!p.native_dialog_open());

    // 6. a typed path that is a folder with saves deep inside; then one that does not exist
    CHECK(press(p, "Type a path"));
    psp_saves::TextAsk ask;
    CHECK(p.text(&ask));
    p.text_done(true, (T / "ppsspp").string() + "/");
    CHECK(p.in_sub() && row_of(p, "Friend card 03") > 0);
    CHECK(p.back());
    CHECK(press(p, "Type a path"));
    p.text_done(true, (T / "nothing-here").string());
    CHECK(dialog_text(p).find("Not found") != std::string::npos);
    CHECK(answer(p, "OK"));
    CHECK(press(p, "Type a path"));
    p.text_done(false, "ignored");
    CHECK(!p.text(nullptr) && !p.dialog(nullptr));

    // 7. the in-game browser: places, into a folder, use it
    f.places = {{"Test place", T.string()}};
    CHECK(press(p, "Browse in the game"));
    CHECK(row_of(p, "Test place") >= 0);
    CHECK(press(p, "Test place"));
    CHECK(press(p, "ppsspp/"));
    CHECK(p.rows()[size_t(row_of(p, "Use this folder"))].value == "3 saves");
    dump_page(dump, "saves_browse", p, row_of(p, "Use this folder"));
    CHECK(row_of(p, "shared.zip") < 0);   // archives are listed only where they are
    CHECK(press(p, "Up one folder"));
    CHECK(row_of(p, "shared.zip") >= 0);
    CHECK(press(p, "shared.zip"));
    CHECK(row_of(p, "Quest 00  JP") > 0);
    p.reset();
    p.wait_idle();
    CHECK(!p.in_sub());

    // 8. export to the found stick (goes into its PSP/SAVEDATA), names changed to this game's ID
    {
        const int names = row_of(p, "Folder names");
        CHECK(names > 0 && p.rows()[size_t(names)].value == "Keep region");
        p.change(names, 1);
        CHECK(p.rows()[size_t(names)].value == "As ULES01505");
        p.change(names, -1);
        CHECK(p.rows()[size_t(names)].value == "Keep region");
    }
    CHECK(press(p, "To Test stick"));
    CHECK(press(p, "Export 3 saves"));
    CHECK(dialog_text(p).find("Exported 3 saves") != std::string::npos);
    CHECK(answer(p, "OK"));
    CHECK(fs::is_directory(T / "stick" / "PSP" / "SAVEDATA" / "ULES01505GameData00"));
    CHECK(press(p, "To Test stick"));
    CHECK(press(p, "Export 3 saves"));
    dump_page(dump, "saves_export_question", p);
    CHECK(answer(p, "Skip those"));
    CHECK(dialog_text(p).find("left alone") != std::string::npos);
    CHECK(answer(p, "OK"));

    // 9. export as a .zip through the file window, then import that zip again
    CHECK(press(p, "Export as a .zip"));
    CHECK(f.kind == psp_filedlg::Kind::ExportZip);
    f.answer = psp_filedlg::State::Done;
    f.answer_path = (T / "out" / "mine.zip").string();
    p.tick(0);
    CHECK(p.in_sub());
    CHECK(press(p, "Export 3 saves"));
    CHECK(answer(p, "OK"));
    CHECK(fs::is_regular_file(T / "out" / "mine.zip"));
    CHECK(psp_savearchive::extract((T / "out" / "mine.zip").string(), (T / "out" / "x").string()).empty());
    CHECK(psp_savemgr::scan((T / "out" / "x" / "PSP" / "SAVEDATA").string()).size() == 3);

    // 10. Back up now, then break a save and restore it from the backup
    const Bytes good = slurp(saves / "ULES01505FCard03" / "FCARD.BIN");
    CHECK(press(p, "Back up now"));
    CHECK(dialog_text(p).find("Backed up") != std::string::npos);
    CHECK(answer(p, "OK"));
    spit(saves / "ULES01505FCard03" / "FCARD.BIN", Bytes{0xBA, 0xD0});
    CHECK(press(p, "Restore a backup"));
    dump_page(dump, "saves_backups", p);
    {
        const auto& r = p.rows();
        CHECK(r.size() >= 3);
        int first = -1;
        for (int i = 0; i < int(r.size()); ++i) if (r[size_t(i)].kind == SetKind::Submenu && r[size_t(i)].value == "by you") { first = i; break; }
        CHECK(first >= 0);
        p.confirm(first);
    }
    CHECK(p.crumbs().size() == 2);
    dump_page(dump, "saves_backup", p);
    CHECK(press(p, "Restore Friend card 03"));
    CHECK(dialog_text(p).find("Restore Friend card 03?") != std::string::npos);
    CHECK(answer(p, "Restore"));
    CHECK(dialog_text(p).find("Restored 1 save") != std::string::npos);
    CHECK(answer(p, "OK"));
    CHECK(slurp(saves / "ULES01505FCard03" / "FCARD.BIN") == good);
    {
        bool before_restore = false;
        for (auto& b : psp_savebackup::list(bak.string()))
            if (b.reason == psp_savebackup::kRestore) { before_restore = true; CHECK(slurp(fs::path(b.path) / "ULES01505FCard03" / "FCARD.BIN") == (Bytes{0xBA, 0xD0})); }
        CHECK(before_restore);
    }
    // delete a backup
    CHECK(press(p, "Restore a backup"));
    const int nb = int(psp_savebackup::list(bak.string()).size());
    p.confirm(1);
    CHECK(press(p, "Delete this backup"));
    CHECK(answer(p, "Delete"));
    CHECK(int(psp_savebackup::list(bak.string()).size()) == nb - 1);
    p.reset();
    p.wait_idle();
}

static void test_conflicts(const fs::path& T) {
    const char* dump = std::getenv("DUMP_SAVES");
    Fake f;
    f.cloud = true;
    f.configured = true;
    psp_saves::Page p;
    const psp_saves::Env env = make_env(T, f);
    make_save(env.saves_root, "ULES01505GameData00", "DISSIDIA 012 FINAL FANTASY", "Date: 17/04/2013 10:59:07 PLAYER: Ender Time: 233:34:37", "DISSIDIA.BIN", 41);
    make_save(env.saves_root, "ULES01505FCard01", "DISSIDIA 012 FINAL FANTASY", "card", "FCARD.BIN", 42);
    const time_t now = std::time(nullptr);
    auto conflict = [&](const char* sfx, time_t local, time_t remote) {
        psp_cloud::Item it;
        it.suffix = sfx;
        it.action = psp_cloud::Action::Conflict;
        it.local_mtime = local;
        it.remote.mtime = remote;
        it.remote.device_name = "steamdeck";
        it.remote.detail = "Date: 17/04/2013 10:59:07 PLAYER: Ender Lv 100 Time: 241:02:11";
        return it;
    };
    f.st.state = psp_cloud_svc::Status::NeedsChoice;
    f.st.items = {conflict("GameData00", now - 7200, now + 3600), conflict("FCard01", now - 60, now - 7200)};   // (the page reads this device's time from the save itself: now)
    f.st.conflicts = 2;
    f.st.revision = 5;
    p.set_env(env);
    p.reset();
    p.wait_idle();
    CHECK(row_of(p, "Game data: changed on two devices") >= 0);
    CHECK(row_of(p, "Keep the newer save of each") >= 0);
    CHECK(row_of(p, "Sync now") >= 0 && row_of(p, "Cloud settings") >= 0);
    dump_page(dump, "saves_conflicts_top", p);
    CHECK(press(p, "Game data: changed on two devices"));
    {
        const auto& r = p.rows();
        bool here = false, cloud = false;
        for (auto& x : r) {
            if (x.kind == SetKind::Header && x.label.find("test-host") != std::string::npos) here = true;
            if (x.kind == SetKind::Header && x.label.find("steamdeck") != std::string::npos) cloud = true;
        }
        CHECK(here && cloud);
        const int keep = row_of(p, "Use the cloud save");
        CHECK(keep > 0 && r[size_t(keep)].value == "newer");   // the cloud one was saved later
        CHECK(p.sel() == keep);                                  // and the cursor starts on it
        CHECK(row_of(p, "...") > 0 && p.rows()[size_t(row_of(p, "..."))].label.find("Time: 233:34:37") != std::string::npos);   // the end of a long description
    }
    dump_page(dump, "saves_conflict", p, row_of(p, "Use the cloud save"));
    CHECK(press(p, "Use the cloud save"));
    CHECK(f.resolved.size() == 1 && f.resolved[0].first == "GameData00" && f.resolved[0].second == psp_cloud::Choice::KeepRemote);
    CHECK(!p.in_sub());
    CHECK(press(p, "Keep the newer save of each"));
    CHECK(answer(p, "Keep the newer ones"));
    CHECK(f.resolved.size() == 3);
    CHECK(f.resolved[1].second == psp_cloud::Choice::KeepRemote && f.resolved[2].second == psp_cloud::Choice::KeepLocal);
    // the cloud settles: the rows follow on the next tick
    f.st.items.clear(); f.st.conflicts = 0; f.st.state = psp_cloud_svc::Status::Idle; f.st.revision = 6;
    CHECK(p.tick(0));
    CHECK(row_of(p, "Needs your choice") < 0 && row_of(p, "Game data: changed") < 0);
    // a cloud download that happened meanwhile counts as a change of the saves (the save list refreshes)
    const unsigned c0 = p.saves_changed();
    psp_cloud::Item pulled; pulled.suffix = "FCard01"; pulled.action = psp_cloud::Action::Pulled;
    f.st.items = {pulled}; f.st.revision = 7;
    p.tick(0);
    CHECK(p.saves_changed() == c0 + 1);
    CHECK(press(p, "Cloud settings"));
    CHECK(p.take_cloud_settings_request() && !p.take_cloud_settings_request());
    CHECK(press(p, "Sync now"));
    CHECK(f.syncs == 1);
}

static std::string sfo_title(const fs::path& dir) {
    psp_savemgr::SaveInfo s;
    return psp_savemgr::read_save(dir.string(), &s) ? s.savetitle : std::string("?");
}

static void test_mine(const fs::path& T) {
    const char* dump = std::getenv("DUMP_SAVES");
    Fake f;
    f.cloud = true;
    f.configured = true;
    std::vector<std::string> deleted;
    psp_saves::Page p;
    psp_saves::Env env = make_env(T, f);
    env.cloud_delete = [&deleted](const std::string& s) { deleted.push_back(s); };
    const fs::path saves = env.saves_root, bak = env.backup_root;
    make_save(saves, "ULES01505GameData00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Ender Time: 10:00:00", "DISSIDIA.BIN", 51);
    make_save(saves, "ULES01505GameData03", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Other Time: 01:00:00", "DISSIDIA.BIN", 52);
    p.set_env(env);
    p.reset();
    p.wait_idle();
    CHECK(press(p, "Duplicate, move, rename or delete"));
    CHECK(row_of(p, "Game data 00") > 0 && row_of(p, "Game data 03") > 0);
    dump_page(dump, "saves_mine", p);

    // duplicate 00 into 01 (empty): a real copy under the new slot name, PARAM.SFO retargeted and signed
    CHECK(press(p, "Game data 00"));
    dump_page(dump, "saves_one", p);
    CHECK(press(p, "Duplicate to another slot"));
    CHECK(row_of(p, "Game data 00") < 0);                                   // not its own slot
    CHECK(p.rows()[size_t(row_of(p, "Game data 01"))].value == "empty");
    CHECK(p.rows()[size_t(row_of(p, "Game data 03"))].value.rfind("replaces", 0) == 0);
    CHECK(row_of(p, "Game data 19") > 0 && row_of(p, "Game data 20") < 0);   // the game's 20 slots
    dump_page(dump, "saves_slots", p);
    CHECK(press(p, "Game data 01"));
    CHECK(dialog_text(p).find("copied to Game data 01") != std::string::npos);
    CHECK(answer(p, "OK"));
    CHECK(slurp(saves / "ULES01505GameData01" / "DISSIDIA.BIN") == slurp(saves / "ULES01505GameData00" / "DISSIDIA.BIN"));
    {
        Bytes sfo = slurp(saves / "ULES01505GameData01" / "PARAM.SFO");
        const std::string n = "ULES01505GameData01";
        CHECK(std::search(sfo.begin(), sfo.end(), n.begin(), n.end()) != sfo.end());
        CHECK(resign_is_noop(saves / "ULES01505GameData01" / "PARAM.SFO"));
    }
    CHECK(f.syncs == 1);                                                    // the new slot goes up
    CHECK(p.crumbs().size() == 1);                                          // back on the list of saves

    // duplicate onto a taken slot asks, and keeps the old one in Backups
    CHECK(press(p, "Game data 00"));
    CHECK(press(p, "Duplicate to another slot"));
    CHECK(press(p, "Game data 03"));
    CHECK(dialog_text(p).find("Replace Game data 03?") != std::string::npos);
    CHECK(answer(p, "Replace"));
    CHECK(answer(p, "OK"));
    CHECK(slurp(saves / "ULES01505GameData03" / "DISSIDIA.BIN") == slurp(saves / "ULES01505GameData00" / "DISSIDIA.BIN"));
    {
        bool kept = false;
        for (auto& b : psp_savebackup::list(bak.string())) for (auto& s : b.saves) if (s.name == "ULES01505GameData03" && psp_savemgr::one_line(s.detail).find("Other") != std::string::npos) kept = true;
        CHECK(kept);
    }

    // move 01 to 05: 01 is free afterwards (its folder in Backups), and the cloud forgets slot 01
    CHECK(press(p, "Game data 01"));
    CHECK(press(p, "Move to another slot"));
    CHECK(press(p, "Game data 05"));
    CHECK(dialog_text(p).find("is now Game data 05") != std::string::npos);
    CHECK(answer(p, "OK"));
    CHECK(!fs::exists(saves / "ULES01505GameData01") && fs::is_directory(saves / "ULES01505GameData05"));
    CHECK(deleted.size() == 1 && deleted[0] == "GameData01");

    // rename: the save list's name changes, the signature stays valid, the old PARAM.SFO is in Backups
    CHECK(press(p, "Game data 05"));
    CHECK(press(p, "Rename"));
    psp_saves::TextAsk ask;
    CHECK(p.text(&ask) && ask.text == "Slot");
    p.text_done(true, "  Before the final boss  ");
    CHECK(sfo_title(saves / "ULES01505GameData05") == "Before the final boss");
    CHECK(resign_is_noop(saves / "ULES01505GameData05" / "PARAM.SFO"));
    CHECK(p.rows()[size_t(row_of(p, "Rename"))].value == "Before the final boss");
    {
        bool kept = false;
        for (auto& b : psp_savebackup::list(bak.string())) if (b.reason == psp_savebackup::kRenamed && !b.saves.empty() && b.saves[0].savetitle == "Slot") kept = true;
        CHECK(kept);
    }
    CHECK(press(p, "Rename"));
    p.text_done(true, std::string(200, 'x'));   // too long for SAVEDATA_TITLE
    CHECK(dialog_text(p).find("too long") != std::string::npos);
    CHECK(answer(p, "OK"));

    // delete: asks, goes to Backups, the cloud is told
    CHECK(press(p, "Delete"));
    CHECK(dialog_text(p).find("Delete Game data 05?") != std::string::npos && dialog_text(p).find("other devices") != std::string::npos);
    dump_page(dump, "saves_delete", p);
    CHECK(answer(p, "Cancel"));
    CHECK(fs::exists(saves / "ULES01505GameData05"));
    CHECK(press(p, "Delete"));
    CHECK(answer(p, "Delete"));
    CHECK(dialog_text(p).find("was deleted") != std::string::npos);
    CHECK(answer(p, "OK"));
    CHECK(!fs::exists(saves / "ULES01505GameData05"));
    CHECK(deleted.size() == 2 && deleted[1] == "GameData05");
    {
        bool kept = false;
        for (auto& b : psp_savebackup::list(bak.string())) if (b.reason == psp_savebackup::kDeleted && !b.saves.empty() && b.saves[0].name == "ULES01505GameData05") kept = true;
        CHECK(kept);
    }
    CHECK(row_of(p, "Game data 05") < 0 && row_of(p, "Game data 00") > 0);
}

// ---------------------------------------------------------------------------------------------------- the real PPSSPP folder
static void test_real(const fs::path& T) {
    const char* e = std::getenv("PSPRECOMP_TEST_PPSSPP_SAVES");
    std::string root = e ? e : std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.config/ppsspp/PSP/SAVEDATA";
    if (!fs::is_directory(root)) { std::printf("real PPSSPP save folder %s not present: skipped\n", root.c_str()); return; }
    auto before = psp_savemgr::scan(root);
    std::printf("real PPSSPP folder: %zu Dissidia 012 save(s)\n", before.size());
    if (before.empty()) return;
    auto snap = [&] {
        std::string r;
        for (auto& s : psp_savemgr::scan(root))
            for (auto& f : fs::directory_iterator(s.path))
                r += f.path().string() + ":" + std::to_string(f.file_size()) + ":" + std::to_string(f.last_write_time().time_since_epoch().count()) + ";";
        return r;
    };
    const std::string snap0 = snap();
    Fake f;
    f.imp = {{"PPSSPP", root}};
    psp_saves::Page p;
    const psp_saves::Env env = make_env(T / "real", f);
    p.set_env(env);
    p.reset();
    p.wait_idle();
    CHECK(press(p, "From PPSSPP"));
    const int run = row_of(p, "Import ");
    CHECK(run > 0);
    p.confirm(run);
    std::printf("real PPSSPP import: %s\n", dialog_text(p).c_str());
    CHECK(answer(p, "OK"));
    for (auto& s : psp_savemgr::scan(env.saves_root)) {
        CHECK(s.disc == "ULES01505");
        if (fs::exists(fs::path(root) / s.name)) CHECK(slurp(fs::path(s.path) / "PARAM.SFO") == slurp(fs::path(root) / s.name / "PARAM.SFO"));
    }
    CHECK(!psp_savemgr::scan(env.saves_root).empty());
    CHECK(snap() == snap0);   // the source is never touched
}

int main() {
    char tmpl[] = "/tmp/savemgrtestXXXXXX";
    if (!mkdtemp(tmpl)) return 2;
    const fs::path T = tmpl;
    test_library(T / "lib");
    test_archives(T / "arc");
    test_backups(T / "bak");
    test_page(T / "page");
    test_conflicts(T / "conf");
    test_mine(T / "mine");
    test_real(T);
    if (!std::getenv("KEEP")) fs::remove_all(T);
    else std::printf("kept %s\n", tmpl);
    if (g_fail) { std::printf("test_savemgr: %d FAILURES\n", g_fail); return 1; }
    std::printf("test_savemgr: all passed\n");
    return 0;
}
