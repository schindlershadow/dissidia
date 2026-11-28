// Settings > Saves: see psp_saves_page.h.
#include "psp_saves_page.h"

#include "psp_savearchive.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <atomic>
#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using psp_savemgr::SaveInfo;
using psp_sddialog::SetKind;
using psp_sddialog::SetRow;

namespace psp_saves {
namespace {

fs::path P(const std::string& s) { return fs::u8path(s); }
std::string S(const fs::path& p) { return p.u8string(); }

std::string fit(const std::string& s, size_t max, bool keep_tail = false) {
    // character-safe (UTF-8) shortening; paths keep their end
    std::vector<size_t> starts;
    for (size_t i = 0; i < s.size(); ++i) if ((uint8_t(s[i]) & 0xC0) != 0x80) starts.push_back(i);
    if (starts.size() <= max || max < 4) return s;
    if (keep_tail) return "..." + s.substr(starts[starts.size() - (max - 3)]);
    return s.substr(0, starts[max - 2]) + "..";
}
std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }
std::string size_text(uint64_t b) {
    char t[32];
    if (b >= (1u << 20)) std::snprintf(t, sizeof t, "%.1f MB", double(b) / 1048576.0);
    else std::snprintf(t, sizeof t, "%llu KB", static_cast<unsigned long long>((b + 1023) / 1024));
    return t;
}
std::tm local_tm(time_t t) {
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    return tmv;
}
const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
std::string when_short(time_t t) {   // "19 Nov 21:04" (this year), "19 Nov 2025"
    if (!t) return "";
    const std::tm a = local_tm(t), now = local_tm(std::time(nullptr));
    char b[40];
    if (a.tm_year == now.tm_year) std::snprintf(b, sizeof b, "%d %s %02d:%02d", a.tm_mday, kMonths[a.tm_mon], a.tm_hour, a.tm_min);
    else std::snprintf(b, sizeof b, "%d %s %d", a.tm_mday, kMonths[a.tm_mon], a.tm_year + 1900);
    return b;
}
// "2 hours newer", "3 days older" ... of a against b
std::string age_diff(time_t a, time_t b) {
    if (!a || !b) return "";
    const long long d = static_cast<long long>(a) - static_cast<long long>(b), ad = d < 0 ? -d : d;
    if (ad < 60) return "same time";
    std::string n;
    if (ad < 3600) n = plural(int(ad / 60), "minute", "minutes");
    else if (ad < 86400 * 2) n = plural(int(ad / 3600), "hour", "hours");
    else n = plural(int(ad / 86400), "day", "days");
    return n + (d > 0 ? " newer" : " older");
}
int kind_rank(const std::string& suffix) {
    if (suffix.rfind("GameData", 0) == 0) return 0;
    if (suffix.rfind("FCard", 0) == 0) return 1;
    if (suffix.rfind("Quest", 0) == 0) return 2;
    if (suffix.rfind("Replay", 0) == 0) return 3;
    return 4;
}
std::string region_label(const std::string& disc) {
    const std::string r = psp_savemgr::region_of(disc);
    return r == "?" ? disc : r;
}
std::string save_title(const SaveInfo& s) {   // the save's own words, one line
    std::string d = psp_savemgr::one_line(s.detail);
    if (d.empty()) d = psp_savemgr::one_line(s.savetitle);
    return d;
}

enum class V { Top, Pick, Browse, Conflict, Backups, Backup, Mine, Save, Slots };
enum class A {
    None, Conflict, KeepNewerAll, Source, PickFolder, PickArchive, Browse, TypeImport, ExportFolder, ExportZip, Dest, BrowseExport,
    TypeExport, Names, BackupNow, Backups, CloudSync, CloudSettings, SelAll, Toggle, Run, BrowseUse, BrowseUp, BrowseDir, BrowseFile,
    BrowseHere, KeepLocal, KeepRemote, Later, OpenBackup, Restore, RestoreAll, DeleteBackup,
    Mine, OpenSave, Duplicate, Move, Rename, DeleteSave, ToSlot,
};
struct Act {
    A a = A::None; int i = 0; std::string s;
    Act(A a_ = A::None, int i_ = 0, std::string s_ = {}) : a(a_), i(i_), s(std::move(s_)) {}
};

struct Level {
    V v = V::Top;
    int sel = 0;
    std::string title;                 // crumb
    // Pick
    bool importing = true;
    std::string src_label, src_path;   // where the saves come from (import)
    std::string dest;                  // export: a folder or a .zip path
    bool dest_zip = false, dest_commit = false;
    std::string temp;                  // an unpacked archive: deleted when the level closes
    std::vector<SaveInfo> saves;
    std::vector<bool> checked;
    // Browse
    std::string dir;                   // "" = places
    int here_count = -1;
    // Conflict
    std::string suffix;
    bool fresh = true;                 // just opened: the cursor goes to the newer side's choice
    // Backup
    std::string backup_id;
    // Save / Slots
    std::string save_name;             // the folder (ULES01505GameData00)
    bool moving = false;               // Slots: move instead of duplicate
};

enum class Native { None, ImportFolder, ImportArchive, ExportFolder, ExportZip };

}  // namespace

std::string slot_label(const std::string& suffix) {
    auto num = [&](size_t n) { return suffix.size() > n ? suffix.substr(n) : std::string(); };
    if (suffix.rfind("GameData", 0) == 0) { const std::string n = num(8); return n.empty() || n == "00" ? "Game data" : "Game data " + n; }
    if (suffix.rfind("FCard", 0) == 0) return "Friend card " + num(5);
    if (suffix.rfind("Quest", 0) == 0) return "Quest " + num(5);
    if (suffix.rfind("Replay", 0) == 0) return "Replay " + num(6);
    return suffix;
}
namespace {
constexpr int kSlots = 20;   // the slots the game offers per kind (its save list asks for GameData00 .. GameData19)
std::string slot_prefix(const std::string& suffix) {   // "GameData07" -> "GameData"
    size_t n = suffix.size();
    while (n > 0 && suffix[n - 1] >= '0' && suffix[n - 1] <= '9') --n;
    return suffix.substr(0, n);
}
std::string slot_numbered(const std::string& suffix) {   // always with its number: "Game data 00"
    const std::string l = slot_label(suffix);
    return suffix.rfind("GameData", 0) == 0 && l == "Game data" ? "Game data " + (suffix.size() > 8 ? suffix.substr(8) : std::string("00")) : l;
}
}  // namespace

std::string when_text(time_t t) {
    if (!t) return "unknown time";
    const std::tm a = local_tm(t);
    char b[40];
    std::snprintf(b, sizeof b, "%d %s %d %02d:%02d", a.tm_mday, kMonths[a.tm_mon], a.tm_year + 1900, a.tm_hour, a.tm_min);
    return b;
}

struct Page::Impl {
    Env env;
    std::vector<Level> stack;
    std::vector<SetRow> rows;
    std::vector<Act> acts;
    bool dirty = true;
    bool keep_region = true;           // export folder names
    std::vector<psp_savemgr::Source> sources, dests;
    std::vector<int> source_counts;
    std::vector<psp_savebackup::Backup> backups;
    bool backups_loaded = false;
    unsigned bak_epoch = 0;            // bumped when the backups changed (an answer of an older load is dropped)
    void inval_backups() { backups_loaded = false; ++bak_epoch; }
    // modals
    bool dlg_on = false;
    Dialog dlg;
    std::function<void(int)> dlg_cb;
    bool ask_on = false;
    TextAsk ask;
    std::function<void(const std::string&)> ask_cb;
    Native native = Native::None;
    unsigned cloud_rev = 0;
    bool want_cloud_settings = false;
    unsigned changed = 0;

    Level& top() { return stack.back(); }
    const Level& top() const { return stack.back(); }

    // ---- environment with defaults ----
    std::string existing(const std::string& suffix) const {
        if (env.existing) return env.existing(suffix);
        std::error_code ec;
        std::string best;
        for (auto it = fs::directory_iterator(P(env.saves_root), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::string d, x;
            const std::string n = S(it->path().filename());
            const auto& ids = psp_savemgr::dissidia012_ids();   // this game's IDs only (not the first Dissidia's ULJM05500...)
            if (psp_savemgr::split_name(n, &d, &x) && x == suffix && it->is_directory(ec) &&
                (d == env.disc_id || std::find(ids.begin(), ids.end(), d) != ids.end())) {
                if (d == env.disc_id) return n;
                if (best.empty()) best = n;
            }
        }
        return best;
    }
    bool dlg_available() const { return env.dlg_available ? env.dlg_available() : psp_filedlg::available(); }
    bool cloud_on() const { return bool(env.cloud_status); }
    bool cloud_configured() const { return env.cloud_configured && env.cloud_configured(); }
    psp_cloud_svc::Status cloud_status() const { return env.cloud_status ? env.cloud_status() : psp_cloud_svc::Status{}; }
    std::vector<psp_cloud::Item> conflicts() const {
        std::vector<psp_cloud::Item> out;
        if (!cloud_on()) return out;
        for (const auto& it : cloud_status().items) if (it.action == psp_cloud::Action::Conflict) out.push_back(it);
        return out;
    }
    void after_local_change() {   // this game's saves changed: the save list refreshes, the cloud gets them
        ++changed;
        if (cloud_configured() && env.cloud_sync) env.cloud_sync();
    }

    // Finding PPSSPP and memory sticks looks at every mounted drive, and a sleeping hard disk can take seconds to answer: it runs on
    // its own thread and the rows say "Looking..." until it is back.
    struct Found {
        std::atomic<bool> done{false};
        std::vector<psp_savemgr::Source> sources, dests;
        std::vector<int> counts;
    };
    std::shared_ptr<Found> finding;
    bool searched = false;
    void load_places() {
        auto f = std::make_shared<Found>();
        finding = f;
        searched = false;
        const auto detect = env.detect;
        const std::string root = env.saves_root;
        std::thread([f, detect, root] {
            f->sources = detect ? detect(true) : psp_savemgr::detect_sources(true, root);
            for (auto& s : f->sources) f->counts.push_back(int(psp_savemgr::scan(s.path).size()));
            f->dests = detect ? detect(false) : psp_savemgr::detect_sources(false, root);
            f->done = true;
        }).detach();
    }
    // Opening the page (and its Backups list) reads every save and every backup on a thread: on a phone's shared storage (FUSE) each
    // file costs a millisecond or more, and a long backup history is thousands of files. Rows say "Loading..." until it is back.
    // PSPRECOMP_SAVES_SYNC_OLD=1: the old behaviour (read on the UI thread when a row needs it).
    struct Preload {
        std::atomic<bool> done{false};
        bool with_mine = false;
        unsigned changed_at = 0, epoch_at = 0;
        std::vector<SaveInfo> mine;
        std::vector<psp_savebackup::Backup> backups;
    };
    std::shared_ptr<Preload> pre;
    bool mine_ok = false;               // mine_count is current (until `changed` moves)
    unsigned mine_at = 0;
    int mine_count = 0;
    static bool sync_old() { static const bool v = [] { const char* e = std::getenv("PSPRECOMP_SAVES_SYNC_OLD"); return e && *e && *e != '0'; }(); return v; }
    bool pending() const { return pre && !pre->done; }
    void start_preload(bool with_mine) {
        if (sync_old()) return;
        auto f = std::make_shared<Preload>();
        f->with_mine = with_mine;
        f->changed_at = changed;
        f->epoch_at = bak_epoch;
        pre = f;
        const std::string saves_root = env.saves_root, backup_root = env.backup_root;
        std::thread([f, saves_root, backup_root] {
            if (f->with_mine) f->mine = psp_savemgr::scan(saves_root);
            f->backups = psp_savebackup::list(backup_root);
            f->done = true;
        }).detach();
    }
    bool take_preload() {
        if (!pre || !pre->done) return false;
        auto f = std::move(pre);
        pre.reset();
        if (f->with_mine && f->changed_at == changed) { mine_count = int(f->mine.size()); mine_at = changed; mine_ok = true; }
        if (f->epoch_at == bak_epoch) { backups = std::move(f->backups); backups_loaded = true; }
        dirty = true;
        return true;
    }
    bool take_places() {
        if (!finding || !finding->done) return false;
        sources = std::move(finding->sources);
        source_counts = std::move(finding->counts);
        dests = std::move(finding->dests);
        finding.reset();
        searched = true;
        dirty = true;
        return true;
    }

    // ---- modals ----
    void message(const std::string& title, const std::string& text) {
        dlg_on = true;
        dlg = {title, text, {"OK"}};
        dlg_cb = nullptr;
        dirty = true;
    }
    void question(const std::string& title, const std::string& text, std::vector<std::string> buttons, std::function<void(int)> cb) {
        dlg_on = true;
        dlg = {title, text, std::move(buttons)};
        dlg_cb = std::move(cb);
        dirty = true;
    }
    void ask_text(const std::string& title, const std::string& init, std::function<void(const std::string&)> cb) {
        ask_on = true;
        ask = {title, init};
        ask_cb = std::move(cb);
        dirty = true;
    }

    // ---- levels ----
    void push(Level l) { stack.push_back(std::move(l)); dirty = true; }
    void pop() {
        if (stack.size() <= 1) return;
        if (!top().temp.empty()) { std::error_code ec; fs::remove_all(P(top().temp), ec); }
        stack.pop_back();
        dirty = true;
    }
    void pop_to_top() { while (stack.size() > 1) pop(); }

    // ---- import ----
    void default_checks(Level& l) {
        l.checked.assign(l.saves.size(), true);
        if (!l.importing) return;
        for (size_t i = 0; i < l.saves.size(); ++i)   // one save per slot: this game's region first, then the newest
            for (size_t j = 0; j < l.saves.size(); ++j) {
                if (i == j || l.saves[i].suffix != l.saves[j].suffix || !l.checked[i] || !l.checked[j]) continue;
                auto rank = [&](const SaveInfo& s) { return std::make_pair(s.disc == env.disc_id, s.mtime); };
                l.checked[rank(l.saves[i]) < rank(l.saves[j]) ? i : j] = false;
            }
    }
    void sort_saves(std::vector<SaveInfo>& v) {
        std::stable_sort(v.begin(), v.end(), [](const SaveInfo& a, const SaveInfo& b) {
            if (kind_rank(a.suffix) != kind_rank(b.suffix)) return kind_rank(a.suffix) < kind_rank(b.suffix);
            if (a.suffix != b.suffix) return a.suffix < b.suffix;
            return a.mtime > b.mtime;
        });
    }
    void open_import(std::vector<SaveInfo> saves, const std::string& label, const std::string& path, const std::string& temp) {
        if (saves.empty()) {
            if (!temp.empty()) { std::error_code ec; fs::remove_all(P(temp), ec); }
            message("No saves found", "No Dissidia 012 save was found in " + fit(path, 60, true) +
                    ".\nSaves of every region (US, EU, JP) are found anywhere inside the folder or archive: a PSP/SAVEDATA folder, a save "
                    "folder such as ULUS10566GameData00, or a save's own files (PARAM.SFO and the rest).");
            return;
        }
        Level l;
        l.v = V::Pick;
        l.importing = true;
        l.title = "Import";
        l.src_label = label;
        l.src_path = path;
        l.temp = temp;
        sort_saves(saves);
        l.saves = std::move(saves);
        default_checks(l);
        l.sel = 1;
        push(std::move(l));
    }
    void import_folder(const std::string& path, const std::string& label) {
        open_import(psp_savemgr::find_saves(path), label, path, "");
    }
    void import_archive(const std::string& path, const std::string& label) {
        const std::string tmp = S(P(env.scratch_dir) / "unpacked");
        std::error_code ec;
        fs::remove_all(P(tmp), ec);
        const std::string e = psp_savearchive::extract(path, tmp);
        if (!e.empty()) {
            fs::remove_all(P(tmp), ec);
            message("Cannot open the archive", fit(path, 60, true) + "\n" + e);
            return;
        }
        open_import(psp_savemgr::find_saves(tmp), label, path, tmp);
    }
    void import_typed(std::string p) {
        while (p.size() > 1 && (p.back() == '/' || p.back() == '\\' || p.back() == ' ')) p.pop_back();
        while (!p.empty() && p.front() == ' ') p.erase(p.begin());
        if (p.empty()) return;
        if (p[0] == '~') if (const char* h = std::getenv("HOME")) p = h + p.substr(1);
        std::error_code ec;
        if (fs::is_regular_file(P(p), ec)) import_archive(p, fit(S(P(p).filename()), 40));
        else if (fs::is_directory(P(p), ec)) import_folder(p, fit(p, 40, true));
        else message("Not found", "There is no folder or file at\n" + fit(p, 70, true));
    }
    int checked_count(const Level& l) const { int n = 0; for (bool c : l.checked) n += c; return n; }
    void toggle(Level& l, size_t i) {
        l.checked[i] = !l.checked[i];
        if (l.importing && l.checked[i])   // one save per slot
            for (size_t j = 0; j < l.saves.size(); ++j)
                if (j != i && l.saves[j].suffix == l.saves[i].suffix) l.checked[j] = false;
        dirty = true;
    }
    void run_import(bool replace) {
        Level& l = top();
        int done = 0, skipped = 0, failed = 0;
        std::string fails;
        for (size_t i = 0; i < l.saves.size(); ++i) {
            if (!l.checked[i]) continue;
            const SaveInfo& s = l.saves[i];
            const std::string have = existing(s.suffix);
            if (!have.empty() && !replace) { ++skipped; continue; }
            const std::string disc = have.size() >= 9 ? have.substr(0, 9) : (psp_savemgr::valid_disc_id(env.disc_id) ? env.disc_id : s.disc);
            std::string name;
            const std::string e = psp_savemgr::copy_save(s, env.saves_root, disc, !have.empty(), &name, env.backup_root, psp_savebackup::kImport);
            if (e.empty()) ++done;
            else { ++failed; fails += "\n" + slot_label(s.suffix) + ": " + e; }
        }
        std::fprintf(stderr, "[SAVEMGR] import from %s: %d copied, %d skipped, %d failed\n", l.src_path.c_str(), done, skipped, failed);
        if (done) { inval_backups(); after_local_change(); }
        pop_to_top();
        std::string t = "Imported " + plural(done, "save", "saves") + ".";
        if (skipped) t += " " + plural(skipped, "save was", "saves were") + " already there and kept.";
        if (done && replace) t += "\nReplaced saves are in Backups.";
        message(failed ? "Import finished with problems" : "Import finished", t + fails);
    }
    void begin_import() {
        Level& l = top();
        int n = 0, exist = 0;
        std::string names;
        for (size_t i = 0; i < l.saves.size(); ++i) {
            if (!l.checked[i]) continue;
            ++n;
            if (!existing(l.saves[i].suffix).empty()) { ++exist; names += (names.empty() ? "" : ", ") + slot_label(l.saves[i].suffix); }
        }
        if (!n) { message("Nothing selected", "Select at least one save first."); return; }
        if (!exist) { run_import(false); return; }
        question("Replace your saves?", "You already have: " + fit(names, 160) + ".\nReplacing keeps your current " + (exist == 1 ? "save" : "saves") +
                 " in Backups, so you can go back.", exist == n ? std::vector<std::string>{"Replace", "Cancel"} : std::vector<std::string>{"Replace", "Only new ones", "Cancel"},
                 [this, exist, n](int b) { if (b == 0) run_import(true); else if (b == 1 && exist != n) run_import(false); });
    }

    // ---- export ----
    void open_export(const std::string& dest, bool zip, bool commit) {
        std::vector<SaveInfo> saves = psp_savemgr::scan(env.saves_root);
        if (saves.empty()) { message("No saves yet", "This game has no saves to export yet."); return; }
        Level l;
        l.v = V::Pick;
        l.importing = false;
        l.title = "Export";
        l.dest = dest;
        l.dest_zip = zip;
        l.dest_commit = commit;
        sort_saves(saves);
        l.saves = std::move(saves);
        default_checks(l);
        l.sel = 1;
        push(std::move(l));
    }
    std::string export_root(const Level& l) const {   // a memory stick / PPSSPP folder: its PSP/SAVEDATA
        std::error_code ec;
        for (const char* sub : {"PSP/SAVEDATA", "memstick/PSP/SAVEDATA"})
            if (fs::is_directory(P(l.dest) / sub, ec)) return S(P(l.dest) / sub);
        return l.dest;
    }
    std::string export_disc(const SaveInfo& s) const { return keep_region || !psp_savemgr::valid_disc_id(env.disc_id) ? s.disc : env.disc_id; }
    void run_export(bool replace) {
        Level& l = top();
        int done = 0, skipped = 0, failed = 0;
        std::string fails, where;
        std::error_code ec;
        if (l.dest_zip) {
            const fs::path stage = P(env.scratch_dir) / "zipstage";
            fs::remove_all(stage, ec);
            std::vector<std::string> dirs;
            for (size_t i = 0; i < l.saves.size(); ++i) {
                if (!l.checked[i]) continue;
                std::string name;
                const std::string e = psp_savemgr::copy_save(l.saves[i], S(stage), export_disc(l.saves[i]), true, &name);
                if (e.empty()) { dirs.push_back(S(stage / P(name))); ++done; }
                else { ++failed; fails += "\n" + slot_label(l.saves[i].suffix) + ": " + e; }
            }
            if (!dirs.empty()) {
                if (std::string e = psp_savearchive::write_zip(l.dest, dirs, "PSP/SAVEDATA"); !e.empty()) { failed += done; done = 0; fails += "\n" + e; }
            }
            fs::remove_all(stage, ec);
            where = l.dest;
        } else {
            const std::string root = export_root(l);
            const std::string backup = S(P(root).parent_path() / P(S(P(root).filename()) + "_backup"));
            for (size_t i = 0; i < l.saves.size(); ++i) {
                if (!l.checked[i]) continue;
                const std::string disc = export_disc(l.saves[i]);
                const bool exists = fs::exists(P(root) / P(disc + l.saves[i].suffix), ec);
                if (exists && !replace) { ++skipped; continue; }
                std::string name;
                const std::string e = psp_savemgr::copy_save(l.saves[i], root, disc, exists, &name, backup, psp_savebackup::kReplaced);
                if (e.empty()) ++done;
                else { ++failed; fails += "\n" + slot_label(l.saves[i].suffix) + ": " + e; }
            }
            where = root;
        }
        if (done && l.dest_commit) {   // Android: copy the staged result to the picked place
            const std::string e = env.dlg_commit ? env.dlg_commit() : psp_filedlg::commit();
            if (!e.empty()) { fails += "\n" + e; failed += done; done = 0; }
        }
        std::fprintf(stderr, "[SAVEMGR] export to %s: %d copied, %d skipped, %d failed\n", where.c_str(), done, skipped, failed);
        const bool zip = l.dest_zip, commit = l.dest_commit;
        pop_to_top();
        std::string t = "Exported " + plural(done, "save", "saves") + (zip ? " into the .zip" : "") + ".";
        if (!commit && done) t += "\n" + fit(where, 70, true);
        if (skipped) t += "\n" + plural(skipped, "save was", "saves were") + " already there and left alone.";
        if (zip && done) t += "\nInside it the saves are in PSP/SAVEDATA: unpack it at a memory stick's (or PPSSPP's) top folder, or import the .zip.";
        message(failed ? "Export finished with problems" : "Export finished", t + fails);
    }
    void begin_export() {
        Level& l = top();
        int n = 0, exist = 0;
        std::error_code ec;
        const std::string root = export_root(l);
        for (size_t i = 0; i < l.saves.size(); ++i) {
            if (!l.checked[i]) continue;
            ++n;
            if (!l.dest_zip && !l.dest_commit && fs::exists(P(root) / P(export_disc(l.saves[i]) + l.saves[i].suffix), ec)) ++exist;
        }
        if (!n) { message("Nothing selected", "Select at least one save first."); return; }
        if (!exist) { run_export(false); return; }
        question("Replace saves there?", plural(exist, "save is", "saves are") + " already in " + fit(root, 50, true) +
                 ".\nThe old copies are kept in a \"" + S(P(root).filename()) + "_backup\" folder next to it.",
                 {"Replace", "Skip those", "Cancel"}, [this](int b) { if (b == 0) run_export(true); else if (b == 1) run_export(false); });
    }

    // ---- the system's file dialog ----
    void native_start(Native what) {
        using K = psp_filedlg::Kind;
        const K k = what == Native::ImportFolder ? K::ImportFolder : what == Native::ImportArchive ? K::ImportArchive
                  : what == Native::ExportFolder ? K::ExportFolder : K::ExportZip;
        const char* title = what == Native::ImportFolder ? "Choose the folder with your Dissidia 012 saves"
                          : what == Native::ImportArchive ? "Choose a save archive (.zip, .7z, .rar)"
                          : what == Native::ExportFolder ? "Choose where to export the saves" : "Save the saves as a .zip file";
        const std::tm t = local_tm(std::time(nullptr));
        char name[64];
        std::snprintf(name, sizeof name, "Dissidia012-saves-%04d%02d%02d.zip", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        std::string start;
        if (const char* h = std::getenv(
#ifdef _WIN32
                "USERPROFILE"
#else
                "HOME"
#endif
                )) start = h;
        const bool ok = env.dlg_start ? env.dlg_start(k, title, start, name) : psp_filedlg::start(k, title, start, name);
        if (!ok) { message("No file window", "The system's file window cannot be opened here. Use \"Browse in the game\" instead."); return; }
        native = what;
        question("Choose in the file window", "The system's file window is open (it may be behind the game window).\nPick there, or cancel here.",
                 {"Cancel"}, [this](int) {
                     if (native == Native::None) return;
                     native = Native::None;
                     if (env.dlg_abandon) env.dlg_abandon(); else psp_filedlg::abandon();
                 });
    }
    bool native_poll() {
        if (native == Native::None) return false;
        std::string path, err;
        const psp_filedlg::State s = env.dlg_poll ? env.dlg_poll(&path, &err) : psp_filedlg::poll(&path, &err);
        if (s == psp_filedlg::State::Running) return false;
        const Native what = native;
        native = Native::None;
        dlg_on = false;
        dlg_cb = nullptr;
        dirty = true;
        if (s == psp_filedlg::State::Idle || s == psp_filedlg::State::Cancelled) return true;
        if (s == psp_filedlg::State::Failed) { message("The file window failed", err.empty() ? "Try \"Browse in the game\" instead." : err); return true; }
#ifdef __ANDROID__
        const bool commit = true;
        const std::string label = what == Native::ImportArchive ? "The picked archive" : "The picked folder";
#else
        const bool commit = false;
        const std::string label = fit(path, 40, true);
#endif
        switch (what) {
        case Native::ImportFolder: import_folder(path, label); break;
        case Native::ImportArchive: import_archive(path, label); break;
        case Native::ExportFolder: open_export(path, false, commit); break;
        case Native::ExportZip: open_export(path, true, commit); break;
        default: break;
        }
        return true;
    }

    // ---- the in-game browser ----
    std::vector<psp_savemgr::Source> places() const {
        if (env.places) return env.places();
        std::vector<psp_savemgr::Source> out;
        std::error_code ec;
        auto add = [&](const std::string& label, const fs::path& p) {
            if (!fs::is_directory(p, ec)) return;
            for (auto& o : out) if (o.path == S(p)) return;
            out.push_back({label, S(p)});
        };
        if (!env.game_dir.empty()) add("Game folder", P(env.game_dir));
#if defined(__ANDROID__)
        add("Phone storage", P("/storage/emulated/0"));
        add("Downloads", P("/storage/emulated/0/Download"));
        for (auto it = fs::directory_iterator(P("/storage"), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string n = S(it->path().filename());
            if (n != "emulated" && n != "self") add("SD card " + n, it->path());
        }
#elif defined(_WIN32)
        if (const char* h = std::getenv("USERPROFILE")) {
            add("Home", P(h)); add("Downloads", P(h) / "Downloads"); add("Documents", P(h) / "Documents"); add("Desktop", P(h) / "Desktop");
        }
        const DWORD drives = GetLogicalDrives();
        for (int i = 2; i < 26; i++)
            if (drives & (1u << i)) { std::string r(1, char('A' + i)); r += ":\\"; add("Drive " + r.substr(0, 2), P(r)); }
#else
        if (const char* h = std::getenv("HOME")) {
            add("Home", P(h)); add("Downloads", P(h) / "Downloads"); add("Documents", P(h) / "Documents"); add("Desktop", P(h) / "Desktop");
        }
        const char* user = std::getenv("USER");
        for (const std::string& m : {std::string("/media/") + (user ? user : ""), std::string("/run/media/") + (user ? user : ""), std::string("/mnt")})
            for (auto it = fs::directory_iterator(P(m), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                add("Drive " + S(it->path().filename()), it->path());
        add("Whole computer", P("/"));
#endif
        for (const auto& s : sources) add(s.label, P(s.path));
        return out;
    }
    void open_browse(bool importing, const std::string& dir) {
        Level l;
        l.v = V::Browse;
        l.importing = importing;
        l.title = importing ? "Browse" : "Export to";
        l.dir = dir;
        if (importing && !dir.empty()) l.here_count = int(psp_savemgr::find_saves(dir, 4, 400).size());
        push(std::move(l));
    }
    void browse_to(const std::string& dir) {   // the same level shows another folder
        Level& l = top();
        l.dir = dir;
        l.sel = 0;
        l.here_count = l.importing && !dir.empty() ? int(psp_savemgr::find_saves(dir, 4, 400).size()) : -1;
        dirty = true;
    }

    // ---- backups ----
    const std::vector<psp_savebackup::Backup>& backup_list() {
        if (!backups_loaded && !pending()) { backups = psp_savebackup::list(env.backup_root); backups_loaded = true; }
        return backups;
    }
    const psp_savebackup::Backup* find_backup(const std::string& id) {
        for (const auto& b : backup_list()) if (b.id == id) return &b;
        return nullptr;
    }
    std::string restore_one(const SaveInfo& s) {
        const std::string have = existing(s.suffix);
        const std::string disc = have.size() >= 9 ? have.substr(0, 9) : (psp_savemgr::valid_disc_id(env.disc_id) ? env.disc_id : s.disc);
        if (!have.empty() && psp_savebackup::fingerprint(S(P(env.saves_root) / P(have))) == psp_savebackup::fingerprint(s.path)) return "same";
        std::string name;
        return psp_savemgr::copy_save(s, env.saves_root, disc, !have.empty(), &name, env.backup_root, psp_savebackup::kRestore);
    }
    void restore(const std::string& id, int which) {   // which < 0: all of the backup
        const psp_savebackup::Backup* b = find_backup(id);
        if (!b) { message("Backup gone", "That backup is no longer there."); return; }
        const std::vector<SaveInfo> saves = which < 0 ? b->saves : std::vector<SaveInfo>{b->saves[size_t(which)]};
        int done = 0, same = 0;
        std::string fails;
        for (const auto& s : saves) {
            const std::string e = restore_one(s);
            if (e.empty()) ++done;
            else if (e == "same") ++same;
            else fails += "\n" + slot_label(s.suffix) + ": " + e;
        }
        std::fprintf(stderr, "[SAVEMGR] restore from %s: %d restored, %d unchanged\n", id.c_str(), done, same);
        inval_backups();
        if (done) after_local_change();
        std::string t = done ? "Restored " + plural(done, "save", "saves") + ". The saves they replaced are in Backups." : "Nothing to restore.";
        if (same) t += "\n" + plural(same, "save was", "saves were") + " already the same as the backup.";
        if (done && cloud_configured()) t += "\nCloud sync uploads the restored " + std::string(done == 1 ? "save" : "saves") + " now, so your other devices get " + (done == 1 ? "it" : "them") + " too.";
        pop_to_top();
        message(fails.empty() ? "Restored" : "Restore finished with problems", t + fails);
    }

    // ---- conflicts ----
    bool local_of(const std::string& suffix, SaveInfo* out) const {
        const std::string have = existing(suffix);
        return !have.empty() && psp_savemgr::read_save(S(P(env.saves_root) / P(have)), out);
    }
    void resolve(const std::string& suffix, psp_cloud::Choice c) {
        if (env.cloud_resolve) env.cloud_resolve(suffix, c);
        if (c != psp_cloud::Choice::Skip) { ++changed; inval_backups(); }
    }
    void keep_newer_all() {
        const auto cs = conflicts();
        int local = 0, remote = 0;
        for (const auto& c : cs) (c.remote.mtime > c.local_mtime ? remote : local)++;
        question("Keep the newer save of each?", "This device's save is newer for " + plural(local, "save", "saves") + ", the cloud's for " +
                 plural(remote, "save", "saves") + ".\nThe save that loses is not lost: yours goes to Backups, the cloud's is kept in the cloud as a .bak.",
                 {"Keep the newer ones", "Cancel"}, [this](int b) {
                     if (b != 0) return;
                     for (const auto& c : conflicts())
                         resolve(c.suffix, c.remote.mtime > c.local_mtime ? psp_cloud::Choice::KeepRemote : psp_cloud::Choice::KeepLocal);
                 });
    }

    // ---- building the rows ----
    void add(SetKind k, const std::string& label, const std::string& value, const std::string& help, Act act) {
        SetRow r;
        r.kind = k;
        r.id = int(rows.size());
        r.label = label;
        r.value = value;
        r.help = help;
        rows.push_back(std::move(r));
        acts.push_back(std::move(act));
    }
    void header(const std::string& label) { add(SetKind::Header, label, "", "", {}); }

    void build_top() {
        const auto cs = conflicts();
        if (!cs.empty()) {
            header("Needs your choice");
            for (const auto& c : cs) {
                add(SetKind::Submenu, slot_label(c.suffix) + ": changed on two devices", "choose",
                    slot_label(c.suffix) + "\nThis save was changed here and on " + (c.remote.device_name.empty() ? std::string("another device") : c.remote.device_name) +
                    " since the last sync, so cloud sync left both alone. Open it to see both and choose which one to keep. This device: " +
                    when_text(c.local_mtime) + ". Cloud: " + when_text(c.remote.mtime) + ".", {A::Conflict, 0, c.suffix});
            }
            if (cs.size() > 1)
                add(SetKind::Action, "Keep the newer save of each", plural(int(cs.size()), "save", "saves"),
                    "Keep the newer of each\nFor every save above, keep whichever side was saved last. The other side is kept as a backup.", {A::KeepNewerAll});
        }
        if (cloud_on()) {
            header("Cloud sync");
            const psp_cloud_svc::Status st = cloud_status();
            using St = psp_cloud_svc::Status;
            const char* v = st.state == St::Syncing ? "working..." : st.state == St::Error ? "problem" : st.state == St::NeedsChoice ? "needs choice"
                          : st.state == St::SigningIn ? "signing in" : st.state == St::Off ? "off" : "ok";
            if (cloud_configured()) {
                add(SetKind::Action, "Sync now", v, "Sync now\nUpload saves changed here and download newer ones. " + st.text, {A::CloudSync});
            } else {
                add(SetKind::Info, "Cloud sync", "off", "Cloud sync\nNot set up. Keep saves in step between your devices with Google Drive, a WebDAV server or a synced folder.", {});
            }
            add(SetKind::Submenu, "Cloud settings...", "", "Cloud settings\nChoose the service and sign in (Settings > Cloud).", {A::CloudSettings});
        }
        {
            const bool known = mine_ok && mine_at == changed;
            const bool wait = !known && pending();
            const int n = known ? mine_count : wait ? 0 : int(psp_savemgr::scan(env.saves_root).size());
            header("This game's saves");
            add(SetKind::Submenu, "Duplicate, move, rename or delete...", wait ? std::string("Loading...") : plural(n, "save", "saves"),
                "This game's saves\nCopy a save into another slot, move it, change the name the save list shows, or delete it (deleted saves go to Backups).",
                {A::Mine});
        }
        const bool native = dlg_available();
        header("Import saves");
        if (!searched)
            add(SetKind::Info, "Looking for PPSSPP and memory sticks...", "", "Looking\nThe drives of this device are searched for other copies of the game's saves.", {});
        for (size_t i = 0; i < sources.size() && i < source_counts.size(); ++i)
            add(SetKind::Submenu, "From " + fit(sources[i].label, 34), plural(source_counts[i], "save", "saves"),
                "From " + sources[i].label + "\nFound automatically: " + sources[i].path + ". Saves of every region work.", {A::Source, int(i)});
        if (native) {
            add(SetKind::Action, "Choose a folder...", "", "Choose a folder\nOpens the system's file window. Pick any folder with Dissidia 012 saves in it (US, EU or JP): "
                "a memory stick, PPSSPP's folder, a backup, a folder someone sent you. Saves are found anywhere inside it.", {A::PickFolder});
            add(SetKind::Action, "Choose an archive...", ".zip .7z .rar", "Choose an archive\nOpens the system's file window. Pick a .zip, .7z, .rar or .tar.gz with saves in it; "
                "it is unpacked and its saves are listed.", {A::PickArchive});
        }
        add(SetKind::Submenu, native ? "Browse in the game..." : "Choose a folder or archive...", "", "Browse\nGo through the folders here and pick a folder or an archive (.zip, .7z, .rar) "
            "that holds saves.", {A::Browse});
        add(SetKind::Text, "Type a path...", "", "Type a path\nType the path of a folder or an archive with saves.", {A::TypeImport});

        header("Export saves");
        if (native) {
            add(SetKind::Action, "Export to a folder...", "", "Export to a folder\nOpens the system's file window: pick where the saves go. If you pick a memory stick or "
                "PPSSPP's folder, they go into its PSP/SAVEDATA.", {A::ExportFolder});
            add(SetKind::Action, "Export as a .zip file...", "", "Export as a .zip\nOpens the system's file window: choose the name of a .zip with the saves (to keep, "
                "send or move to another device; this game imports it as it is).", {A::ExportZip});
        }
        for (size_t i = 0; i < dests.size(); ++i)
            add(SetKind::Submenu, "To " + fit(dests[i].label, 36), "", "To " + dests[i].label + "\n" + dests[i].path, {A::Dest, int(i)});
        add(SetKind::Submenu, native ? "Browse in the game..." : "Choose a folder...", "", "Browse\nGo through the folders here and pick where the saves go.", {A::BrowseExport});
        add(SetKind::Text, "Type a path...", "", "Type a path\nType the folder the saves go to (created when missing), or a path ending in .zip.", {A::TypeExport});
        {
            add(SetKind::Choice, "Folder names", keep_region ? "Keep region" : "As " + env.disc_id,
                keep_region ? "Folder names\nEach save keeps its own region's ID (ULUS10566..., ULES01505...)."
                            : "Folder names\nEvery save gets this game's ID (" + env.disc_id + "), so a PPSSPP copy of this same disc finds them.", {A::Names});
            rows.back().can_dec = !keep_region;
            rows.back().can_inc = keep_region;
        }

        header("Backups");
        const auto& bl = backup_list();
        add(SetKind::Action, "Back up now", "", "Back up now\nCopy every save into a new backup that stays until you delete it.", {A::BackupNow});
        add(SetKind::Submenu, "Restore a backup", pending() ? std::string("Loading...") : plural(int(bl.size()), "backup", "backups"),
            "Restore a backup\nEvery save that cloud sync, an import or a restore replaced is kept here, and the saves are copied once a day. "
            "Put any of them back, for example to undo a cloud download. Folder: " + env.backup_root, {A::Backups});

    }

    void build_pick() {
        Level& l = top();
        header(l.importing ? "From " + fit(l.src_label, 50) : "To " + (l.dest_commit ? std::string(l.dest_zip ? "the picked .zip" : "the picked folder") : fit(l.dest, 50, true)));
        const int n = checked_count(l);
        add(SetKind::Toggle, "All saves", n ? plural(n, "selected", "selected") : "", l.importing
            ? "All saves\nSelect the usual set (one save per slot, this game's region first, then the newest), or clear the selection."
            : "All saves\nSelect every save, or clear the selection.", {A::SelAll});
        rows.back().on = n > 0;
        for (size_t i = 0; i < l.saves.size(); ++i) {
            const SaveInfo& s = l.saves[i];
            std::string help = slot_label(s.suffix) + " (" + region_label(s.disc) + ")\n";
            const std::string t = save_title(s);
            if (!t.empty()) help += t + ". ";
            help += "Saved " + when_text(s.mtime) + ", " + size_text(s.bytes) + ", folder " + s.name + ". ";
            if (l.importing) {
                SaveInfo mine;
                if (local_of(s.suffix, &mine)) {
                    const std::string a = age_diff(s.mtime, mine.mtime);
                    help += "Replaces your " + slot_label(s.suffix) + " from " + when_text(mine.mtime) + (a.empty() || a == "same time" ? "" : " (this one is " + a + ")") +
                            "; yours goes to Backups first.";
                } else help += "You do not have this slot yet.";
                if (l.importing) help += " Source: " + s.path;
            }
            add(SetKind::Toggle, slot_label(s.suffix) + "  " + region_label(s.disc), when_short(s.mtime), help, {A::Toggle, int(i)});
            rows.back().on = l.checked[i];
        }
        add(SetKind::Action, (l.importing ? "Import " : "Export ") + plural(n, "save", "saves"), "",
            l.importing ? "Import\nCopy the selected saves into this game. They get this game's folder names; the save data itself is not changed."
                        : (l.dest_zip ? "Export\nWrite the selected saves into the .zip." : "Export\nCopy the selected saves there."), {A::Run});
        if (!n) rows.back().disabled = true;
    }

    void build_browse() {
        Level& l = top();
        std::error_code ec;
        if (l.dir.empty()) {
            header("Places");
            const auto pl = places();
            for (size_t i = 0; i < pl.size(); ++i) add(SetKind::Submenu, fit(pl[i].label, 40), "", pl[i].label + "\n" + pl[i].path, {A::BrowseDir, 0, pl[i].path});
            if (pl.empty()) add(SetKind::Info, "No places found", "", "", {});
            return;
        }
        header(fit(l.dir, 60, true));
        if (l.importing) {
            add(SetKind::Action, "Use this folder", l.here_count >= 0 ? plural(l.here_count, "save", "saves") : "",
                "Use this folder\nImport from " + l.dir + ". Saves inside its sub-folders are found too.", {A::BrowseUse});
            if (l.here_count == 0) rows.back().disabled = true;
        } else {
            add(SetKind::Action, "Export here", "", "Export here\nThe saves go into " + l.dir + " (or its PSP/SAVEDATA, when it has one).", {A::BrowseHere});
        }
        const fs::path parent = P(l.dir).parent_path();
        add(SetKind::Action, "Up one folder", "", "Up\n" + (parent.empty() || parent == P(l.dir) ? std::string("Places") : S(parent)), {A::BrowseUp});
        std::vector<std::pair<std::string, bool>> items;   // name, is folder
        for (auto it = fs::directory_iterator(P(l.dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string n = S(it->path().filename());
            if (n.empty() || n[0] == '.') continue;
            std::error_code e2;
            if (it->is_directory(e2)) items.push_back({n, true});
            else if (l.importing && psp_savearchive::looks_like_archive(n)) items.push_back({n, false});
        }
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second : a.first < b.first; });
        const size_t cap = 180;
        for (size_t i = 0; i < items.size() && i < cap; ++i) {
            const std::string full = S(P(l.dir) / P(items[i].first));
            if (items[i].second) {
                std::string d, x;
                const bool save = psp_savemgr::split_name(items[i].first, &d, &x);
                add(SetKind::Submenu, fit(items[i].first, 44) + "/", save ? "save" : "", items[i].first + "\n" + full, {A::BrowseDir, 0, full});
            } else {
                add(SetKind::Action, fit(items[i].first, 44), "archive", items[i].first + "\nOpen this archive and list the saves in it.", {A::BrowseFile, 0, full});
            }
        }
        if (items.size() > cap) add(SetKind::Info, plural(int(items.size() - cap), "more item", "more items") + " not shown", "", "Type the path instead.", {});
        if (ec && items.empty()) add(SetKind::Info, "Cannot read this folder", "", ec.message(), {});
    }

    void build_conflict() {
        Level& l = top();
        psp_cloud::Item c;
        bool found = false;
        for (const auto& it : conflicts()) if (it.suffix == l.suffix) { c = it; found = true; }
        if (!found) { add(SetKind::Info, "Already decided", "", "This save no longer needs a choice.", {}); return; }
        SaveInfo mine;
        const bool have = local_of(l.suffix, &mine);
        const time_t lt = have ? mine.mtime : c.local_mtime, rt = c.remote.mtime;
        const std::string dev = env.device_name.empty() ? "this device" : env.device_name;
        const std::string other = c.remote.device_name.empty() ? "another device" : c.remote.device_name;
        const bool local_newer = lt >= rt;
        // Per side: the save's own description and when it was saved (which is newer). A long description shows its end: games put the
        // changing part (play time, progress) last, after a fixed start (Dissidia: "Date: ... PLAYER: ... Time: ...").
        auto side = [&](const std::string& head, time_t t, bool newer, const std::string& desc, const std::string& help) {
            header(head);   // one row per side, so both and the three choices fit on one screen
            add(SetKind::Info, desc.empty() ? "Saved" : fit(desc, 40, true), when_short(t) + (newer ? "  newer" : ""), help, {});
        };
        const std::string ld = have ? save_title(mine) : "";
        side("On this device (" + fit(dev, 30) + ")", lt, local_newer && lt != rt, ld,
             "This device\n" + (ld.empty() ? std::string() : ld + ". ") + "Saved " + when_text(lt) + (have ? ", " + size_text(mine.bytes) : std::string()) + ".");
        const std::string rd = psp_savemgr::one_line(c.remote.detail.empty() ? c.remote.savetitle : c.remote.detail);
        side("In the cloud (from " + fit(other, 30) + ")", rt, !local_newer, rd.empty() ? "(no description: uploaded by an older version)" : rd,
             "Cloud\n" + (rd.empty() ? std::string("(The device that uploaded it runs an older version, which does not send the save's description.) ") : rd + ". ") +
             "Saved " + when_text(rt) + " on " + other + ".");
        header("Which one do you keep?");
        if (l.fresh) { l.fresh = false; l.sel = int(rows.size()) + (local_newer ? 0 : 1); }
        add(SetKind::Action, "Keep this device's save", local_newer ? "newer" : "",
            "Keep this device's save\nIt is uploaded and your other devices get it at their next sync. The cloud's copy is kept in the cloud as " + l.suffix + ".bak.",
            {A::KeepLocal});
        add(SetKind::Action, "Use the cloud save", !local_newer ? "newer" : "",
            "Use the cloud save\nIt replaces the save on this device. Yours is kept in Backups (Saves > Restore a backup), so you can switch back.", {A::KeepRemote});
        add(SetKind::Action, "Decide later", "", "Decide later\nLeave both as they are. This save is not synced until you choose.", {A::Later});
    }

    void build_backups() {
        const auto& bl = backup_list();
        if (pending()) { add(SetKind::Info, "Loading backups...", "", "Loading\nThe backups are being read.", {}); return; }
        header(bl.empty() ? "No backups yet" : "Newest first");
        const size_t cap = 120;
        for (size_t i = 0; i < bl.size() && i < cap; ++i) {
            const auto& b = bl[i];
            std::string names;
            for (const auto& s : b.saves) names += (names.empty() ? "" : ", ") + slot_label(s.suffix);
            const char* why = b.reason == psp_savebackup::kCloudDownload ? "cloud download" : b.reason == psp_savebackup::kCloudConflict ? "cloud choice"
                            : b.reason == psp_savebackup::kImport ? "import" : b.reason == psp_savebackup::kRestore ? "restore"
                            : b.reason == psp_savebackup::kManual ? "by you" : b.reason == psp_savebackup::kDaily ? "daily" : b.reason == psp_savebackup::kLegacy ? "older" : "replaced";
            add(SetKind::Submenu, when_text(b.when), why, std::string(psp_savebackup::reason_text(b.reason)) + "\n" + plural(int(b.saves.size()), "save", "saves") +
                " (" + size_text(b.bytes) + "): " + fit(names, 200) + ".", {A::OpenBackup, 0, b.id});
        }
        if (bl.size() > cap) add(SetKind::Info, plural(int(bl.size() - cap), "older backup", "older backups") + " not shown", "", "They are in " + env.backup_root, {});
    }

    void build_backup() {
        if (pending()) { add(SetKind::Info, "Loading backups...", "", "Loading\nThe backups are being read.", {}); return; }
        const psp_savebackup::Backup* b = find_backup(top().backup_id);
        if (!b) { add(SetKind::Info, "This backup is gone", "", "", {}); return; }
        header(std::string(psp_savebackup::reason_text(b->reason)) + ", " + when_text(b->when));
        for (size_t i = 0; i < b->saves.size(); ++i) {
            const SaveInfo& s = b->saves[i];
            SaveInfo mine;
            std::string cmp = "You do not have this slot now: restoring adds it.";
            if (local_of(s.suffix, &mine)) {
                if (psp_savebackup::fingerprint(mine.path) == psp_savebackup::fingerprint(s.path)) cmp = "Your current save is the same as this one.";
                else cmp = "Your current save (" + when_text(mine.mtime) + ") goes to Backups first, so this can be undone.";
            }
            const std::string t = save_title(s);
            add(SetKind::Action, "Restore " + slot_label(s.suffix), region_label(s.disc) + "  " + when_short(s.mtime),
                "Restore " + slot_label(s.suffix) + "\n" + (t.empty() ? "" : t + ". ") + "Saved " + when_text(s.mtime) + ". " + cmp +
                (cloud_configured() ? " Cloud sync then uploads it, so your other devices get it too." : ""), {A::Restore, int(i)});
        }
        if (b->saves.size() > 1) add(SetKind::Action, "Restore all", plural(int(b->saves.size()), "save", "saves"), "Restore all\nPut every save of this backup back.", {A::RestoreAll});
        add(SetKind::Danger, "Delete this backup", "", "Delete this backup\nRemove it for good.", {A::DeleteBackup});
    }

    // ---- this game's saves: duplicate, move, rename, delete ----
    std::vector<SaveInfo> mine() {
        std::vector<SaveInfo> v = psp_savemgr::scan(env.saves_root);
        sort_saves(v);
        return v;
    }
    bool mine_one(const std::string& name, SaveInfo* out) const {
        return psp_savemgr::read_save(S(P(env.saves_root) / P(name)), out) && out->name == name;
    }
    void build_mine() {
        const auto v = mine();
        header(v.empty() ? "No saves yet" : "Choose a save");
        for (size_t i = 0; i < v.size() && i < 150; ++i) {
            const std::string t = save_title(v[i]);
            add(SetKind::Submenu, slot_numbered(v[i].suffix) + "  " + region_label(v[i].disc), when_short(v[i].mtime),
                slot_numbered(v[i].suffix) + "\n" + (t.empty() ? "" : t + ". ") + "Saved " + when_text(v[i].mtime) + ", " + size_text(v[i].bytes) + ", folder " + v[i].name + ".",
                {A::OpenSave, 0, v[i].name});
        }
    }
    void build_save() {
        SaveInfo s;
        if (!mine_one(top().save_name, &s)) { add(SetKind::Info, "This save is gone", "", "", {}); return; }
        const std::string t = save_title(s), shown = psp_savemgr::one_line(s.savetitle);
        header(slot_numbered(s.suffix) + "  " + region_label(s.disc) + "  (" + s.name + ")");
        add(SetKind::Info, t.empty() ? slot_numbered(s.suffix) : fit(t, 40, true), when_short(s.mtime),
            slot_numbered(s.suffix) + "\n" + (t.empty() ? "" : t + ". ") + "Saved " + when_text(s.mtime) + ", " + size_text(s.bytes) + ", folder " + s.name + ".", {});
        add(SetKind::Submenu, "Duplicate to another slot...", "", "Duplicate\nCopy this save into another slot, to keep a copy you can go back to or play on separately. "
            "The game shows both on its save list.", {A::Duplicate});
        add(SetKind::Submenu, "Move to another slot...", "", "Move\nPut this save into another slot; this slot becomes free. The old folder goes to Backups.", {A::Move});
        add(SetKind::Text, "Rename...", fit(shown, 24), "Rename\nChange the name the save list shows for this save (now \"" + shown + "\"), e.g. to tell copies apart. "
            "The game writes its own name back the next time it saves into this slot.", {A::Rename});
        add(SetKind::Danger, "Delete", "", "Delete\nRemove this save. It goes to Backups, so it can be restored." +
            std::string(cloud_configured() ? " Cloud sync deletes it on your other devices too (each keeps a backup)." : ""), {A::DeleteSave});
    }
    void build_slots() {
        SaveInfo s;
        if (!mine_one(top().save_name, &s)) { add(SetKind::Info, "This save is gone", "", "", {}); return; }
        const std::string pre = slot_prefix(s.suffix);
        header(std::string(top().moving ? "Move " : "Copy ") + slot_numbered(s.suffix) + " to which slot?");
        for (int n = 0; n < kSlots; ++n) {
            char num[8];
            std::snprintf(num, sizeof num, "%02d", n);
            const std::string sfx = pre + num;
            if (sfx == s.suffix) continue;
            const std::string have = existing(sfx);
            SaveInfo other;
            const bool taken = !have.empty() && psp_savemgr::read_save(S(P(env.saves_root) / P(have)), &other);
            add(SetKind::Action, slot_numbered(sfx), taken ? "replaces " + when_short(other.mtime) : "empty",
                slot_numbered(sfx) + "\n" + (taken ? "Holds a save from " + when_text(other.mtime) + (save_title(other).empty() ? "" : " (" + save_title(other) + ")") +
                                                      ", which goes to Backups first."
                                                    : std::string("An empty slot.")), {A::ToSlot, 0, sfx});
        }
    }
    void to_slot(const std::string& target) {
        SaveInfo s;
        const bool moving = top().moving;
        if (!mine_one(top().save_name, &s)) { message("This save is gone", "It is no longer there."); return; }
        const std::string have = existing(target);
        SaveInfo t = s;
        t.suffix = target;
        const std::string disc = have.size() >= 9 ? have.substr(0, 9) : s.disc;
        std::string name;
        std::string e = psp_savemgr::copy_save(t, env.saves_root, disc, !have.empty(), &name, env.backup_root, psp_savebackup::kReplaced);
        if (e.empty() && moving) {
            e = psp_savebackup::set_aside(s.path, env.backup_root, psp_savebackup::kMoved);
            if (e.empty() && cloud_configured() && env.cloud_delete) env.cloud_delete(s.suffix);
        }
        std::fprintf(stderr, "[SAVEMGR] %s %s -> %s: %s\n", moving ? "move" : "duplicate", s.name.c_str(), name.c_str(), e.empty() ? "ok" : e.c_str());
        inval_backups();
        if (e.empty()) after_local_change();
        while (stack.size() > 2) pop();   // back to the list of saves
        if (!e.empty()) { message(moving ? "Move failed" : "Copy failed", e); return; }
        message(moving ? "Moved" : "Duplicated", slot_numbered(s.suffix) + (moving ? " is now " : " was copied to ") + slot_numbered(target) + "." +
                (have.empty() ? "" : "\nThe save that was there is in Backups."));
    }
    void delete_save() {
        SaveInfo s;
        if (!mine_one(top().save_name, &s)) return;
        const std::string e = psp_savebackup::set_aside(s.path, env.backup_root, psp_savebackup::kDeleted);
        std::fprintf(stderr, "[SAVEMGR] delete %s: %s\n", s.name.c_str(), e.empty() ? "ok (in the backups)" : e.c_str());
        if (!e.empty()) { message("Cannot delete", e); return; }
        const bool cloud = cloud_configured() && env.cloud_delete;
        if (cloud) env.cloud_delete(s.suffix);
        ++changed;
        inval_backups();
        pop();
        message("Deleted", slot_numbered(s.suffix) + " was deleted. It is in Backups (Restore a backup) if you want it back." +
                std::string(cloud ? "\nIt is deleted in the cloud and on your other devices at their next sync too." : ""));
    }
    void rename_save(const std::string& title_in) {
        std::string title = title_in;
        while (!title.empty() && title.back() == ' ') title.pop_back();
        while (!title.empty() && title.front() == ' ') title.erase(title.begin());
        SaveInfo s;
        if (title.empty() || !mine_one(top().save_name, &s)) return;
        if (title.size() > 127) { message("Cannot rename", "the name is too long (at most 127 bytes)"); return; }   // (SAVEDATA_TITLE holds 128)
        std::string id;
        psp_savebackup::snapshot(env.saves_root, env.backup_root, psp_savebackup::kRenamed, {s.name}, &id);
        const std::string e = psp_savemgr::set_title(s.path, title);
        std::fprintf(stderr, "[SAVEMGR] rename %s: %s\n", s.name.c_str(), e.empty() ? "ok" : e.c_str());
        inval_backups();
        if (!e.empty()) { message("Cannot rename", e); return; }
        ++changed;   // (the content hash does not cover PARAM.SFO: the name stays on this device)
        dirty = true;
    }

    void build() {
        rows.clear();
        acts.clear();
        switch (top().v) {
        case V::Top: build_top(); break;
        case V::Pick: build_pick(); break;
        case V::Browse: build_browse(); break;
        case V::Conflict: build_conflict(); break;
        case V::Backups: build_backups(); break;
        case V::Backup: build_backup(); break;
        case V::Mine: build_mine(); break;
        case V::Save: build_save(); break;
        case V::Slots: build_slots(); break;
        }
        // never rest on a header
        int& s = top().sel;
        s = std::clamp(s, 0, std::max(0, int(rows.size()) - 1));
        if (!rows.empty() && rows[size_t(s)].kind == SetKind::Header) {
            int t = s;
            while (t < int(rows.size()) && rows[size_t(t)].kind == SetKind::Header) ++t;
            if (t >= int(rows.size())) { t = s; while (t > 0 && rows[size_t(t)].kind == SetKind::Header) --t; }
            s = t;
        }
        dirty = false;
    }

    // ---- actions ----
    void act(int row, int d) {
        if (row < 0 || row >= int(acts.size())) return;
        const Act a = acts[size_t(row)];
        Level& l = top();
        l.sel = row;
        switch (a.a) {
        case A::None: break;
        case A::Conflict: { Level n; n.v = V::Conflict; n.suffix = a.s; n.title = slot_label(a.s); n.sel = 0; push(std::move(n)); break; }
        case A::KeepNewerAll: keep_newer_all(); break;
        case A::Source: if (size_t(a.i) < sources.size()) import_folder(sources[size_t(a.i)].path, sources[size_t(a.i)].label); break;
        case A::PickFolder: native_start(Native::ImportFolder); break;
        case A::PickArchive: native_start(Native::ImportArchive); break;
        case A::Browse: open_browse(true, ""); break;
        case A::TypeImport: ask_text("Folder or archive with saves", "", [this](const std::string& v) { import_typed(v); }); break;
        case A::ExportFolder: native_start(Native::ExportFolder); break;
        case A::ExportZip: native_start(Native::ExportZip); break;
        case A::Dest: if (size_t(a.i) < dests.size()) open_export(dests[size_t(a.i)].path, false, false); break;
        case A::BrowseExport: open_browse(false, ""); break;
        case A::TypeExport:
            ask_text("Export to (a folder, or a name ending in .zip)", "", [this](std::string v) {
                while (!v.empty() && (v.back() == ' ' || v.back() == '/' || v.back() == '\\')) v.pop_back();
                if (v.empty()) return;
                if (v[0] == '~') if (const char* h = std::getenv("HOME")) v = h + v.substr(1);
                const bool zip = v.size() > 4 && (v.compare(v.size() - 4, 4, ".zip") == 0 || v.compare(v.size() - 4, 4, ".ZIP") == 0);
                std::error_code ec;
                if (!zip && !fs::is_directory(P(v), ec)) {
                    fs::create_directories(P(v), ec);
                    if (ec) { message("Cannot use that folder", fit(v, 60, true) + "\n" + ec.message()); return; }
                }
                open_export(v, zip, false);
            });
            break;
        case A::Names: if (d == 0 || (d > 0) == keep_region) keep_region = !keep_region; dirty = true; break;
        case A::BackupNow: {
            std::string id;
            const std::string e = psp_savebackup::snapshot(env.saves_root, env.backup_root, psp_savebackup::kManual, {}, &id);
            inval_backups();
            if (!e.empty()) message("Backup failed", e);
            else if (id.empty()) message("Nothing to back up", "This game has no saves yet.");
            else message("Backed up", "Your saves were copied into a new backup (" + id + "). It stays until you delete it.");
            break;
        }
        case A::Backups: { Level n; n.v = V::Backups; n.title = "Backups"; inval_backups(); start_preload(false); push(std::move(n)); break; }
        case A::CloudSync: if (env.cloud_sync && cloud_status().state != psp_cloud_svc::Status::Syncing) env.cloud_sync(); dirty = true; break;
        case A::CloudSettings: want_cloud_settings = true; break;
        case A::SelAll: {
            std::vector<bool> keep = l.checked;
            default_checks(l);
            if (keep == l.checked) l.checked.assign(l.checked.size(), false);   // already the usual set: clear
            dirty = true;
            break;
        }
        case A::Toggle: if (size_t(a.i) < l.saves.size()) toggle(l, size_t(a.i)); break;
        case A::Run: if (l.importing) begin_import(); else begin_export(); break;
        case A::BrowseUse: import_folder(l.dir, fit(l.dir, 40, true)); break;
        case A::BrowseUp: {
            const fs::path parent = P(l.dir).parent_path();
            browse_to(parent.empty() || parent == P(l.dir) ? "" : S(parent));
            break;
        }
        case A::BrowseDir: {
            std::error_code ec;
            fs::directory_iterator probe(P(a.s), ec);
            if (ec) { message("Cannot open the folder", fit(a.s, 60, true) + "\n" + ec.message()); break; }
            browse_to(a.s);
            break;
        }
        case A::BrowseFile: import_archive(a.s, fit(S(P(a.s).filename()), 40)); break;
        case A::BrowseHere: open_export(l.dir, false, false); break;
        case A::KeepLocal: resolve(l.suffix, psp_cloud::Choice::KeepLocal); pop(); break;
        case A::KeepRemote: resolve(l.suffix, psp_cloud::Choice::KeepRemote); pop(); break;
        case A::Later: pop(); break;
        case A::OpenBackup: { Level n; n.v = V::Backup; n.backup_id = a.s; n.title = "Backup"; push(std::move(n)); break; }
        case A::Restore: {
            const psp_savebackup::Backup* b = find_backup(l.backup_id);
            if (!b || size_t(a.i) >= b->saves.size()) break;
            const std::string id = l.backup_id;
            const int which = a.i;
            question("Restore " + slot_label(b->saves[size_t(a.i)].suffix) + "?", "Put back the save from " + when_text(b->saves[size_t(a.i)].mtime) +
                     ".\nYour current save goes to Backups first." + (cloud_configured() ? "\nCloud sync then uploads it to your other devices." : ""),
                     {"Restore", "Cancel"}, [this, id, which](int bt) { if (bt == 0) restore(id, which); });
            break;
        }
        case A::RestoreAll: {
            const std::string id = l.backup_id;
            question("Restore every save of this backup?", "Your current saves go to Backups first." + std::string(cloud_configured() ? "\nCloud sync then uploads them to your other devices." : ""),
                     {"Restore all", "Cancel"}, [this, id](int bt) { if (bt == 0) restore(id, -1); });
            break;
        }
        case A::Mine: { Level n; n.v = V::Mine; n.title = "This game's saves"; push(std::move(n)); break; }
        case A::OpenSave: { Level n; n.v = V::Save; n.save_name = a.s; std::string d, x; psp_savemgr::split_name(a.s, &d, &x); n.title = slot_numbered(x); n.sel = 2; push(std::move(n)); break; }   // (on Duplicate)
        case A::Duplicate: case A::Move: { Level n; n.v = V::Slots; n.save_name = l.save_name; n.moving = a.a == A::Move; n.title = a.a == A::Move ? "Move" : "Duplicate"; push(std::move(n)); break; }
        case A::Rename: {
            SaveInfo s;
            if (!mine_one(l.save_name, &s)) break;
            ask_text("Name shown on the save list", psp_savemgr::one_line(s.savetitle), [this](const std::string& v) { rename_save(v); });
            break;
        }
        case A::DeleteSave: {
            SaveInfo s;
            if (!mine_one(l.save_name, &s)) break;
            const std::string t = save_title(s);
            question("Delete " + slot_numbered(s.suffix) + "?", (t.empty() ? std::string() : fit(t, 70, true) + "\n") + "It goes to Backups, so it can be restored." +
                     std::string(cloud_configured() ? "\nIt is deleted in the cloud and on your other devices too (each keeps a backup)." : ""),
                     {"Delete", "Cancel"}, [this](int b) { if (b == 0) delete_save(); });
            break;
        }
        case A::ToSlot: {
            const std::string target = a.s, have = existing(a.s);
            if (have.empty()) { to_slot(target); break; }
            SaveInfo other;
            psp_savemgr::read_save(S(P(env.saves_root) / P(have)), &other);
            question("Replace " + slot_numbered(target) + "?", "It holds a save from " + when_text(other.mtime) + ". That save goes to Backups first.",
                     {"Replace", "Cancel"}, [this, target](int b) { if (b == 0) to_slot(target); });
            break;
        }
        case A::DeleteBackup: {
            const std::string id = l.backup_id;
            question("Delete this backup?", "It cannot be brought back.", {"Delete", "Cancel"}, [this, id](int bt) {
                if (bt != 0) return;
                const std::string e = psp_savebackup::remove(env.backup_root, id);
                inval_backups();
                pop();
                if (!e.empty()) message("Cannot delete", e);
            });
            break;
        }
        }
        dirty = true;
    }
};

Page::Page() : d_(std::make_unique<Impl>()) { d_->stack.push_back(Level{}); }
Page::~Page() = default;

void Page::set_env(const Env& e) {
    d_->env = e;
    d_->dirty = true;
    if (!e.scratch_dir.empty() && !e.dlg_start) psp_filedlg::set_scratch_dir(e.scratch_dir);   // Android copies picked documents there
}
const Env& Page::env() const { return d_->env; }

void Page::reset() {
    if (d_->native != Native::None) { if (d_->env.dlg_abandon) d_->env.dlg_abandon(); else psp_filedlg::abandon(); d_->native = Native::None; }
    d_->pop_to_top();
    d_->dlg_on = d_->ask_on = false;
    d_->dlg_cb = nullptr;
    d_->inval_backups();
    d_->mine_ok = false;
    d_->start_preload(true);
    d_->load_places();
    if (!d_->env.scratch_dir.empty()) { std::error_code ec; fs::remove_all(P(d_->env.scratch_dir), ec); fs::create_directories(P(d_->env.scratch_dir), ec); }
    if (d_->env.cloud_status) d_->cloud_rev = d_->env.cloud_status().revision;
    d_->dirty = true;
}

const std::vector<SetRow>& Page::rows() { if (d_->dirty) d_->build(); return d_->rows; }

std::vector<std::string> Page::crumbs() const {
    std::vector<std::string> c;
    for (size_t i = 1; i < d_->stack.size(); ++i) c.push_back(d_->stack[i].title);
    return c;
}
bool Page::in_sub() const { return d_->stack.size() > 1; }
int Page::sel() const { return d_->top().sel; }
void Page::set_sel(int i) { d_->top().sel = i; }

void Page::confirm(int row) {
    rows();
    if (row < 0 || row >= int(d_->rows.size())) return;
    const SetRow& r = d_->rows[size_t(row)];
    if (r.kind == SetKind::Header || r.kind == SetKind::Info || r.disabled) { d_->top().sel = row; return; }
    d_->act(row, 0);
}
void Page::change(int row, int dir) {
    rows();
    if (row < 0 || row >= int(d_->acts.size())) return;
    const A a = d_->acts[size_t(row)].a;
    if (a == A::Names) d_->act(row, dir);
    else if (a == A::Toggle || a == A::SelAll) d_->act(row, dir);
}
bool Page::back() {
    if (d_->stack.size() <= 1) return false;
    d_->pop();
    return true;
}
bool Page::dialog(Dialog* out) const { if (d_->dlg_on && out) *out = d_->dlg; return d_->dlg_on; }
void Page::dialog_choose(int b) {
    if (!d_->dlg_on) return;
    d_->dlg_on = false;
    auto cb = std::move(d_->dlg_cb);
    d_->dlg_cb = nullptr;
    d_->dirty = true;
    if (cb) cb(b);
}
bool Page::text(TextAsk* out) const { if (d_->ask_on && out) *out = d_->ask; return d_->ask_on; }
void Page::text_done(bool ok, const std::string& v) {
    if (!d_->ask_on) return;
    d_->ask_on = false;
    auto cb = std::move(d_->ask_cb);
    d_->ask_cb = nullptr;
    d_->dirty = true;
    if (ok && cb) cb(v);
}
bool Page::tick(int64_t) {
    bool changed = d_->native_poll();
    if (d_->take_places()) changed = true;
    if (d_->take_preload()) changed = true;
    if (d_->env.cloud_status) {
        const psp_cloud_svc::Status st = d_->env.cloud_status();
        if (st.revision != d_->cloud_rev) {
            d_->cloud_rev = st.revision;
            if (st.state != psp_cloud_svc::Status::Syncing)
                for (const auto& it : st.items) if (it.action == psp_cloud::Action::Pulled || it.action == psp_cloud::Action::Removed) { ++d_->changed; d_->inval_backups(); break; }
            d_->dirty = true;
            changed = true;
        }
    }
    return changed || d_->dirty;
}
bool Page::wait_idle(int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 5) {
        if ((!d_->finding || d_->finding->done) && (!d_->pre || d_->pre->done)) { d_->take_places(); d_->take_preload(); return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
bool Page::native_dialog_open() const { return d_->native != Native::None; }
bool Page::take_cloud_settings_request() { const bool r = d_->want_cloud_settings; d_->want_cloud_settings = false; return r; }
unsigned Page::saves_changed() const { return d_->changed; }
void Page::open_conflict(const std::string& suffix) {
    Level n; n.v = V::Conflict; n.suffix = suffix; n.title = slot_label(suffix);
    d_->push(std::move(n));
}

}  // namespace psp_saves
