// Builds a payload with release/make_single.py semantics by hand (zip via python is not available here: the test is given a
// wrapped file by the caller) -- usage: test_payload <wrapped-file> <expected-version>
// Checks find_payload + extract_payload (+ re-extract is a no-op).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "../payload.h"

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: test_payload FILE VERSION\n"); return 2; }
    namespace fs = std::filesystem;
    setup::Payload p;
    if (!setup::find_payload(argv[1], p)) { std::fprintf(stderr, "FAIL: no payload\n"); return 1; }
    if (p.version != argv[2]) { std::fprintf(stderr, "FAIL: version %s\n", p.version.c_str()); return 1; }
    fs::path dest = fs::temp_directory_path() / "test_payload_dest";
    fs::remove_all(dest);
    double last = 0;
    std::string e = setup::extract_payload(argv[1], p, dest, [&](double f) { last = f; return true; });
    if (!e.empty()) { std::fprintf(stderr, "FAIL: %s\n", e.c_str()); return 1; }
    if (!fs::exists(dest / ".payload-ok") || last < 0.999) { std::fprintf(stderr, "FAIL: marker/progress (%f)\n", last); return 1; }
    e = setup::extract_payload(argv[1], p, dest, nullptr);
    if (!e.empty()) { std::fprintf(stderr, "FAIL second: %s\n", e.c_str()); return 1; }
    std::printf("ok %s\n", dest.string().c_str());
    return 0;
}
