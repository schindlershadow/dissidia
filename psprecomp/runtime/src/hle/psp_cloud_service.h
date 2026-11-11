// Cloud sync service: owns the cloud configuration (cloud.ini, secrets included, mode 0600), a worker thread that runs the
// syncs (psp_cloudsync) through the chosen provider, and the status the menus show. The menus (Manage Saves on the save
// list, Settings > Cloud) and the savedata HLE only call these functions; every call returns at once, the work happens
// on the worker thread.
//
// When things run:
//   start-up        a full sync (uploads and downloads) when enabled; the savedata HLE waits for it (wait_ready) before the
//                   game's first save dialog, so a download never replaces a save the game is reading
//   after a save    a full sync a few seconds later (debounced); the savedata HLE waits for a running sync before the game's next
//                   save dialog, so a download never lands while the game is reading a save
//   on request      "Sync now" from a menu: full sync
// Conflicts (both sides changed) are never resolved silently: they wait in status().items until resolve() is called.
#pragma once

#include "psp_cloudsync.h"

#include <ctime>
#include <string>
#include <vector>

namespace psp_cloud_svc {

enum class Provider { Off, Folder, WebDav, Google };

struct Config {
    Provider provider = Provider::Off;
    std::string folder;                 // Folder: a directory (a Syncthing / Drive / OneDrive / NAS folder)
    std::string webdav_url, webdav_user, webdav_pass;
    std::string google_refresh;         // set by the sign-in
    bool auto_sync = true;              // sync at start-up and after saving
    bool replays = false;               // include replays (large)
};

struct Paths {
    std::string workdir;                // cloud.ini and cloud/ live here
    std::string saves_root;             // ms0/PSP/SAVEDATA
    std::string backup_root;            // ms0/PSP/SAVEDATA_BACKUP
    std::string disc_id;                // the running game's disc ID (ULUS10566 ...)
};

struct Status {
    enum State { Off, Idle, Syncing, SigningIn, NeedsChoice, Error } state = Off;
    std::string text;                   // one line: "Synced 2 min ago", "Sync failed: ...", ...
    std::vector<psp_cloud::Item> items; // the last run's results (conflicts included)
    time_t last_ok = 0;
    std::string user_code, verify_url;  // while SigningIn
    int conflicts = 0;
    unsigned revision = 0;              // changes whenever anything above does (menus re-render on it)
};

// Loads cloud.ini and remembers the paths. Safe to call again (a new game directory).
void init(const Paths& p);
// Stops the worker (lets a running transfer finish for up to wait_ms).
void shutdown(int wait_ms = 3000);

bool initialised();                     // init() was called (the game's folders are known)

Config config();
void set_config(const Config& c);       // saved at once; used from the next sync
bool configured();                      // a provider is chosen and has what it needs (Google: signed in)

void sync_now();                        // full sync
void startup_sync();                    // sync_now when auto_sync is on
void after_save();                      // debounced sync when auto_sync is on
void resolve(const std::string& suffix, psp_cloud::Choice c);

void signin_start();                    // Google device flow; status() shows user_code / verify_url meanwhile
void signin_cancel();
void signout();                         // forgets the Google sign-in

Status status();
// Waits until no sync is running (true) or the timeout passes (false). Conflicts waiting for a choice do not block it.
bool wait_ready(int timeout_ms);

inline const char* provider_name(Provider p) {
    switch (p) {
    case Provider::Folder: return "Folder";
    case Provider::WebDav: return "WebDAV";
    case Provider::Google: return "Google Drive";
    default: return "Off";
    }
}

// Tests: build providers from something else than the configuration.
void set_provider_factory_for_tests(std::unique_ptr<psp_cloud::Provider> (*f)(const Config&));

}  // namespace psp_cloud_svc
