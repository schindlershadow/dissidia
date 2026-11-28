// psp_report.h - the "Report an issue" button (Settings > Debug > Report an issue).
//
// Collects what a developer needs to understand a problem - the player's description, system information, the run log, setup log, crash
// reports, settings, and (optionally) a compressed dump of the PSP's RAM - into one zip of at most 24 MB and POSTs it to the report service
// (psprecomp/tools/report_service/report_service.py, which opens a GitHub issue and answers its URL). The request is signed with an HMAC
// (report_key.h; not a real secret, the service rate-limits). Everything runs on a worker thread; the settings page polls status().
//
// Never collected: cloud.ini, anything under cloud/, credentials (lines that look like tokens / passwords are blanked). The player's home
// folder and user name are replaced by "~" in every text file.
//
// Build flags of the setup programs: PSP_REPORT_STANDALONE (no game: version / files come from set_product), PSP_REPORT_NO_HTTP (no upload code:
// the Android app posts from Java).
// PSPRECOMP_REPORT_URL=<base> sends to another service (tests: http://127.0.0.1:18791). PSPRECOMP_REPORT_OLD=1 hides the button.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace psp_report {

using Pairs = std::vector<std::pair<std::string, std::string>>;

/// The PSP's memory (main thread, once at start): what "include memory dump" compresses.
void set_ram(const uint8_t* rdram, size_t size);
/// The setup programs (PC wizard, Android setup app) use the same code for their "Report an issue": they name the product, its version, the files to
/// send (instead of the game's run.log / settings.ini ...) and a folder whose free space is reported. No game facts (GPU, game id, mods) then.
struct ProductFile { std::string rel, path; size_t tail; };   // name inside the zip, file, bytes kept from its end
struct Product {
    std::string name;      // "installer"
    std::string version;
    std::vector<ProductFile> files;
    std::string space_dir; // free space of this folder is reported
    std::string save_dir;  // a report that could not be sent is saved here (default: ./reports)
    Pairs facts;           // anything else (installer kind, android version ...)
};
void set_product(const Product& p);
/// The graphics strings the event loop already prints at start (render thread): version, renderer, vendor.
void note_gpu(const char* version, const char* renderer, const char* vendor);
/// A game module's own facts for the report, one "key=value" per line (for example in_battle=1). Called on the main thread when a report starts.
void set_state_hook(std::string (*fn)());

struct Params {
    std::string title;
    std::string description;
    bool include_dump = true;
    Pairs extra;     // system facts only the settings menu knows (window, renderer, mods, changed debug switches ...)
};

/// Builds the report zip into `path` now (the calling thread; no upload). For the Android app, whose Java side does the HTTPS POST.
bool build_zip_file(const Params& p, const std::string& path, std::string* error);

struct FileInfo { std::string name; size_t size; };
/// The files a report would hold right now (names inside the zip, raw size). The memory dump is listed as "ram.bin.z" with the used size.
std::vector<FileInfo> preview(bool include_dump);

/// Main thread: snapshots the RAM and the system facts, then collects + uploads on a worker thread. False when a report is already running.
bool start(const Params& p);

struct Status {
    enum State { Idle, Working, Done, Failed } state = Idle;
    std::string text;        // progress while Working; "Issue #N opened" / the error text afterwards
    std::string issue_url;   // Done
    int number = 0;          // Done
    std::string saved_path;  // Failed (and Done when the server sent no issue): the zip saved locally
    unsigned rev = 0;        // changes with every update
};
Status status();
/// Main thread: true once after a report finished (so the caller opens the URL and shows the toast).
bool take_finished();

// ---- the pieces, public for the unit test (runtime/tests/test_report.cpp) ----
std::string sha256_hex(const uint8_t* data, size_t n);
std::string hmac_sha256_hex(const std::string& key, const std::string& msg);
/// Builds the zip in memory (no upload). `zip_out` gets the bytes; `notes` what was left out and why.
bool build_zip(const Params& p, const Pairs& system, const std::vector<uint8_t>& ram, std::vector<uint8_t>* zip_out, std::string* notes);
/// Signs and POSTs `zip` to <base>/v1/report; fills the Status (Done or Failed, saves the zip locally on failure).
void upload(const std::vector<uint8_t>& zip, const std::string& base, Status* out);
/// The base URL: PSPRECOMP_REPORT_URL, else the public service.
std::string service_base();
/// Replaces the home folder and user name in `text` by "~" and blanks lines that look like credentials.
std::string scrub(const std::string& text);

}  // namespace psp_report
