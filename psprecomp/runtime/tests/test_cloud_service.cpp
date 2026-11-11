// Host test for the cloud sync service (psp_cloud_service): configuration file, start-up sync, push-only sync after a save,
// conflicts that wait for a choice, the Google sign-in flow against the fake server, sign-out and persistence. The worker
// thread is real; "the other device" is a second install synced through the same cloud folder.
#include "psp_cloud_service.h"
#include "psp_cloud_providers.h"
#include "psp_http.h"
#include "psp_savemgr.h"
#include "cloud_fixture.h"

#include <chrono>
#include <thread>
#include <sys/stat.h>

using namespace psp_cloud;
namespace svc = psp_cloud_svc;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool wait_for(const std::function<bool()>& pred, int ms = 15000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}
static bool idle_after_sync() { return svc::wait_ready(15000); }

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string port = argv[1];
    char tmpl[] = "/tmp/cloudsvc_testXXXXXX";
    const std::string T = mkdtemp(tmpl);
    const std::string cloud = T + "/cloud";
    Dev A{T + "/A", "ULUS10566"}, B{T + "/B", "ULES01505"};
    make_save(A.saves(), "ULUS10566GameData00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Ender", "DISSIDIA.BIN", 1);
    make_save(A.saves(), "ULUS10566FCard00", "DISSIDIA 012 FINAL FANTASY", "PLAYER: Akan", "FCARD.BIN", 2);
    auto cloud_prov = make_folder_provider(cloud);

    svc::Paths paths{A.root, A.saves(), A.root + "/ms0/PSP/SAVEDATA_BACKUP", A.disc};
    svc::init(paths);
    CHECK(!svc::configured() && svc::status().state == svc::Status::Off);
    svc::startup_sync();                                   // not configured: nothing happens
    CHECK(svc::wait_ready(500) && !fs::exists(cloud));

    // configure a folder: the ini is written private, and start-up sync uploads everything
    svc::Config c = svc::config();
    c.provider = svc::Provider::Folder;
    c.folder = cloud;
    c.webdav_pass = "s3cret";
    svc::set_config(c);
    CHECK(svc::configured());
    struct stat sb{};
    CHECK(stat((A.root + "/cloud.ini").c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0600);
    svc::startup_sync();
    CHECK(idle_after_sync());
    svc::Status s = svc::status();
    CHECK(s.state == svc::Status::Idle && s.text.find("2 up") != std::string::npos);
    std::printf("   status: %s\n", s.text.c_str());
    CHECK(fs::exists(cloud + "/GameData00.save") && fs::exists(cloud + "/FCard00.meta"));

    // the other device pulls, changes, pushes; a full sync here downloads it
    Options ob = B.opts();
    sync_all(*cloud_prov, ob);
    B.change("GameData00", "DISSIDIA.BIN", 5);
    sync_all(*cloud_prov, ob);
    svc::sync_now();
    CHECK(idle_after_sync());
    CHECK(slurp(A.dir("GameData00") / "DISSIDIA.BIN") == slurp(B.dir("GameData00") / "DISSIDIA.BIN"));
    CHECK(svc::status().text.find("1 down") != std::string::npos);

    // after a save: push-only, debounced. The other device's newer FCard00 is NOT downloaded during play.
    B.change("FCard00", "FCARD.BIN", 6);
    sync_all(*cloud_prov, ob);
    const Bytes fcard_before = slurp(A.dir("FCard00") / "FCARD.BIN");
    A.change("GameData00", "DISSIDIA.BIN", 9);
    svc::after_save();
    svc::after_save();                                      // two saves in a row: one upload
    CHECK(wait_for([&] { Bytes b; return make_folder_provider(cloud)->get("GameData00.meta", &b).empty() &&
                                 meta_from_text(std::string(b.begin(), b.end())).hash == hash_save(A.dir("GameData00").string()); }));
    CHECK(idle_after_sync());
    CHECK(slurp(A.dir("FCard00") / "FCARD.BIN") == fcard_before);        // untouched
    CHECK(svc::status().state == svc::Status::Idle && svc::status().text.find("next start") != std::string::npos);
    svc::sync_now();                                        // the next full sync takes it
    CHECK(idle_after_sync());
    CHECK(slurp(A.dir("FCard00") / "FCARD.BIN") == slurp(B.dir("FCard00") / "FCARD.BIN"));

    // conflict: waits for a choice, does not block wait_ready, then resolve
    sync_all(*cloud_prov, ob);                              // B takes A's latest first
    A.change("GameData00", "DISSIDIA.BIN", 11);
    B.change("GameData00", "DISSIDIA.BIN", 12);
    sync_all(*cloud_prov, ob);                              // B pushes
    svc::sync_now();
    CHECK(idle_after_sync());
    s = svc::status();
    CHECK(s.state == svc::Status::NeedsChoice && s.conflicts == 1);
    bool found = false;
    for (auto& i : s.items) found |= i.suffix == "GameData00" && i.action == Action::Conflict;
    CHECK(found);
    std::printf("   status: %s\n", s.text.c_str());
    const unsigned rev = s.revision;
    svc::resolve("GameData00", Choice::KeepRemote);
    CHECK(wait_for([&] { return svc::status().revision != rev && svc::status().state != svc::Status::Syncing; }));
    CHECK(idle_after_sync());
    s = svc::status();
    CHECK(s.state == svc::Status::Idle && s.conflicts == 0);
    CHECK(slurp(A.dir("GameData00") / "DISSIDIA.BIN") == slurp(B.dir("GameData00") / "DISSIDIA.BIN"));

    // auto sync off: start-up and after-save do nothing
    c = svc::config(); c.auto_sync = false; svc::set_config(c);
    A.change("FCard00", "FCARD.BIN", 40);
    svc::after_save();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    svc::startup_sync();
    CHECK(svc::wait_ready(500));
    Bytes meta;
    make_folder_provider(cloud)->get("FCard00.meta", &meta);
    CHECK(meta_from_text(std::string(meta.begin(), meta.end())).hash != hash_save(A.dir("FCard00").string()));
    c.auto_sync = true; svc::set_config(c);

    // persistence: a new init reads the ini back
    svc::shutdown();
    svc::init(paths);
    c = svc::config();
    CHECK(c.provider == svc::Provider::Folder && c.folder == cloud && c.webdav_pass == "s3cret" && c.auto_sync && !c.replays);

    // ---------------- Google sign-in against the fake server
    setenv("PSPRECOMP_GOOGLE_CLIENT_ID", "fast", 1);
    setenv("PSPRECOMP_GOOGLE_CLIENT_SECRET", "SECRET", 1);
    setenv("PSPRECOMP_GOOGLE_AUTH_BASE", ("http://127.0.0.1:" + port).c_str(), 1);
    setenv("PSPRECOMP_GOOGLE_API_BASE", ("http://127.0.0.1:" + port).c_str(), 1);
    {
        psp_http::Request r; r.url = "http://127.0.0.1:" + port + "/admin/reset"; psp_http::perform(r);
    }
    svc::signin_start();
    CHECK(wait_for([] { return svc::status().state == svc::Status::SigningIn && !svc::status().user_code.empty(); }));
    s = svc::status();
    CHECK(s.user_code == "ABCD-EFGH" && s.verify_url == "https://www.google.com/device");
    std::printf("   status: %s\n", s.text.c_str());
    CHECK(wait_for([] { return svc::status().state != svc::Status::SigningIn; }));
    CHECK(svc::config().provider == svc::Provider::Google && svc::config().google_refresh == "RT1");
    CHECK(idle_after_sync());                               // the sync that follows a sign-in
    CHECK(wait_for([] { return svc::status().text.find("Synced") == 0; }));
    s = svc::status();
    CHECK(s.state == svc::Status::Idle);
    std::printf("   status: %s\n", s.text.c_str());
    {   // the saves really are in the fake Drive
        GoogleConfig gc; gc.client_id = "fast"; gc.client_secret = "SECRET"; gc.refresh_token = "RT1";
        gc.auth_base = gc.api_base = "http://127.0.0.1:" + port;
        std::vector<RemoteItem> items;
        CHECK(make_google_drive_provider(gc)->list(&items).empty() && items.size() >= 4);
    }
    // cancel a sign-in in progress
    setenv("PSPRECOMP_GOOGLE_CLIENT_ID", "client-1", 1);
    svc::signin_start();
    CHECK(wait_for([] { return svc::status().state == svc::Status::SigningIn; }));
    svc::signin_cancel();
    CHECK(wait_for([] { return svc::status().state != svc::Status::SigningIn; }));
    CHECK(svc::status().text.find("cancelled") != std::string::npos);
    // a bad client id: a clear error
    setenv("PSPRECOMP_GOOGLE_CLIENT_ID", "bad", 1);
    svc::signin_start();
    CHECK(wait_for([] { return svc::status().state == svc::Status::Error; }));
    CHECK(svc::status().text.find("invalid_client") != std::string::npos);
    // sign out
    svc::signout();
    CHECK(svc::config().google_refresh.empty() && svc::config().provider == svc::Provider::Off && !svc::configured());
    CHECK(svc::status().state == svc::Status::Off);

    svc::shutdown();
    std::error_code ec;
    fs::remove_all(T, ec);
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("cloud service: all checks passed\n");
    return 0;
}
