#include "psp_savemgr_ui.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace psp_sddialog {
namespace {

constexpr uint32_t K_START = 0x0008, K_TRIANGLE = 0x1000;

// ---- UTF-8 <-> UTF-16 (BMP) for the keyboard ----
std::u16string to_u16(const std::string& s) {
    std::u16string o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        int n = 0;
        if (c >= 0xF0) { cp = c & 7; n = 3; }
        else if (c >= 0xE0) { cp = c & 15; n = 2; }
        else if (c >= 0xC0) { cp = c & 31; n = 1; }
        for (int k = 1; k <= n && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 63);
        i += size_t(n) + 1;
        o.push_back(cp < 0x10000 ? char16_t(cp) : u'?');
    }
    return o;
}
std::string to_u8(const std::u16string& s) {
    std::string o;
    for (char16_t ch : s) {
        uint32_t c = ch;
        if (c < 0x80) o += char(c);
        else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 63)); }
        else { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
    }
    return o;
}
// Shorten a UTF-8 string to `max` characters, keeping the end ("...tail") when `keep_tail` (paths).
std::string fit(const std::string& s, size_t max, bool keep_tail = false) {
    std::u16string u = to_u16(s);
    if (u.size() <= max || max < 4) return s;
    return keep_tail ? "..." + to_u8(u.substr(u.size() - (max - 3))) : to_u8(u.substr(0, max - 2)) + "..";
}
std::string when(time_t t) {
    if (!t) return "";
    char b[32];
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tmv);
    return b;
}
std::string size_text(uint64_t bytes) {
    char b[32];
    std::snprintf(b, sizeof b, "%llu KB", (unsigned long long)((bytes + 1023) / 1024));
    return b;
}
std::string describe(const psp_savemgr::SaveInfo& s) {
    std::string d = psp_savemgr::one_line(s.detail);
    std::string out = s.name;
    if (!d.empty()) out = d + "  (" + s.name + ")";
    const std::string w = when(s.mtime);
    if (!w.empty()) out += "  " + w;
    return out + "  " + size_text(s.bytes);
}
std::string title_of(const psp_savemgr::SaveInfo& s) {
    return std::string(psp_savemgr::region_of(s.disc)) + " " + psp_savemgr::kind_of(s.suffix);
}

}  // namespace

void ManagerDialog::go(Screen s, int sel) {
    screen_ = s;
    sel_ = sel;
    held_dir_ = 0;
    ++rev_;
}

void ManagerDialog::begin(const ManagerConfig& cfg) {
    cfg_ = cfg;
    changed_ = false;
    armed_ = false;
    prev_ = held_dir_ = 0;
    note_.clear();
    result_.clear();
    keep_region_ = true;
    go(Screen::Main);
}

uint32_t ManagerDialog::revision() const {
    uint32_t r = rev_ + (screen_ == Screen::TypePath || screen_ == Screen::CloudType ? kbd_.revision() : 0);
    if ((screen_ == Screen::Cloud || screen_ == Screen::CloudConflict) && cfg_.cloud_status) r += cfg_.cloud_status().revision * 7919u;
    return r;
}

void ManagerDialog::load_sources() {
    sources_ = cfg_.detect ? cfg_.detect(importing_)
                           : psp_savemgr::detect_sources(importing_, cfg_.saves_root);
    source_counts_.clear();
    for (auto& s : sources_)
        source_counts_.push_back(importing_ ? int(psp_savemgr::scan(s.path).size()) : 0);
}

int ManagerDialog::row_count() const {
    std::string a, b;
    return int(rows(&a, &b).size());
}

int ManagerDialog::start_row() const {   // PickSaves: index of the "Start" row
    return 1 + int(saves_.size()) + (importing_ ? 0 : 1);
}

std::vector<MenuRow> ManagerDialog::rows(std::string* title, std::string* tab) const {
    std::vector<MenuRow> r;
    auto row = [&](const std::string& l, const std::string& v, const std::string& h) {
        MenuRow m; m.label = l; m.value = v; m.help = h; r.push_back(m);
    };
    *tab = "Saves";
    switch (screen_) {
    case Screen::Main:
        *title = "Manage Saves";
        row("Import saves", "", "Copy Dissidia 012 saves from PPSSPP, a PSP memory stick or any folder into this game. "
            "Saves from every region work.");
        row("Export saves", "", "Copy this game's saves to PPSSPP, a PSP memory stick or any folder.");
        if (cfg_.cloud_status) {
            const psp_cloud_svc::Status st = cfg_.cloud_status();
            const bool on = cfg_.cloud_configured && cfg_.cloud_configured();
            row("Cloud sync", on ? (st.conflicts ? "needs choice" : st.state == psp_cloud_svc::Status::Syncing ? "syncing" : "on") : "off",
                "Keep your saves in step between devices (Windows, Linux, Android) through Google Drive, a WebDAV server or a "
                "synced folder. " + st.text);
        }
        row("Back", "", "Return to the save list.");
        break;
    case Screen::Cloud: {
        *title = "Cloud Sync";
        const psp_cloud_svc::Status st = cfg_.cloud_status();
        const psp_cloud_svc::Config cc = cfg_.cloud_config ? cfg_.cloud_config() : psp_cloud_svc::Config{};
        for (const CloudRow& cr : cloud_rows()) {
            switch (cr.act) {
            case CloudAct::Info: {
                const char* v = st.state == psp_cloud_svc::Status::Off ? "off" : st.state == psp_cloud_svc::Status::Syncing ? "working..."
                              : st.state == psp_cloud_svc::Status::SigningIn ? "signing in" : st.state == psp_cloud_svc::Status::Error ? "problem"
                              : st.state == psp_cloud_svc::Status::NeedsChoice ? "needs choice" : "ok";
                row(cc.provider == psp_cloud_svc::Provider::Off ? "Status" : std::string(psp_cloud_svc::provider_name(cc.provider)), v,
                    st.text + (cc.provider == psp_cloud_svc::Provider::Off || (cc.provider != psp_cloud_svc::Provider::Google && !(cfg_.cloud_configured && cfg_.cloud_configured()))
                               ? "\nOther services (WebDAV, a synced folder) are set up in the settings menu (F1 / Guide button), Cloud page." : ""));
                break;
            }
            case CloudAct::SyncNow:
                row("Sync now", st.state == psp_cloud_svc::Status::Syncing ? "working..." : "",
                    "Upload changed saves and download newer ones now. " + st.text);
                break;
            case CloudAct::SignIn:
                row("Google Drive: sign in", "", "Shows a code and opens google.com/device (the code is copied). The game only gets access to a folder it creates in your Drive.");
                break;
            case CloudAct::CancelSignIn:
                row("Cancel sign-in", st.user_code, st.text);
                break;
            case CloudAct::SetupWebDav:
                row("WebDAV server...", "", "Nextcloud, ownCloud and most NAS boxes. You type the address of a folder on the server, the user name and the password "
                    "(for Nextcloud use an app password). The password is shown while you type.");
                break;
            case CloudAct::SetupFolder:
                row("Synced folder...", "", "A folder that another program keeps in sync between your devices (Syncthing, the Drive / OneDrive / Dropbox desktop apps, "
                    "a network share). You type its path.");
                break;
            case CloudAct::ChangeService:
                row("Change service", psp_cloud_svc::provider_name(cc.provider), "Stop using " + std::string(psp_cloud_svc::provider_name(cc.provider)) +
                    " and pick another. Your saves and the sign-in details are not deleted.");
                break;
            case CloudAct::Conflict: {
                std::string what;
                for (auto& it : st.items) if (it.suffix == cr.suffix) what = "This device: " + when(it.local_mtime) + "   Cloud: " + when(it.remote.mtime);
                row("Choose: " + cr.suffix, "", "Changed on both sides. " + what);
                break;
            }
            case CloudAct::SignOut:
                row("Sign out of Google Drive", "", "Forget the sign-in on this device. The saves in your Drive stay where they are.");
                break;
            case CloudAct::Back:
                row("Back", "", "");
                break;
            }
        }
        break;
    }
    case Screen::CloudConflict: {
        *title = "Which Save To Keep?";
        std::string lt, rt, dev;
        for (auto& it : cfg_.cloud_status().items)
            if (it.suffix == cloud_suffix_) { lt = when(it.local_mtime); rt = when(it.remote.mtime); dev = it.remote.device; }
        row("Keep this device's save", lt, "The save on this device goes to the cloud. The cloud copy is kept once as " + cloud_suffix_ + ".bak.");
        row("Take the cloud save", rt, "The cloud save replaces the one on this device. Yours is kept in the save backup folder.");
        row("Decide later", "", "Leave both as they are. Nothing is uploaded or downloaded for this save until you choose.");
        break;
    }
    case Screen::PickSource:
    case Screen::PickDest: {
        *title = importing_ ? "Import From" : "Export To";
        for (size_t i = 0; i < sources_.size(); ++i) {
            char n[32] = "";
            if (importing_) std::snprintf(n, sizeof n, "%d save%s", source_counts_[i], source_counts_[i] == 1 ? "" : "s");
            row(fit(sources_[i].label, 30), n, sources_[i].path);
        }
        row("Type a path...", "", note_.empty() ? std::string("Enter the folder with the save folders (a PSP/SAVEDATA folder, "
            "or a memory stick or PPSSPP's own folder).") : note_);
        row("Back", "", "");
        if (sources_.empty() && note_.empty())
            r[0].help = std::string("No ") + (importing_ ? "folder with Dissidia 012 saves" : "PPSSPP or memory stick folder") +
                        " was found automatically. Type its path instead.";
        break;
    }
    case Screen::TypePath:
        *title = "Path";
        break;
    case Screen::CloudType:
        *title = "Cloud Sync";
        break;
    case Screen::PickSaves: {
        *title = importing_ ? "Import Saves" : "Export Saves";
        int n = 0;
        for (bool c : checked_) n += c;
        row("All saves", n > 0 ? "[x]" : "[ ]", importing_ ? "Select every save (one per slot: this game's region first), or clear the selection."
                                                          : "Select every save, or clear the selection.");
        for (size_t i = 0; i < saves_.size(); ++i)
            row(std::string(checked_[i] ? "[x] " : "[ ] ") + fit(saves_[i].suffix, 24), title_of(saves_[i]), describe(saves_[i]));
        if (!importing_) {
            MenuRow m;
            m.label = "Folder names";
            m.value = keep_region_ ? "keep region" : "as " + cfg_.disc_id;
            m.help = keep_region_ ? "Keep each save's own region ID (ULUS10566..., ULES01505...)."
                                  : "Rename every save to this game's ID so a matching PPSSPP game finds it.";
            m.can_dec = m.can_inc = true;
            r.push_back(m);
        }
        char b[64];
        std::snprintf(b, sizeof b, "%s (%d)", importing_ ? "Import selected" : "Export selected", n);
        row(b, "", !note_.empty() ? note_ : importing_ ? "Copy the selected saves into this game. The data is untouched; the folder gets this game's ID."
                              : "Copy the selected saves to " + path_ + ".");
        row("Back", "", "");
        break;
    }
    case Screen::Confirm: {
        *title = "Replace Existing?";
        std::string names;
        for (int i : conflicts_) names += (names.empty() ? "" : ", ") + saves_[size_t(i)].suffix;
        row("Replace them", "", "These saves already exist: " + names + ". The old copies are kept as .bak folders.");
        row("Skip existing", "", "Only copy the saves that are not there yet.");
        row("Cancel", "", "");
        break;
    }
    case Screen::Result:
        *title = importing_ ? "Import Finished" : "Export Finished";
        for (size_t i = 0; i < result_.size(); ++i)
            row(fit(result_[i], 30), result_ok_[i] ? "done" : "skipped", result_help_[i]);
        row("OK", "", "");
        break;
    }
    return r;
}

void ManagerDialog::render(Canvas& c) const {
    if (screen_ == Screen::TypePath || screen_ == Screen::CloudType) { kbd_.render(c); return; }
    std::string title, tab;
    auto r = rows(&title, &tab);
    render_menu(c, title, tab, r, sel_, cfg_.confirm_cross);
}

void ManagerDialog::move(int d) {
    const int n = row_count();
    if (!n) return;
    const int old = sel_;
    sel_ = std::clamp(sel_ + d, 0, n - 1);
    if (sel_ != old) ++rev_;
}

void ManagerDialog::back() {
    note_.clear();
    switch (screen_) {
    case Screen::Main: break;   // handled by the caller (Cancelled)
    case Screen::Cloud: go(Screen::Main, 2); break;
    case Screen::CloudType: osk_set_active(false); go(Screen::Cloud, 0); break;
    case Screen::CloudConflict: go(Screen::Cloud, 0); break;
    case Screen::PickSource: case Screen::PickDest: go(Screen::Main, importing_ ? 0 : 1); break;
    case Screen::TypePath: go(importing_ ? Screen::PickSource : Screen::PickDest, int(sources_.size())); break;
    case Screen::PickSaves: go(importing_ ? Screen::PickSource : Screen::PickDest, 0); break;
    case Screen::Confirm: go(Screen::PickSaves, start_row()); break;
    case Screen::Result: go(Screen::Main); break;
    }
}

// A typed path: use it as the SAVEDATA folder, or look for PSP/SAVEDATA below it.
bool ManagerDialog::open_path(const std::string& typed) {
    std::string p = typed;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\' || p.back() == ' ')) p.pop_back();
    if (p.empty()) return false;
    std::error_code ec;
    for (const char* sub : {"", "PSP/SAVEDATA", "SAVEDATA", "memstick/PSP/SAVEDATA", "PSP/SAVEDATA"}) {
        const fs::path c = sub[0] ? fs::u8path(p) / sub : fs::u8path(p);
        if (fs::is_directory(c, ec)) {
            if (importing_) { if (psp_savemgr::scan(c.u8string()).empty() && !fs::is_regular_file(c / "PARAM.SFO", ec)) continue; }
            path_ = c.u8string();
            return true;
        }
    }
    if (!importing_) {   // an export folder may not exist yet
        if (fs::is_directory(fs::u8path(p).parent_path(), ec) || fs::create_directories(fs::u8path(p), ec)) { path_ = p; return true; }
    }
    return false;
}

void ManagerDialog::begin_run() {
    int n = 0;
    conflicts_.clear();
    for (size_t i = 0; i < saves_.size(); ++i) {
        if (!checked_[i]) continue;
        ++n;
        bool exists;
        if (importing_) {
            exists = !cfg_.existing(saves_[i].suffix).empty();
        } else {
            const std::string disc = keep_region_ ? saves_[i].disc : cfg_.disc_id;
            std::error_code ec;
            exists = fs::exists(fs::u8path(path_) / (disc + saves_[i].suffix), ec);
        }
        if (exists) conflicts_.push_back(int(i));
    }
    if (!n) { note_ = "Select at least one save first."; return; }
    if (conflicts_.empty()) run(false);
    else go(Screen::Confirm, 0);
}

void ManagerDialog::run(bool replace) {
    result_.clear(); result_help_.clear(); result_ok_.clear();
    int done = 0;
    for (size_t i = 0; i < saves_.size(); ++i) {
        if (!checked_[i]) continue;
        const auto& s = saves_[i];
        const bool conflict = std::find(conflicts_.begin(), conflicts_.end(), int(i)) != conflicts_.end();
        if (conflict && !replace) {
            result_.push_back(s.name); result_help_.push_back("Skipped: already there."); result_ok_.push_back(false);
            continue;
        }
        std::string disc, dest_root, backup, name;
        if (importing_) {
            const std::string have = cfg_.existing(s.suffix);   // replace the folder the game already uses
            disc = have.size() >= 9 ? have.substr(0, 9) : cfg_.disc_id;
            dest_root = cfg_.saves_root;
            backup = cfg_.backup_root;
        } else {
            disc = keep_region_ ? s.disc : cfg_.disc_id;
            dest_root = path_;
            backup = (fs::u8path(path_).parent_path() / (fs::u8path(path_).filename().u8string() + "_backup")).u8string();
        }
        const std::string err = psp_savemgr::copy_save(s, dest_root, disc, replace && conflict, &name, backup);
        if (err.empty()) {
            ++done;
            result_.push_back(name);
            result_help_.push_back("Copied to " + (fs::u8path(dest_root) / name).u8string() + (conflict ? " (old copy kept as .bak)" : ""));
            result_ok_.push_back(true);
        } else {
            result_.push_back(s.name);
            result_help_.push_back("Failed: " + err);
            result_ok_.push_back(false);
        }
    }
    if (done && importing_) changed_ = true;
    std::fprintf(stderr, "[SAVEMGR] %s: %d save(s) copied (%s)\n", importing_ ? "import" : "export", done, path_.c_str());
    go(Screen::Result, int(result_.size()));
}

void ManagerDialog::adjust(int d) {
    (void)d;
    if (screen_ == Screen::PickSaves && !importing_ && sel_ == start_row() - 1) { keep_region_ = !keep_region_; ++rev_; }
}

// Importing: saves of different regions with the same suffix (ULES01505GameData00 and ULUS10566GameData00) end up
// in the same folder, so only one of each can be selected: the one of this game's region, else the newest.
void ManagerDialog::default_checks() {
    checked_.assign(saves_.size(), true);
    if (!importing_) return;
    for (size_t i = 0; i < saves_.size(); ++i)
        for (size_t j = 0; j < saves_.size(); ++j) {
            if (i == j || saves_[i].suffix != saves_[j].suffix || !checked_[i] || !checked_[j]) continue;
            auto rank = [&](const psp_savemgr::SaveInfo& s) { return std::make_pair(s.disc == cfg_.disc_id, s.mtime); };
            checked_[rank(saves_[i]) < rank(saves_[j]) ? i : j] = false;
        }
}
void ManagerDialog::toggle(size_t i) {
    checked_[i] = !checked_[i];
    if (importing_ && checked_[i])   // one save per suffix
        for (size_t j = 0; j < saves_.size(); ++j)
            if (j != i && saves_[j].suffix == saves_[i].suffix) checked_[j] = false;
    ++rev_;
}

void ManagerDialog::activate() {
    note_.clear();
    switch (screen_) {
    case Screen::Main:
        if (sel_ == main_back()) break;   // Back: handled by update
        if (cfg_.cloud_status && sel_ == 2) { cloud_seen_rev_ = 0; go(Screen::Cloud, 0); break; }
        importing_ = sel_ == 0;
        load_sources();
        go(importing_ ? Screen::PickSource : Screen::PickDest, 0);
        break;
    case Screen::PickSource: case Screen::PickDest: {
        const int n = int(sources_.size());
        if (sel_ < n) {
            path_ = sources_[size_t(sel_)].path;
            if (importing_) {
                saves_ = psp_savemgr::scan(path_);
                default_checks();
                go(Screen::PickSaves, 0);
            } else {
                saves_ = psp_savemgr::scan(cfg_.saves_root);
                default_checks();
                if (saves_.empty()) { note_ = "This game has no saves to export yet."; break; }
                go(Screen::PickSaves, 0);
            }
        } else if (sel_ == n) {
            kbd_.begin(u"Path to the SAVEDATA folder", to_u16(path_), 512, cfg_.confirm_cross);
            osk_set_active(true);
            go(Screen::TypePath);
        } else {
            back();
        }
        break;
    }
    case Screen::PickSaves: {
        const int ns = int(saves_.size());
        if (sel_ == 0) {
            std::vector<bool> keep = checked_;
            default_checks();
            if (keep == checked_) checked_.assign(checked_.size(), false);   // already the default: clear
            ++rev_;
        } else if (sel_ <= ns) {
            toggle(size_t(sel_ - 1));
        } else if (!importing_ && sel_ == ns + 1) {
            adjust(1);
        } else if (sel_ == start_row()) {
            begin_run();
        } else {
            back();
        }
        break;
    }
    case Screen::Confirm:
        if (sel_ == 0) run(true);
        else if (sel_ == 1) run(false);
        else back();
        break;
    case Screen::Result:
        go(Screen::Main);
        break;
    case Screen::Cloud: activate_cloud(); break;
    case Screen::CloudConflict: activate_conflict(); break;
    case Screen::CloudType: break;
    case Screen::TypePath: break;
    }
}

std::vector<ManagerDialog::CloudRow> ManagerDialog::cloud_rows() const {
    using St = psp_cloud_svc::Status;
    std::vector<CloudRow> r{{CloudAct::Info, ""}};
    const St st = cfg_.cloud_status();
    const psp_cloud_svc::Config cc = cfg_.cloud_config ? cfg_.cloud_config() : psp_cloud_svc::Config{};
    const bool configured = cfg_.cloud_configured && cfg_.cloud_configured();
    if (st.state == St::SigningIn) r.push_back({CloudAct::CancelSignIn, ""});
    else if (!configured) {
        r.push_back({CloudAct::SignIn, ""});
        r.push_back({CloudAct::SetupWebDav, ""});
        r.push_back({CloudAct::SetupFolder, ""});
    } else {
        r.push_back({CloudAct::SyncNow, ""});
        for (auto& it : st.items)
            if (it.action == psp_cloud::Action::Conflict) r.push_back({CloudAct::Conflict, it.suffix});
        r.push_back({CloudAct::ChangeService, ""});
        if (cc.provider == psp_cloud_svc::Provider::Google) r.push_back({CloudAct::SignOut, ""});
    }
    r.push_back({CloudAct::Back, ""});
    return r;
}

void ManagerDialog::activate_cloud() {
    const auto rows_ = cloud_rows();
    if (sel_ < 0 || sel_ >= int(rows_.size())) return;
    const CloudRow& cr = rows_[size_t(sel_)];
    const bool syncing = cfg_.cloud_status().state == psp_cloud_svc::Status::Syncing;
    switch (cr.act) {
    case CloudAct::Info: break;
    case CloudAct::SyncNow: if (!syncing && cfg_.cloud_sync) cfg_.cloud_sync(); break;
    case CloudAct::SignIn: if (cfg_.cloud_signin) cfg_.cloud_signin(); break;
    case CloudAct::CancelSignIn: if (cfg_.cloud_signin_cancel) cfg_.cloud_signin_cancel(); break;
    case CloudAct::Conflict: if (!syncing) { cloud_suffix_ = cr.suffix; go(Screen::CloudConflict, 0); } break;
    case CloudAct::SetupWebDav: cloud_type_begin(true); return;
    case CloudAct::SetupFolder: cloud_type_begin(false); return;
    case CloudAct::ChangeService:
        if (cfg_.cloud_set_config) { psp_cloud_svc::Config c = cfg_.cloud_config ? cfg_.cloud_config() : psp_cloud_svc::Config{}; c.provider = psp_cloud_svc::Provider::Off; cfg_.cloud_set_config(c); }
        go(Screen::Cloud, 0);
        break;
    case CloudAct::SignOut: if (cfg_.cloud_signout) cfg_.cloud_signout(); go(Screen::Cloud, 0); break;
    case CloudAct::Back: back(); break;
    }
    ++rev_;
}

void ManagerDialog::cloud_type_begin(bool webdav) {
    cloud_cfg_ = cfg_.cloud_config ? cfg_.cloud_config() : psp_cloud_svc::Config{};
    cloud_webdav_ = webdav;
    cloud_step_ = 0;
    cloud_type_ask();
}

void ManagerDialog::cloud_type_ask() {
    if (!cloud_webdav_) kbd_.begin(u"Folder path (synced by another program)", to_u16(cloud_cfg_.folder), 255, cfg_.confirm_cross);
    else if (cloud_step_ == 0) kbd_.begin(u"WebDAV folder address (https://...)", to_u16(cloud_cfg_.webdav_url), 255, cfg_.confirm_cross);
    else if (cloud_step_ == 1) kbd_.begin(u"WebDAV user name", to_u16(cloud_cfg_.webdav_user), 63, cfg_.confirm_cross);
    else kbd_.begin(u"WebDAV password (app password)", to_u16(cloud_cfg_.webdav_pass), 63, cfg_.confirm_cross);
    osk_set_active(true);
    go(Screen::CloudType);
}

// The keyboard was closed. An empty first field cancels the setup; later fields may be empty (a public share has no login).
void ManagerDialog::cloud_type_done() {
    osk_set_active(false);
    const std::string typed = to_u8(kbd_.text());
    if (cloud_step_ == 0 && typed.empty()) { go(Screen::Cloud, 0); return; }
    if (!cloud_webdav_) cloud_cfg_.folder = typed;
    else if (cloud_step_ == 0) cloud_cfg_.webdav_url = typed;
    else if (cloud_step_ == 1) cloud_cfg_.webdav_user = typed;
    else cloud_cfg_.webdav_pass = typed;
    if (cloud_webdav_ && cloud_step_ < 2) { ++cloud_step_; cloud_type_ask(); return; }
    cloud_cfg_.provider = cloud_webdav_ ? psp_cloud_svc::Provider::WebDav : psp_cloud_svc::Provider::Folder;
    if (cfg_.cloud_set_config) cfg_.cloud_set_config(cloud_cfg_);
    if (cfg_.cloud_sync) cfg_.cloud_sync();         // try it at once: the status shows whether it works
    cloud_seen_rev_ = 0;
    go(Screen::Cloud, 0);
}

void ManagerDialog::activate_conflict() {
    if (sel_ == 0 || sel_ == 1) {
        if (cfg_.cloud_resolve) cfg_.cloud_resolve(cloud_suffix_, sel_ == 0 ? psp_cloud::Choice::KeepLocal : psp_cloud::Choice::KeepRemote);
    }
    go(Screen::Cloud, 0);
}

Result ManagerDialog::update(uint32_t b, int64_t now_ms) {
    if ((screen_ == Screen::Cloud || screen_ == Screen::CloudConflict) && cfg_.cloud_status) {
        const psp_cloud_svc::Status st = cfg_.cloud_status();
        if (st.revision != cloud_seen_rev_) {
            cloud_seen_rev_ = st.revision;
            if (st.state != psp_cloud_svc::Status::Syncing)
                for (auto& it : st.items) if (it.action == psp_cloud::Action::Pulled) changed_ = true;   // the list behind refreshes
            const int n = int(cloud_rows().size());
            if (screen_ == Screen::Cloud && sel_ >= n) sel_ = n - 1;
        }
    }
    if (screen_ == Screen::CloudType) {
        if (kbd_.update(b, now_ms) != Result::Running) cloud_type_done();
        return Result::Running;
    }
    if (screen_ == Screen::TypePath) {
        const Result r = kbd_.update(b, now_ms);
        if (r != Result::Running) {
            osk_set_active(false);
            const std::string typed = to_u8(kbd_.text());
            if (typed.empty()) { back(); return Result::Running; }
            if (open_path(typed)) {
                if (importing_) {
                    saves_ = psp_savemgr::scan(path_);
                    default_checks();
                    go(Screen::PickSaves, 0);
                } else {
                    saves_ = psp_savemgr::scan(cfg_.saves_root);
                    default_checks();
                    if (saves_.empty()) { note_ = "This game has no saves to export yet."; go(Screen::PickDest, int(sources_.size())); }
                    else go(Screen::PickSaves, 0);
                }
            } else {
                note_ = importing_ ? "No Dissidia 012 saves found in " + fit(typed, 40, true)
                                   : "Cannot use " + fit(typed, 40, true);
                go(importing_ ? Screen::PickSource : Screen::PickDest, int(sources_.size()));
            }
        }
        return Result::Running;
    }
    const uint32_t action = 0x4000 | 0x2000;   // CROSS | CIRCLE
    if (!armed_) {                              // the press that opened the screen may still be down
        if ((b & action) == 0 && (b & K_TRIANGLE) == 0) armed_ = true;
        prev_ = b;
        return Result::Running;
    }
    const uint32_t pressed = b & ~prev_;
    prev_ = b;
    const uint32_t confirm = cfg_.confirm_cross ? 0x4000 : 0x2000;
    const uint32_t cancel = cfg_.confirm_cross ? 0x2000 : 0x4000;
    if (pressed & cancel) {
        if (screen_ == Screen::Main) return Result::Cancelled;
        back();
        return Result::Running;
    }
    if (pressed & confirm) {
        if (screen_ == Screen::Main && sel_ == main_back()) return Result::Cancelled;
        activate();
        return Result::Running;
    }
    if ((pressed & K_START) && screen_ == Screen::PickSaves) { begin_run(); return Result::Running; }
    if (pressed & 0x0080) adjust(-1);   // left
    if (pressed & 0x0020) adjust(1);    // right
    uint32_t dir = b & (0x0010 | 0x0040);
    if (dir == (0x0010 | 0x0040)) dir = 0;
    if (dir == 0) {
        held_dir_ = 0;
    } else if (dir != held_dir_) {
        held_dir_ = dir;
        move(dir == 0x0010 ? -1 : 1);
        next_repeat_ = now_ms + 400;
    } else if (now_ms >= next_repeat_) {
        move(dir == 0x0010 ? -1 : 1);
        next_repeat_ = now_ms + 80;
    }
    if (pressed & 0x0100) move(-MENU_ROWS);
    if (pressed & 0x0200) move(MENU_ROWS);
    return Result::Running;
}

}  // namespace psp_sddialog
