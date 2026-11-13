// Custom Vulkan drivers: see psp_gpu_driver.h.
#include "psp_gpu_driver.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zlib.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

#include "hle/psp_http.h"

#if defined(__ANDROID__)
#include <android/api-level.h>
#include <dlfcn.h>
#if defined(PSPRECOMP_HAVE_ADRENOTOOLS)
#include <adrenotools/driver.h>
#endif
#endif

namespace psp_gpu {

namespace {

// ---------------------------------------------------------------- minimal JSON (objects, arrays, strings, numbers, literals)
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J* get(const char* k) const {
        for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string str(const char* k) const { const J* v = get(k); return v && v->t == Str ? v->s : std::string(); }
    double num(const char* k, double d = 0) const { const J* v = get(k); return v && v->t == Num ? v->n : d; }
};

struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    explicit Parser(const std::string& str) : s(str) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++; }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned cp = unsigned(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {   // surrogate pair
                        unsigned lo = unsigned(std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16));
                        if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += e;   // " \ /
            }
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    }
    bool value(J& v) {
        ws();
        if (i >= s.size() || ++depth > 64) return false;
        struct Dec { int& d; ~Dec() { d--; } } dec{depth};
        char c = s[i];
        if (c == '{') {
            v.t = J::Obj; i++; ws();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i >= s.size() || s[i++] != ':') return false;
                J child;
                if (!value(child)) return false;
                v.o.emplace_back(std::move(k), std::move(child));
                ws();
                if (i >= s.size()) return false;
                if (s[i] == ',') { i++; continue; }
                if (s[i] == '}') { i++; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.t = J::Arr; i++; ws();
            if (i < s.size() && s[i] == ']') { i++; return true; }
            for (;;) {
                J child;
                if (!value(child)) return false;
                v.a.push_back(std::move(child));
                ws();
                if (i >= s.size()) return false;
                if (s[i] == ',') { i++; continue; }
                if (s[i] == ']') { i++; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = J::Str; return str(v.s); }
        if (s.compare(i, 4, "true") == 0) { v.t = J::Bool; v.b = true; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { v.t = J::Bool; i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; return true; }
        char* end = nullptr;
        v.n = std::strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) return false;
        v.t = J::Num;
        i = size_t(end - s.c_str());
        return true;
    }
};

bool parse_json(const std::string& text, J& out) {
    Parser p(text);
    return p.value(out);
}

// ---------------------------------------------------------------- files
bool is_dir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
bool is_file(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }

bool make_dirs(const std::string& p) {
    std::string cur;
    for (size_t i = 0; i <= p.size(); i++) {
        if (i == p.size() || p[i] == '/') {
            if (!cur.empty() && !is_dir(cur) && mkdir(cur.c_str(), 0755) != 0 && !is_dir(cur)) return false;
        }
        if (i < p.size()) cur += p[i];
    }
    return true;
}

bool read_text(const std::string& p, std::string* out) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out->clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    std::fclose(f);
    return true;
}

bool write_file(const std::string& p, const void* data, size_t n) {
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, n, f) == n;
    return std::fclose(f) == 0 && ok;
}

void remove_tree(const std::string& p) {
    if (DIR* d = opendir(p.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            const std::string c = p + "/" + n;
            if (is_dir(c)) remove_tree(c); else unlink(c.c_str());
        }
        closedir(d);
    }
    rmdir(p.c_str());
}

// ---------------------------------------------------------------- zip (central directory; stored + deflate; no zip64)
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

struct ZipEntry { std::string name; uint16_t method = 0; uint32_t csize = 0, usize = 0, lho = 0; };

bool zip_list(const std::vector<uint8_t>& z, std::vector<ZipEntry>* out, std::string* err) {
    if (z.size() < 22) { *err = "not a zip file"; return false; }
    size_t e = std::string::npos;
    for (size_t i = z.size() - 22 + 1; i-- > 0 && z.size() - i <= 22 + 65535;)
        if (rd32(&z[i]) == 0x06054b50) { e = i; break; }
    if (e == std::string::npos) { *err = "not a zip file"; return false; }
    const unsigned n = rd16(&z[e + 10]);
    size_t p = rd32(&z[e + 16]);
    for (unsigned k = 0; k < n; k++) {
        if (p + 46 > z.size() || rd32(&z[p]) != 0x02014b50) { *err = "damaged zip directory"; return false; }
        ZipEntry ze;
        ze.method = rd16(&z[p + 10]);
        ze.csize = rd32(&z[p + 20]);
        ze.usize = rd32(&z[p + 24]);
        const size_t nl = rd16(&z[p + 28]), xl = rd16(&z[p + 30]), cl = rd16(&z[p + 32]);
        ze.lho = rd32(&z[p + 42]);
        if (p + 46 + nl > z.size()) { *err = "damaged zip directory"; return false; }
        ze.name.assign(reinterpret_cast<const char*>(&z[p + 46]), nl);
        if (ze.csize == 0xFFFFFFFFu || ze.usize == 0xFFFFFFFFu) { *err = "zip64 archives are not supported"; return false; }
        out->push_back(std::move(ze));
        p += 46 + nl + xl + cl;
    }
    return true;
}

bool zip_read(const std::vector<uint8_t>& z, const ZipEntry& e, std::vector<uint8_t>* out, std::string* err) {
    const size_t h = e.lho;
    if (h + 30 > z.size() || rd32(&z[h]) != 0x04034b50) { *err = "damaged zip entry " + e.name; return false; }
    const size_t data = h + 30 + rd16(&z[h + 26]) + rd16(&z[h + 28]);
    if (data + e.csize > z.size()) { *err = "truncated zip entry " + e.name; return false; }
    if (e.usize > (256u << 20)) { *err = "zip entry too large"; return false; }
    out->assign(e.usize, 0);
    if (e.method == 0) {
        if (e.csize != e.usize) { *err = "damaged zip entry " + e.name; return false; }
        std::memcpy(out->data(), &z[data], e.usize);
        return true;
    }
    if (e.method != 8) { *err = "unsupported zip compression in " + e.name; return false; }
    z_stream s;
    std::memset(&s, 0, sizeof s);
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { *err = "zlib failed"; return false; }
    s.next_in = const_cast<Bytef*>(&z[data]); s.avail_in = e.csize;
    s.next_out = out->data(); s.avail_out = e.usize;
    const int r = e.usize ? inflate(&s, Z_FINISH) : Z_STREAM_END;
    inflateEnd(&s);
    if (r != Z_STREAM_END || s.total_out != e.usize) { *err = "damaged zip entry " + e.name; return false; }
    return true;
}

std::string base_name(const std::string& n) {
    const size_t k = n.find_last_of("/\\");
    return k == std::string::npos ? n : n.substr(k + 1);
}

// ---------------------------------------------------------------- live list state
std::mutex g_mu;
std::vector<Remote> g_catalog;
bool g_loaded = false;
Status g_status;
bool g_busy = false;

void set_status(Status::State st, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_status.state = st;
    g_status.text = text;
    g_status.revision++;
}

std::string size_text(uint64_t b) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", double(b) / 1e6);
    return buf;
}

std::vector<std::string> feeds() {
    std::string f = "K11MCH1/AdrenoToolsDrivers";
    if (const char* e = std::getenv("PSPRECOMP_GPU_DRIVER_FEEDS")) if (e[0]) f = e;
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= f.size()) {
        size_t q = f.find(',', p);
        if (q == std::string::npos) q = f.size();
        std::string one = f.substr(p, q - p);
        while (!one.empty() && one.back() == ' ') one.pop_back();
        while (!one.empty() && one[0] == ' ') one.erase(0, 1);
        if (one.find('/') != std::string::npos && one.find("..") == std::string::npos) out.push_back(one);
        p = q + 1;
    }
    return out;
}

}  // namespace

std::string sanitize_id(const std::string& s) {
    std::string o;
    for (char c : s) o += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') ? c : '_';
    while (!o.empty() && o[0] == '.') o.erase(0, 1);   // no hidden / ".." folders
    return o.substr(0, 96);
}

std::string root_dir() {
    if (const char* e = std::getenv("PSPRECOMP_GPU_DRIVER_DIR")) if (e[0]) return e;
    return "gpu_drivers";
}

bool supported(std::string* why) {
#if defined(__ANDROID__) && defined(PSPRECOMP_HAVE_ADRENOTOOLS)
    if (access("/dev/kgsl-3d0", F_OK) != 0) {
        if (why) *why = "This phone does not have an Adreno GPU (custom drivers only exist for Adreno).";
        return false;
    }
    return true;
#else
    if (why) *why = "Custom GPU drivers are an Android feature (Adreno phones).";
    return false;
#endif
}

static std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

void curate(std::vector<Remote>* list, int gpu_gen) {
    std::vector<Remote> out;
    for (Remote& r : *list) {
        const std::string l = lower(r.id);
        if (l.find("gmem") != std::string::npos || l.find("sysmem") != std::string::npos) continue;
        if (gpu_gen > 0 && gpu_gen < 8 && l.find("a8xx") != std::string::npos) continue;
        bool dup = false;
        for (auto& o : out) if (o.id == r.id) dup = true;
        if (dup) continue;
        r.name = r.id;
        for (auto& c : r.name) if (c == '_') c = ' ';
        out.push_back(std::move(r));
    }
    auto group = [](const Remote& r) {
        const std::string l = lower(r.id);
        if (l.find("turnip") != std::string::npos || l.find("mesa") != std::string::npos) return 0;
        if (l.find("qualcomm") != std::string::npos) return 2;
        return 1;
    };
    std::stable_sort(out.begin(), out.end(), [&](const Remote& a, const Remote& b) {
        const int ga = group(a), gb = group(b);
        return ga != gb ? ga < gb : a.published > b.published;
    });
    *list = std::move(out);
}

int gpu_generation() {
    std::string t;
    if (!read_text("/sys/class/kgsl/kgsl-3d0/gpu_model", &t)) return 0;
    for (size_t i = 0; i < t.size(); i++)
        if (std::isdigit(static_cast<unsigned char>(t[i]))) return t[i] - '0';
    return 0;
}

bool parse_meta(const std::string& json, Installed* out) {
    J j;
    if (!parse_json(json, j) || j.t != J::Obj) return false;
    out->name = j.str("name");
    out->version = j.str("driverVersion");
    out->library = j.str("libraryName");
    out->min_api = int(j.num("minApi", 0));
    if (out->library.empty() || out->library.find('/') != std::string::npos || out->library.find("..") != std::string::npos) return false;
    if (out->name.empty()) out->name = out->library;
    return true;
}

std::vector<Installed> installed() {
    std::vector<Installed> v;
    const std::string root = root_dir();
    DIR* d = opendir(root.c_str());
    if (!d) return v;
    while (dirent* e = readdir(d)) {
        const std::string id = e->d_name;
        if (id.empty() || id[0] == '.') continue;
        std::string txt;
        Installed in;
        if (!read_text(root + "/" + id + "/meta.json", &txt) || !parse_meta(txt, &in)) continue;
        if (!is_file(root + "/" + id + "/" + in.library)) continue;
        in.id = id;
        v.push_back(std::move(in));
    }
    closedir(d);
    std::sort(v.begin(), v.end(), [](const Installed& a, const Installed& b) { return a.name < b.name; });
    return v;
}

bool remove(const std::string& id, std::string* err) {
    if (id.empty() || id != sanitize_id(id)) { if (err) *err = "bad driver id"; return false; }
    remove_tree(root_dir() + "/" + id);
    return true;
}

std::string selected() {
    const char* e = std::getenv("PSPRECOMP_GPU_DRIVER");
    return e ? e : "";
}

static std::string crash_path() { return root_dir() + "/.crashed"; }

bool crashed(const std::string& id) {
    std::string t;
    return !id.empty() && read_text(crash_path(), &t) && t == id;
}
void clear_crash(const std::string& id) {
    std::string t;
    if (read_text(crash_path(), &t) && (id.empty() || t == id)) unlink(crash_path().c_str());
}
void note_started() { unlink(crash_path().c_str()); }   // Vulkan came up: the driver did not crash

bool parse_releases(const std::string& json, std::vector<Remote>* out, std::string* err) {
    J j;
    if (!parse_json(json, j)) { if (err) *err = "the driver list could not be read"; return false; }
    if (j.t != J::Arr) {
        if (err) *err = j.t == J::Obj && !j.str("message").empty() ? j.str("message") : "the driver list has an unexpected shape";
        return false;
    }
    for (const J& rel : j.a) {
        if (rel.t != J::Obj) continue;
        const J* assets = rel.get("assets");
        if (!assets || assets->t != J::Arr) continue;
        const std::string title = rel.str("name").empty() ? rel.str("tag_name") : rel.str("name");
        const std::string date = rel.str("published_at").substr(0, 10);
        for (const J& a : assets->a) {
            const std::string file = a.str("name"), url = a.str("browser_download_url");
            const uint64_t size = uint64_t(a.num("size", 0));
            if (file.size() < 5 || file.compare(file.size() - 4, 4, ".zip") != 0 || url.compare(0, 8, "https://") != 0) continue;
            if (size == 0 || size > (96u << 20)) continue;
            Remote r;
            r.id = sanitize_id(file.substr(0, file.size() - 4));
            if (r.id.empty()) continue;
            r.name = file.substr(0, file.size() - 4);
            if (!title.empty() && r.name.find(title) == std::string::npos && title.size() < 40) r.name += "  (" + title + ")";
            r.url = url;
            r.size = size;
            r.published = date;
            out->push_back(std::move(r));
        }
    }
    return true;
}

bool unpack_package(const std::vector<uint8_t>& zip, const std::string& dir, Installed* out, std::string* err) {
    std::string e;
    std::vector<ZipEntry> ents;
    if (!zip_list(zip, &ents, &e)) { *err = e; return false; }
    auto find = [&](const std::string& base) -> const ZipEntry* {
        for (auto& z : ents) if (!z.name.empty() && z.name.back() != '/' && base_name(z.name) == base) return &z;
        return nullptr;
    };
    const ZipEntry* meta = find("meta.json");
    if (!meta) { *err = "not a driver package (no meta.json)"; return false; }
    std::vector<uint8_t> mb;
    if (!zip_read(zip, *meta, &mb, &e)) { *err = e; return false; }
    Installed in;
    if (!parse_meta(std::string(mb.begin(), mb.end()), &in)) { *err = "the driver's meta.json is not valid"; return false; }
    const ZipEntry* lib = find(in.library);
    if (!lib) { *err = "the package has no " + in.library; return false; }
    std::vector<uint8_t> lb;
    if (!zip_read(zip, *lib, &lb, &e)) { *err = e; return false; }
    if (lb.size() < 4 || std::memcmp(lb.data(), "\x7f" "ELF", 4) != 0) { *err = in.library + " is not a shared library"; return false; }
    if (!make_dirs(dir)) { *err = "cannot create " + dir; return false; }
    if (!write_file(dir + "/meta.json", mb.data(), mb.size()) || !write_file(dir + "/" + in.library, lb.data(), lb.size())) {
        *err = "cannot write the driver (disk full?)";
        return false;
    }
    chmod((dir + "/" + in.library).c_str(), 0755);
    if (out) *out = in;
    return true;
}

// ---------------------------------------------------------------- the live list
void refresh() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_busy) return;
        g_busy = true;
    }
    set_status(Status::Busy, "Loading the driver list...");
    std::thread([] {
        std::vector<Remote> all;
        std::string last_err;
        int ok_feeds = 0;
        for (const std::string& repo : feeds()) {
            psp_http::Request rq;
            rq.url = "https://api.github.com/repos/" + repo + "/releases?per_page=30";
            rq.headers = {{"Accept", "application/vnd.github+json"}, {"User-Agent", "psprecomp-dissidia"}};
            rq.max_redirects = 3;
            rq.timeout_ms = 15000;
            psp_http::Response rs = psp_http::perform(rq);
            if (!rs.ok()) { last_err = rs.status ? "the list server answered " + std::to_string(rs.status) : rs.error; continue; }
            std::string e;
            if (!parse_releases(rs.text(), &all, &e)) { last_err = e; continue; }
            ok_feeds++;
        }
        curate(&all, gpu_generation());
        if (all.size() > 40) all.resize(40);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (ok_feeds) { g_catalog = all; g_loaded = true; }
            g_busy = false;
        }
        if (!ok_feeds) set_status(Status::Error, "Could not load the list: " + (last_err.empty() ? std::string("no network") : last_err));
        else set_status(Status::Done, std::to_string(all.size()) + " drivers available");
    }).detach();
}

std::vector<Remote> catalog() { std::lock_guard<std::mutex> lk(g_mu); return g_catalog; }
bool catalog_loaded() { std::lock_guard<std::mutex> lk(g_mu); return g_loaded; }
Status status() { std::lock_guard<std::mutex> lk(g_mu); return g_status; }

void install(const Remote& r) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_busy) return;
        g_busy = true;
    }
    set_status(Status::Busy, "Downloading " + r.name + " (" + size_text(r.size) + ")...");
    std::thread([r] {
        auto finish = [&](Status::State st, const std::string& text) {
            { std::lock_guard<std::mutex> lk(g_mu); g_busy = false; }
            set_status(st, text);
        };
        psp_http::Request rq;
        rq.url = r.url;
        rq.headers = {{"User-Agent", "psprecomp-dissidia"}};
        rq.max_redirects = 6;
        rq.timeout_ms = 30000;
        rq.max_body = size_t(112) << 20;
        psp_http::Response rs = psp_http::perform(rq);
        if (!rs.ok()) { finish(Status::Error, "Download failed: " + (rs.status ? "server answered " + std::to_string(rs.status) : rs.error)); return; }
        set_status(Status::Busy, "Installing " + r.name + "...");
        const std::string root = root_dir(), tmp = root + "/.tmp-" + r.id, fin = root + "/" + r.id;
        remove_tree(tmp);
        Installed in;
        std::string err;
        if (!unpack_package(rs.body, tmp, &in, &err)) { remove_tree(tmp); finish(Status::Error, "Install failed: " + err); return; }
#if defined(__ANDROID__)
        if (in.min_api > android_get_device_api_level()) {
            remove_tree(tmp);
            finish(Status::Error, r.name + " needs Android API " + std::to_string(in.min_api) + " or newer");
            return;
        }
#endif
        remove_tree(fin);
        if (rename(tmp.c_str(), fin.c_str()) != 0) { remove_tree(tmp); finish(Status::Error, "Install failed: cannot move the driver into place"); return; }
        finish(Status::Done, "Installed " + (in.name.empty() ? r.name : in.name));
    }).detach();
}

// ---------------------------------------------------------------- opening Vulkan through a driver (Android)
#if defined(__ANDROID__)
static bool g_custom_active = false;
bool custom_driver_active() { return g_custom_active; }

void* open_vulkan(std::string* err) {
    err->clear();
    const std::string id = selected();
    if (id.empty()) return nullptr;
#if !defined(PSPRECOMP_HAVE_ADRENOTOOLS)
    *err = "this build has no custom driver support";
    return nullptr;
#else
    std::string why;
    if (!supported(&why)) { *err = why; return nullptr; }
    if (crashed(id)) { *err = "driver " + id + " crashed the last time it was tried; pick it again in Settings to retry"; return nullptr; }
    Installed in;
    bool found = false;
    for (auto& d : installed()) if (d.id == id) { in = d; found = true; }
    if (!found) { *err = "driver " + id + " is not installed"; return nullptr; }
    if (in.min_api > android_get_device_api_level()) { *err = in.name + " needs a newer Android"; return nullptr; }

    Dl_info di;
    if (!dladdr(reinterpret_cast<void*>(&open_vulkan), &di) || !di.dli_fname) { *err = "cannot find the app's library folder"; return nullptr; }
    std::string hook = di.dli_fname;
    hook = hook.substr(0, hook.find_last_of('/'));   // nativeLibraryDir: the hook libraries are packed next to libmain.so
    const std::string dir = root_dir() + "/" + id + "/";

    // If the driver takes the process down while Vulkan is brought up, the next start finds this file and skips the driver;
    // note_started() removes it once the device exists.
    make_dirs(root_dir());
    write_file(crash_path(), id.data(), id.size());
    const std::string tmpdir = root_dir() + "/.tmp";
    make_dirs(tmpdir);
    void* h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmpdir.c_str(), (hook + "/").c_str(), dir.c_str(), in.library.c_str(), nullptr, nullptr);
    if (!h) { unlink(crash_path().c_str()); *err = "the system refused to load " + in.name + " (hook libraries missing or driver incompatible)"; return nullptr; }
    void* gpa = dlsym(h, "vkGetInstanceProcAddr");
    if (!gpa) { unlink(crash_path().c_str()); *err = in.name + " has no vkGetInstanceProcAddr"; return nullptr; }
    g_custom_active = true;
    std::fprintf(stderr, "[GPU-DRIVER] using %s (%s)\n", in.name.c_str(), in.library.c_str());
    return gpa;
#endif
}
#endif

}  // namespace psp_gpu
