// "Report an issue": collect, zip, sign, upload (psp_report.h).
#include "psp_report.h"
#include "report_key.h"
#ifndef PSP_REPORT_NO_HTTP
#include "hle/psp_http.h"
#endif

#include <zlib.h>
#if defined(PSPRECOMP_HAVE_VKGL)
#include "vk/vkgl.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#if !defined(_WIN32)
#include <sys/utsname.h>
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifndef PSPRECOMP_GIT_COMMIT
#define PSPRECOMP_GIT_COMMIT "unknown"
#endif

#ifndef PSP_REPORT_STANDALONE
std::string psp_release_version();   // psp_ge_draw.cpp
#endif

namespace fs = std::filesystem;

std::string psp_git_commit() { return PSPRECOMP_GIT_COMMIT; }   // the commit this build was made from (net build id, games/dissidia/runtime/net_packs.cpp)

namespace psp_report {
namespace {

constexpr size_t kZipCap = 24u << 20;        // the whole report
constexpr size_t kLogTail = 8u << 20;        // one log
constexpr size_t kSmallTail = 1u << 20;      // ini / env / crash files
const auto g_t0 = std::chrono::steady_clock::now();

std::mutex g_mu;
Status g_status;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_finished{false};
const uint8_t* g_ram = nullptr;
size_t g_ram_size = 0;
std::string g_gpu_version, g_gpu_renderer, g_gpu_vendor;
std::string (*g_state_hook)() = nullptr;
Product g_product;
bool g_has_product = false;

void set_status(Status::State st, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_status.state = st;
    g_status.text = text;
    g_status.rev++;
}

// ---- SHA-256 / HMAC ----
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t blen = 0;
    uint64_t total = 0;
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) {
        total += n;
        while (n) {
            const size_t k = std::min(n, 64 - blen);
            std::memcpy(buf + blen, p, k);
            blen += k; p += k; n -= k;
            if (blen == 64) { block(buf); blen = 0; }
        }
    }
    void final(uint8_t out[32]) {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (blen != 56) update(&zero, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (56 - 8 * i));
        update(l, 8);
        for (int i = 0; i < 8; i++) { out[i * 4] = uint8_t(h[i] >> 24); out[i * 4 + 1] = uint8_t(h[i] >> 16); out[i * 4 + 2] = uint8_t(h[i] >> 8); out[i * 4 + 3] = uint8_t(h[i]); }
    }
};
std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

// ---- text helpers ----
std::string json_escape(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += char(c);
        }
    }
    return o + "\"";
}
// The string value of "key" in a flat JSON object ("" when absent), plus raw scalars (numbers, true / false).
std::string json_field(const std::string& j, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t at = j.find(pat);
    if (at == std::string::npos) return "";
    at = j.find(':', at + pat.size());
    if (at == std::string::npos) return "";
    ++at;
    while (at < j.size() && (j[at] == ' ' || j[at] == '\n')) ++at;
    if (at >= j.size()) return "";
    std::string v;
    if (j[at] == '"') {
        for (++at; at < j.size() && j[at] != '"'; ++at) {
            if (j[at] == '\\' && at + 1 < j.size()) {
                ++at;
                switch (j[at]) { case 'n': v += '\n'; break; case 't': v += '\t'; break; case 'u': v += '?'; at = std::min(j.size() - 1, at + 4); break; default: v += j[at]; }
            } else v += j[at];
        }
    } else {
        while (at < j.size() && j[at] != ',' && j[at] != '}' && j[at] != ' ' && j[at] != '\n') v += j[at++];
    }
    return v;
}

std::string env_or(const char* n) { const char* e = std::getenv(n); return e ? e : ""; }
std::string home_dir() { std::string h = env_or("HOME"); return h.empty() ? env_or("USERPROFILE") : h; }
std::string user_name() { std::string u = env_or("USER"); return u.empty() ? env_or("USERNAME") : u; }

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
}
std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(uint8_t(c))); return s; }

}  // namespace

std::string scrub(const std::string& text) {
    std::string s = text;
    std::string home = home_dir();
    while (home.size() > 1 && (home.back() == '/' || home.back() == '\\')) home.pop_back();
    if (home.size() > 1) {
        replace_all(s, home, "~");
        std::string fw = home, bw = home;
        std::replace(fw.begin(), fw.end(), '\\', '/');
        std::replace(bw.begin(), bw.end(), '/', '\\');
        replace_all(s, fw, "~");
        replace_all(s, bw, "~");
    }
    const std::string user = user_name();
    if (user.size() >= 3) {   // only as a path component, so a host name that contains it stays readable
        for (const char* sep : {"/", "\\"}) {
            replace_all(s, std::string(sep) + user + sep, std::string(sep) + "~" + sep);
        }
    }
    // credentials: blank what follows a secret-looking name on the same line
    static const char* const kSecret[] = {"token", "secret", "password", "passwd", "authorization", "api_key", "apikey", "refresh"};
    std::string out;
    out.reserve(s.size());
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        const size_t end = nl == std::string::npos ? s.size() : nl;
        std::string line = s.substr(pos, end - pos);
        const std::string ll = lower(line);
        for (const char* k : kSecret) {
            const size_t at = ll.find(k);
            if (at == std::string::npos) continue;
            size_t v = at + std::strlen(k);
            while (v < line.size() && (std::isalnum(uint8_t(line[v])) || line[v] == '_')) ++v;   // e.g. access_token / tokens
            if (v < line.size() && (line[v] == '=' || line[v] == ':' || line[v] == ' ')) { line = line.substr(0, v + 1) + " <removed>"; break; }
        }
        out += line;
        if (nl != std::string::npos) out += '\n';
        pos = end + (nl == std::string::npos ? 0 : 1);
    }
    return out;
}

std::string sha256_hex(const uint8_t* data, size_t n) {
    Sha256 s;
    s.update(data, n);
    uint8_t d[32];
    s.final(d);
    return hex(d, 32);
}
std::string hmac_sha256_hex(const std::string& key, const std::string& msg) {
    uint8_t k[64] = {};
    if (key.size() > 64) { Sha256 s; s.update(reinterpret_cast<const uint8_t*>(key.data()), key.size()); s.final(k); }
    else std::memcpy(k, key.data(), key.size());
    uint8_t ip[64], op[64];
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    uint8_t inner[32], outer[32];
    Sha256 a;
    a.update(ip, 64);
    a.update(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    a.final(inner);
    Sha256 b;
    b.update(op, 64);
    b.update(inner, 32);
    b.final(outer);
    return hex(outer, 32);
}

std::string service_base() {
    const std::string e = env_or("PSPRECOMP_REPORT_URL");
    std::string b = e.empty() ? "https://party.schindlershadow.com/dissidia-report" : e;
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
}

void set_ram(const uint8_t* rdram, size_t size) { g_ram = rdram; g_ram_size = size; }
void note_gpu(const char* version, const char* renderer, const char* vendor) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_gpu_version = version ? version : "";
    g_gpu_renderer = renderer ? renderer : "";
    g_gpu_vendor = vendor ? vendor : "";
}
void set_state_hook(std::string (*fn)()) { g_state_hook = fn; }
void set_product(const Product& p) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_product = p;
    g_has_product = true;
}

namespace {

// ---- files ----
struct Src { std::string rel; std::string path; size_t tail; };

std::string settings_ini_path() {
    const char* p = std::getenv("PSPRECOMP_SETTINGS");
    return (p && p[0]) ? p : "settings.ini";
}
bool is_forbidden(const std::string& rel) {
    const std::string l = lower(rel);
    return l.find("cloud.ini") != std::string::npos || l.rfind("cloud/", 0) == 0 || l.find("/cloud/") != std::string::npos;
}
std::vector<Src> sources() {
    std::vector<Src> v;
    std::error_code ec;
    auto add = [&](const std::string& rel, const std::string& path, size_t tail) {
        if (is_forbidden(rel) || is_forbidden(path)) return;
        if (fs::is_regular_file(fs::u8path(path), ec)) v.push_back({rel, path, tail});
    };
    if (g_has_product) {
        for (const ProductFile& f : g_product.files) add(f.rel, f.path, f.tail);
        return v;
    }
    add("last_run.log", "last_run.log", kLogTail);
    add("prev_run.log", "prev_run.log", kLogTail);   // the run before (kept by the launcher / the Android log pump)
    add("run.log", "run.log", kLogTail);
    add("setup.log", "setup.log", kLogTail);
    add("settings.ini", settings_ini_path(), kSmallTail);
    add("env.txt", "env.txt", kSmallTail);
    for (const char* d : {".", "crash", "crashes"}) {
        for (auto it = fs::directory_iterator(fs::u8path(d), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            const std::string n = it->path().filename().u8string(), l = lower(n);
            const bool crashy = l.rfind("crash", 0) == 0 || l.rfind("hs_err", 0) == 0 || l.rfind("tombstone", 0) == 0 || l.rfind("sigsegv", 0) == 0;
            if (!crashy && std::string(d) == ".") continue;
            if (l.size() > 4 && (l.substr(l.size() - 4) == ".zip" || l.substr(l.size() - 4) == ".dmp")) continue;
            add(std::string(d) == "." ? n : std::string(d) + "/" + n, it->path().u8string(), kSmallTail);
        }
        ec.clear();
    }
    return v;
}
bool read_tail(const std::string& path, size_t tail, std::string* out) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    std::streamoff from = 0;
    if (size > std::streamoff(tail)) from = size - std::streamoff(tail);
    f.seekg(from);
    out->resize(size_t(size - from));
    f.read(&(*out)[0], std::streamsize(out->size()));
    out->resize(size_t(f.gcount()));
    if (from > 0) {   // drop the cut line
        const size_t nl = out->find('\n');
        if (nl != std::string::npos) out->erase(0, nl + 1);
        *out = "[... " + std::to_string(from) + " earlier bytes left out ...]\n" + *out;
    }
    return true;
}

// ---- zip writer (deflate, in memory) ----
struct Zip {
    std::vector<uint8_t> out, cd;
    uint16_t count = 0;
    uint16_t dtime = 0, ddate = 0;
    Zip() {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        dtime = uint16_t(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
        ddate = uint16_t(std::max(0, tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    }
    static void p16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
    static void p32(std::vector<uint8_t>& v, uint32_t x) { p16(v, uint16_t(x)); p16(v, uint16_t(x >> 16)); }
    size_t size_with_cd() const { return out.size() + cd.size() + 22; }
    // Adds one entry; deflates at `level` and stops (false, nothing added) when the zip would pass `limit` bytes.
    bool add(const std::string& name, const uint8_t* data, size_t n, int level, size_t limit) {
        if (count >= 0xFFF0) return false;
        const uint32_t crc = uint32_t(crc32(0, data, uInt(std::min<size_t>(n, 0xFFFFFFFFu))));   // (entries are far below 4 GB)
        std::vector<uint8_t> comp;
        uint16_t method = 8;
        {
            z_stream z{};
            if (deflateInit2(&z, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
            const size_t room = limit > size_with_cd() + 200 + name.size() * 2 ? limit - size_with_cd() - 200 - name.size() * 2 : 0;
            std::vector<uint8_t> chunk(1u << 20);
            size_t off = 0;
            bool ok = true;
            int rc = Z_OK;
            while (rc != Z_STREAM_END) {
                const size_t take = std::min<size_t>(n - off, 4u << 20);
                z.next_in = const_cast<uint8_t*>(data) + off;
                z.avail_in = uInt(take);
                off += take;
                const int flush = off >= n ? Z_FINISH : Z_NO_FLUSH;
                do {
                    z.next_out = chunk.data();
                    z.avail_out = uInt(chunk.size());
                    rc = deflate(&z, flush);
                    if (rc == Z_STREAM_ERROR) { ok = false; break; }
                    comp.insert(comp.end(), chunk.data(), chunk.data() + (chunk.size() - z.avail_out));
                    if (comp.size() > room) { ok = false; break; }
                } while (z.avail_out == 0);
                if (!ok) break;
            }
            deflateEnd(&z);
            if (!ok) return false;
        }
        const uint8_t* body = comp.data();
        size_t blen = comp.size();
        if (blen >= n) { method = 0; body = data; blen = n; }
        if (size_with_cd() + blen + name.size() * 2 + 120 > limit) return false;
        const uint32_t off = uint32_t(out.size());
        p32(out, 0x04034b50); p16(out, 20); p16(out, 0x0800); p16(out, method); p16(out, dtime); p16(out, ddate);
        p32(out, crc); p32(out, uint32_t(blen)); p32(out, uint32_t(n)); p16(out, uint16_t(name.size())); p16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), body, body + blen);
        p32(cd, 0x02014b50); p16(cd, 20); p16(cd, 20); p16(cd, 0x0800); p16(cd, method); p16(cd, dtime); p16(cd, ddate);
        p32(cd, crc); p32(cd, uint32_t(blen)); p32(cd, uint32_t(n)); p16(cd, uint16_t(name.size()));
        p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0); p32(cd, 0); p32(cd, off);
        cd.insert(cd.end(), name.begin(), name.end());
        ++count;
        return true;
    }
    void finish() {
        const uint32_t cd_off = uint32_t(out.size());
        out.insert(out.end(), cd.begin(), cd.end());
        p32(out, 0x06054b50); p16(out, 0); p16(out, 0); p16(out, count); p16(out, count);
        p32(out, uint32_t(cd.size())); p32(out, cd_off); p16(out, 0);
    }
};

std::string os_name() {
#if defined(__ANDROID__)
    return "Android";
#elif defined(_WIN32)
    std::string s = "Windows";
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        if (Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) {
            OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof vi;
            if (fn(&vi) == 0) s += " " + std::to_string(vi.dwMajorVersion) + "." + std::to_string(vi.dwMinorVersion) + " build " + std::to_string(vi.dwBuildNumber);
        }
    }
    return s;
#else
    struct utsname u;
    std::string s = uname(&u) == 0 ? std::string(u.sysname) + " " + u.release + " " + u.machine : "unknown";
    std::ifstream f("/etc/os-release");
    std::string l;
    while (std::getline(f, l)) if (l.rfind("PRETTY_NAME=", 0) == 0) { std::string n = l.substr(12); n.erase(std::remove(n.begin(), n.end(), '"'), n.end()); s = n + " (" + s + ")"; break; }
    return s;
#endif
}
#if defined(__ANDROID__)
std::string prop(const char* k) { char b[PROP_VALUE_MAX] = {}; __system_property_get(k, b); return b; }
#endif
std::string cpu_name() {
    std::string s;
#if defined(_WIN32)
    s = env_or("PROCESSOR_IDENTIFIER");
#else
    std::ifstream f("/proc/cpuinfo");
    std::string l;
    while (s.empty() && std::getline(f, l)) {
        for (const char* k : {"model name", "Hardware"}) if (l.rfind(k, 0) == 0) { const size_t c = l.find(':'); if (c != std::string::npos) s = l.substr(c + 2); }
    }
#endif
#if defined(__ANDROID__)
    if (s.empty() || s.find("AArch64") != std::string::npos) { const std::string soc = prop("ro.soc.model"), plat = prop("ro.board.platform"); s = (soc.empty() ? plat : soc) + " " + s; }
#endif
    return s + " (" + std::to_string(std::thread::hardware_concurrency()) + " threads)";
}
std::string game_id() {
    const char* d = std::getenv("PSPRECOMP_DISC0");
    std::ifstream f(fs::u8path(std::string(d && d[0] ? d : "disc0") + "/PSP_GAME/PARAM.SFO"), std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (size_t i = 0; i + 9 <= b.size(); i++) {
        bool ok = true;
        for (int k = 0; k < 4 && ok; k++) ok = b[i + k] >= 'A' && b[i + k] <= 'Z';
        for (int k = 4; k < 9 && ok; k++) ok = b[i + k] >= '0' && b[i + k] <= '9';
        if (ok) return b.substr(i, 9);
    }
    return "";
}
std::string region_of(const std::string& id) {
    if (id.size() < 4) return "unknown";
    if (id[2] == 'U') return "US";
    if (id[2] == 'E') return "EU";
    if (id[2] == 'J' || id.rfind("NPJH", 0) == 0) return "JP";
    return "other";
}
std::string list_dir_names(const std::string& dir) {
    std::string s;
    std::error_code ec;
    for (auto it = fs::directory_iterator(fs::u8path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        if (!s.empty()) s += ", ";
        s += it->path().filename().u8string();
        if (s.size() > 400) break;
    }
    return s;
}

std::string ram_total() {
#if defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) return std::to_string(m.ullTotalPhys >> 20) + " MB";
#else
    std::ifstream f("/proc/meminfo");
    std::string l;
    while (std::getline(f, l))
        if (l.rfind("MemTotal:", 0) == 0) return std::to_string(std::atoll(l.c_str() + 9) / 1024) + " MB";
#endif
    return "unknown";
}

Pairs installer_info() {
    Pairs v;
    v.push_back({"product", g_product.name});
    v.push_back({"version", g_product.version});
    v.push_back({"os", os_name()});
#if defined(__ANDROID__)
    v.push_back({"device_model", prop("ro.product.manufacturer") + " " + prop("ro.product.model")});
    v.push_back({"android_version", prop("ro.build.version.release")});
    v.push_back({"android_api", prop("ro.build.version.sdk")});
#else
    v.push_back({"device_model", "desktop"});
#endif
    v.push_back({"cpu", cpu_name()});
    v.push_back({"ram_total", ram_total()});
    if (!g_product.space_dir.empty()) {
        std::error_code ec;
        const auto sp = fs::space(fs::u8path(g_product.space_dir), ec);
        v.push_back({"free_space", ec ? std::string("unknown") : std::to_string(sp.available >> 20) + " MB free in " + g_product.space_dir});
    }
    for (const auto& kv : g_product.facts) v.push_back(kv);
    v.push_back({"uptime_s", std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_t0).count())});
    return v;
}

Pairs system_info() {
    if (g_has_product) return installer_info();
#ifdef PSP_REPORT_STANDALONE
    return Pairs();
#else
    Pairs v;
    v.push_back({"version", psp_release_version()});
    v.push_back({"git_commit", PSPRECOMP_GIT_COMMIT});
    v.push_back({"os", os_name()});
#if defined(__ANDROID__)
    v.push_back({"device_model", prop("ro.product.manufacturer") + " " + prop("ro.product.model")});
    v.push_back({"android_version", prop("ro.build.version.release")});
    v.push_back({"android_api", prop("ro.build.version.sdk")});
#else
    v.push_back({"device_model", "desktop"});
#endif
    v.push_back({"cpu", cpu_name()});
    {
        std::lock_guard<std::mutex> lk(g_mu);
#if defined(PSPRECOMP_HAVE_VKGL)
        vkgl::DeviceInfo di;
        if (g_gpu_renderer.empty() && vkgl::device_info(&di)) { g_gpu_renderer = di.name + " (" + di.api + ")"; g_gpu_version = di.driver; }
#endif
        v.push_back({"gpu", g_gpu_renderer + (g_gpu_vendor.empty() ? "" : " (" + g_gpu_vendor + ")")});
        v.push_back({"gpu_driver", g_gpu_version});
    }
    const std::string id = game_id();
    v.push_back({"game_id", id.empty() ? std::string("unknown") : id});
    v.push_back({"game_region", region_of(id)});
    const char* mods = std::getenv("PSPRECOMP_MODS");
    v.push_back({"mods", list_dir_names(mods && mods[0] ? mods : "mods")});
    v.push_back({"texture_packs", list_dir_names(std::string(mods && mods[0] ? mods : "mods") + "/texpacks")});
    v.push_back({"uptime_s", std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_t0).count())});
    return v;
#endif
}

}  // namespace

std::vector<FileInfo> preview(bool include_dump) {
    std::vector<FileInfo> v;
    v.push_back({"report.json", 0});
    std::error_code ec;
    for (const Src& s : sources()) {
        const uintmax_t n = fs::file_size(fs::u8path(s.path), ec);
        v.push_back({s.rel, ec ? 0 : size_t(std::min<uintmax_t>(n, s.tail))});
    }
    if (include_dump && g_ram) {
        size_t end = g_ram_size;
        while (end >= 4096) {
            const uint8_t* p = g_ram + end - 4096;
            bool zero = true;
            for (size_t i = 0; i < 4096 && zero; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); zero = w == 0; }
            if (!zero) break;
            end -= 4096;
        }
        v.push_back({"ram.bin (memory dump, compressed)", end});
    }
    return v;
}

bool build_zip(const Params& p, const Pairs& system, const std::vector<uint8_t>& ram, std::vector<uint8_t>* zip_out, std::string* notes) {
    Zip z;
    std::string note, files_json;
    const size_t reserve = 96 * 1024;   // report.json
    for (const Src& s : sources()) {
        std::string t;
        if (!read_tail(s.path, s.tail, &t)) continue;
        t = scrub(t);
        if (!z.add(s.rel, reinterpret_cast<const uint8_t*>(t.data()), t.size(), 6, kZipCap - reserve)) { note += s.rel + " left out (too big); "; continue; }
        files_json += (files_json.empty() ? "" : ",") + json_escape(s.rel);
    }
    bool dump = false;
    if (p.include_dump) {
        if (ram.empty()) note += "memory dump not available; ";
        else if (z.add("ram.bin", ram.data(), ram.size(), 3, kZipCap - reserve)) { dump = true; files_json += (files_json.empty() ? "" : ",") + json_escape("ram.bin"); }
        else note += "memory dump left out (does not fit in 24 MB); ";
    }
    std::string j = "{\"title\":" + json_escape(scrub(p.title)) + ",\"description\":" + json_escape(scrub(p.description)) + ",\"system\":{";
    bool first = true;
    auto kv = [&](const std::string& k, const std::string& val) { j += (first ? "" : ",") + json_escape(k) + ":" + json_escape(scrub(val)); first = false; };
    for (const auto& s : system) kv(s.first, s.second);
    for (const auto& s : p.extra) kv(s.first, s.second);
    kv("memory_dump", dump ? "included (ram.bin: raw PSP memory from address 0, deflate)" : (p.include_dump ? "left out" : "not requested"));
    j += "},\"files\":[" + files_json + "],\"notes\":" + json_escape(note) + "}";
    if (!z.add("report.json", reinterpret_cast<const uint8_t*>(j.data()), j.size(), 6, kZipCap)) return false;
    z.finish();
    *zip_out = std::move(z.out);
    if (notes) *notes = note;
    return zip_out->size() <= kZipCap;
}

#ifndef PSP_REPORT_NO_HTTP
void upload(const std::vector<uint8_t>& zip, const std::string& base, Status* out) {
    const std::string time = std::to_string(static_cast<long long>(std::time(nullptr)));
    const std::string sig = hmac_sha256_hex(PSPRECOMP_REPORT_KEY, time + "\n" + sha256_hex(zip.data(), zip.size()));
    psp_http::Request rq;
    rq.method = "POST";
    rq.url = base + "/v1/report";
    rq.headers = {{"Content-Type", "application/zip"}, {"X-Report-Time", time}, {"X-Report-Sig", sig}};
    rq.body = zip;
    rq.timeout_ms = 60000;
    const psp_http::Response r = psp_http::perform(rq);
    const std::string body = r.text();
    auto save_local = [&]() {
        std::error_code ec;
        const std::string dir = g_has_product && !g_product.save_dir.empty() ? g_product.save_dir : std::string("reports");
        fs::create_directories(fs::u8path(dir), ec);
        const std::string path = dir + "/report-" + time + ".zip";
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(zip.data()), std::streamsize(zip.size()));
        out->saved_path = f ? fs::absolute(path, ec).u8string() : "";
    };
    if (r.status == 0) {
        out->state = Status::Failed;
        out->text = "Could not reach the report service: " + r.error;
        save_local();
    } else if (r.ok() && json_field(body, "ok") == "true" && !json_field(body, "issue_url").empty()) {
        out->state = Status::Done;
        out->issue_url = json_field(body, "issue_url");
        out->number = std::atoi(json_field(body, "number").c_str());
        out->text = "Issue #" + std::to_string(out->number) + " opened";
    } else {
        out->state = Status::Failed;
        std::string e = json_field(body, "error");
        if (e.empty()) e = "the service answered " + std::to_string(r.status);
        out->text = "The report was not sent: " + e + " (" + std::to_string(r.status) + ")";
        save_local();
    }
    if (out->state == Status::Failed && !out->saved_path.empty()) out->text += ". A copy of the report is saved at " + out->saved_path;
}

#endif  // PSP_REPORT_NO_HTTP

bool build_zip_file(const Params& p, const std::string& path, std::string* error) {
    const Pairs sys = system_info();
    std::vector<uint8_t> zip, none;
    std::string notes;
    if (!build_zip(p, sys, none, &zip, &notes)) { if (error) *error = "could not build the report"; return false; }
    std::ofstream f(fs::u8path(path), std::ios::binary);
    f.write(reinterpret_cast<const char*>(zip.data()), std::streamsize(zip.size()));
    if (!f) { if (error) *error = "could not write " + path; return false; }
    return true;
}

#ifndef PSP_REPORT_NO_HTTP
bool start(const Params& p) {
    bool exp = false;
    if (!g_busy.compare_exchange_strong(exp, true)) return false;
    set_status(Status::Working, "Collecting the report...");
    Pairs sys = system_info();
    if (g_state_hook) {
        const std::string s = g_state_hook();
        size_t pos = 0;
        while (pos < s.size()) {
            size_t nl = s.find('\n', pos);
            if (nl == std::string::npos) nl = s.size();
            const std::string line = s.substr(pos, nl - pos);
            const size_t eq = line.find('=');
            if (eq != std::string::npos) sys.push_back({line.substr(0, eq), line.substr(eq + 1)});
            pos = nl + 1;
        }
    }
    std::vector<uint8_t> ram;
    if (p.include_dump && g_ram) {   // the game is paused while the menu is open: a consistent copy of the used part
        size_t end = 0;
        for (const FileInfo& f : preview(true)) if (f.name.rfind("ram.bin", 0) == 0) end = f.size;
        ram.assign(g_ram, g_ram + end);
    }
    Params pc = p;
    std::thread([pc, sys, ram = std::move(ram)]() mutable {
        std::vector<uint8_t> zip;
        std::string notes;
        Status res;
        set_status(Status::Working, "Compressing the report...");
        if (!build_zip(pc, sys, ram, &zip, &notes)) {
            set_status(Status::Failed, "Could not build the report");
        } else {
            ram.clear();
            ram.shrink_to_fit();
            char b[96];
            std::snprintf(b, sizeof b, "Sending %.1f MB...", double(zip.size()) / (1024.0 * 1024.0));
            set_status(Status::Working, b);
            upload(zip, service_base(), &res);
            std::lock_guard<std::mutex> lk(g_mu);
            res.rev = g_status.rev + 1;
            g_status = res;
        }
        std::fprintf(stderr, "[REPORT] %s\n", status().text.c_str());
        g_finished = true;
        g_busy = false;
    }).detach();
    return true;
}

#endif  // PSP_REPORT_NO_HTTP

Status status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}
bool take_finished() { return g_finished.exchange(false); }

}  // namespace psp_report
