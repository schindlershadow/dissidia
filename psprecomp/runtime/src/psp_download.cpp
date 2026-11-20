#include "psp_download.h"

#include "hle/psp_http.h"

#include <cstdio>
#include <filesystem>

bool psp_download_available() { return psp_http::tls_available(); }

bool psp_download_to_file(const std::string& url, const std::string& path, const std::function<void(uint64_t bytes)>& on_bytes, std::string* err) {
    namespace fs = std::filesystem;
    std::FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(fs::u8path(path).wstring().c_str(), L"wb");
#else
    f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) { if (err) *err = "could not write " + path; return false; }
    uint64_t got = 0;
    bool write_ok = true;
    psp_http::Request rq;
    rq.url = url;
    rq.max_redirects = 5;
    rq.timeout_ms = 60000;
    rq.sink = [&](const uint8_t* p, size_t n) {
        if (std::fwrite(p, 1, n, f) != n) { write_ok = false; return false; }
        got += n;
        if (on_bytes) on_bytes(got);
        return true;
    };
    const psp_http::Response r = psp_http::perform(rq);
    std::fclose(f);
    std::string why;
    if (!write_ok) why = "could not write " + path + " (is the disk full?)";
    else if (!r.ok()) why = "the download failed: " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error);
    if (!why.empty()) {
        std::error_code ec;
        fs::remove(fs::u8path(path), ec);
        if (err) *err = why;
        return false;
    }
    std::fprintf(stderr, "[DOWNLOAD] %s: %llu bytes -> %s\n", url.c_str(), static_cast<unsigned long long>(got), path.c_str());
    return true;
}
