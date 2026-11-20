// psp_download.h — fetch a URL straight into a file (https through the runtime's HTTP client, src/hle/psp_http.h: mbedTLS with the
// embedded CA bundle), for large downloads a game module offers (Dissidia: its HD texture pack). Blocking: call it from a worker thread.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

// https works in this build.
bool psp_download_available();
// Downloads `url` (following up to 5 redirects) into `path` (created / replaced), handing the running byte count to `on_bytes` (may be
// empty) as it arrives; nothing is kept in memory. false and `err` (for the player) on any failure; a partial file is deleted.
bool psp_download_to_file(const std::string& url, const std::string& path, const std::function<void(uint64_t bytes)>& on_bytes, std::string* err);
