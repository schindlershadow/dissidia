// Automatic save backups: see psp_savebackup.h.
#include "psp_savebackup.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <sstream>

namespace fs = std::filesystem;

namespace psp_savebackup {
namespace {

fs::path P(const std::string& s) { return fs::u8path(s); }
std::string S(const fs::path& p) { return p.u8string(); }

std::mutex g_mu;
struct Recent { std::string id; time_t at = 0; };
std::map<std::string, Recent> g_recent;   // backup_root + reason -> the backup a replacement may join

std::tm local_tm(time_t t) {
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    return tmv;
}

bool plain_code(const std::string& c) {
    if (c.empty() || c.size() > 24) return false;
    for (char ch : c) if (!((ch >= 'a' && ch <= 'z') || ch == '-')) return false;
    return true;
}

// "20261119-210405_cloud-download" (or "-2" ... after the time when that name is taken).
bool parse_id(const std::string& id, time_t* when, std::string* reason) {
    const size_t us = id.find('_');
    if (us == std::string::npos || us < 15) return false;
    const std::string stamp = id.substr(0, 15), code = id.substr(us + 1);
    if (stamp[8] != '-' || !plain_code(code)) return false;
    for (int i = 0; i < 15; ++i) if (i != 8 && (stamp[size_t(i)] < '0' || stamp[size_t(i)] > '9')) return false;
    std::tm tmv{};
    tmv.tm_year = std::atoi(stamp.substr(0, 4).c_str()) - 1900;
    tmv.tm_mon = std::atoi(stamp.substr(4, 2).c_str()) - 1;
    tmv.tm_mday = std::atoi(stamp.substr(6, 2).c_str());
    tmv.tm_hour = std::atoi(stamp.substr(9, 2).c_str());
    tmv.tm_min = std::atoi(stamp.substr(11, 2).c_str());
    tmv.tm_sec = std::atoi(stamp.substr(13, 2).c_str());
    tmv.tm_isdst = -1;
    if (when) *when = std::mktime(&tmv);
    if (reason) *reason = code;
    return true;
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A new, empty backup folder for `reason`; "" when it cannot be made.
std::string make_backup_dir(const fs::path& root, const std::string& reason, time_t now) {
    const std::tm t = local_tm(now);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &t);
    std::error_code ec;
    fs::create_directories(root, ec);
    for (int n = 1; n < 100; ++n) {
        const std::string id = std::string(stamp) + (n > 1 ? "-" + std::to_string(n) : "") + "_" + reason;
        const fs::path d = root / P(id);
        if (fs::exists(d, ec)) continue;
        if (!fs::create_directory(d, ec) || ec) return "";
        std::ofstream(d / "backup.txt", std::ios::binary) << "reason=" << reason << "\ntime=" << static_cast<long long>(now) << "\n";
        return id;
    }
    return "";
}

std::string copy_tree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) return "cannot create " + S(to) + ": " + ec.message();
    for (auto it = fs::directory_iterator(from, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        fs::copy_file(it->path(), to / it->path().filename(), fs::copy_options::overwrite_existing, e2);
        if (e2) return "cannot copy " + S(it->path().filename()) + ": " + e2.message();
    }
    return ec ? "cannot read " + S(from) + ": " + ec.message() : "";
}

// Moves a folder (rename, else copy and delete: the backup folder may be on another file system).
std::string move_tree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return "";
    if (std::string e = copy_tree(from, to); !e.empty()) return e;
    fs::remove_all(from, ec);
    return "";
}

uint64_t fnv(const std::string& data, uint64_t h) {
    for (unsigned char c : data) { h ^= c; h *= 1099511628211ull; }
    return h;
}

}  // namespace

const char* reason_text(const std::string& code) {
    if (code == kCloudDownload) return "Before a cloud download";
    if (code == kCloudConflict) return "Before taking the cloud save";
    if (code == kImport) return "Before an import";
    if (code == kRestore) return "Before a restore";
    if (code == kManual) return "Backed up by you";
    if (code == kDeleted) return "Deleted save";
    if (code == kMoved) return "Before a move to another slot";
    if (code == kRenamed) return "Before a rename";
    if (code == kDaily) return "Daily backup";
    if (code == kLegacy) return "Older backup";
    return "Before a save was replaced";
}
bool is_automatic(const std::string& code) { return code != kManual && code != kLegacy; }

std::string fingerprint(const std::string& save_dir) {
    uint64_t h = 14695981039346656037ull;
    for (const std::string& f : psp_savemgr::save_files(save_dir)) {
        h = fnv(f, h);
        h = fnv(read_text(P(save_dir) / P(f)), h);
    }
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

std::string set_aside(const std::string& save_dir, const std::string& backup_root, const std::string& reason_in) {
    const std::string reason = plain_code(reason_in) ? reason_in : kReplaced;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const fs::path root = P(backup_root), src = P(save_dir);
        const std::string name = S(src.filename());
        const time_t now = std::time(nullptr);
        std::error_code ec;
        Recent& r = g_recent[backup_root + "|" + reason];
        std::string id;
        if (!r.id.empty() && now - r.at < 120 && fs::is_directory(root / P(r.id), ec) && !fs::exists(root / P(r.id) / P(name), ec)) id = r.id;
        if (id.empty()) id = make_backup_dir(root, reason, now);
        if (id.empty()) return "cannot create a backup folder in " + backup_root;
        r.id = id;
        r.at = now;
        if (std::string e = move_tree(src, root / P(id) / P(name)); !e.empty()) return e;
        std::fprintf(stderr, "[SAVEMGR] backup: %s -> %s/%s\n", name.c_str(), id.c_str(), name.c_str());
    }
    if (is_automatic(reason)) prune(backup_root);
    return "";
}

std::string snapshot(const std::string& saves_root, const std::string& backup_root, const std::string& reason_in,
                     const std::vector<std::string>& names, std::string* id_out) {
    const std::string reason = plain_code(reason_in) ? reason_in : kManual;
    if (id_out) id_out->clear();
    std::vector<psp_savemgr::SaveInfo> saves = psp_savemgr::scan(saves_root);
    if (!names.empty())
        saves.erase(std::remove_if(saves.begin(), saves.end(),
                                   [&](const psp_savemgr::SaveInfo& s) { return std::find(names.begin(), names.end(), s.name) == names.end(); }),
                    saves.end());
    if (saves.empty()) return "";
    std::string id;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        id = make_backup_dir(P(backup_root), reason, std::time(nullptr));
    }
    if (id.empty()) return "cannot create a backup folder in " + backup_root;
    for (const auto& s : saves)
        if (std::string e = copy_tree(P(s.path), P(backup_root) / P(id) / P(s.name)); !e.empty()) return e;
    if (id_out) *id_out = id;
    std::fprintf(stderr, "[SAVEMGR] backup %s: %zu save(s)\n", id.c_str(), saves.size());
    if (is_automatic(reason)) prune(backup_root);
    return "";
}

bool daily(const std::string& saves_root, const std::string& backup_root) {
    const std::vector<psp_savemgr::SaveInfo> saves = psp_savemgr::scan(saves_root);
    if (saves.empty()) return false;
    const std::vector<Backup> all = list(backup_root);
    const std::tm today = local_tm(std::time(nullptr));
    for (const Backup& b : all) {
        if (b.reason != kDaily) continue;
        const std::tm t = local_tm(b.when);
        if (t.tm_year == today.tm_year && t.tm_yday == today.tm_yday) return false;   // already made today
        break;
    }
    // Unchanged since the newest backup that holds every save: nothing new to keep.
    for (const Backup& b : all) {
        std::map<std::string, std::string> have;
        for (const auto& s : b.saves) have[s.name] = s.path;
        bool covers = true;
        for (const auto& s : saves) if (!have.count(s.name)) { covers = false; break; }
        if (!covers) continue;
        bool same = true;
        for (const auto& s : saves) if (fingerprint(s.path) != fingerprint(have[s.name])) { same = false; break; }
        if (same) return false;
        break;
    }
    std::string id;
    return snapshot(saves_root, backup_root, kDaily, {}, &id).empty() && !id.empty();
}

// A backup folder never changes once it is made (it only goes away, or a save is taken out of it), so what was read from it is kept
// until the folder's own modification time moves: opening Settings > Saves again does not read every backup again (on a phone's
// shared storage each file costs a millisecond or more). PSPRECOMP_SAVES_SYNC_OLD=1 reads everything every time.
namespace {
struct Cached { long long mtime_ns = 0; bool keep = false; Backup b; };
std::mutex g_list_mu;
std::map<std::string, Cached> g_list_cache;
long long dir_mtime_ns(const fs::path& p) {
    struct stat st;
    if (::stat(p.string().c_str(), &st) != 0) return -1;
#ifdef __APPLE__
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}
bool list_uncached() { static const bool v = [] { const char* e = std::getenv("PSPRECOMP_SAVES_SYNC_OLD"); return e && *e && *e != '0'; }(); return v; }
}  // namespace

static bool read_backup(const fs::directory_entry& it, Backup* out);

std::vector<Backup> list(const std::string& backup_root) {
    std::vector<Backup> out;
    std::error_code ec;
    const fs::path root = P(backup_root);
    std::set<std::string> seen;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        const std::string key = S(it->path());
        const long long mt = list_uncached() ? -1 : dir_mtime_ns(it->path());
        seen.insert(key);
        if (mt >= 0) {
            std::lock_guard<std::mutex> lk(g_list_mu);
            auto c = g_list_cache.find(key);
            if (c != g_list_cache.end() && c->second.mtime_ns == mt) { if (c->second.keep) out.push_back(c->second.b); continue; }
        }
        Backup b;
        const bool keep = read_backup(*it, &b);
        if (mt >= 0 && mt < (std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - 3000000000LL)) {   // not one still being written
            std::lock_guard<std::mutex> lk(g_list_mu);
            g_list_cache[key] = {mt, keep, keep ? b : Backup{}};
        }
        if (keep) out.push_back(std::move(b));
    }
    if (!list_uncached()) {
        std::lock_guard<std::mutex> lk(g_list_mu);
        for (auto c = g_list_cache.begin(); c != g_list_cache.end();) {   // forget folders of this root that are gone
            if (c->first.compare(0, backup_root.size(), backup_root) == 0 && !seen.count(c->first)) c = g_list_cache.erase(c); else ++c;
        }
    }
    std::sort(out.begin(), out.end(), [](const Backup& a, const Backup& b) { return a.when != b.when ? a.when > b.when : a.id > b.id; });
    return out;
}

static bool read_backup(const fs::directory_entry& it, Backup* outp) {
    {
        const std::string n = S(it.path().filename());
        Backup b;
        b.id = n;
        b.path = S(it.path());
        if (parse_id(n, &b.when, &b.reason)) {
            const std::string txt = read_text(it.path() / "backup.txt");
            const size_t tp = txt.find("time=");
            if (tp != std::string::npos) b.when = time_t(std::strtoll(txt.c_str() + tp + 5, nullptr, 10));
            b.saves = psp_savemgr::scan(b.path);
        } else {
            // An older runtime's "<name>.bak[N]": one save folder whose name has the extra ending.
            const size_t bak = n.rfind(".bak");
            if (bak == std::string::npos) return false;
            psp_savemgr::SaveInfo s;
            if (!psp_savemgr::read_save(b.path, &s)) return false;
            s.name = n.substr(0, bak);
            std::string d, x;
            if (!psp_savemgr::split_name(s.name, &d, &x)) return false;
            s.disc = d; s.suffix = x;
            b.reason = kLegacy;
            b.when = s.mtime;
            b.saves.push_back(s);
        }
        if (b.saves.empty()) return false;
        for (const auto& s : b.saves) b.bytes += s.bytes;
        *outp = std::move(b);
        return true;
    }
}

std::string remove(const std::string& backup_root, const std::string& id) {
    if (id.empty() || id.find('/') != std::string::npos || id.find('\\') != std::string::npos || id == "." || id == "..") return "bad backup name";
    std::lock_guard<std::mutex> lk(g_mu);
    std::error_code ec;
    const fs::path d = P(backup_root) / P(id);
    if (!fs::is_directory(d, ec)) return "the backup is gone";
    fs::remove_all(d, ec);
    return ec ? "cannot delete " + id + ": " + ec.message() : "";
}

void prune(const std::string& backup_root, int keep_auto, int keep_daily) {
    int n_auto = 0, n_daily = 0;
    for (const Backup& b : list(backup_root)) {   // newest first
        if (b.reason == kDaily) { if (++n_daily > keep_daily) remove(backup_root, b.id); }
        else if (b.reason != kManual && b.reason != kLegacy) { if (++n_auto > keep_auto) remove(backup_root, b.id); }
    }
}

}  // namespace psp_savebackup
