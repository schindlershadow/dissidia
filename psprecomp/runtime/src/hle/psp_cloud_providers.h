// Cloud storage providers for psp_cloudsync (psp_cloudsync.h): WebDAV (Nextcloud, ownCloud, most NAS boxes) and Google
// Drive (the hidden per-app "appDataFolder": the app sees only its own files). Both run on psp_http, so call them from a
// worker thread. Endpoints are configurable so the tests can point them at a local fake server.
#pragma once

#include "psp_cloudsync.h"

#include <memory>
#include <string>

namespace psp_cloud {

// ---------------------------------------------------------------- WebDAV
struct WebDavConfig {
    std::string url;        // the folder that holds the saves, e.g. https://cloud.example.com/remote.php/dav/files/me/Dissidia
    std::string user;
    std::string password;   // for Nextcloud / ownCloud an app password
};
// The folder is created on the first upload when missing (its parent must exist).
std::unique_ptr<Provider> make_webdav_provider(const WebDavConfig& cfg);

// ---------------------------------------------------------------- Google Drive
struct GoogleConfig {
    std::string client_id;
    std::string client_secret;       // not confidential for a "TVs and limited-input devices" OAuth client
    std::string refresh_token;       // from the sign-in below; stored by the caller (settings.ini)
    std::string auth_base = "https://oauth2.googleapis.com";
    std::string api_base = "https://www.googleapis.com";
};

// Sign-in with the OAuth device flow: show `user_code` and `verification_url` to the person (they enter the code on any
// phone or computer), then poll every `interval` seconds until the result is Done or a final state.
struct DeviceCode {
    std::string device_code, user_code, verification_url;
    int expires_in = 0;
    int interval = 5;
    std::string error;               // "" on success
};
DeviceCode google_start_device_flow(const GoogleConfig& cfg);

struct TokenResult {
    enum State { Pending, SlowDown, Denied, Expired, Done, Error } state = Error;
    std::string refresh_token;       // Done
    std::string error;
};
TokenResult google_poll_device_flow(const GoogleConfig& cfg, const std::string& device_code);

// Uses cfg.refresh_token; refreshes the access token as needed (and once more after a 401).
std::unique_ptr<Provider> make_google_drive_provider(const GoogleConfig& cfg);

}  // namespace psp_cloud
