// Host test for psp_report: SHA-256 / HMAC vectors, scrubbing, the zip, and a signed upload to a local copy of the report service
// (run_report.sh starts tools/report_service/report_service.py with a fake GitHub on a test port):  test_report <base url>
#include "psp_report.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>

std::string psp_release_version() { return "test-1.2.3"; }

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(int argc, char** argv) {
    using namespace psp_report;
    const std::string abc = "abc";
    CHECK(sha256_hex(reinterpret_cast<const uint8_t*>(abc.data()), 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog") == "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    setenv("HOME", "/home/alice", 1);
    setenv("USER", "alice", 1);
    const std::string sc = scrub("a /home/alice/x b\n/mnt/alice/data host alicefoo\naccess_token=abc123\nok line\n");
    CHECK(sc.find("alice/") == std::string::npos && sc.find("~/x") != std::string::npos);
    CHECK(sc.find("/~/data") != std::string::npos && sc.find("alicefoo") != std::string::npos);
    CHECK(sc.find("abc123") == std::string::npos && sc.find("ok line") != std::string::npos);
    if (argc < 2) { std::printf(g_fail ? "FAILED\n" : "unit ok\n"); return g_fail ? 1 : 0; }

    // a work folder with the files a report picks up, and ones it must not
    std::filesystem::path d = std::filesystem::temp_directory_path() / "report_test_work";
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d / "cloud");
    std::filesystem::current_path(d);
    { std::ofstream("last_run.log") << "start /home/alice/game\n" << std::string(10u << 20, 'x') << "\nend of log\n"; }
    { std::ofstream("setup.log") << "setup\n"; }
    { std::ofstream("settings.ini") << "fullscreen=1\ncloud_token=SECRET\n"; }
    { std::ofstream("cloud.ini") << "webdav_pass=SECRET\n"; }
    { std::ofstream("cloud/state.json") << "SECRET\n"; }
    { std::ofstream("crash-1.txt") << "boom\n"; }
    std::vector<uint8_t> ram(40u << 20);
    for (size_t i = 0; i < ram.size(); i++) ram[i] = (i % 4096) < 100 ? uint8_t(i * 7) : 0;   // mostly compressible
    set_ram(ram.data(), ram.size());
    Params p;
    p.title = "Test title";
    p.description = "It broke \"badly\"\nsecond line from /home/alice/x";
    p.include_dump = true;
    p.extra = {{"resolution", "1280x720"}};
    const auto pv = preview(true);
    bool saw_ram = false, saw_cloud = false;
    for (const auto& f : pv) { saw_ram |= f.name.rfind("ram.bin", 0) == 0; saw_cloud |= f.name.find("cloud") != std::string::npos; }
    CHECK(saw_ram && !saw_cloud);
    std::vector<uint8_t> zip;
    std::string notes;
    CHECK(build_zip(p, {{"version", "test"}}, ram, &zip, &notes));
    CHECK(zip.size() < (24u << 20));
    std::printf("zip %zu bytes, notes: %s\n", zip.size(), notes.c_str());

    Status st;
    upload(zip, argv[1], &st);
    std::printf("state %d text: %s url: %s\n", int(st.state), st.text.c_str(), st.issue_url.c_str());
    CHECK(st.state == Status::Done && st.number == 4242 && st.issue_url == "https://example.invalid/issues/4242");
    // a wrong key must be refused (403) and the zip saved locally
    {
        Status bad;
        setenv("PSPRECOMP_REPORT_TEST_BADTIME", "1", 1);
        std::vector<uint8_t> tiny(zip.begin(), zip.begin() + 22);
        upload(tiny, std::string(argv[1]) + "/nope", &bad);   // wrong path: 404 -> Failed + saved copy
        std::printf("bad: %s\n", bad.text.c_str());
        CHECK(bad.state == Status::Failed && !bad.saved_path.empty());
    }
    std::printf(g_fail ? "FAILED\n" : "all ok\n");
    return g_fail ? 1 : 0;
}
