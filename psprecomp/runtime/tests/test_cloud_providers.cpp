// Host test for the cloud providers (psp_cloud_providers): WebDAV and Google Drive (device sign-in, refresh, a Drive folder
// files) against a local fake server (tests/cloud_fake_server.py, started by run_cloud.sh), each through the same provider
// conformance checks and a two-device sync. Plain http only: no TLS, no real accounts, no network.
#include "psp_cloud_providers.h"
#include "psp_http.h"
#include "psp_savemgr.h"
#include "cloud_fixture.h"

#include <algorithm>

using namespace psp_cloud;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string g_base;
static void admin(const char* what) { psp_http::Request r; r.url = g_base + "/admin/" + what; psp_http::perform(r); }
static std::string stat(const char* key) {
    psp_http::Request r; r.url = g_base + "/admin/stats";
    const std::string t = psp_http::perform(r).text();
    const size_t p = t.find(std::string("\"") + key + "\": ");
    return p == std::string::npos ? "" : t.substr(p + std::strlen(key) + 4, t.find_first_of(",}", p) - p - std::strlen(key) - 4);
}

static Bytes blob(size_t n, uint8_t seed) { Bytes b(n); for (size_t i = 0; i < n; i++) b[i] = uint8_t(seed + i * 7); return b; }
static bool has(const std::vector<RemoteItem>& v, const std::string& n, uint64_t size) {
    for (auto& i : v) if (i.name == n) return i.size == size;
    return false;
}

// What every provider must do.
static void conformance(Provider& p, const char* what) {
    std::printf("-- %s\n", what);
    std::vector<RemoteItem> items;
    std::string e = p.list(&items);
    CHECK(e.empty() && items.empty());
    if (!e.empty()) std::printf("  list: %s\n", e.c_str());

    CHECK(p.put("GameData00.save", blob(300000, 1)).empty());        // includes zero bytes and every byte value
    CHECK(p.put("GameData00.meta", Bytes{'h', 'i'}).empty());
    CHECK(p.put("FCard00.save", blob(10, 2)).empty());
    CHECK(p.put("FCard00.meta", blob(20, 3)).empty());
    CHECK(p.put("Quest05.meta", Bytes{}).empty());                   // an empty object
    e = p.list(&items);
    CHECK(e.empty() && items.size() == 5);                            // (the Drive fake pages by 2)
    CHECK(has(items, "GameData00.save", 300000) && has(items, "GameData00.meta", 2) && has(items, "FCard00.save", 10) &&
          has(items, "FCard00.meta", 20) && has(items, "Quest05.meta", 0));

    Bytes got;
    CHECK(p.get("GameData00.save", &got).empty() && got == blob(300000, 1));
    CHECK(p.get("Quest05.meta", &got).empty() && got.empty());
    CHECK(!p.get("Nope.save", &got).empty());                         // missing object: an error

    CHECK(p.put("GameData00.save", blob(1234, 9)).empty());          // replace
    CHECK(p.get("GameData00.save", &got).empty() && got == blob(1234, 9));
    p.list(&items);
    CHECK(items.size() == 5 && has(items, "GameData00.save", 1234));

    CHECK(p.remove("FCard00.save").empty());
    CHECK(p.remove("FCard00.save").empty());                          // removing it again is not an error
    p.list(&items);
    CHECK(items.size() == 4 && !has(items, "FCard00.save", 10));
    CHECK(p.remove("FCard00.meta").empty() && p.remove("Quest05.meta").empty() && p.remove("GameData00.meta").empty() &&
          p.remove("GameData00.save").empty());
    p.list(&items);
    CHECK(items.empty());
}

// The same two-device scenario as test_cloudsync, through a real provider.
static void two_devices(Provider& p, const std::string& T, const char* what) {
    std::printf("-- %s: two devices\n", what);
    Dev A{T + "/A", "ULUS10566"}, B{T + "/B", "ULES01505"};
    make_save(A.saves(), "ULUS10566GameData00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Ender", "DISSIDIA.BIN", 1);
    make_save(A.saves(), "ULUS10566FCard00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Akan", "FCARD.BIN", 2);
    std::string err;
    auto r = sync_all(p, A.opts(), &err);
    CHECK(err.empty() && r.size() == 2 && r[0].action == Action::Pushed && r[1].action == Action::Pushed);
    if (!err.empty()) std::printf("  error: %s\n", err.c_str());
    r = sync_all(p, B.opts(), &err);
    CHECK(err.empty() && r.size() == 2 && r[0].action == Action::Pulled && r[1].action == Action::Pulled);
    CHECK(slurp(B.dir("GameData00") / "DISSIDIA.BIN") == slurp(A.dir("GameData00") / "DISSIDIA.BIN"));
    r = sync_all(p, B.opts(), &err);
    CHECK(r.size() == 2 && r[0].action == Action::Same && r[1].action == Action::Same);
    A.change("GameData00", "DISSIDIA.BIN", 5);
    r = sync_all(p, A.opts(), &err);
    CHECK(r[0].suffix == "FCard00" && r[0].action == Action::Same && r[1].action == Action::Pushed);
    r = sync_all(p, B.opts(), &err);
    CHECK(r[1].action == Action::Pulled && slurp(B.dir("GameData00") / "DISSIDIA.BIN") == slurp(A.dir("GameData00") / "DISSIDIA.BIN"));
    // conflict, then keep the cloud copy
    A.change("GameData00", "DISSIDIA.BIN", 7);
    B.change("GameData00", "DISSIDIA.BIN", 8);
    sync_all(p, A.opts());
    r = sync_all(p, B.opts(), &err);
    CHECK(r[1].action == Action::Conflict);
    Item it = resolve_conflict(p, B.opts(), "GameData00", Choice::KeepRemote);
    CHECK(it.action == Action::Pulled && slurp(B.dir("GameData00") / "DISSIDIA.BIN") == slurp(A.dir("GameData00") / "DISSIDIA.BIN"));
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    g_base = std::string("http://127.0.0.1:") + argv[1];
    char tmpl[] = "/tmp/cloudprov_testXXXXXX";
    const std::string T = mkdtemp(tmpl);

    // ---------------- WebDAV
    admin("reset");
    WebDavConfig dc;
    dc.url = g_base + "/dav/";
    dc.user = "me"; dc.password = "app-password";
    {
        auto p = make_webdav_provider(dc);
        conformance(*p, "WebDAV");
        admin("reset");
        two_devices(*make_webdav_provider(dc), T + "/dav", "WebDAV");
        // wrong password: a clear error, nothing listed
        WebDavConfig bad = dc; bad.password = "wrong";
        std::vector<RemoteItem> items;
        const std::string e = make_webdav_provider(bad)->list(&items);
        CHECK(!e.empty() && e.find("401") != std::string::npos);
        CHECK(!make_webdav_provider(bad)->put("x.save", Bytes{1}).empty());
        std::printf("   bad password: %s\n", e.c_str());
    }

    // ---------------- Google: device sign-in
    admin("reset");
    GoogleConfig gc;
    gc.client_id = "client-1"; gc.client_secret = "SECRET";
    gc.auth_base = g_base; gc.api_base = g_base;
    DeviceCode dcd = google_start_device_flow(gc);
    CHECK(dcd.error.empty() && dcd.user_code == "ABCD-EFGH" && dcd.verification_url == "https://www.google.com/device" &&
          dcd.device_code == "DEV-client-1" && dcd.interval == 1 && dcd.expires_in == 1800);
    TokenResult t;
    std::vector<TokenResult::State> seen;
    for (int i = 0; i < 6; i++) {
        t = google_poll_device_flow(gc, dcd.device_code);
        seen.push_back(t.state);
        if (t.state == TokenResult::Done) break;
    }
    CHECK(seen.size() == 4 && seen[0] == TokenResult::Pending && seen[1] == TokenResult::Pending && seen[2] == TokenResult::SlowDown &&
          seen[3] == TokenResult::Done && t.refresh_token == "RT1");
    {
        GoogleConfig x = gc; x.client_id = "bad";
        CHECK(!google_start_device_flow(x).error.empty());
        std::printf("   bad client: %s\n", google_start_device_flow(x).error.c_str());
        x = gc; x.client_id = "deny";
        CHECK(google_poll_device_flow(x, "d").state == TokenResult::Denied);
        x.client_id = "expire";
        CHECK(google_poll_device_flow(x, "d").state == TokenResult::Expired);
        x = gc; x.client_secret = "wrong";
        CHECK(google_poll_device_flow(x, "d").state == TokenResult::Error);
    }

    // ---------------- Google Drive provider
    admin("reset");
    gc.refresh_token = "RT1";
    {
        auto p = make_google_drive_provider(gc);
        conformance(*p, "Google Drive");
        CHECK(stat("refreshes") == "1");                              // one access token for the whole run
        CHECK(stat("folders_created") == "1");                        // the saves folder is created once, then found
        // the access token expires between calls: refreshed transparently, the call still succeeds
        CHECK(p->put("Zed.meta", Bytes{'z'}).empty());
        admin("expire");
        Bytes got;
        CHECK(p->get("Zed.meta", &got).empty() && got == Bytes{'z'});
        CHECK(stat("refreshes") == "2");
        CHECK(p->remove("Zed.meta").empty());
        // a second device (fresh provider, no cached ids) finds the same folder and files; it does not create another folder
        CHECK(make_google_drive_provider(gc)->put("Shared0.meta", Bytes{'s'}).empty());
        CHECK(stat("folders_created") == "1");
        p->remove("Shared0.meta");
        CHECK(make_google_drive_provider(gc)->put("Shared.meta", Bytes{'s'}).empty());
        std::vector<RemoteItem> items;
        CHECK(p->list(&items).empty() && has(items, "Shared.meta", 1));
        p->remove("Shared.meta");
        // a revoked sign-in: a clear message
        GoogleConfig rev = gc; rev.refresh_token = "revoked";
        std::vector<RemoteItem> it2;
        const std::string e = make_google_drive_provider(rev)->list(&it2);
        CHECK(e.find("sign in again") != std::string::npos);
        std::printf("   revoked: %s\n", e.c_str());
        GoogleConfig none = gc; none.refresh_token.clear();
        CHECK(!make_google_drive_provider(none)->list(&it2).empty());
    }
    admin("reset");
    two_devices(*make_google_drive_provider(gc), T + "/drive", "Google Drive");

    // an unreachable server: errors, never a crash or a hang
    {
        GoogleConfig down = gc; down.api_base = "http://127.0.0.1:1"; down.auth_base = "http://127.0.0.1:1";
        std::vector<RemoteItem> items;
        CHECK(!make_google_drive_provider(down)->list(&items).empty());
        WebDavConfig wd = dc; wd.url = "http://127.0.0.1:1/dav";
        CHECK(!make_webdav_provider(wd)->list(&items).empty());
    }

    std::error_code ec;
    fs::remove_all(T, ec);
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("cloud providers: all checks passed\n");
    return 0;
}
