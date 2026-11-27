// adhoc_relay — UDP relay for the native build's ad-hoc multiplayer (see
// runtime/src/hle/psp_hle_adhoc.cpp and games/dissidia/docs/multiplayer.md).
//
// Players that cannot reach each other directly (NAT, no port forwarding) send everything to this
// server instead. A client registers into a *room* with a cookie handshake; the server then
//   * copies a client's beacon to every other client in its room, and
//   * copies PDP / matching / PTP packets to the client whose MAC is the packet's destination,
//     or to everyone else in the room when the destination is the broadcast MAC.
// Datagrams are forwarded untouched. The server never looks inside a game message.
//
// Wire format (shared with the client): "DSAH", type(1), source MAC(6), body length(2, LE), body.
// Types 1-4 and 6 are the client-to-client packets (6 = a fragment of a large message: the client splits what
// does not fit one datagram, because IP fragments are dropped by many routers; the destination MAC leads its body
// like a PDP packet); type 5 is client <-> server:
//   client -> server  op 1 HELLO: room[32], cookie(8)      (cookie 0 asks for one)
//                     op 4 BYE
//                     op 5 INTRO: target MAC(6), the client's own LAN address(4) and port(2, LE)   (version 2)
//   server -> client  op 2 CHALLENGE: cookie(8)
//                     op 3 OK: players in the room(2, LE), version(1)
//                     op 6 PEER: MAC(6), its public address(4) + port(2, LE), its LAN address(4) + port(2, LE)   (version 2)
// INTRO asks to be introduced to another player in the same room, for a direct path (UDP hole punching): both get a PEER with
// the other's public address (what this server sees) and the LAN address it reported, then send to each other directly and
// keep the server for broadcasts and as the fallback. Addresses go out only between two registered players of one room.
// The cookie is SipHash(secret, client address, 30 s time bucket). A spoofed source address can
// not complete the handshake, so the server cannot be used to flood a third party.
//
// Session log (27 Nov): one JSON line per event in --log-dir (default ~/.local/state/adhoc_relay/, relay-YYYY-MM-DD.jsonl, rotating
// daily in UTC, 90 days kept, dir 0700 / files 0600), written by a background thread so the packet path only formats a line and
// queues it. Read it with adhoc_relay_stats (README.md has the line formats). The relay still never looks into game messages;
// it only reads what it needs for statistics: the beacon header (nickname, group), the build id in two side-channel messages, and
// the destination port of a PDP packet (to tell battle traffic from pings and pack transfers).
//
// Build: g++ -O2 -std=c++17 -pthread -o adhoc_relay adhoc_relay.cpp
// Run:   ./adhoc_relay [--port 27500] [--bind 0.0.0.0] [--max-room 32] [--max-clients 2000]
//                      [--pps 6000] [--kbps 32000] [--log-dir DIR | --no-log] [--keep-days 90] [--rate-secs 30] [--verbose]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using Mac = std::array<uint8_t, 6>;
constexpr size_t HDR = 13;
constexpr uint8_t T_BEACON = 1, T_PDP = 2, T_MATCH = 3, T_PTP = 4, T_RELAY = 5, T_FRAG = 6;
constexpr uint8_t OP_HELLO = 1, OP_CHALLENGE = 2, OP_OK = 3, OP_BYE = 4, OP_INTRO = 5, OP_PEER = 6;
constexpr uint8_t VERSION = 2;   // 2: INTRO / PEER (direct paths)
constexpr size_t MAX_DATAGRAM = 16384;
constexpr int64_t CLIENT_TIMEOUT_US = 8000000;
constexpr int64_t COOKIE_BUCKET_US = 30000000;

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- SipHash-2-4 (cookie) ----
inline uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
uint64_t siphash24(const uint8_t key[16], const uint8_t* in, size_t n) {
    uint64_t k0, k1;
    std::memcpy(&k0, key, 8); std::memcpy(&k1, key + 8, 8);
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
    auto round = [&] {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t m; std::memcpy(&m, in + i, 8);
        v3 ^= m; round(); round(); v0 ^= m;
    }
    uint64_t b = (uint64_t)n << 56;
    for (size_t j = 0; i + j < n; j++) b |= (uint64_t)in[i + j] << (8 * j);
    v3 ^= b; round(); round(); v0 ^= b;
    v2 ^= 0xff; round(); round(); round(); round();
    return v0 ^ v1 ^ v2 ^ v3;
}

uint8_t g_key[16];
uint64_t cookie_for(const sockaddr_in& a, int64_t bucket) {
    uint8_t m[16];
    std::memcpy(m, &a.sin_addr.s_addr, 4);
    std::memcpy(m + 4, &a.sin_port, 2);
    std::memset(m + 6, 0, 2);
    std::memcpy(m + 8, &bucket, 8);
    return siphash24(g_key, m, sizeof m);
}
bool cookie_ok(const sockaddr_in& a, uint64_t c) {
    const int64_t b = now_us() / COOKIE_BUCKET_US;
    return c != 0 && (c == cookie_for(a, b) || c == cookie_for(a, b - 1));
}

uint64_t addr_key(const sockaddr_in& a) { return ((uint64_t)ntohl(a.sin_addr.s_addr) << 16) | ntohs(a.sin_port); }
std::string addr_str(const sockaddr_in& a) {
    char b[64];
    std::snprintf(b, sizeof b, "%s:%u", inet_ntoa(a.sin_addr), ntohs(a.sin_port));
    return b;
}
std::string mac_str(const Mac& m) {
    char b[24];
    std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

// ---- configuration ----
struct Config {
    uint16_t port = 27500;
    std::string bind = "0.0.0.0";
    size_t max_room = 32, max_clients = 2000;
    double pps = 6000;          // sustained packets / s per client
    double kbps = 32000;        // sustained kilobit / s per client
    bool verbose = false;
    std::string log_dir;        // "" = default; logging is off with no_log
    bool no_log = false;
    int keep_days = 90;
    int rate_secs = 30;         // interval of the per-client "rate" and per-IP "error" events
} cfg;

// ---- JSON lines log ----
// The packet thread formats a line (J) and pushes it to a queue; the writer thread appends batches to the day's file every ~0.5 s.
double wall_now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string day_of(double t) {
    time_t s = (time_t)t; tm g; gmtime_r(&s, &g);
    char b[48]; std::snprintf(b, sizeof b, "%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    return b;
}
std::string iso_of(double t) {
    time_t s = (time_t)t; tm g; gmtime_r(&s, &g);
    char b[96]; std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour,
                              g.tm_min, g.tm_sec, (int)((t - (double)s) * 1000));
    return b;
}

class Logger {
public:
    bool on = false;
    uint64_t lost = 0;   // lines dropped because the queue was full
    bool start(const std::string& dir, int keep_days) {
        dir_ = dir; keep_ = keep_days;
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        if (chmod(dir_.c_str(), 0700) != 0) { std::perror("log dir chmod"); }
        umask(077);
        if (!open_day(day_of(wall_now()))) return false;
        on = true;
        th_ = std::thread([this] { run(); });
        return true;
    }
    void push(std::string&& line) {
        if (!on) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (q_.size() >= 200000) { lost++; return; }
        q_.push_back(std::move(line));
    }
    void stop() {
        if (!on) return;
        { std::lock_guard<std::mutex> lk(mu_); stop_ = true; }
        cv_.notify_all();
        th_.join();
        on = false;
        if (f_) { std::fclose(f_); f_ = nullptr; }
    }
private:
    std::string dir_, day_;
    int keep_ = 90;
    FILE* f_ = nullptr;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> q_;
    bool stop_ = false;
    std::thread th_;
    bool open_day(const std::string& day) {
        if (f_) { std::fclose(f_); f_ = nullptr; }
        const std::string path = dir_ + "/relay-" + day + ".jsonl";
        f_ = std::fopen(path.c_str(), "ae");
        if (!f_) { std::fprintf(stderr, "cannot open %s: %s\n", path.c_str(), std::strerror(errno)); return false; }
        chmod(path.c_str(), 0600);
        day_ = day;
        prune();
        return true;
    }
    void prune() {   // delete day files older than keep_ days (by the date in the name)
        std::error_code ec;
        const time_t limit = time(nullptr) - (time_t)keep_ * 86400;
        for (auto& e : std::filesystem::directory_iterator(dir_, ec)) {
            const std::string n = e.path().filename().string();
            int y, m, d;
            if (n.size() != 22 || std::sscanf(n.c_str(), "relay-%4d-%2d-%2d.jsonl", &y, &m, &d) != 3) continue;
            tm g{}; g.tm_year = y - 1900; g.tm_mon = m - 1; g.tm_mday = d;
            if (timegm(&g) < limit) std::filesystem::remove(e.path(), ec);
        }
    }
    void run() {
        std::deque<std::string> batch;
        for (;;) {
            bool done;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait_for(lk, std::chrono::milliseconds(500), [this] { return stop_; });
                batch.swap(q_);
                done = stop_;
            }
            if (!batch.empty()) {
                const std::string day = day_of(wall_now());
                if (day != day_) open_day(day);
                if (f_) {
                    for (auto& l : batch) std::fwrite(l.data(), 1, l.size(), f_);
                    std::fflush(f_);
                }
                batch.clear();
            }
            if (done) return;
        }
    }
} g_log;

std::string json_str(const std::string& v) {   // JSON string body: printable ASCII and valid UTF-8 kept, the rest escaped or replaced
    std::string o;
    o.reserve(v.size() + 2);
    for (size_t i = 0; i < v.size(); i++) {
        const unsigned char ch = (unsigned char)v[i];
        if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
        else if (ch < 0x20 || ch == 0x7f) o += '?';
        else if (ch < 0x80) o += (char)ch;
        else {
            const int len = ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : ch >= 0xC0 ? 2 : 0;
            bool ok = len && i + len <= v.size();
            for (int k = 1; ok && k < len; k++) ok = ((unsigned char)v[i + k] & 0xC0) == 0x80;
            if (ok) { o.append(v, i, len); i += len - 1; } else o += '?';
        }
    }
    return o;
}

struct J {
    std::string s;
    explicit J(const char* ev) {
        const double t = wall_now();
        char b[32]; std::snprintf(b, sizeof b, "%.3f", t);
        s = "{\"ts\":\"" + iso_of(t) + "\",\"t\":" + b + ",\"ev\":\"" + ev + "\"";
    }
    J& str(const char* k, const std::string& v) { s += ",\""; s += k; s += "\":\""; s += json_str(v); s += '"'; return *this; }
    J& num(const char* k, double v) {
        char b[40];
        if (v == (double)(int64_t)v && v < 1e15 && v > -1e15) std::snprintf(b, sizeof b, "%lld", (long long)v);
        else std::snprintf(b, sizeof b, "%.3f", v);
        s += ",\""; s += k; s += "\":"; s += b; return *this;
    }
    J& u(const char* k, uint64_t v) { s += ",\""; s += k; s += "\":"; s += std::to_string(v); return *this; }
    J& boo(const char* k, bool v) { s += ",\""; s += k; s += "\":"; s += v ? "true" : "false"; return *this; }
    J& raw(const char* k, const std::string& v) { s += ",\""; s += k; s += "\":"; s += v; return *this; }
    void emit() { if (!g_log.on) return; s += "}\n"; g_log.push(std::move(s)); }
};

// ---- classification ----
// What the relay can tell about the network a client is on, from its public address alone.
const char* nat_class(uint32_t ip, const char** range) {
    const unsigned a = ip >> 24, b = (ip >> 16) & 255;
    *range = "";
    if (a == 127) return "loopback";
    if (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168)) return "private";
    if (a == 169 && b == 254) return "linklocal";
    if (a == 100 && b >= 64 && b <= 127) { *range = "100.64.0.0/10"; return "cgnat"; }
    if (a == 172 && b >= 56 && b <= 63) { *range = "172.56.0.0/13"; return "cgnat"; }   // mobile carrier NAT (no port forwarding, usually symmetric)
    return "public";
}

// ---- state ----
using Pairs = std::map<Mac, int>;
struct RttStat {
    uint32_t n = 0; double sum = 0, mn = 1e9, mx = 0;
    void add(double v) { n++; sum += v; mn = std::min(mn, v); mx = std::max(mx, v); }
    void clear() { *this = RttStat(); }
    void json(J& j) const { j.u("rtt_n", n); if (n) { j.num("rtt_min", mn).num("rtt_avg", sum / n).num("rtt_max", mx); } }
};
struct Sess;
struct Client {
    uint32_t id = 0;
    sockaddr_in addr{};
    std::string ip;         // "a.b.c.d"
    std::string ipport;     // "a.b.c.d:port"
    const char* nat = "";
    Mac mac{};
    std::string room;
    int64_t last_seen = 0, joined_us = 0;
    // token buckets
    double pkt_tokens = 0, byte_tokens = 0;
    int64_t refill_at = 0;
    uint64_t rx = 0, fwd = 0, dropped = 0;       // packets from the client, copies sent for it, rate-limited
    uint64_t rx_bytes = 0, tx_pkts = 0, tx_bytes = 0, game_pkts = 0, game_bytes = 0;
    uint8_t lan[6] = {0};   // the LAN address and port it reported in its last INTRO (for two players behind one router)
    int64_t intro_at = 0;   // last INTRO handled (at most a few a second)
    bool intro_seen = false;  // sent an INTRO: a version 2 client
    // identity, from the beacon and the side-channel build messages
    std::string nick, group, build, game_hex;
    int proto = -1;
    int connected = 0, fps = 0;
    int64_t last_beacon_us = 0;
    bool ident_logged = false;
    // RTT probe
    int64_t last_probe = 0, probe_at = 0;
    RttStat rtt, rtt_w;     // whole stay, this rate window
    // accounting snapshots
    uint64_t a_rxb = 0, a_rx = 0, a_txb = 0, a_game = 0;     // at the last per-second accounting
    uint64_t w_rx = 0, w_rxb = 0, w_txp = 0, w_txb = 0, w_drop = 0, w_game = 0;   // at the last rate event
    uint64_t peak_pps_w = 0, peak_bps_w = 0, peak_bps = 0;
    Sess* sess = nullptr;
    int64_t sess_since = 0;
};

struct Member {
    std::string nick, ip;
    uint64_t rx_pkts = 0, rx_bytes = 0, tx_pkts = 0, tx_bytes = 0, game_pkts = 0, peak_bps = 0;
    RttStat rtt;
};
struct Sess {   // a group of >= 2 connected players of one group name in one room
    std::string room, group;
    int64_t start_us = 0;
    std::map<Mac, Member> members;
    std::set<Mac> now;
    uint64_t rx_bytes = 0, game_pkts = 0, peak_bps = 0, peak_pps = 0, sec_rxb = 0, sec_game = 0, sec_pkts = 0;
    size_t max_players = 0;
    // battle: relayed game traffic (>= 10 packets/s for 2 s ... silent for 5 s)
    bool battle = false;
    int act = 0, idle = 0;
    int64_t b_start = 0, b_last = 0;
    uint64_t b_bytes = 0, b_game = 0, b_peak_bps = 0, b_peak_pps = 0;
    uint32_t battles = 0;
    uint64_t battle_s = 0;
};
struct Pair {
    Client *a = nullptr, *b = nullptr;
    int64_t intro_at = 0, first_intro = 0, last_uni = 0, intro_logged = 0;
    uint32_t intros = 0;
    uint64_t uni = 0;
    int state = 0;   // 0 unknown, 1 relay, 2 direct
    std::string reason;
    int64_t state_at = 0;
    uint32_t flaps = 0;
};

// per source IP: what a client that never registers (or misbehaves) does. Counted only off the normal path, flushed with the rate events.
enum { E_MALFORMED, E_UNREG, E_SPOOF, E_CHALLENGE, E_ROOMFULL, E_SERVERFULL, E_BADROOM, E_BADTYPE, E_BADOP, E_BADINTRO, E_COUNT };
const char* const E_NAME[E_COUNT] = {"malformed", "unregistered", "spoofed_mac", "challenge", "room_full", "server_full", "bad_room", "bad_type", "bad_op", "bad_intro"};
struct IpStat {
    uint64_t pkts = 0, bytes = 0;
    uint32_t err[E_COUNT] = {0};
    uint32_t joins = 0, leaves = 0, reregs = 0;
    uint16_t port = 0;
    std::set<std::string> rooms;
    std::string last_room;
};

std::unordered_map<uint64_t, std::unique_ptr<Client>> g_clients;                 // by address
std::unordered_map<std::string, std::map<Mac, Client*>> g_rooms;                 // room -> MAC -> client
std::map<std::string, Sess> g_sess;                                              // room + '\x01' + group
std::unordered_map<uint64_t, Pair> g_pairs;                                      // (id lo << 32 | id hi)
std::unordered_map<uint32_t, IpStat> g_ips;
uint32_t g_next_id = 1;
int g_sock = -1;
volatile sig_atomic_t g_stop = 0;
uint64_t g_total_rx = 0, g_total_fwd = 0, g_total_drop = 0;

void send_to(const sockaddr_in& to, const uint8_t* d, size_t n) {
    (void)!sendto(g_sock, d, n, 0, (const sockaddr*)&to, sizeof to);
}

void send_relay(const sockaddr_in& to, uint8_t op, const uint8_t* payload, size_t n) {
    uint8_t p[HDR + 1 + 32];
    std::memcpy(p, "DSAH", 4);
    p[4] = T_RELAY;
    std::memset(p + 5, 0, 6);
    uint16_t bl = (uint16_t)(1 + n);
    std::memcpy(p + 11, &bl, 2);
    p[HDR] = op;
    if (n) std::memcpy(p + HDR + 1, payload, n);
    send_to(to, p, HDR + 1 + n);
}

void ip_err(const sockaddr_in& from, int kind, size_t bytes, const std::string& room = "") {
    const uint32_t ip = ntohl(from.sin_addr.s_addr);
    auto it = g_ips.find(ip);
    if (it == g_ips.end()) {
        if (g_ips.size() >= 50000) return;
        it = g_ips.emplace(ip, IpStat()).first;
    }
    IpStat& s = it->second;
    s.pkts++; s.bytes += bytes; s.err[kind]++; s.port = ntohs(from.sin_port);
    if (!room.empty()) s.last_room = room;
}
IpStat* ip_stat(const sockaddr_in& from) {
    const uint32_t ip = ntohl(from.sin_addr.s_addr);
    auto it = g_ips.find(ip);
    if (it == g_ips.end()) {
        if (g_ips.size() >= 50000) return nullptr;
        it = g_ips.emplace(ip, IpStat()).first;
    }
    it->second.port = ntohs(from.sin_port);
    return &it->second;
}

uint64_t pair_key(uint32_t x, uint32_t y) { return x < y ? ((uint64_t)x << 32) | y : ((uint64_t)y << 32) | x; }

std::string players_json(const std::map<Mac, Member>& ms, const std::set<Mac>* only) {
    std::string o = "[";
    bool first = true;
    for (auto& [m, mem] : ms) {
        if (only && !only->count(m)) continue;
        if (!first) o += ',';
        first = false;
        o += "{\"mac\":\"" + mac_str(m) + "\",\"nick\":\"" + json_str(mem.nick) + "\"}";
    }
    return o + "]";
}

// Fold the traffic since the last accounting into the player's match totals and the session's current second.
void account(Client& c) {
    const uint64_t drb = c.rx_bytes - c.a_rxb, dr = c.rx - c.a_rx, dtb = c.tx_bytes - c.a_txb, dg = c.game_pkts - c.a_game;
    c.a_rxb = c.rx_bytes; c.a_rx = c.rx; c.a_txb = c.tx_bytes; c.a_game = c.game_pkts;
    c.peak_pps_w = std::max(c.peak_pps_w, dr);
    c.peak_bps_w = std::max(c.peak_bps_w, drb * 8);
    c.peak_bps = std::max(c.peak_bps, drb * 8);
    if (Sess* s = c.sess) {
        Member& m = s->members[c.mac];
        m.rx_pkts += dr; m.rx_bytes += drb; m.tx_bytes += dtb; m.game_pkts += dg;
        m.peak_bps = std::max(m.peak_bps, drb * 8);
        s->sec_rxb += drb; s->sec_game += dg; s->sec_pkts += dr;
        s->rx_bytes += drb; s->game_pkts += dg;
    }
}

void emit_rate(Client& c, double secs) {
    J j("rate");
    j.str("room", c.room).str("mac", mac_str(c.mac)).str("ip", c.ip).u("port", ntohs(c.addr.sin_port)).num("secs", secs)
        .u("pkts", c.rx - c.w_rx).u("bytes", c.rx_bytes - c.w_rxb).u("tx_pkts", c.tx_pkts - c.w_txp).u("tx_bytes", c.tx_bytes - c.w_txb)
        .u("dropped", c.dropped - c.w_drop).u("game_pkts", c.game_pkts - c.w_game).u("peak_pps", c.peak_pps_w).u("peak_bps", c.peak_bps_w);
    c.rtt_w.json(j);
    j.emit();
    c.w_rx = c.rx; c.w_rxb = c.rx_bytes; c.w_txp = c.tx_pkts; c.w_txb = c.tx_bytes; c.w_drop = c.dropped; c.w_game = c.game_pkts;
    c.peak_pps_w = c.peak_bps_w = 0;
    c.rtt_w.clear();
}

void drop_client(uint64_t key, const char* why) {
    auto it = g_clients.find(key);
    if (it == g_clients.end()) return;
    Client* c = it->second.get();
    account(*c);
    auto rit = g_rooms.find(c->room);
    if (rit != g_rooms.end()) {
        auto mit = rit->second.find(c->mac);
        if (mit != rit->second.end() && mit->second == c) rit->second.erase(mit);
        for (auto& [m, o] : rit->second) g_pairs.erase(pair_key(c->id, o->id));
        if (rit->second.empty()) g_rooms.erase(rit);
    }
    std::printf("- %s %s left room \"%s\" (%s; rx %llu fwd %llu drop %llu)\n", addr_str(c->addr).c_str(),
                mac_str(c->mac).c_str(), c->room.c_str(), why, (unsigned long long)c->rx, (unsigned long long)c->fwd,
                (unsigned long long)c->dropped);
    if (g_log.on) {
        emit_rate(*c, 0);   // the traffic of the last partial window
        J j("leave");
        j.str("room", c->room).str("mac", mac_str(c->mac)).str("nick", c->nick).str("ip", c->ip).u("port", ntohs(c->addr.sin_port))
            .str("reason", why).num("dur_s", (now_us() - c->joined_us) / 1e6).u("rx_pkts", c->rx).u("rx_bytes", c->rx_bytes)
            .u("fwd_pkts", c->fwd).u("tx_pkts", c->tx_pkts).u("tx_bytes", c->tx_bytes).u("dropped", c->dropped).u("game_pkts", c->game_pkts)
            .u("peak_bps", c->peak_bps).num("client_v", c->intro_seen ? 2 : 1).str("build", c->build);
        c->rtt.json(j);
        j.emit();
    }
    if (IpStat* s = ip_stat(c->addr)) s->leaves++;
    if (c->sess) c->sess->now.erase(c->mac);
    g_clients.erase(it);
}

bool valid_room(const std::string& r) {
    if (r.size() > 31) return false;
    for (unsigned char ch : r) if (ch < 0x20 || ch > 0x7e) return false;
    return true;
}

void handle_hello(const sockaddr_in& from, const Mac& mac, const uint8_t* body, size_t len) {
    if (len < 1 + 32 + 8) { ip_err(from, E_MALFORMED, len); return; }
    char room_raw[33] = {0};
    std::memcpy(room_raw, body + 1, 32);
    std::string room(room_raw);
    uint64_t cookie; std::memcpy(&cookie, body + 33, 8);
    if (!valid_room(room)) { ip_err(from, E_BADROOM, len); return; }
    if (!cookie_ok(from, cookie)) {
        uint64_t c = cookie_for(from, now_us() / COOKIE_BUCKET_US);
        send_relay(from, OP_CHALLENGE, (const uint8_t*)&c, 8);
        ip_err(from, E_CHALLENGE, len, room);
        return;
    }
    const uint64_t key = addr_key(from);
    const int64_t t = now_us();
    auto it = g_clients.find(key);
    if (it != g_clients.end()) {
        Client* c = it->second.get();
        if (c->mac == mac && c->room == room) {   // keep-alive
            c->last_seen = t;
            if (c->probe_at) {   // answer to our RTT probe (an unsolicited CHALLENGE makes the client send its HELLO at once)
                if (t - c->probe_at < 900000) { const double ms = (t - c->probe_at) / 1000.0; c->rtt.add(ms); c->rtt_w.add(ms); }
                c->probe_at = 0;
            }
        } else {
            if (IpStat* s = ip_stat(from)) s->reregs++;
            drop_client(key, "re-registered");
            it = g_clients.end();
        }
    }
    if (it == g_clients.end()) {
        if (g_clients.size() >= cfg.max_clients) { ip_err(from, E_SERVERFULL, len, room); return; }
        auto& members = g_rooms[room];
        auto old = members.find(mac);
        if (old != members.end()) drop_client(addr_key(old->second->addr), "same player reconnected");
        auto& members2 = g_rooms[room];
        if (members2.size() >= cfg.max_room) {
            if (members2.empty()) g_rooms.erase(room);
            ip_err(from, E_ROOMFULL, len, room);
            return;   // room full: stay silent, the client keeps retrying and shows "no answer"
        }
        auto c = std::make_unique<Client>();
        c->id = g_next_id++;
        c->addr = from; c->mac = mac; c->room = room; c->last_seen = t; c->joined_us = t;
        c->ip = inet_ntoa(from.sin_addr);
        c->ipport = addr_str(from);
        const char* range; c->nat = nat_class(ntohl(from.sin_addr.s_addr), &range);
        c->pkt_tokens = cfg.pps * 2; c->byte_tokens = cfg.kbps * 125 * 2; c->refill_at = t;
        c->last_probe = t;
        members2[mac] = c.get();
        std::printf("+ %s %s joined room \"%s\" (%zu in room, %zu total)\n", addr_str(from).c_str(),
                    mac_str(mac).c_str(), room.c_str(), members2.size(), g_clients.size() + 1);
        if (g_log.on) {
            J j("join");
            j.str("room", room).str("mac", mac_str(mac)).str("ip", c->ip).u("port", ntohs(from.sin_port)).str("nat", c->nat)
                .u("in_room", members2.size()).u("total", g_clients.size() + 1);
            if (*range) j.str("nat_range", range);
            j.emit();
        }
        if (IpStat* s = ip_stat(from)) { s->joins++; if (s->rooms.size() < 32) s->rooms.insert(room); s->last_room = room; }
        g_clients[key] = std::move(c);
    }
    Client* c = g_clients[key].get();
    uint8_t ok[3];
    uint16_t n = (uint16_t)g_rooms[c->room].size();
    std::memcpy(ok, &n, 2);
    ok[2] = VERSION;
    send_relay(from, OP_OK, ok, 3);
    if (!c->probe_at && t - c->last_probe >= 5000000) {   // RTT probe: a CHALLENGE with the current cookie, sent right after a keep-alive so the
        const uint64_t ck = cookie_for(from, t / COOKIE_BUCKET_US);   // client's own next HELLO (1 s away) cannot be mistaken for the answer
        send_relay(from, OP_CHALLENGE, (const uint8_t*)&ck, 8);
        c->probe_at = c->last_probe = t;
    }
}

// INTRO: introduce `from` and the target to each other (both get the other's public and LAN address).
void handle_intro(const sockaddr_in& from, const Mac& mac, const uint8_t* body, size_t len) {
    if (len < 1 + 6 + 6) { ip_err(from, E_BADINTRO, len); return; }
    auto it = g_clients.find(addr_key(from));
    if (it == g_clients.end() || it->second->mac != mac) { ip_err(from, E_UNREG, len); return; }
    Client& c = *it->second;
    const int64_t t = now_us();
    if (t - c.intro_at < 250000) return;   // a few a second at most
    c.intro_at = t;
    c.last_seen = t;
    if (!c.intro_seen) {
        c.intro_seen = true;
        if (g_log.on) {
            char lan[40]; in_addr la; std::memcpy(&la.s_addr, body + 7, 4);
            uint16_t lp; std::memcpy(&lp, body + 11, 2);
            std::snprintf(lan, sizeof lan, "%s:%u", inet_ntoa(la), lp);
            const uint32_t lan_ip = ntohl(la.s_addr), pub_ip = ntohl(c.addr.sin_addr.s_addr);
            const char* mapping = lan_ip == pub_ip ? "no_nat" : lp == ntohs(c.addr.sin_port) ? "port_preserved" : "port_remapped";
            J("client").str("room", c.room).str("mac", mac_str(c.mac)).str("ip", c.ip).u("port", ntohs(c.addr.sin_port)).num("client_v", 2)
                .str("lan", lan).str("nat", c.nat).str("mapping", mapping).emit();
        }
    }
    std::memcpy(c.lan, body + 7, 6);
    Mac target; std::memcpy(target.data(), body + 1, 6);
    auto rit = g_rooms.find(c.room);
    if (rit == g_rooms.end()) return;
    auto mit = rit->second.find(target);
    if (mit == rit->second.end() || mit->second == &c) { ip_err(from, E_BADINTRO, len, c.room); return; }
    Client& o = *mit->second;
    auto peer_of = [](const Client& p, uint8_t out[18]) {
        std::memcpy(out, p.mac.data(), 6);
        std::memcpy(out + 6, &p.addr.sin_addr.s_addr, 4);
        const uint16_t port = ntohs(p.addr.sin_port);
        std::memcpy(out + 10, &port, 2);
        std::memcpy(out + 12, p.lan, 6);
    };
    uint8_t a[18], b[18];
    peer_of(o, a);
    peer_of(c, b);
    send_relay(c.addr, OP_PEER, a, 18);
    send_relay(o.addr, OP_PEER, b, 18);
    if (cfg.verbose) std::printf("  intro %s <-> %s\n", mac_str(c.mac).c_str(), mac_str(o.mac).c_str());
    Pair& p = g_pairs[pair_key(c.id, o.id)];
    p.a = &c; p.b = &o;
    if (!p.first_intro) p.first_intro = t;
    p.intro_at = t; p.intros++;
    if (g_log.on && t - p.intro_logged > 10000000) {   // the clients repeat INTRO until the path is up: one line per pair every 10 s
        p.intro_logged = t;
        J("intro").str("room", c.room).str("from", mac_str(c.mac)).str("from_ip", c.ipport).str("from_nat", c.nat)
            .str("to", mac_str(o.mac)).str("to_ip", o.ipport).str("to_nat", o.nat).u("n", p.intros).emit();
    }
}

bool allow(Client& c, size_t bytes) {
    const int64_t t = now_us();
    const double dt = (t - c.refill_at) / 1e6;
    c.refill_at = t;
    c.pkt_tokens = std::min(cfg.pps * 2, c.pkt_tokens + dt * cfg.pps);
    c.byte_tokens = std::min(cfg.kbps * 125 * 2, c.byte_tokens + dt * cfg.kbps * 125);
    if (c.pkt_tokens < 1 || c.byte_tokens < (double)bytes) return false;
    c.pkt_tokens -= 1; c.byte_tokens -= (double)bytes;
    return true;
}

// ---- reading just enough of a packet for the log ----
void ident_client(Client& c) {
    if (c.ident_logged) return;
    c.ident_logged = true;
    J j("ident");
    j.str("room", c.room).str("mac", mac_str(c.mac)).str("nick", c.nick).str("group", c.group).u("fps", (uint64_t)c.fps).str("game_data", c.game_hex);
    j.emit();
}
void note_build(Client& c, int proto, const std::string& commit) {
    std::string b;
    for (char ch : commit) b += (ch >= 32 && ch <= 126) ? ch : '?';
    if (c.build == b && c.proto == proto) return;
    c.build = b; c.proto = proto;
    J("build").str("room", c.room).str("mac", mac_str(c.mac)).str("nick", c.nick).str("build", b).num("proto", proto).emit();
}
// A beacon: nick[32] group[8] connected fps, gossip count(1) (+ 6 each), then optionally 'G' 'D' len bytes (the game module's own data).
void sniff_beacon(Client& c, const uint8_t* body, size_t blen, int64_t t) {
    if (blen < 42) return;
    char nick[33] = {0}, grp[9] = {0};
    std::memcpy(nick, body, 32); std::memcpy(grp, body + 32, 8);
    c.connected = body[40]; c.fps = body[41]; c.last_beacon_us = t;
    if (c.nick != nick) c.nick = nick;
    if (c.group != grp) c.group = grp;
    if (!c.ident_logged) {
        size_t o = 42;
        if (blen > o) o += 1 + (size_t)body[o] * 6;
        if (o + 3 <= blen && body[o] == 'G' && body[o + 1] == 'D' && o + 3 + body[o + 2] <= blen) {
            static const char* hx = "0123456789abcdef";
            for (size_t i = 0; i < body[o + 2]; i++) { c.game_hex += hx[body[o + 3 + i] >> 4]; c.game_hex += hx[body[o + 3 + i] & 15]; }
        }
        ident_client(c);
    }
}
// What a PDP packet is: the log counts only battle traffic as "game" (not pings, pack transfers, the lobby).
bool pdp_is_game(const uint8_t* body, size_t blen) {
    if (blen < 10) return false;
    const uint16_t dport = (uint16_t)(body[8] | (body[9] << 8));
    if (dport == 0xD1D2) return false;                                   // ping
    if (dport == 0xD1D1) return blen > 10 && body[10] == 'B';            // side channel: 'B' = battle pad link; manifests and pack pieces are not game
    if (dport == 0xFF03) return false;                                   // tag roster exchange
    return true;
}
// The build id: manifest 'M' 1 nuse{name[24] ver(2) state(1)} nready{name[24] ver(2)} 'V' proto(2) len commit, on the side port 0xD1D1;
// and 'T' 'V' proto(1) len commit on the tag roster channel 0xFF03 (both added 26 Nov, tt-netdesync2; older builds send neither).
void sniff_build(Client& c, const uint8_t* body, size_t blen) {
    if (blen < 12) return;
    const uint16_t dport = (uint16_t)(body[8] | (body[9] << 8));
    const uint8_t* d = body + 10; const size_t n = blen - 10;
    if (dport == 0xFF03 && n >= 4 && d[0] == 'T' && d[1] == 'V' && 4u + d[3] <= n) {
        note_build(c, d[2], std::string((const char*)d + 4, d[3]));
    } else if (dport == 0xD1D1 && n >= 3 && d[0] == 'M' && d[1] == 1) {
        size_t o = 3 + (size_t)d[2] * 27;
        if (o >= n) return;
        o += 1 + (size_t)d[o] * 26;
        if (o + 4 <= n && d[o] == 'V' && o + 4 + d[o + 3] <= n)
            note_build(c, d[o + 1] | (d[o + 2] << 8), std::string((const char*)d + o + 4, d[o + 3]));
    }
}

void handle_packet(const uint8_t* d, size_t n, const sockaddr_in& from) {
    if (n < HDR || std::memcmp(d, "DSAH", 4) != 0) { ip_err(from, E_MALFORMED, n); return; }
    const uint8_t type = d[4];
    Mac src; std::memcpy(src.data(), d + 5, 6);
    uint16_t blen; std::memcpy(&blen, d + 11, 2);
    if (HDR + blen > n) { ip_err(from, E_MALFORMED, n); return; }
    const uint8_t* body = d + HDR;

    if (type == T_RELAY) {
        if (blen < 1) { ip_err(from, E_MALFORMED, n); return; }
        if (body[0] == OP_HELLO) handle_hello(from, src, body, blen);
        else if (body[0] == OP_INTRO) handle_intro(from, src, body, blen);
        else if (body[0] == OP_BYE) {
            auto it = g_clients.find(addr_key(from));
            if (it != g_clients.end() && it->second->mac == src) drop_client(addr_key(from), "bye");
        } else ip_err(from, E_BADOP, n);   // an op this relay does not know: a newer client than this server
        return;
    }
    if (type < T_BEACON || type > T_FRAG) { ip_err(from, E_BADTYPE, n); return; }

    auto it = g_clients.find(addr_key(from));
    if (it == g_clients.end()) { ip_err(from, E_UNREG, n); return; }
    if (it->second->mac != src) { ip_err(from, E_SPOOF, n); return; }   // spoofed MAC
    Client& c = *it->second;
    const int64_t t = now_us();
    c.last_seen = t;
    c.rx++; g_total_rx++; c.rx_bytes += n;
    if (!allow(c, n)) { c.dropped++; g_total_drop++; return; }

    auto& members = g_rooms[c.room];
    auto forward = [&](Client* to, bool unicast) {
        send_to(to->addr, d, n);
        c.fwd++; g_total_fwd++;
        to->tx_pkts++; to->tx_bytes += n;
        if (unicast) {   // relayed player-to-player traffic: the direct-path detector watches it stop
            Pair& p = g_pairs[pair_key(c.id, to->id)];
            if (!p.a) { p.a = &c; p.b = to; }
            p.last_uni = t; p.uni++;
        }
    };
    if (type == T_BEACON) {
        sniff_beacon(c, body, blen, t);
        for (auto& [m, o] : members) if (o != &c) forward(o, false);
        return;
    }
    if (blen < 6) return;
    if (type == T_PDP) { sniff_build(c, body, blen); if (pdp_is_game(body, blen)) { c.game_pkts++; c.game_bytes += n; } }
    else if (type == T_PTP || type == T_FRAG) { c.game_pkts++; c.game_bytes += n; }
    Mac dst; std::memcpy(dst.data(), body, 6);
    bool bcast = true;
    for (auto b : dst) if (b != 0xff) bcast = false;
    if (bcast) {
        for (auto& [m, o] : members) if (o != &c) forward(o, false);
    } else {
        auto mit = members.find(dst);
        if (mit != members.end() && mit->second != &c) forward(mit->second, true);
    }
}

// ---- once a second: groups (matches), battles, direct paths ----
void end_battle(Sess& s, int64_t t) {
    if (!s.battle) return;
    s.battle = false;
    const double dur = std::max(0.0, (s.b_last - s.b_start) / 1e6);
    s.battles++; s.battle_s += (uint64_t)dur;
    J("battle_end").str("room", s.room).str("group", s.group).raw("players", players_json(s.members, &s.now)).num("dur_s", dur)
        .u("relayed_bytes", s.b_bytes).u("relayed_game_pkts", s.b_game).num("avg_bps", dur > 0 ? s.b_bytes * 8 / dur : 0)
        .u("peak_bps", s.b_peak_bps).u("peak_pps", s.b_peak_pps).emit();
    (void)t;
}

std::string pairs_json(Sess& s) {
    std::string o = "[";
    bool first = true;
    for (auto& [k, p] : g_pairs) {
        if (!p.a || !p.b || p.a->sess != &s || p.b->sess != &s) continue;
        if (!first) o += ',';
        first = false;
        o += "{\"a\":\"" + mac_str(p.a->mac) + "\",\"b\":\"" + mac_str(p.b->mac) + "\",\"state\":\"" +
             (p.state == 2 ? "direct" : p.state == 1 ? "relay" : "unknown") + "\",\"reason\":\"" + json_str(p.reason) + "\"}";
    }
    return o + "]";
}

void end_sess(std::map<std::string, Sess>::iterator it, int64_t t) {
    Sess& s = it->second;
    end_battle(s, t);
    const double dur = (t - s.start_us) / 1e6;
    std::string pl = "[";
    bool first = true;
    for (auto& [m, mem] : s.members) {
        if (!first) pl += ',';
        first = false;
        J j("x");   // reuse the number formatting only
        j.str("mac", mac_str(m)).str("nick", mem.nick).str("ip", mem.ip).u("rx_pkts", mem.rx_pkts).u("rx_bytes", mem.rx_bytes).u("tx_bytes", mem.tx_bytes)
            .u("game_pkts", mem.game_pkts).u("peak_bps", mem.peak_bps).num("avg_bps", dur > 0 ? mem.rx_bytes * 8 / dur : 0);
        mem.rtt.json(j);
        pl += "{" + j.s.substr(j.s.find(",\"mac\"") + 1) + "}";
    }
    pl += "]";
    J("match_end").str("room", s.room).str("group", s.group).num("dur_s", dur).u("max_players", s.max_players).raw("players", pl)
        .raw("pairs", pairs_json(s)).u("relayed_bytes", s.rx_bytes).u("relayed_game_pkts", s.game_pkts).num("avg_bps", dur > 0 ? s.rx_bytes * 8 / dur : 0)
        .u("peak_bps", s.peak_bps).u("peak_pps", s.peak_pps).u("battles", s.battles).u("battle_s", s.battle_s).emit();
    for (auto& [id, c] : g_clients) if (c->sess == &s) c->sess = nullptr;
    g_sess.erase(it);
}

void evaluate_pairs(int64_t t) {
    for (auto& [k, p] : g_pairs) {
        Client *a = p.a, *b = p.b;
        if (!a || !b || !a->sess || a->sess != b->sess) continue;
        const int64_t both = std::max(a->sess_since, b->sess_since);
        const bool recent = t - p.last_uni < 2500000;
        const int64_t ref = std::max(both, p.intro_at);
        auto set_state = [&](int st, const std::string& why) {
            if (p.state == st && p.reason == why) return;
            if (p.state) p.flaps++;
            p.state = st; p.reason = why; p.state_at = t;
            J("path").str("room", a->room).str("a", mac_str(a->mac)).str("a_ip", a->ipport).str("b", mac_str(b->mac)).str("b_ip", b->ipport)
                .str("state", st == 2 ? "direct" : "relay").str("reason", why).u("intros", p.intros)
                .num("after_intro_s", p.first_intro ? (t - p.first_intro) / 1e6 : -1).num("after_group_s", (t - both) / 1e6).emit();
        };
        if (!p.intro_at) {   // never introduced: an old client, direct paths off, or INTRO lost
            if (recent && t - both > 8000000 && !p.state) {
                set_state(1, !a->intro_seen && !b->intro_seen ? "no_intro_either_side" : !a->intro_seen || !b->intro_seen ? "no_intro_one_side" : "no_intro");
            }
            continue;
        }
        if (recent) {
            if (t - ref >= 6000000 && p.state != 1) {
                std::string why;
                if (std::strcmp(a->nat, "cgnat") == 0 || std::strcmp(b->nat, "cgnat") == 0) why = "cgnat";
                else if (!a->intro_seen || !b->intro_seen) why = "peer_no_intro";
                else why = "no_hole";   // punching did not get through: symmetric NAT, firewall, or the path is lost
                if (p.state == 2) why = "direct_lost";
                set_state(1, why);
            }
        } else if (p.uni > 0 && t - std::max(ref, p.last_uni) >= 4000000 && t - std::max(a->last_beacon_us, b->last_beacon_us) < 4000000 &&
                   t - std::min(a->last_beacon_us, b->last_beacon_us) < 4000000 && p.state != 2) {
            set_state(2, "silent_on_relay");
        }
    }
}

void tick_second() {
    const int64_t t = now_us();
    // 1. accounting (before the groups change, so a match that ends now still gets this second)
    for (auto& [k, s] : g_sess) s.sec_rxb = s.sec_game = s.sec_pkts = 0;
    for (auto& [k, c] : g_clients) account(*c);
    // 2. groups: >= 2 connected players with the same group name in a room, beaconing within 5 s
    std::map<std::string, std::vector<Client*>> groups;
    for (auto& [k, c] : g_clients)
        if (c->connected && !c->group.empty() && t - c->last_beacon_us < 5000000) groups[c->room + '\x01' + c->group].push_back(c.get());
    for (auto it = g_sess.begin(); it != g_sess.end();) {
        auto g = groups.find(it->first);
        if (g == groups.end() || g->second.size() < 2) { auto cur = it++; end_sess(cur, t); }
        else ++it;
    }
    for (auto& [key, cl] : groups) {
        if (cl.size() < 2) continue;
        auto it = g_sess.find(key);
        const bool fresh = it == g_sess.end();
        if (fresh) {
            it = g_sess.emplace(key, Sess()).first;
            it->second.room = cl[0]->room; it->second.group = cl[0]->group; it->second.start_us = t;
        }
        Sess& s = it->second;
        std::set<Mac> cur;
        for (Client* c : cl) {
            cur.insert(c->mac);
            if (c->sess != &s) { c->sess = &s; c->sess_since = t; }
            Member& m = s.members[c->mac];
            m.nick = c->nick; m.ip = c->ip;
        }
        const bool changed = cur != s.now;
        s.now = cur;
        s.max_players = std::max(s.max_players, cur.size());
        if (fresh) J("match_start").str("room", s.room).str("group", s.group).raw("players", players_json(s.members, &s.now)).u("n", cur.size()).emit();
        else if (changed) J("match_players").str("room", s.room).str("group", s.group).raw("players", players_json(s.members, &s.now)).u("n", cur.size()).emit();
    }
    for (auto& [k, c] : g_clients) if (c->sess && !c->sess->now.count(c->mac)) c->sess = nullptr;
    // 3. peaks and battle detection (relayed game traffic: >= 10 packets/s for 2 s starts one, 5 s without ends it)
    for (auto& [k, s] : g_sess) {
        s.peak_bps = std::max(s.peak_bps, s.sec_rxb * 8); s.peak_pps = std::max(s.peak_pps, s.sec_pkts);
        if (s.sec_game >= 10) {
            s.act++; s.idle = 0; s.b_last = t;
            if (!s.battle && s.act >= 2) {
                s.battle = true; s.b_start = t - 1000000; s.b_bytes = s.b_game = s.b_peak_bps = s.b_peak_pps = 0;
                J("battle_start").str("room", s.room).str("group", s.group).raw("players", players_json(s.members, &s.now)).emit();
            }
        } else { s.idle++; s.act = 0; }
        if (s.battle) {
            s.b_bytes += s.sec_rxb; s.b_game += s.sec_game;
            s.b_peak_bps = std::max(s.b_peak_bps, s.sec_rxb * 8); s.b_peak_pps = std::max(s.b_peak_pps, s.sec_pkts);
            if (s.idle >= 5) end_battle(s, t);
        }
    }
    evaluate_pairs(t);
}

// ---- every rate_secs: per-client rates, per-IP abuse counters ----
void tick_rate(double secs) {
    for (auto& [k, c] : g_clients) emit_rate(*c, secs);
    for (auto it = g_ips.begin(); it != g_ips.end();) {
        IpStat& s = it->second;
        uint32_t errs = 0;
        for (int e = 0; e < E_COUNT; e++) errs += s.err[e];
        if (errs || s.joins >= 3 || s.rooms.size() >= 3 || s.reregs >= 3) {
            in_addr a; a.s_addr = htonl(it->first);
            for (int e = 0; e < E_COUNT; e++)
                if (s.err[e]) J("error").str("kind", E_NAME[e]).str("ip", inet_ntoa(a)).u("port", s.port).str("room", s.last_room).u("count", s.err[e]).num("secs", secs).emit();
            J j("ipstat");
            j.str("ip", inet_ntoa(a)).num("secs", secs).u("unreg_pkts", s.pkts).u("unreg_bytes", s.bytes).u("joins", s.joins).u("leaves", s.leaves)
                .u("rereg", s.reregs).u("rooms", s.rooms.size()).u("errors", errs);
            j.emit();
        }
        it = g_ips.erase(it);
    }
}

void housekeeping() {
    const int64_t t = now_us();
    std::vector<uint64_t> dead;
    for (auto& [k, c] : g_clients) if (t - c->last_seen > CLIENT_TIMEOUT_US) dead.push_back(k);
    for (uint64_t k : dead) drop_client(k, "timeout");
}

void on_signal(int) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port") cfg.port = (uint16_t)std::atoi(next());
        else if (a == "--bind") cfg.bind = next();
        else if (a == "--max-room") cfg.max_room = (size_t)std::atoi(next());
        else if (a == "--max-clients") cfg.max_clients = (size_t)std::atoi(next());
        else if (a == "--pps") cfg.pps = std::atof(next());
        else if (a == "--kbps") cfg.kbps = std::atof(next());
        else if (a == "--log-dir") cfg.log_dir = next();
        else if (a == "--no-log") cfg.no_log = true;
        else if (a == "--keep-days") cfg.keep_days = std::atoi(next());
        else if (a == "--rate-secs") cfg.rate_secs = std::max(1, std::atoi(next()));
        else if (a == "--verbose") cfg.verbose = true;
        else { std::fprintf(stderr, "usage: %s [--port N] [--bind IP] [--max-room N] [--max-clients N] [--pps N] [--kbps N] [--log-dir DIR | --no-log] [--keep-days N] [--rate-secs N] [--verbose]\n", argv[0]); return 2; }
    }
    FILE* ur = std::fopen("/dev/urandom", "rb");
    if (!ur || std::fread(g_key, 1, sizeof g_key, ur) != sizeof g_key) { std::fprintf(stderr, "no entropy\n"); return 1; }
    std::fclose(ur);

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { std::perror("socket"); return 1; }
    int big = 4 << 20;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    timeval tv{0, 200000};   // wakes five times a second at least: the once-a-second work stays on time on a quiet server
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(cfg.port);
    if (inet_pton(AF_INET, cfg.bind.c_str(), &a.sin_addr) != 1) { std::fprintf(stderr, "bad --bind\n"); return 2; }
    if (bind(g_sock, (sockaddr*)&a, sizeof a) != 0) { std::perror("bind"); return 1; }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("adhoc_relay listening on %s:%u (rooms of up to %zu, %zu clients, %.0f pkt/s and %.0f kbit/s per client)\n",
                cfg.bind.c_str(), cfg.port, cfg.max_room, cfg.max_clients, cfg.pps, cfg.kbps);
    if (!cfg.no_log) {
        if (cfg.log_dir.empty()) {
            const char* sh = std::getenv("XDG_STATE_HOME"); const char* home = std::getenv("HOME");
            cfg.log_dir = sh && *sh ? std::string(sh) + "/adhoc_relay" : std::string(home ? home : ".") + "/.local/state/adhoc_relay";
        }
        if (g_log.start(cfg.log_dir, cfg.keep_days)) std::printf("event log in %s (kept %d days)\n", cfg.log_dir.c_str(), cfg.keep_days);
        else std::printf("event log DISABLED (cannot write %s)\n", cfg.log_dir.c_str());
        J("start").str("bind", cfg.bind).u("port", cfg.port).u("version", VERSION).u("max_room", cfg.max_room).emit();
    }

    static uint8_t buf[MAX_DATAGRAM + 1];
    int64_t next_house = now_us() + 1000000, next_stats = now_us() + 60000000, next_rate = now_us() + (int64_t)cfg.rate_secs * 1000000;
    uint64_t last_rx = 0, last_fwd = 0;
    while (!g_stop) {
        sockaddr_in from{}; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(g_sock, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n > 0 && (size_t)n <= MAX_DATAGRAM) handle_packet(buf, (size_t)n, from);
        const int64_t t = now_us();
        if (t >= next_house) { next_house = t + 1000000; housekeeping(); tick_second(); }
        if (t >= next_rate) { next_rate = t + (int64_t)cfg.rate_secs * 1000000; tick_rate(cfg.rate_secs); }
        if (t >= next_stats) {
            next_stats = t + 60000000;
            std::printf("stats: %zu clients in %zu rooms; last minute rx %llu forwarded %llu; dropped (rate limit) total %llu\n",
                        g_clients.size(), g_rooms.size(), (unsigned long long)(g_total_rx - last_rx),
                        (unsigned long long)(g_total_fwd - last_fwd), (unsigned long long)g_total_drop);
            J("stats").u("clients", g_clients.size()).u("rooms", g_rooms.size()).u("matches", g_sess.size()).u("rx", g_total_rx - last_rx)
                .u("fwd", g_total_fwd - last_fwd).u("dropped_total", g_total_drop).u("log_lost", g_log.lost).emit();
            last_rx = g_total_rx; last_fwd = g_total_fwd;
        }
    }
    std::printf("shutting down\n");
    if (g_log.on) {
        const int64_t t = now_us();
        for (auto it = g_sess.begin(); it != g_sess.end();) { auto cur = it++; end_sess(cur, t); }
        while (!g_clients.empty()) drop_client(g_clients.begin()->first, "shutdown");
        J("stop").emit();
        g_log.stop();
    }
    return 0;
}
