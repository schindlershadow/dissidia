// Host test for cloud sync (psp_cloudsync): two simulated installs (a US and an EU one) syncing through a folder
// provider: first upload, download across regions, no-op resync, one-sided changes, conflicts (never silent, both
// resolutions, backups), damaged downloads, unsafe containers and the content hash. No network, no game data.
#include "psp_cloudsync.h"
#include "psp_savemgr.h"
#include "cloud_fixture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using namespace psp_cloud;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const Item* find(const std::vector<Item>& v, const std::string& sfx) {
    for (auto& i : v) if (i.suffix == sfx) return &i;
    return nullptr;
}
static int count(const std::vector<Item>& v, Action a) { int n = 0; for (auto& i : v) n += i.action == a; return n; }
static int backups(const Dev& d) {
    int n = 0; std::error_code ec;
    if (fs::exists(d.root + "/backup", ec)) for (auto& e : fs::directory_iterator(d.root + "/backup")) { (void)e; ++n; }
    return n;
}

int main() {
    // known SHA-256 vectors
    CHECK(sha256_hex(Bytes{}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex(Bytes{'a', 'b', 'c'}) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Bytes million(1000000, 'a');
    CHECK(sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    char tmpl[] = "/tmp/cloudsync_testXXXXXX";
    const std::string T = mkdtemp(tmpl);
    const std::string cloud = T + "/cloud";
    Dev A{T + "/A", "ULUS10566"}, B{T + "/B", "ULES01505"};
    make_save(A.saves(), "ULUS10566GameData00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Ender", "DISSIDIA.BIN", 1);
    make_save(A.saves(), "ULUS10566FCard00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Akan", "FCARD.BIN", 2);
    auto prov = make_folder_provider(cloud);

    // first sync of A: both saves go up; the state file gets a device ID
    std::string err;
    auto r = sync_all(*prov, A.opts(), &err);
    CHECK(err.empty());
    CHECK(r.size() == 2 && count(r, Action::Pushed) == 2);
    CHECK(fs::exists(cloud + "/GameData00.save") && fs::exists(cloud + "/GameData00.meta"));
    const std::string devA = device_id(A.opts().state_path);
    CHECK(devA.size() == 16 && devA == device_id(A.opts().state_path));

    // B (another region, empty): downloads both; names carry B's disc ID, PARAM.SFO was retargeted
    r = sync_all(*prov, B.opts(), &err);
    CHECK(err.empty() && count(r, Action::Pulled) == 2);
    CHECK(fs::exists(B.dir("GameData00") / "DISSIDIA.BIN"));
    CHECK(slurp(B.dir("GameData00") / "DISSIDIA.BIN") == slurp(A.dir("GameData00") / "DISSIDIA.BIN"));
    CHECK(slurp(B.dir("FCard00") / "FCARD.BIN") == slurp(A.dir("FCard00") / "FCARD.BIN"));
    {
        Bytes sfo = slurp(B.dir("GameData00") / "PARAM.SFO");
        CHECK(std::search(sfo.begin(), sfo.end(), (const uint8_t*)"ULES01505GameData00", (const uint8_t*)"ULES01505GameData00" + 19) != sfo.end());
        CHECK(std::search(sfo.begin(), sfo.end(), (const uint8_t*)"ULUS10566", (const uint8_t*)"ULUS10566" + 9) == sfo.end());
    }
    CHECK(hash_save(A.dir("GameData00").string()) == hash_save(B.dir("GameData00").string()));
    CHECK(device_id(B.opts().state_path) != devA);
    CHECK(!fs::exists(T + "/B/sync/cloud_staging"));

    // nothing changed: everything is Same on both
    r = sync_all(*prov, A.opts(), &err); CHECK(count(r, Action::Same) == 2 && r.size() == 2);
    r = sync_all(*prov, B.opts(), &err); CHECK(count(r, Action::Same) == 2 && r.size() == 2);

    // A changes one save: pushed; B pulls it and keeps its old copy in the backup folder
    A.change("GameData00", "DISSIDIA.BIN", 5);
    r = sync_all(*prov, A.opts(), &err);
    CHECK(find(r, "GameData00")->action == Action::Pushed && find(r, "FCard00")->action == Action::Same);
    CHECK(backups(B) == 0);
    r = sync_all(*prov, B.opts(), &err);
    CHECK(find(r, "GameData00")->action == Action::Pulled && find(r, "FCard00")->action == Action::Same);
    CHECK(slurp(B.dir("GameData00") / "DISSIDIA.BIN") == slurp(A.dir("GameData00") / "DISSIDIA.BIN"));
    CHECK(backups(B) == 1);
    CHECK(count(sync_all(*prov, B.opts()), Action::Same) == 2);

    // B changes it back the other way: pushed from B, pulled by A
    B.change("GameData00", "DISSIDIA.BIN", 6);
    r = sync_all(*prov, B.opts(), &err); CHECK(find(r, "GameData00")->action == Action::Pushed);
    r = sync_all(*prov, A.opts(), &err); CHECK(find(r, "GameData00")->action == Action::Pulled);

    // CONFLICT: both change the same save without syncing in between. Never resolved silently.
    A.change("GameData00", "DISSIDIA.BIN", 7);
    B.change("GameData00", "DISSIDIA.BIN", 8);
    const Bytes aData = slurp(A.dir("GameData00") / "DISSIDIA.BIN"), bData = slurp(B.dir("GameData00") / "DISSIDIA.BIN");
    r = sync_all(*prov, A.opts(), &err);                         // A goes first: remote unchanged since its base -> pushed
    CHECK(find(r, "GameData00")->action == Action::Pushed);
    const int bBackups = backups(B);
    r = sync_all(*prov, B.opts(), &err);                         // B now meets a changed remote and a changed local
    CHECK(find(r, "GameData00")->action == Action::Conflict);
    CHECK(slurp(B.dir("GameData00") / "DISSIDIA.BIN") == bData);      // untouched
    CHECK(backups(B) == bBackups);
    r = sync_all(*prov, B.opts(), &err);                         // still a conflict next time: nothing was decided
    CHECK(find(r, "GameData00")->action == Action::Conflict);
    CHECK(find(r, "GameData00")->local_mtime != 0 && find(r, "GameData00")->remote.valid());
    // keep the cloud save: B takes A's, its own goes to the backup folder
    Item it = resolve_conflict(*prov, B.opts(), "GameData00", Choice::KeepRemote);
    CHECK(it.action == Action::Pulled);
    CHECK(slurp(B.dir("GameData00") / "DISSIDIA.BIN") == aData);
    CHECK(backups(B) == bBackups + 1);
    CHECK(count(sync_all(*prov, B.opts()), Action::Same) == 2);

    // conflict again; this time the choice callback keeps the local one: remote gets it, the old remote becomes .bak
    A.change("GameData00", "DISSIDIA.BIN", 9);
    B.change("GameData00", "DISSIDIA.BIN", 10);
    sync_all(*prov, A.opts());
    const Bytes bData2 = slurp(B.dir("GameData00") / "DISSIDIA.BIN");
    Options ob = B.opts();
    int asked = 0;
    ob.on_conflict = [&](const Item& i) { ++asked; CHECK(i.suffix == "GameData00"); return Choice::KeepLocal; };
    r = sync_all(*prov, ob, &err);
    CHECK(asked == 1 && find(r, "GameData00")->action == Action::Pushed);
    CHECK(fs::exists(cloud + "/GameData00.bak"));
    CHECK(slurp(B.dir("GameData00") / "DISSIDIA.BIN") == bData2);
    r = sync_all(*prov, A.opts(), &err); CHECK(find(r, "GameData00")->action == Action::Pulled);
    CHECK(slurp(A.dir("GameData00") / "DISSIDIA.BIN") == bData2);

    // Skip leaves the conflict for later
    A.change("GameData00", "DISSIDIA.BIN", 12);
    B.change("GameData00", "DISSIDIA.BIN", 13);
    sync_all(*prov, A.opts());
    ob.on_conflict = [](const Item&) { return Choice::Skip; };
    r = sync_all(*prov, ob, &err);
    CHECK(find(r, "GameData00")->action == Action::Skipped);
    CHECK(find(sync_all(*prov, B.opts()), "GameData00")->action == Action::Conflict);
    resolve_conflict(*prov, B.opts(), "GameData00", Choice::KeepRemote);

    // a fresh install with a local save that differs from the cloud and no base: a conflict, not an overwrite
    Dev C{T + "/C", "ULUS10566"};
    make_save(C.saves(), "ULUS10566GameData00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Other", "DISSIDIA.BIN", 99);
    r = sync_all(*prov, C.opts(), &err);
    CHECK(find(r, "GameData00")->action == Action::Conflict);
    CHECK(find(r, "FCard00")->action == Action::Pulled);

    // push-only run: uploads, but never downloads (a save that is newer in the cloud is left for the next full sync)
    {
        A.change("FCard00", "FCARD.BIN", 21);
        sync_all(*prov, A.opts());                                        // cloud now has A's new FCard00
        Options ob2 = B.opts();
        ob2.allow_pull = false;
        const Bytes before = slurp(B.dir("FCard00") / "FCARD.BIN");
        r = sync_all(*prov, ob2, &err);
        CHECK(find(r, "FCard00")->action == Action::Skipped);
        CHECK(slurp(B.dir("FCard00") / "FCARD.BIN") == before);          // untouched
        B.change("GameData00", "DISSIDIA.BIN", 31);
        r = sync_all(*prov, ob2, &err);
        CHECK(find(r, "GameData00")->action == Action::Pushed);           // its own change still goes up
        CHECK(find(r, "FCard00")->action == Action::Skipped);
        r = sync_all(*prov, B.opts(), &err);
        CHECK(find(r, "FCard00")->action == Action::Pulled);              // the next full sync takes it
        sync_all(*prov, A.opts());
    }

    // include filter: no replays / only what the caller wants
    Dev D{T + "/D", "ULUS10566"};
    Options od = D.opts();
    od.include = [](const std::string& s) { return s.rfind("GameData", 0) == 0; };
    r = sync_all(*prov, od, &err);
    CHECK(r.size() == 1 && r[0].suffix == "GameData00" && r[0].action == Action::Pulled);
    CHECK(!fs::exists(D.dir("FCard00")));

    // a damaged blob (checksum mismatch) is refused and the local save stays as it was
    {
        Dev E{T + "/E", "ULUS10566"};
        const Bytes good = slurp(cloud + "/GameData00.save");
        for (size_t at : {size_t(40), good.size() - 5}) {      // inside a data file, and inside PARAM.SFO (not in the content hash)
            Bytes blob = good;
            blob[at] ^= 0xFF;
            spit(cloud + "/GameData00.save", blob);
            r = sync_all(*prov, E.opts(), &err);
            CHECK(find(r, "GameData00")->action == Action::Error);
            CHECK(!fs::exists(E.dir("GameData00")));
            CHECK(!fs::exists(T + "/E/sync/cloud_staging"));
        }
        spit(cloud + "/GameData00.save", good);
    }

    // meta without a blob is ignored; a blob without meta is ignored (a half-finished upload)
    {
        Bytes m = slurp(cloud + "/FCard00.meta");
        spit(cloud + "/Quest05.meta", m);
        spit(cloud + "/Quest06.save", Bytes{1, 2, 3});
        Dev F{T + "/F", "ULUS10566"};
        r = sync_all(*prov, F.opts(), &err);
        CHECK(!find(r, "Quest05") && !find(r, "Quest06"));
    }

    // containers: round trip, and unsafe or damaged ones are refused
    {
        Bytes blob;
        CHECK(pack_save(A.dir("FCard00").string(), &blob).empty());
        CHECK(unpack_save(blob, T + "/unp/ULUS10566FCard00").empty());
        CHECK(hash_save(T + "/unp/ULUS10566FCard00") == hash_save(A.dir("FCard00").string()));
        Bytes evil = {'P', 'S', 'C', 'S', '1', 0, 0, 0, 1, 0, 0, 0, 6, 0, '.', '.', '/', 'x', 'y', 'z', 1, 0, 0, 0, 0, 0, 0, 0, 'q'};
        CHECK(!unpack_save(evil, T + "/unp2").empty());
        CHECK(!unpack_save(Bytes{1, 2, 3}, T + "/unp3").empty());
        Bytes cut(blob.begin(), blob.begin() + blob.size() / 2);
        CHECK(!unpack_save(cut, T + "/unp4").empty());
        CHECK(!fs::exists(T + "/xyz") && !fs::exists(T + "/unp2/../x"));
    }

    // meta text round trip and rejection of junk
    {
        Meta m; m.hash = std::string(64, 'a'); m.blob = std::string(64, 'b'); m.device = "dev"; m.disc = "ULUS10566"; m.mtime = 12345;
        Meta n = meta_from_text(meta_to_text(m));
        CHECK(n.blob == m.blob && n.hash == m.hash && n.device == "dev" && n.disc == m.disc && n.mtime == 12345);
        CHECK(!meta_from_text("hash=zz\n").valid());
        CHECK(!meta_from_text("").valid());
    }

    // an unreachable remote stops the run with an error and touches nothing
    {
        auto bad = make_folder_provider("/proc/definitely/not/writable");
        Dev G{T + "/G", "ULUS10566"};
        make_save(G.saves(), "ULUS10566GameData00", "DISSIDIA 012 FINAL FANTASY", "x", "DISSIDIA.BIN", 3);
        r = sync_all(*bad, G.opts(), &err);
        CHECK(count(r, Action::Pushed) == 0);
    }

    std::error_code ec;
    fs::remove_all(T, ec);
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("cloudsync: all checks passed\n");
    return 0;
}
