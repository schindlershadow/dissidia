// Cloud sync service: see psp_cloud_service.h.
#include "psp_cloud_service.h"

#include "psp_cloud_keys.h"
#include "psp_cloud_providers.h"
#include "psp_savemgr.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
using namespace psp_cloud;

namespace psp_cloud_svc {
namespace {

using Clock = std::chrono::steady_clock;
using CloudProvider = psp_cloud::Provider;   // (psp_cloud_svc::Provider is the backend choice)

struct Job {
    enum Kind { Sync, Push, Resolve, Signin } kind = Sync;
    std::string suffix;
    Choice choice = Choice::Skip;
};

struct G {
    std::mutex m;
    std::condition_variable cv;
    std::thread worker;
    bool started = false, quit = false, worker_done = false;
    Paths paths;
    Config cfg;
    Status st;
    std::deque<Job> jobs;
    bool busy = false;                        // a sync / push / resolve is running
    bool push_pending = false;
    Clock::time_point push_at;
    bool signin_cancel = false;
    std::unique_ptr<CloudProvider> (*factory)(const Config&) = nullptr;
} g;

std::string one_line(std::string s) {
    for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
    return s;
}
fs::path P(const std::string& s) { return fs::u8path(s); }

std::string ini_path() { return (P(g.paths.workdir) / "cloud.ini").u8string(); }

void touch() { ++g.st.revision; }

void load_ini() {
    g.cfg = Config{};
    std::ifstream f(P(ini_path()));
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "provider") g.cfg.provider = v == "folder" ? Provider::Folder : v == "webdav" ? Provider::WebDav : v == "google" ? Provider::Google : Provider::Off;
        else if (k == "folder") g.cfg.folder = v;
        else if (k == "webdav_url") g.cfg.webdav_url = v;
        else if (k == "webdav_user") g.cfg.webdav_user = v;
        else if (k == "webdav_pass") g.cfg.webdav_pass = v;
        else if (k == "google_refresh") g.cfg.google_refresh = v;
        else if (k == "auto") g.cfg.auto_sync = v != "0";
        else if (k == "replays") g.cfg.replays = v == "1";
    }
}

void save_ini() {
    const Config& c = g.cfg;
    const char* pn = c.provider == Provider::Folder ? "folder" : c.provider == Provider::WebDav ? "webdav" : c.provider == Provider::Google ? "google" : "off";
    std::string t = "# Cloud sync settings. Holds secrets (WebDAV password, Google sign-in): keep this file private.\n";
    t += std::string("provider=") + pn + "\nfolder=" + one_line(c.folder) + "\nwebdav_url=" + one_line(c.webdav_url) +
         "\nwebdav_user=" + one_line(c.webdav_user) + "\nwebdav_pass=" + one_line(c.webdav_pass) +
         "\ngoogle_refresh=" + one_line(c.google_refresh) + "\nauto=" + (c.auto_sync ? "1" : "0") +
         "\nreplays=" + (c.replays ? "1" : "0") + "\n";
    std::error_code ec;
    fs::create_directories(P(g.paths.workdir), ec);
    const fs::path tmp = P(ini_path() + ".tmp");
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        // private before the secrets are written
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        f << t;
    }
    fs::rename(tmp, P(ini_path()), ec);
}

std::string env_or(const char* name, const char* dflt) {
    const char* e = std::getenv(name);
    return e && *e ? e : dflt;
}

std::unique_ptr<CloudProvider> make_provider(const Config& c, std::string* err) {
    if (g.factory) return g.factory(c);
    switch (c.provider) {
    case Provider::Folder:
        if (c.folder.empty()) { *err = "no folder chosen"; return nullptr; }
        return make_folder_provider(c.folder);
    case Provider::WebDav:
        if (c.webdav_url.empty()) { *err = "no WebDAV address entered"; return nullptr; }
        return make_webdav_provider({c.webdav_url, c.webdav_user, c.webdav_pass});
    case Provider::Google: {
        if (c.google_refresh.empty()) { *err = "not signed in to Google Drive"; return nullptr; }
        GoogleConfig gc;
        gc.client_id = env_or("PSPRECOMP_GOOGLE_CLIENT_ID", PSP_GOOGLE_CLIENT_ID);
        gc.client_secret = env_or("PSPRECOMP_GOOGLE_CLIENT_SECRET", PSP_GOOGLE_CLIENT_SECRET);
        gc.refresh_token = c.google_refresh;
        gc.auth_base = env_or("PSPRECOMP_GOOGLE_AUTH_BASE", gc.auth_base.c_str());
        gc.api_base = env_or("PSPRECOMP_GOOGLE_API_BASE", gc.api_base.c_str());
        return make_google_drive_provider(gc);
    }
    default:
        *err = "cloud sync is off";
        return nullptr;
    }
}

Options make_options(const Config& c, const Paths& p, bool allow_pull) {
    Options o;
    o.saves_root = p.saves_root;
    o.backup_root = p.backup_root;
    o.state_path = (P(p.workdir) / "cloud" / "state.txt").u8string();
    o.disc_id = p.disc_id;
    o.allow_pull = allow_pull;
    const std::string root = p.saves_root;
    o.existing = [root](const std::string& suffix) -> std::string {   // the folder already holding this slot, any region's ID
        std::error_code ec;
        for (fs::directory_iterator it(P(root), ec), end; !ec && it != end; it.increment(ec)) {
            std::string d, x;
            const std::string n = it->path().filename().u8string();
            if (psp_savemgr::split_name(n, &d, &x) && x == suffix && it->is_directory(ec)) return n;
        }
        return "";
    };
    const bool replays = c.replays;
    o.include = [replays](const std::string& suffix) { return replays || suffix.rfind("Replay", 0) != 0; };
    return o;
}

std::string ago(time_t t) {
    const long d = long(std::time(nullptr) - t);
    if (d < 45) return "just now";
    if (d < 3600) return std::to_string(std::max(1L, d / 60)) + " min ago";
    if (d < 86400) return std::to_string(d / 3600) + " h ago";
    return std::to_string(d / 86400) + " d ago";
}

// ---- results -> status (call with the lock held) ----
void summarise(const std::vector<Item>& items, const std::string& fatal, bool push_only) {
    int up = 0, down = 0, conflicts = 0, errors = 0, skipped = 0;
    std::string first_error;
    for (const Item& i : items) {
        up += i.action == Action::Pushed;
        down += i.action == Action::Pulled;
        conflicts += i.action == Action::Conflict;
        skipped += i.action == Action::Skipped;
        if (i.action == Action::Error) { errors++; if (first_error.empty()) first_error = i.suffix + ": " + i.note; }
    }
    g.st.items = items;
    g.st.conflicts = conflicts;
    if (!fatal.empty()) { g.st.state = Status::Error; g.st.text = "Sync failed: " + one_line(fatal); }
    else if (errors) { g.st.state = Status::Error; g.st.text = "Sync problem: " + one_line(first_error); }
    else {
        g.st.last_ok = std::time(nullptr);
        std::string t = "Synced";
        if (up || down) t += " (" + std::string(up ? std::to_string(up) + " up" : "") + (up && down ? ", " : "") +
                             (down ? std::to_string(down) + " down" : "") + ")";
        if (conflicts) { g.st.state = Status::NeedsChoice; t = std::to_string(conflicts) + " save" + (conflicts == 1 ? "" : "s") + " changed on both sides: choose which to keep"; }
        else g.st.state = Status::Idle;
        if (skipped && !conflicts && push_only) t += "; newer cloud saves arrive at the next start";
        g.st.text = t;
    }
    touch();
}

void do_sync(std::unique_lock<std::mutex>& lk, bool allow_pull) {
    const Config cfg = g.cfg;
    const Paths paths = g.paths;
    std::string err;
    std::unique_ptr<CloudProvider> p = make_provider(cfg, &err);
    if (!p) {
        g.st.state = err == "cloud sync is off" ? Status::Off : Status::Error;
        g.st.text = err == "cloud sync is off" ? "Off" : "Not set up: " + err;
        touch();
        return;
    }
    g.st.state = Status::Syncing;
    g.st.text = "Syncing with " + p->label() + "...";
    touch();
    lk.unlock();
    std::string fatal;
    std::vector<Item> items = sync_all(*p, make_options(cfg, paths, allow_pull), &fatal);
    lk.lock();
    summarise(items, fatal, !allow_pull);
}

void do_resolve(std::unique_lock<std::mutex>& lk, const std::string& suffix, Choice c) {
    const Config cfg = g.cfg;
    const Paths paths = g.paths;
    std::string err;
    std::unique_ptr<CloudProvider> p = make_provider(cfg, &err);
    if (!p) { g.st.state = Status::Error; g.st.text = "Not set up: " + err; touch(); return; }
    g.st.state = Status::Syncing;
    g.st.text = "Applying your choice...";
    touch();
    lk.unlock();
    const Item done = resolve_conflict(*p, make_options(cfg, paths, true), suffix, c);
    lk.lock();
    std::vector<Item> items = g.st.items;
    bool found = false;
    for (Item& i : items) if (i.suffix == suffix) { i = done; found = true; }
    if (!found) items.push_back(done);
    summarise(items, "", false);
}

void do_signin(std::unique_lock<std::mutex>& lk) {
    GoogleConfig gc;
    gc.client_id = env_or("PSPRECOMP_GOOGLE_CLIENT_ID", PSP_GOOGLE_CLIENT_ID);
    gc.client_secret = env_or("PSPRECOMP_GOOGLE_CLIENT_SECRET", PSP_GOOGLE_CLIENT_SECRET);
    gc.auth_base = env_or("PSPRECOMP_GOOGLE_AUTH_BASE", gc.auth_base.c_str());
    gc.api_base = env_or("PSPRECOMP_GOOGLE_API_BASE", gc.api_base.c_str());
    g.signin_cancel = false;
    g.st.state = Status::SigningIn;
    g.st.text = "Contacting Google...";
    g.st.user_code.clear(); g.st.verify_url.clear();
    touch();
    lk.unlock();
    const DeviceCode dc = google_start_device_flow(gc);
    lk.lock();
    if (!dc.error.empty()) { g.st.state = Status::Error; g.st.text = dc.error; touch(); return; }
    g.st.user_code = dc.user_code;
    g.st.verify_url = dc.verification_url;
    g.st.text = "On your phone or computer open " + dc.verification_url + " and enter the code " + dc.user_code;
    touch();
    int interval = std::max(1, dc.interval);
    const auto deadline = Clock::now() + std::chrono::seconds(dc.expires_in > 0 ? dc.expires_in : 600);
    while (!g.quit && !g.signin_cancel && Clock::now() < deadline) {
        g.cv.wait_for(lk, std::chrono::seconds(interval), [] { return g.quit || g.signin_cancel; });
        if (g.quit || g.signin_cancel) break;
        lk.unlock();
        const TokenResult t = google_poll_device_flow(gc, dc.device_code);
        lk.lock();
        if (g.signin_cancel || g.quit) break;
        if (t.state == TokenResult::Pending) continue;
        if (t.state == TokenResult::SlowDown) { interval += 5; continue; }
        g.st.user_code.clear(); g.st.verify_url.clear();
        if (t.state == TokenResult::Done) {
            g.cfg.provider = Provider::Google;
            g.cfg.google_refresh = t.refresh_token;
            save_ini();
            g.st.state = Status::Idle;
            g.st.text = "Signed in to Google Drive";
            touch();
            g.jobs.push_back({Job::Sync, "", Choice::Skip});
        } else {
            g.st.state = Status::Error;
            g.st.text = t.error.empty() ? "Sign-in failed" : t.error;
            touch();
        }
        return;
    }
    g.st.user_code.clear(); g.st.verify_url.clear();
    if (g.st.state == Status::SigningIn) {
        g.st.state = g.signin_cancel ? (g.cfg.provider == Provider::Off ? Status::Off : Status::Idle) : Status::Error;
        g.st.text = g.signin_cancel ? "Sign-in cancelled" : "The code expired: start the sign-in again";
        touch();
    }
}

void worker_main() {
    std::unique_lock<std::mutex> lk(g.m);
    while (!g.quit) {
        Job job;
        bool have = false;
        if (!g.jobs.empty()) { job = g.jobs.front(); g.jobs.pop_front(); have = true; }
        else if (g.push_pending && Clock::now() >= g.push_at) { g.push_pending = false; job.kind = Job::Push; have = true; }
        if (!have) {
            if (g.push_pending) g.cv.wait_until(lk, g.push_at);
            else g.cv.wait(lk);
            continue;
        }
        if (job.kind == Job::Signin) { do_signin(lk); g.cv.notify_all(); continue; }
        g.busy = true;
        if (job.kind == Job::Sync) do_sync(lk, true);
        else if (job.kind == Job::Push) do_sync(lk, false);
        else do_resolve(lk, job.suffix, job.choice);
        g.busy = false;
        g.cv.notify_all();
    }
    g.worker_done = true;
    g.cv.notify_all();
}

void ensure_worker() {   // lock held
    if (g.started) return;
    g.started = true;
    g.quit = false;
    g.worker_done = false;
    g.worker = std::thread(worker_main);
}

}  // namespace

void init(const Paths& p) {
    std::lock_guard<std::mutex> lk(g.m);
    g.paths = p;
    load_ini();
    g.st = Status{};
    g.st.revision = 1;
    if (g.cfg.provider == Provider::Off) g.st.text = "Off";
    else g.st.state = Status::Idle, g.st.text = "Not synced yet";
}

void shutdown(int wait_ms) {
    std::unique_lock<std::mutex> lk(g.m);
    if (!g.started) return;
    g.quit = true;
    g.cv.notify_all();
    g.cv.wait_for(lk, std::chrono::milliseconds(wait_ms), [] { return g.worker_done; });
    const bool done = g.worker_done;
    lk.unlock();
    if (done) g.worker.join(); else g.worker.detach();
    lk.lock();
    g.started = false;
}

bool initialised() { std::lock_guard<std::mutex> lk(g.m); return !g.paths.workdir.empty(); }

Config config() { std::lock_guard<std::mutex> lk(g.m); return g.cfg; }

void set_config(const Config& c) {
    std::lock_guard<std::mutex> lk(g.m);
    g.cfg = c;
    save_ini();
    if (c.provider == Provider::Off) { g.st.state = Status::Off; g.st.text = "Off"; g.st.items.clear(); g.st.conflicts = 0; }
    else if (g.st.state == Status::Off || g.st.state == Status::Error) { g.st.state = Status::Idle; g.st.text = "Not synced yet"; }
    touch();
}

bool configured() {
    std::lock_guard<std::mutex> lk(g.m);
    if (g.factory) return g.cfg.provider != Provider::Off;
    switch (g.cfg.provider) {
    case Provider::Folder: return !g.cfg.folder.empty();
    case Provider::WebDav: return !g.cfg.webdav_url.empty();
    case Provider::Google: return !g.cfg.google_refresh.empty();
    default: return false;
    }
}

void sync_now() {
    std::lock_guard<std::mutex> lk(g.m);
    ensure_worker();
    g.jobs.push_back({Job::Sync, "", Choice::Skip});
    g.cv.notify_all();
}

void startup_sync() {
    if (!configured() || !config().auto_sync) return;
    sync_now();
}

void after_save() {
    if (!configured() || !config().auto_sync) return;
    std::lock_guard<std::mutex> lk(g.m);
    ensure_worker();
    g.push_pending = true;
    g.push_at = Clock::now() + std::chrono::seconds(4);     // several saves in a row become one upload
    g.cv.notify_all();
}

void resolve(const std::string& suffix, Choice c) {
    std::lock_guard<std::mutex> lk(g.m);
    ensure_worker();
    g.jobs.push_back({Job::Resolve, suffix, c});
    g.cv.notify_all();
}

void signin_start() {
    std::lock_guard<std::mutex> lk(g.m);
    ensure_worker();
    g.jobs.push_back({Job::Signin, "", Choice::Skip});
    g.cv.notify_all();
}

void signin_cancel() {
    std::lock_guard<std::mutex> lk(g.m);
    g.signin_cancel = true;
    g.cv.notify_all();
}

void signout() {
    std::lock_guard<std::mutex> lk(g.m);
    g.cfg.google_refresh.clear();
    if (g.cfg.provider == Provider::Google) g.cfg.provider = Provider::Off;
    save_ini();
    g.st.state = Status::Off;
    g.st.text = "Signed out of Google Drive";
    g.st.items.clear();
    g.st.conflicts = 0;
    touch();
}

Status status() {
    std::lock_guard<std::mutex> lk(g.m);
    Status s = g.st;
    if (s.state == Status::Idle && s.last_ok && s.text.rfind("Synced", 0) == 0) s.text += ", " + ago(s.last_ok);
    return s;
}

bool wait_ready(int timeout_ms) {
    std::unique_lock<std::mutex> lk(g.m);
    return g.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] {
        bool sync_queued = false;
        for (const Job& j : g.jobs) sync_queued |= j.kind != Job::Signin;
        return !g.busy && !sync_queued;
    });
}

void set_provider_factory_for_tests(std::unique_ptr<CloudProvider> (*f)(const Config&)) {
    std::lock_guard<std::mutex> lk(g.m);
    g.factory = f;
}

}  // namespace psp_cloud_svc
