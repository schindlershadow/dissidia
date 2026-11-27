// PSP ad-hoc networking over UDP: sceNetAdhocctl*, sceNetAdhocMatching*, sceNetAdhoc (PDP/PTP).
// See include/hle/psp_hle_adhoc.h for the environment variables.
//
// Design
//  * One UDP socket and one network thread. Everything the network thread and the guest
//    threads share is guarded by g_mu.
//  * Peers are found by beacons (LAN broadcast, 127.0.0.1 for same-machine tests, and the
//    PSPRECOMP_ADHOC_PEERS list). A beacon carries the MAC, nickname, group name and the
//    frame rate the player wants.
//  * Guest callbacks (adhocctl handlers, matching callbacks) are queued and run on a guest
//    thread by psp_adhoc_pump(), which psp_kernel_check_callbacks() and the vblank wait call.
//  * PTP is a small reliable byte stream (go-back-N) over the same UDP socket.
#include "hle/psp_hle.h"
#include "hle/psp_hle_adhoc.h"
#include "psp_netplay.h"
#include "hle/psp_hle_kernel.h"
#include "psp_scheduler.h"
#include "psp_memory.h"
#include "recomp.h"

#include "psp_platform.h"
#include "psp_ge_draw.h"
#include "hle/psp_upnp.h"
#ifdef __ANDROID__
#include "psp_android.h"
#endif
#ifndef _WIN32
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- error codes (PPSSPP/pspnet) ----
constexpr int32_t E_INVALID_SOCKET = (int32_t)0x80410701;
constexpr int32_t E_INVALID_ARG = (int32_t)0x80410711;
constexpr int32_t E_INVALID_PORT = (int32_t)0x80410703;
constexpr int32_t E_INVALID_DATALEN = (int32_t)0x80410705;
constexpr int32_t E_WOULD_BLOCK = (int32_t)0x80410709;
constexpr int32_t E_PORT_IN_USE = (int32_t)0x8041070B;
constexpr int32_t E_NOT_INIT = (int32_t)0x80410712;
constexpr int32_t E_TIMEOUT = (int32_t)0x80410715;
constexpr int32_t E_NO_ENTRY = (int32_t)0x80410716;
constexpr int32_t E_REFUSED = (int32_t)0x80410718;
constexpr int32_t E_NOT_CONNECTED = (int32_t)0x8041071B;
constexpr int32_t E_DISCONNECTED = (int32_t)0x80410719;

// ---- adhocctl events ----
constexpr int CTL_ERROR = 0, CTL_CONNECT = 1, CTL_DISCONNECT = 2, CTL_SCAN = 3;

// ---- matching events ----
constexpr int M_HELLO = 1, M_REQUEST = 2, M_LEAVE = 3, M_DENY = 4, M_CANCEL = 5, M_ACCEPT = 6,
              M_ESTABLISHED = 7, M_TIMEOUT = 8, M_BYE = 10, M_DATA = 11, M_DATA_ACK = 12;
// ---- matching wire ops ----
constexpr uint8_t MO_HELLO = 1, MO_JOIN = 2, MO_ACCEPT = 3, MO_CANCEL = 4, MO_ESTAB = 5, MO_BYE = 6,
                  MO_DATA = 7, MO_DATA_ACK = 8, MO_PING = 9;

// ---- wire packet types ----
constexpr uint8_t T_BEACON = 1, T_PDP = 2, T_MATCH = 3, T_PTP = 4, T_RELAY = 5, T_FRAG = 6, T_PUNCH = 7;   // 7: direct path (never through the relay)
constexpr uint8_t RO_HELLO = 1, RO_CHALLENGE = 2, RO_OK = 3, RO_BYE = 4, RO_INTRO = 5, RO_PEER = 6;   // relay server ops (tools/adhoc_relay)
constexpr uint8_t P_SYN = 1, P_SYNACK = 2, P_DATA = 3, P_ACK = 4, P_FIN = 5, P_RST = 6;

using Mac = std::array<uint8_t, 6>;
constexpr size_t HDR = 13;  // magic(4) type(1) srcmac(6) bodylen(2)

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool g_log = false;
bool g_trace = false;   // PSPRECOMP_ADHOC_LOG=2: also every matching message sent / received per peer (op, size, verdict)
#define ALOG(...) do { if (g_log) { std::fprintf(stderr, "[ADHOC] " __VA_ARGS__); std::fputc('\n', stderr); } } while (0)
#define ANOTE(...) do { std::fprintf(stderr, "[ADHOC] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)

std::string mac_str(const Mac& m) {
    char b[24];
    std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

inline uint8_t* gp(uint8_t* rdram, uint32_t a) { return rdram + (a & PSP_ADDR_MASK); }
inline Mac read_mac(uint8_t* rdram, uint32_t a) { Mac m; std::memcpy(m.data(), gp(rdram, a), 6); return m; }
inline void w32(uint8_t* rdram, uint32_t a, uint32_t v) { std::memcpy(gp(rdram, a), &v, 4); }
inline void w16(uint8_t* rdram, uint32_t a, uint16_t v) { std::memcpy(gp(rdram, a), &v, 2); }
inline uint32_t r32(uint8_t* rdram, uint32_t a) { uint32_t v; std::memcpy(&v, gp(rdram, a), 4); return v; }
inline bool is_bcast(const Mac& m) { for (auto b : m) if (b != 0xff) return false; return true; }

// ---- state ----
struct Peer {
    sockaddr_in addr{};
    char nick[32] = {0};
    char group[8] = {0};
    bool in_group = false;
    uint8_t fps = 0;
    uint8_t game_data[48] = {0}; uint8_t game_len = 0;   // the game module's own bytes (psp_adhoc_set_game_data)
    int64_t last_seen = 0;
};

// Ping: a PDP datagram to a reserved port, once a second to every player in our group, answered at once (any build with this
// code; older builds never answer and simply show no ping). Over the relay it travels like any PDP packet.
constexpr uint16_t PING_PORT = 0xD1D2;
struct PingStat {
    double srtt_ms = -1;                    // smoothed round trip
    double var_ms = 0;                      // smoothed deviation of the round trip from srtt_ms (jitter; as TCP's RTTVAR)
    uint32_t asked = 0, answered = 0;       // pings sent to / answered by this player since it joined (psp_adhoc_ping_loss_permille)
    uint32_t seq = 0;
    std::map<uint32_t, int64_t> outstanding;   // seq -> sent at
    std::deque<bool> window;                // the last answers: true answered, false lost (2 s)
    bool ever = false;                      // this peer answered once (an older build never does)
    int high = 0;                           // seconds in a row over the latency limit
};

struct Ev {  // queued guest callback
    int kind = 0;  // 0 adhocctl handler, 1 matching
    uint32_t func = 0, arg = 0;
    int a = 0, b = 0;  // ctl: flag,error ; matching: id,event
    Mac mac{};
    std::vector<uint8_t> data;
};

struct MPeer {
    enum S { HELLO, REQ_BY, REQ_OF, ACCEPTED, ESTAB } s = HELLO;
    std::vector<uint8_t> opt;
    int64_t last_seen = 0, last_event = 0;
    bool parent = false;
};

struct Match {
    int id = 0, mode = 0, maxpeers = 0, port = 0, rxbuf = 0;
    uint32_t hello_us = 0, ping_us = 0, msg_us = 0;
    int init_count = 0;
    uint32_t cb = 0;
    bool started = false;
    std::vector<uint8_t> hello_opt;
    std::map<Mac, MPeer> peers;
    int64_t next_hello = 0, next_ping = 0;
    uint32_t data_id = 1;
};

struct PdpMsg { Mac src; uint16_t sport; std::vector<uint8_t> data; };
struct Pdp { int id; Mac mac; uint16_t port; int bufsize; std::deque<PdpMsg> q; };

struct Ptp {
    enum S { LISTEN, SYN_SENT, ESTAB, CLOSED, REFUSED } s = CLOSED;
    int id = 0;
    Mac smac{}, dmac{};
    uint16_t sport = 0, dport = 0;
    int bufsize = 0;
    int64_t rexmt_us = 200000;
    int rexmt_cnt = 10;
    // stream
    std::deque<uint8_t> sendq;  // unacked + unsent bytes; sendq[0] has seq snd_una
    uint32_t snd_una = 0, snd_next = 0;
    int64_t last_progress = 0, last_syn = 0;
    int retries = 0;
    std::deque<uint8_t> rxq;
    uint32_t rcv_next = 0;
    bool fin_rx = false;
    std::deque<int> accept_q;  // listener only
    int parent = 0;
    uint32_t cid = 0;  // connection id: carried in SYN, echoed in SYNACK
};

std::mutex g_mu;
std::condition_variable_any* g_unused = nullptr;
bool g_enabled = true;
const char* g_off_reason = "Off (PSPRECOMP_ADHOC=0)";
bool g_net_ready = false;
std::atomic<bool> g_stop{false};
int g_sock = -1;
int g_base_port = 27312;
int g_span = 16;   // ports tried / probed: base..base+span-1 (several instances per machine)
uint16_t g_my_port = 0;
Mac g_mac{};
char g_nick[32] = {0};
std::string g_game_nick;   // psp_adhoc_set_nickname (the game's player name); PSPRECOMP_ADHOC_NICK wins over it
std::thread g_thread;
std::vector<sockaddr_in> g_extra_peers;
std::map<Mac, Peer> g_peers;

// ---- network mode: LAN (broadcast / loopback / listed peers) or Internet (relay server) ----
constexpr const char* DEFAULT_SERVER = "party.schindlershadow.com:27500";
std::once_flag g_cfg_once;
bool g_relay_mode = true;                 // Internet
std::string g_cfg_server = DEFAULT_SERVER, g_cfg_room, g_cfg_peers;
sockaddr_in g_relay_addr{};
bool g_relay_resolved = false, g_relay_resolve_failed = false, g_relay_ok = false;
uint64_t g_relay_cookie = 0;
int g_relay_players = 0;
int64_t g_relay_hello_next = 0, g_relay_resolve_next = 0, g_relay_ok_at = 0, g_relay_resolved_at = 0;
int64_t g_relay_last_rx = 0;              // anything heard from the server
// Direct paths (Internet mode): the relay introduces two players of a room (INTRO / PEER, relay version 2), both send T_PUNCH to
// the other's public and LAN address, and once one arrives, packets to that player go straight to the address it came from. The
// relay stays the path for broadcasts and the fallback: a direct path silent for 3 s is dropped and tried again later.
struct Direct {
    sockaddr_in pub{}, lan{}, active{};
    int state = 0;                     // 0 none, 1 punching, 2 up
    int64_t started = 0, last_rx = 0, next_punch = 0, asked = 0, up_at = 0;
    uint32_t sweep = 0;                // punch round counter (port prediction)
    std::string reason;                // why this player is reached through the relay (last failure), shown in the notification
};
std::map<Mac, Direct> g_direct;
struct ConnTold { int mode = 0; int64_t since = 0; };   // per player of the group: 0 nothing shown yet, 1 "connected directly" shown, 2 "via relay" shown
std::map<Mac, ConnTold> g_conn_told;
int g_relay_version = 0;
bool g_direct_off = false;             // PSPRECOMP_ADHOC_DIRECT=0
uint32_t g_lan_ip = 0;                 // this machine's address toward the relay (network order), for two players behind one router
int64_t g_ctl_init_at = 0;                // when the game started using the network (adhocctl init)
std::map<Mac, PingStat> g_ping;
int64_t g_next_net_check = 0;
bool g_cfg_dirty = false;                 // set by psp_adhoc_configure; the network thread applies it
std::string g_relay_host_shown;
// Peers other players told us about (address -> give-up time). They are probed with beacons and, if
// they answer, become normal peers. This builds a full mesh from a single configured address.
std::map<uint64_t, int64_t> g_gossip;
constexpr int MAX_GOSSIP = 64;
uint64_t addr_key(const sockaddr_in& a) { return ((uint64_t)ntohl(a.sin_addr.s_addr) << 16) | ntohs(a.sin_port); }

bool g_adhoc_init = false, g_ctl_init = false, g_match_init = false;
bool g_connected = false;
char g_group[8] = {0};
int g_next_handler = 1;
std::map<int, std::pair<uint32_t, uint32_t>> g_handlers;  // id -> (func,arg)
int64_t g_scan_due = 0;
int g_local_fps = 0;
uint8_t g_game_data[48] = {0}; uint8_t g_game_len = 0;   // this player's game module bytes, sent in the beacons

std::deque<Ev> g_events;
std::map<int, Match> g_match;
int g_next_match = 1;
std::map<int, Pdp> g_pdp;
int g_next_pdp = 1;
std::map<int, Ptp> g_ptp;
int g_next_ptp = 1;
uint16_t g_next_eph = 0x4000;

uint32_t g_scratch = 0;  // guest scratch memory for callback arguments
constexpr uint32_t SCRATCH_SIZE = 0x8000;
std::atomic<bool> g_pumping{false};

// ---- send ----
void send_dgram(const sockaddr_in& to, uint8_t type, const std::vector<uint8_t>& body) {
    if (g_sock < 0) return;
    std::vector<uint8_t> p(HDR + body.size());
    p[0] = 'D'; p[1] = 'S'; p[2] = 'A'; p[3] = 'H';
    p[4] = type;
    std::memcpy(&p[5], g_mac.data(), 6);
    uint16_t n = (uint16_t)body.size();
    std::memcpy(&p[11], &n, 2);
    if (!body.empty()) std::memcpy(&p[HDR], body.data(), body.size());
    (void)!sendto(g_sock, reinterpret_cast<const char*>(p.data()), (int)p.size(), 0, (const sockaddr*)&to, sizeof to);
}

// A datagram larger than the path MTU is split by IP into fragments, and many routers (home NAT, a
// hairpin) drop those. The game's lobby hello is 4.7 KB, so large messages are split here instead,
// into pieces that fit one packet: T_FRAG = dst MAC(6), message id(4), index(1), count(1), original
// type(1), then a chunk of the body. The relay routes a T_FRAG by its destination like a PDP packet.
constexpr size_t MAX_SINGLE = 1300, FRAG_CHUNK = 1100, MAX_FRAGS = 16;
uint32_t g_frag_id = 1;   // caller holds g_mu

void send_raw(const sockaddr_in& to, uint8_t type, const std::vector<uint8_t>& body) {
    if (HDR + body.size() <= MAX_SINGLE || type < T_PDP || type > T_PTP || body.size() < 6) {
        send_dgram(to, type, body);
        return;
    }
    const size_t count = (body.size() + FRAG_CHUNK - 1) / FRAG_CHUNK;
    if (count > MAX_FRAGS) return;
    const uint32_t id = g_frag_id++;
    for (size_t i = 0; i < count; i++) {
        const size_t off = i * FRAG_CHUNK, n = std::min(FRAG_CHUNK, body.size() - off);
        std::vector<uint8_t> f;
        f.reserve(13 + n);
        f.insert(f.end(), body.begin(), body.begin() + 6);   // destination MAC (all of these types start with it)
        for (int k = 0; k < 4; k++) f.push_back((id >> (8 * k)) & 0xff);
        f.push_back((uint8_t)i); f.push_back((uint8_t)count); f.push_back(type);
        f.insert(f.end(), body.begin() + off, body.begin() + off + n);
        send_dgram(to, T_FRAG, f);
    }
}

bool same_group_peer_known(const Mac& m) {   // caller holds g_mu
    auto it = g_peers.find(m);
    return it != g_peers.end() && it->second.in_group;
}

// Caller holds g_mu. Sends to one MAC (or, for broadcast, every peer in our group).
void send_to_mac(const Mac& dst, uint8_t type, const std::vector<uint8_t>& body) {
    if (g_relay_mode && !is_bcast(dst)) {   // a direct path to this player: straight to it
        auto dit = g_direct.find(dst);
        if (dit != g_direct.end() && dit->second.state == 2 && now_us() - dit->second.last_rx < 3000000) {
            send_raw(dit->second.active, type, body);
            return;
        }
    }
    if (g_relay_mode) {   // everything goes through the server, which fans broadcasts out itself
        if (g_relay_ok && (is_bcast(dst) || same_group_peer_known(dst))) send_raw(g_relay_addr, type, body);
        return;
    }
    if (is_bcast(dst)) {
        for (auto& [m, p] : g_peers)
            if (p.in_group && g_connected && std::strncmp(p.group, g_group, 8) == 0)
                send_raw(p.addr, type, body);
        return;
    }
    auto it = g_peers.find(dst);
    if (it != g_peers.end()) send_raw(it->second.addr, type, body);
}

void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xff); v.push_back(x >> 8); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xff); }
uint16_t get16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

bool same_group(const Mac& m) {
    auto it = g_peers.find(m);
    return g_connected && it != g_peers.end() && it->second.in_group &&
           std::strncmp(it->second.group, g_group, 8) == 0;
}

void queue_ctl(int flag, int err) {
    for (auto& [id, h] : g_handlers) {
        Ev e; e.kind = 0; e.func = h.first; e.arg = h.second; e.a = flag; e.b = err;
        g_events.push_back(std::move(e));
    }
}

void queue_match(Match& m, int ev, const Mac& mac, const std::vector<uint8_t>& data) {
    Ev e; e.kind = 1; e.func = m.cb; e.a = m.id; e.b = ev; e.mac = mac; e.data = data;
    g_events.push_back(std::move(e));
    ALOG("match %d event %d from %s (%zu bytes)", m.id, ev, mac_str(mac).c_str(), data.size());
}

// ---- matching wire helpers ----
const char* mo_name(uint8_t op) {
    static const char* n[] = {"?", "HELLO", "JOIN", "ACCEPT", "CANCEL", "ESTAB", "BYE", "DATA", "DATA_ACK", "PING"};
    return op < sizeof n / sizeof *n ? n[op] : "?";
}
void match_send(Match& m, const Mac& to, uint8_t op, const std::vector<uint8_t>& payload, uint32_t extra = 0) {
    std::vector<uint8_t> b;
    b.insert(b.end(), to.begin(), to.end());
    put16(b, (uint16_t)m.port);
    b.push_back(op);
    put32(b, extra);
    b.insert(b.end(), payload.begin(), payload.end());
    if (g_trace) ALOG("match %d tx op %s -> %s (%zu bytes)", m.id, mo_name(op), mac_str(to).c_str(), payload.size());
    send_to_mac(to, T_MATCH, b);
}

void match_remove_peer(Match& m, const Mac& mac) { m.peers.erase(mac); }

int established_count(Match& m) {
    int n = 0;
    for (auto& [k, p] : m.peers) if (p.s == MPeer::ESTAB) n++;
    return n;
}

// ---- side channel (psp_adhoc_side_*): PDP to the reserved port, handed to the game module, never to the game ----
void (*g_side_handler)(const uint8_t mac[6], const uint8_t* data, size_t len) = nullptr;
bool (*g_match_filter)(const uint8_t mac[6]) = nullptr;

void handle_match(const Mac& src, const uint8_t* body, size_t len) {
    if (len < 6 + 2 + 1 + 4) return;
    Mac dst; std::memcpy(dst.data(), body, 6);
    if (dst != g_mac && !is_bcast(dst)) return;
    if (g_trace) ALOG("match rx op %s <- %s (%zu bytes)%s", mo_name(body[8]), mac_str(src).c_str(), len - 13,
                      !same_group(src) ? ": DROPPED (not in our group)" : g_match_filter && !g_match_filter(src.data()) ? ": DROPPED (the game module refuses this player)" : "");
    if (!same_group(src)) return;
    if (g_match_filter && !g_match_filter(src.data())) return;   // the game module refuses matches with this player
    int port = get16(body + 6);
    uint8_t op = body[8];
    uint32_t extra = get32(body + 9);
    std::vector<uint8_t> payload(body + 13, body + len);
    int64_t t = now_us();
    for (auto& [id, m] : g_match) {
        if (!m.started || m.port != port) continue;
        bool host = m.mode == 1, client = m.mode == 2, p2p = m.mode == 3;
        auto it = m.peers.find(src);
        switch (op) {
        case MO_HELLO: {
            if (host) break;
            MPeer& p = m.peers[src];
            bool changed = p.s == MPeer::HELLO && (p.opt != payload || t - p.last_event > 2000000);
            bool isnew = p.last_seen == 0;
            p.last_seen = t;
            if (p.s == MPeer::HELLO) {
                if (isnew || changed) { p.opt = payload; p.last_event = t; queue_match(m, M_HELLO, src, payload); }
            }
            break;
        }
        case MO_JOIN: {
            if (client) break;
            if (established_count(m) >= m.maxpeers - 1) { match_send(m, src, MO_CANCEL, {}); break; }
            MPeer& p = m.peers[src];
            if (p.s == MPeer::REQ_BY || p.s == MPeer::ESTAB) { p.last_seen = t; break; }
            p.s = MPeer::REQ_BY; p.opt = payload; p.last_seen = t;
            queue_match(m, M_REQUEST, src, payload);
            break;
        }
        case MO_ACCEPT: {
            if (it == m.peers.end() || it->second.s != MPeer::REQ_OF) break;
            it->second.s = MPeer::ESTAB; it->second.parent = !p2p || true; it->second.last_seen = t;
            queue_match(m, M_ACCEPT, src, payload);
            match_send(m, src, MO_ESTAB, {});
            queue_match(m, M_ESTABLISHED, src, {});
            break;
        }
        case MO_ESTAB: {
            if (it == m.peers.end()) break;
            if (it->second.s == MPeer::ACCEPTED) {
                it->second.s = MPeer::ESTAB; it->second.last_seen = t;
                queue_match(m, M_ESTABLISHED, src, {});
            }
            break;
        }
        case MO_CANCEL: {
            if (it == m.peers.end()) break;
            MPeer::S st = it->second.s;
            if (st == MPeer::REQ_OF) queue_match(m, M_DENY, src, {});
            else if (st == MPeer::REQ_BY || st == MPeer::ACCEPTED) queue_match(m, M_CANCEL, src, {});
            if (st != MPeer::HELLO) match_remove_peer(m, src);
            break;
        }
        case MO_BYE: {
            if (it == m.peers.end() || it->second.s != MPeer::ESTAB) break;
            bool was_parent = it->second.parent;
            match_remove_peer(m, src);
            queue_match(m, was_parent ? M_BYE : M_LEAVE, src, {});
            break;
        }
        case MO_DATA: {
            if (it == m.peers.end() || it->second.s != MPeer::ESTAB) break;
            it->second.last_seen = t;
            queue_match(m, M_DATA, src, payload);
            match_send(m, src, MO_DATA_ACK, {}, extra);
            break;
        }
        case MO_DATA_ACK: {
            if (it == m.peers.end()) break;
            it->second.last_seen = t;
            queue_match(m, M_DATA_ACK, src, {});
            break;
        }
        case MO_PING: {   // extra 1: the sender holds us as a pending request, 0: as established
            const bool pending = extra & 1;
            if (it == m.peers.end() || it->second.s == MPeer::HELLO) {
                if (pending) match_send(m, src, MO_CANCEL, {});   // no such request here (its cancel was lost): let the sender drop it
                break;
            }
            it->second.last_seen = t;
            if (!pending && it->second.s == MPeer::ACCEPTED) {   // the requester has our accept but its MO_ESTAB was lost
                it->second.s = MPeer::ESTAB;
                queue_match(m, M_ESTABLISHED, src, {});
            }
            break;
        }
        }
    }
}

// ---- PDP ----
// Side channel for a game module (psp_adhoc_side_send / _recv): PDP datagrams on ports SIDE_PORT0+ (the PSP game never opens them), so
// they take the same paths as game PDP traffic (LAN, relay). Queued here, read by the game thread.
constexpr uint16_t SIDE_PORT0 = 0xFF00;
struct SideMsg { Mac src; uint16_t chan; std::vector<uint8_t> data; };
std::deque<SideMsg> g_side_q;
// The runtime's own messages (the netplay desync check) travel as PDP between this port on both ends: the game never
// uses it, older builds drop it (no socket on it), and the relay forwards it like any PDP packet.
constexpr uint16_t AUX_PORT = 0xFFF0;
void handle_pdp(const Mac& src, const uint8_t* body, size_t len) {
    if (len < 6 + 2 + 2) return;
    Mac dst; std::memcpy(dst.data(), body, 6);
    if (dst != g_mac && !is_bcast(dst)) return;
    if (!same_group(src)) return;
    uint16_t sport = get16(body + 6), dport = get16(body + 8);
    if (dport == AUX_PORT && sport == AUX_PORT) { psp_netplay_on_message(src.data(), body + 10, len - 10); return; }
    if (dport == PING_PORT) {   // ping: 0 = ask (answered at once), 1 = answer
        if (len < 10 + 13) return;
        const uint8_t* q = body + 10;
        if (q[0] == 0) {
            std::vector<uint8_t> b(src.begin(), src.end());
            put16(b, PING_PORT); put16(b, PING_PORT);
            b.insert(b.end(), q, q + 13);
            b[10] = 1;   // after dst MAC (6) and the two ports (4): the kind byte
            send_to_mac(src, T_PDP, b);
        } else if (q[0] == 1) {
            PingStat& st = g_ping[src];
            const uint32_t seq = get32(q + 1);
            auto it = st.outstanding.find(seq);
            if (it == st.outstanding.end()) return;
            const double rtt = (now_us() - it->second) / 1000.0;
            st.outstanding.erase(it);
            st.answered++;
            st.var_ms = st.srtt_ms < 0 ? 0.0 : st.var_ms * 0.75 + std::fabs(rtt - st.srtt_ms) * 0.25;
            st.srtt_ms = st.srtt_ms < 0 ? rtt : st.srtt_ms * 0.75 + rtt * 0.25;
            st.ever = true;
            st.window.push_back(true);
            if (st.window.size() > 20) st.window.pop_front();
        }
        return;
    }
    if (dport == PSP_ADHOC_SIDE_PORT) {      // (g_mu is held: the handler only copies the bytes and returns)
        if (g_side_handler && len - 10 <= PSP_ADHOC_SIDE_MAX) g_side_handler(src.data(), body + 10, len - 10);
        return;
    }
    if (dport >= SIDE_PORT0) {   // a game module's side channel (psp_adhoc_side_send), never a game socket
        if (g_side_q.size() < 4096) g_side_q.push_back({src, (uint16_t)(dport - SIDE_PORT0), std::vector<uint8_t>(body + 10, body + len)});
        return;
    }
    (void)sport;
    for (auto& [id, s] : g_pdp) {
        if (s.port != dport) continue;
        size_t used = 0;
        for (auto& m : s.q) used += m.data.size();
        static const size_t kBufMul = std::getenv("PSPRECOMP_ADHOC_PDP_BUF") ? (size_t)std::max(1, std::atoi(std::getenv("PSPRECOMP_ADHOC_PDP_BUF"))) : 1;   // diagnosis: multiply the receive buffer
        if (used + (len - 10) > (size_t)std::max(s.bufsize, 0x4000) * kBufMul) {   // drop when full
            static int dropped = 0; dropped++;
            if (dropped <= 20 || dropped % 200 == 0) std::fprintf(stderr, "[NET] PDP port %u receive queue full (%zu bytes queued, buffer %d): a %zu byte message from the peer was dropped (%d dropped)\n", (unsigned)dport, used, s.bufsize, len - 10, dropped);
            break;
        }
        s.q.push_back({src, sport, std::vector<uint8_t>(body + 10, body + len)});
        break;
    }
}

// ---- PTP ----
void ptp_send_pkt(Ptp& p, uint8_t flags, uint32_t seq, uint32_t ack, const uint8_t* data = nullptr, size_t n = 0) {
    std::vector<uint8_t> b;
    b.insert(b.end(), p.dmac.begin(), p.dmac.end());
    put16(b, p.sport); put16(b, p.dport);
    b.push_back(flags);
    put32(b, seq); put32(b, ack); put32(b, p.cid);
    if (n) b.insert(b.end(), data, data + n);
    send_to_mac(p.dmac, T_PTP, b);
}

void handle_ptp(const Mac& src, const uint8_t* body, size_t len) {
    if (len < 6 + 2 + 2 + 1 + 4 + 4 + 4) return;
    Mac dst; std::memcpy(dst.data(), body, 6);
    if (dst != g_mac) return;
    if (!same_group(src)) return;
    uint16_t sport = get16(body + 6), dport = get16(body + 8);  // sender's sport -> our dport match
    uint8_t flags = body[10];
    uint32_t seq = get32(body + 11), ack = get32(body + 15), cid = get32(body + 19);
    const uint8_t* data = body + 23;
    size_t n = len - 23;
    int64_t t = now_us();

    if (flags == P_SYN) {
        // Like TCP: every SYN with a new connection id is a new connection and gets its own
        // child socket. A repeated SYN (same id) only repeats the SYNACK.
        for (auto& [id, c] : g_ptp)
            if (c.s == Ptp::ESTAB && c.parent && c.cid == cid && c.dmac == src) {
                ptp_send_pkt(c, P_SYNACK, 0, 0);
                return;
            }
        for (auto& [id, l] : g_ptp) {
            if (l.s != Ptp::LISTEN || l.sport != dport) continue;
            Ptp c;
            c.id = g_next_ptp++;
            c.s = Ptp::ESTAB; c.smac = g_mac; c.dmac = src; c.sport = dport; c.dport = sport;
            c.bufsize = l.bufsize; c.rexmt_us = l.rexmt_us; c.rexmt_cnt = l.rexmt_cnt;
            c.parent = l.id; c.cid = cid; c.last_progress = t;
            int child = c.id, lid = l.id;
            ptp_send_pkt(c, P_SYNACK, 0, 0);
            g_ptp[child] = std::move(c);
            g_ptp[lid].accept_q.push_back(child);
            ALOG("ptp %d: incoming connection from %s:%u -> child %d", lid, mac_str(src).c_str(), sport, child);
            return;
        }
        // Nobody listens: refuse, as a closed TCP port does. The game reopens and retries.
        Ptp tmp; tmp.smac = g_mac; tmp.dmac = src; tmp.sport = dport; tmp.dport = sport; tmp.cid = cid;
        ptp_send_pkt(tmp, P_RST, 0, 0);
        return;
    }

    // Every other packet belongs to exactly one connection, found by its id.
    Ptp* p = nullptr;
    for (auto& [id, q] : g_ptp)
        if (q.cid == cid && q.dmac == src && q.sport == dport && q.dport == sport && q.s != Ptp::LISTEN) { p = &q; break; }
    if (!p) {
        if (flags == P_DATA) {  // stale stream; tell the peer
            Ptp tmp; tmp.smac = g_mac; tmp.dmac = src; tmp.sport = dport; tmp.dport = sport; tmp.cid = cid;
            ptp_send_pkt(tmp, P_RST, 0, 0);
        }
        return;
    }
    switch (flags) {
    case P_SYNACK:
        if (p->s == Ptp::SYN_SENT) { p->s = Ptp::ESTAB; p->last_progress = t; ALOG("ptp %d connected", p->id); }
        break;
    case P_DATA:
        if (p->s != Ptp::ESTAB) break;
        if (seq == p->rcv_next) {
            p->rxq.insert(p->rxq.end(), data, data + n);
            p->rcv_next += (uint32_t)n;
        }
        ptp_send_pkt(*p, P_ACK, 0, p->rcv_next);
        break;
    case P_ACK: {
        if (p->s != Ptp::ESTAB) break;
        uint32_t acked = ack - p->snd_una;
        if (acked > 0 && acked <= p->sendq.size()) {
            p->sendq.erase(p->sendq.begin(), p->sendq.begin() + acked);
            p->snd_una = ack;
            if ((int32_t)(p->snd_next - p->snd_una) < 0) p->snd_next = p->snd_una;
            p->last_progress = t; p->retries = 0;
        }
        break;
    }
    case P_FIN:
        p->fin_rx = true;
        break;
    case P_RST:
        p->fin_rx = true;
        p->s = (p->s == Ptp::SYN_SENT) ? Ptp::REFUSED : Ptp::CLOSED;
        break;
    }
}

// ---- beacons / packet intake ----
void handle_beacon(const Mac& src, const sockaddr_in& from, const uint8_t* body, size_t len) {
    if (len < 32 + 8 + 2) return;   // older beacons end here; newer ones append a peer list
    Peer& p = g_peers[src];
    bool fresh = p.last_seen == 0;
    p.addr = from;
    std::memcpy(p.nick, body, 32); p.nick[31] = 0;
    std::memcpy(p.group, body + 32, 8);
    p.in_group = body[40] != 0;
    p.fps = body[41];
    p.last_seen = now_us();
    g_gossip.erase(addr_key(from));   // it answered: now a real peer
    if (!g_relay_mode && len >= 43) {
        int n = body[42];
        for (int i = 0; i < n && 43 + (size_t)(i + 1) * 6 <= len; i++) {
            sockaddr_in a{}; a.sin_family = AF_INET;
            std::memcpy(&a.sin_addr.s_addr, body + 43 + i * 6, 4);
            std::memcpy(&a.sin_port, body + 43 + i * 6 + 4, 2);
            uint16_t port = ntohs(a.sin_port);
            if (!port || a.sin_addr.s_addr == 0 || a.sin_addr.s_addr == htonl(INADDR_BROADCAST)) continue;
            bool known = false;
            for (auto& [m2, p2] : g_peers) if (addr_key(p2.addr) == addr_key(a)) { known = true; break; }
            if (known || (int)g_gossip.size() >= MAX_GOSSIP) continue;
            g_gossip.emplace(addr_key(a), now_us() + 15000000);   // probe for 15 s
        }
    }
    {   // the game module's bytes: after the peer list, 'G' 'D' <len> <bytes> (absent in older beacons)
        const size_t at = len >= 43 ? 43 + (size_t)body[42] * 6 : len;
        p.game_len = 0;
        if (at + 3 <= len && body[at] == 'G' && body[at + 1] == 'D') { const size_t n = std::min<size_t>(body[at + 2], sizeof p.game_data); if (at + 3 + n <= len) { std::memcpy(p.game_data, body + at + 3, n); p.game_len = (uint8_t)n; } }
    }
    if (fresh) ANOTE("peer %s (\"%s\") at %s:%u", mac_str(src).c_str(), p.nick, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
}

struct Reasm {
    uint8_t count = 0, type = 0;
    uint32_t got = 0;
    std::vector<std::vector<uint8_t>> parts;
    int64_t first = 0;
};
std::map<std::pair<Mac, uint32_t>, Reasm> g_reasm;

void dispatch_packet(uint8_t type, const Mac& src, const sockaddr_in& from, const uint8_t* body, size_t blen);

void handle_frag(const Mac& src, const sockaddr_in& from, const uint8_t* body, size_t len) {
    if (len < 6 + 4 + 1 + 1 + 1) return;
    Mac dst; std::memcpy(dst.data(), body, 6);
    if (dst != g_mac && !is_bcast(dst)) return;
    const uint32_t id = get32(body + 6);
    const uint8_t idx = body[10], count = body[11], type = body[12];
    if (count == 0 || count > MAX_FRAGS || idx >= count || type < T_PDP || type > T_PTP) return;
    if (g_reasm.size() > 64) return;   // bounded memory
    Reasm& r = g_reasm[{src, id}];
    if (r.count == 0) { r.count = count; r.type = type; r.parts.resize(count); r.first = now_us(); }
    if (r.count != count || r.parts[idx].size()) return;
    r.parts[idx].assign(body + 13, body + len);
    if (r.parts[idx].empty()) return;
    if (++r.got < r.count) return;
    std::vector<uint8_t> whole;
    for (auto& part : r.parts) whole.insert(whole.end(), part.begin(), part.end());
    const uint8_t t = r.type;
    g_reasm.erase({src, id});
    dispatch_packet(t, src, from, whole.data(), whole.size());
}

void dispatch_packet(uint8_t type, const Mac& src, const sockaddr_in& from, const uint8_t* body, size_t blen) {
    switch (type) {
    case T_BEACON: handle_beacon(src, from, body, blen); break;
    case T_PDP: handle_pdp(src, body, blen); break;
    case T_MATCH: handle_match(src, body, blen); break;
    case T_PTP: handle_ptp(src, body, blen); break;
    case T_FRAG: handle_frag(src, from, body, blen); break;
    }
}

// Test option: artificial network delay on what this instance receives (PSPRECOMP_NET_LAG_MS=<one-way ms>,
// PSPRECOMP_NET_JITTER_MS=<extra 0..n ms per packet>, PSPRECOMP_NET_LOSS=<percent dropped>). Game traffic only (PDP, matching,
// PTP and their fragments); beacons and the relay handshake are left alone so peers do not time out. Set it on both
// copies for a round trip of twice the delay. Packets with jitter may arrive out of order, as on a real path.
struct Delayed { int64_t due; std::vector<uint8_t> data; sockaddr_in from; };
int g_lag_us = 0, g_jitter_us = 0, g_loss_pct = 0;
uint64_t g_lag_rng = 0x2545F4914F6CDD1Dull;
std::deque<Delayed> g_delayed;   // net thread only
uint32_t lag_rand() { g_lag_rng ^= g_lag_rng << 13; g_lag_rng ^= g_lag_rng >> 7; g_lag_rng ^= g_lag_rng << 17; return (uint32_t)g_lag_rng; }
void load_lag_cfg() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* e = std::getenv("PSPRECOMP_NET_LAG_MS")) g_lag_us = std::max(0, std::atoi(e)) * 1000;
    if (const char* e = std::getenv("PSPRECOMP_NET_JITTER_MS")) g_jitter_us = std::max(0, std::atoi(e)) * 1000;
    if (const char* e = std::getenv("PSPRECOMP_NET_LOSS")) g_loss_pct = std::clamp(std::atoi(e), 0, 100);
    if (g_lag_us || g_jitter_us || g_loss_pct)
        ANOTE("test network: +%d ms one-way, jitter %d ms, loss %d%% on received game packets", g_lag_us / 1000, g_jitter_us / 1000, g_loss_pct);
}
void on_packet_now(const uint8_t* d, size_t n, const sockaddr_in& from);
void on_packet(const uint8_t* d, size_t n, const sockaddr_in& from) {
    const bool game = n > HDR && std::memcmp(d, "DSAH", 4) == 0 && d[4] >= T_PDP && d[4] != T_RELAY && d[4] != T_BEACON;
    if (game && (g_lag_us || g_jitter_us || g_loss_pct)) {
        if (g_loss_pct && (int)(lag_rand() % 100) < g_loss_pct) return;
        const int64_t due = now_us() + g_lag_us + (g_jitter_us ? (int64_t)(lag_rand() % (uint32_t)g_jitter_us) : 0);
        auto at = g_delayed.end();
        while (at != g_delayed.begin() && std::prev(at)->due > due) --at;
        g_delayed.insert(at, Delayed{due, std::vector<uint8_t>(d, d + n), from});
        return;
    }
    on_packet_now(d, n, from);
}
void flush_delayed() {
    const int64_t t = now_us();
    while (!g_delayed.empty() && g_delayed.front().due <= t) {
        Delayed p = std::move(g_delayed.front());
        g_delayed.pop_front();
        on_packet_now(p.data.data(), p.data.size(), p.from);
    }
}
void on_packet_now(const uint8_t* d, size_t n, const sockaddr_in& from) {
    if (n < HDR || std::memcmp(d, "DSAH", 4) != 0) return;
    uint16_t blen = get16(d + 11);
    if (HDR + blen > n) return;
    Mac src; std::memcpy(src.data(), d + 5, 6);
    if (src == g_mac) return;
    const uint8_t* body = d + HDR;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_relay_mode) {   // Internet: only the relay server may talk to us
        const bool from_relay = g_relay_resolved && from.sin_addr.s_addr == g_relay_addr.sin_addr.s_addr && from.sin_port == g_relay_addr.sin_port;
        if (!from_relay) {   // a direct path: a punch from an introduced player, or traffic from the address its punch came from
            auto dit = g_direct.find(src);
            if (dit == g_direct.end() || g_direct_off) return;
            {   // test aid: PSPRECOMP_ADHOC_DIRECT_DROP=<s>: direct packets are ignored from <s> seconds after the first one on (0: all; shows the "via relay" notices)
                static const char* dd = std::getenv("PSPRECOMP_ADHOC_DIRECT_DROP"); static int64_t d0 = 0; if (!d0) d0 = now_us();
                if (dd && now_us() - d0 >= (int64_t)(std::atof(dd) * 1e6)) return;
            }
            Direct& dp = dit->second;
            if (d[4] == T_PUNCH) {
                if (blen < 6 || std::memcmp(body, g_mac.data(), 6) != 0) return;
                if (dp.state != 2) ANOTE("direct: path to %s is up (%s:%u)", mac_str(src).c_str(), inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                if (dp.state != 2) dp.up_at = now_us();
                dp.active = from; dp.state = 2; dp.last_rx = now_us(); dp.reason.clear();
                return;
            }
            if (dp.state != 2 || dp.active.sin_addr.s_addr != from.sin_addr.s_addr || dp.active.sin_port != from.sin_port) return;
            dp.last_rx = now_us();
            if (d[4] == T_RELAY || d[4] == T_BEACON) return;
            dispatch_packet(d[4], src, from, body, blen);
            return;
        }
        g_relay_last_rx = now_us();
        if (d[4] == T_RELAY) {
            if (blen < 1) return;
            if (body[0] == RO_CHALLENGE && blen >= 9) {
                std::memcpy(&g_relay_cookie, body + 1, 8);
                g_relay_hello_next = 0;   // answer at once
            } else if (body[0] == RO_OK && blen >= 3) {
                if (!g_relay_ok) ANOTE("connected to %s", g_relay_host_shown.c_str());
                g_relay_ok = true; g_relay_ok_at = now_us();
                g_relay_players = get16(body + 1);
                if (blen >= 4) g_relay_version = body[3];
            } else if (body[0] == RO_PEER && blen >= 19 && !g_direct_off) {   // introduced: punch to both of its addresses
                Mac m; std::memcpy(m.data(), body + 1, 6);
                Direct& dp = g_direct[m];
                if (dp.state == 2) return;
                auto addr = [](const uint8_t* p) {
                    sockaddr_in a{}; a.sin_family = AF_INET;
                    std::memcpy(&a.sin_addr.s_addr, p, 4);
                    a.sin_port = htons(get16(p + 4));
                    return a;
                };
                dp.pub = addr(body + 7);
                dp.lan = addr(body + 13);
                if (dp.state == 0) { dp.started = now_us(); dp.next_punch = 0; }
                dp.state = 1;
                if (now_us() - dp.asked > 2000000) {   // introduced by the other side's request: send ours too, so the relay learns our LAN
                    dp.asked = now_us();                // address and introduces us again with both (two players behind one router)
                    std::vector<uint8_t> b{RO_INTRO};
                    b.insert(b.end(), m.begin(), m.end());
                    const uint8_t* ip = (const uint8_t*)&g_lan_ip;
                    b.insert(b.end(), ip, ip + 4);
                    put16(b, (uint16_t)g_my_port);
                    send_raw(g_relay_addr, T_RELAY, b);
                }
                char pub[32], lan[32];
                std::snprintf(pub, sizeof pub, "%s:%u", inet_ntoa(dp.pub.sin_addr), ntohs(dp.pub.sin_port));
                std::snprintf(lan, sizeof lan, "%s:%u", inet_ntoa(dp.lan.sin_addr), ntohs(dp.lan.sin_port));
                ANOTE("direct: introduced to %s (%s, LAN %s)", mac_str(m).c_str(), pub, lan);
            }
            return;
        }
    } else if (d[4] == T_RELAY) return;
    dispatch_packet(d[4], src, from, body, blen);
}

// LAN beacons (23 Nov, PR #129: "LAN never finds the other player on the same WiFi; the relay does"): 255.255.255.255 goes out of one
// interface only on Windows (often a virtual adapter: WSL / Hyper-V / VPN) and is dropped by many Wi-Fi access points / phone drivers in power
// save. Every up IPv4 interface's own subnet broadcast (address | ~netmask) is sent to as well, refreshed every 10 s.
// PSPRECOMP_ADHOC_NOSUBNETCAST=1: only 255.255.255.255 (old).
std::vector<uint32_t> lan_broadcasts() {   // network byte order
    static std::vector<uint32_t> cache; static int64_t at = -1;
    const int64_t t = now_us();
    if (at >= 0 && t - at < 10000000) return cache;
    at = t; cache.clear();
    if (std::getenv("PSPRECOMP_ADHOC_NOSUBNETCAST")) return cache;
#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd != INVALID_SOCKET) {
        INTERFACE_INFO info[32]; DWORD got = 0;
        if (WSAIoctl(fd, SIO_GET_INTERFACE_LIST, nullptr, 0, info, sizeof info, &got, nullptr, nullptr) == 0) {
            for (DWORD i = 0; i < got / sizeof(INTERFACE_INFO); i++) {
                const INTERFACE_INFO& f = info[i];
                if (!(f.iiFlags & IFF_UP) || (f.iiFlags & IFF_LOOPBACK)) continue;
                const uint32_t a = f.iiAddress.AddressIn.sin_addr.s_addr, m = f.iiNetmask.AddressIn.sin_addr.s_addr;
                if (!a || !m || m == 0xFFFFFFFFu) continue;
                cache.push_back(a | ~m);
            }
        }
        closesocket(fd);
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* f = ifs; f; f = f->ifa_next) {
            if (!f->ifa_addr || f->ifa_addr->sa_family != AF_INET || !f->ifa_netmask) continue;
            if (!(f->ifa_flags & IFF_UP) || (f->ifa_flags & IFF_LOOPBACK)) continue;
            const uint32_t a = ((sockaddr_in*)f->ifa_addr)->sin_addr.s_addr, m = ((sockaddr_in*)f->ifa_netmask)->sin_addr.s_addr;
            if (!a || !m || m == 0xFFFFFFFFu) continue;
            cache.push_back(a | ~m);
        }
        freeifaddrs(ifs);
    }
#endif
    std::sort(cache.begin(), cache.end()); cache.erase(std::unique(cache.begin(), cache.end()), cache.end());
    static std::string shown;
    std::string now; for (uint32_t b : cache) { in_addr x; x.s_addr = b; now += " "; now += inet_ntoa(x); }
    if (now != shown) { shown = now; ANOTE("LAN broadcast addresses:%s", now.empty() ? " (none found; 255.255.255.255 only)" : now.c_str()); }
    return cache;
}

// ---- periodic work (network thread) ----
void send_beacons() {
    if (g_relay_mode && !g_relay_ok) return;
    std::vector<uint8_t> b(42, 0);
    std::memcpy(b.data(), g_nick, 31);
    std::memcpy(b.data() + 32, g_group, 8);
    b[40] = g_connected ? 1 : 0;
    b[41] = (uint8_t)g_local_fps;
    auto game_trailer = [&]() { if (!g_game_len) return; b.push_back('G'); b.push_back('D'); b.push_back(g_game_len); b.insert(b.end(), g_game_data, g_game_data + g_game_len); };
    if (g_relay_mode) {   // one copy to the server; it fans it out to the room
        b.push_back(0);   // no gossip list
        game_trailer();
        send_raw(g_relay_addr, T_BEACON, b);
        return;
    }
    // gossip: addresses of the peers we hear directly (so everyone learns everyone)
    {
        std::vector<uint8_t> list;
        for (auto& [m, p] : g_peers) {
            if (list.size() >= 6 * 32) break;
            list.insert(list.end(), (const uint8_t*)&p.addr.sin_addr.s_addr, (const uint8_t*)&p.addr.sin_addr.s_addr + 4);
            list.insert(list.end(), (const uint8_t*)&p.addr.sin_port, (const uint8_t*)&p.addr.sin_port + 2);
        }
        b.push_back((uint8_t)(list.size() / 6));
        b.insert(b.end(), list.begin(), list.end());
    }
    game_trailer();
    std::vector<sockaddr_in> targets;
    for (int i = 0; i < g_span; i++) {
        uint16_t port = (uint16_t)(g_base_port + i);
        if (port == g_my_port) continue;
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); targets.push_back(a);
        a.sin_addr.s_addr = htonl(INADDR_BROADCAST); targets.push_back(a);
        for (uint32_t bc : lan_broadcasts()) { a.sin_addr.s_addr = bc; targets.push_back(a); }
    }
    for (auto& a : g_extra_peers) targets.push_back(a);
    // every peer we hear gets our beacon directly too: if only one direction's broadcast gets through, both still find each other
    // (PSPRECOMP_ADHOC_NODIRECTBEACON=1: old)
    static const bool direct_beacon = !std::getenv("PSPRECOMP_ADHOC_NODIRECTBEACON");
    if (direct_beacon) for (auto& [m, pe] : g_peers) if (pe.addr.sin_port && pe.addr.sin_addr.s_addr && pe.addr.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) targets.push_back(pe.addr);
    int64_t now = now_us();
    for (auto it = g_gossip.begin(); it != g_gossip.end();) {
        if (it->second < now) { it = g_gossip.erase(it); continue; }
        sockaddr_in a{}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl((uint32_t)(it->first >> 16)); a.sin_port = htons((uint16_t)(it->first & 0xffff));
        targets.push_back(a);
        ++it;
    }
    for (auto& a : targets) send_raw(a, T_BEACON, b);
}

// ---- what the player is told about the network (27 Nov, tree ttnetmsg; PSPRECOMP_ADHOC_MSG_OLD=1: the short old texts) ----
// Carrier-grade NAT: a mobile / carrier network gives many customers one public address and its NAT never lets an unknown
// sender in, so the hole punch cannot work (T-Mobile: 172.56.0.0/13; shared address space: 100.64.0.0/10).
bool msg_old() { static const bool o = std::getenv("PSPRECOMP_ADHOC_MSG_OLD") != nullptr; return o; }
bool ip_cgnat(uint32_t h) { return (h >> 22) == (0x64400000u >> 22) || (h >> 19) == (0xAC380000u >> 19); }   // 100.64.0.0/10, 172.56.0.0/13
bool ip_private(uint32_t h) { return (h >> 24) == 10 || (h >> 20) == (0xAC100000u >> 20) || (h >> 16) == 0xC0A8; }   // 10/8, 172.16/12, 192.168/16
// Test aid PSPRECOMP_ADHOC_FAKE_NAT=a.b.c.d: that address stands in for every introduced player's public address in the messages.
uint32_t fake_nat_ip() {
    static uint32_t ip = [] { const char* e = std::getenv("PSPRECOMP_ADHOC_FAKE_NAT"); in_addr a{}; return e && inet_pton(AF_INET, e, &a) == 1 ? ntohl(a.s_addr) : 0u; }();
    return ip;
}
// "Ender's network (mobile / carrier-grade NAT) blocks a direct connection" for a failed punch; empty = no known cause.
std::string nat_cause(const Direct& dp, const char* who) {
    const uint32_t pub = fake_nat_ip() ? fake_nat_ip() : ntohl(dp.pub.sin_addr.s_addr), lan = ntohl(dp.lan.sin_addr.s_addr);
    const uint32_t mine = ntohl(g_lan_ip);
    if (mine && ip_cgnat(mine)) return "Your network (mobile / carrier-grade NAT) blocks a direct connection";
    if (pub && ip_cgnat(pub)) return std::string(who) + "'s network (mobile / carrier-grade NAT) blocks a direct connection";
    if (pub && ip_private(pub)) return std::string(who) + "'s network (behind a carrier NAT) blocks a direct connection";
    if (pub && !ip_private(pub) && (lan >> 24) == 10 && lan != pub) return std::string(who) + "'s network (probably mobile / carrier-grade NAT: a 10.x address behind a public one) blocks a direct connection";
    return "";
}
// The Network Delay tip for a slow path (ping over 120 ms): what the setting is and what this ping needs.
std::string delay_tip(int ping, int jitter) {
    const int need = psp_netplay_delay_for(ping, std::max(0, jitter));
    char b[200];
    const int set = psp_netplay_delay_setting();
    if (set == PSP_NETPLAY_AUTO_DELAY) std::snprintf(b, sizeof b, "Network Delay is Auto (%d frames now, set at each battle start). If it stalls, raise it in Settings > Network.", psp_netplay_input_delay());
    else std::snprintf(b, sizeof b, "Network Delay is %d frame(s); this ping needs about %d. Raise it in Settings > Network (before the next session).", set, need);
    return b;
}

// What the player is told about each other player of the group's connection: direct (LAN, or an Internet path that was punched) or through the relay and why;
// again when it switches. Called once a second from net_check (g_mu held).
void conn_toasts(int64_t t) {
    for (auto& [mac, p] : g_peers) {
        if (!same_group(mac)) continue;
        ConnTold& ct = g_conn_told[mac];
        if (!ct.since) ct.since = t;
        const char* who = p.nick[0] ? p.nick : "a player";
        const std::string id = mac_str(mac);
        auto pit = g_ping.find(mac);
        const double ping = pit != g_ping.end() && pit->second.ever ? pit->second.srtt_ms : -1;
        char pg[32] = ""; if (ping >= 0) std::snprintf(pg, sizeof pg, ", ping %d ms", (int)ping);
        if (!g_relay_mode) {   // LAN: every player is reached directly
            if (ct.mode != 1 && (ping >= 0 || t - ct.since > 3000000)) { ct.mode = 1; psp_net_notice(("conn:" + id).c_str(), 1, "Connected directly to %s (LAN%s)", who, pg); }
            continue;
        }
        auto dit = g_direct.find(mac);
        Direct* dp = dit == g_direct.end() ? nullptr : &dit->second;
        if (dp && dp->state == 2) {
            if (ct.mode != 1 && (ping >= 0 || t - dp->up_at > 2500000)) {
                ct.mode = 1;
                const bool lan = dp->active.sin_addr.s_addr == dp->lan.sin_addr.s_addr && dp->lan.sin_addr.s_addr && dp->lan.sin_addr.s_addr != dp->pub.sin_addr.s_addr;
                psp_net_notice(("conn:" + id).c_str(), 1, "Connected directly to %s (%s%s)", who, lan ? "same network" : "Internet", pg);
            }
            continue;
        }
        std::string why;
        if (g_direct_off) why = "direct connections are turned off";
        else if (g_relay_ok && g_relay_version < 2) why = "server too old for direct connections";
        else if (dp && !dp->reason.empty()) why = dp->reason;
        else if (ct.mode == 0 && t - ct.since > 25000000) why = "no introduction from the server (peer in LAN mode / old version?)";
        if (!why.empty() && ct.mode != 2 && t - ct.since > 1000000) {
            if (msg_old()) { ct.mode = 2; psp_net_notice(("conn:" + id).c_str(), 1, "Playing via relay with %s: %s", who, why.c_str()); continue; }
            if (ping < 0 && t - ct.since < 4000000) continue;   // a second more for the ping through the relay
            ct.mode = 2;
            std::string cause = dp && dp->state == 0 && dp->started ? nat_cause(*dp, who) : "";
            if (cause.empty()) cause = std::string("No direct connection to ") + who + ": " + why + ".";
            else cause += ",\nso the game goes through the relay.";
            const int jit = pit != g_ping.end() ? (int)(pit->second.var_ms + 0.5) : 0;
            std::string m = std::string("Playing through the relay with ") + who + (ping >= 0 ? std::string(" (ping ") + std::to_string((int)ping) + " ms)" : "") + "\n" + cause + "\nThe relay is a detour: higher ping than a direct link.";
            if (ping > 120) m += "\n" + delay_tip((int)ping, jit);
            psp_net_notice(("conn:" + id).c_str(), 60, "%s", m.c_str());
        }
    }
    for (auto it = g_conn_told.begin(); it != g_conn_told.end();)
        if (!g_peers.count(it->first) || !same_group(it->first)) it = g_conn_told.erase(it); else ++it;
}

// Once a second (network thread, g_mu held): pings to the players in our group, and what the player is told about network trouble.
void net_check(int64_t t) {
    // the party server
    if (g_relay_mode && g_ctl_init && !g_relay_ok && t - g_ctl_init_at > 6000000)
        psp_net_notice("server", 30, "Network: no answer from the party server %s. Check your internet connection or the Server setting.", g_cfg_server.c_str());
    if (g_relay_mode && g_relay_ok && g_relay_last_rx && t - g_relay_last_rx > 6000000)
        psp_net_notice("server_lost", 30, "Network: lost the connection to the party server (nothing for %d s).", (int)((t - g_relay_last_rx) / 1000000));
    if (!g_connected) { g_ping.clear(); g_direct.clear(); g_conn_told.clear(); return; }
    if (g_relay_mode && g_relay_ok && g_relay_version >= 2 && !g_direct_off) {   // ask the relay to introduce us to the others
        if (!g_lan_ip) {   // our address toward the relay (the route's source address; no packet is sent)
            const int fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
            if (fd >= 0) {
                if (connect(fd, (const sockaddr*)&g_relay_addr, sizeof g_relay_addr) == 0) {
                    sockaddr_in me{}; socklen_t ml = sizeof me;
                    if (getsockname(fd, (sockaddr*)&me, &ml) == 0) g_lan_ip = me.sin_addr.s_addr;
                }
                psp_plat::close_socket(fd);
            }
        }
        for (auto& [mac, p] : g_peers) {
            if (!same_group(mac)) continue;
            Direct& dp = g_direct[mac];
            if (dp.state == 0 && t - dp.asked > 15000000) {
                dp.asked = t;
                std::vector<uint8_t> b{RO_INTRO};
                b.insert(b.end(), mac.begin(), mac.end());
                const uint8_t* ip = (const uint8_t*)&g_lan_ip;
                b.insert(b.end(), ip, ip + 4);
                put16(b, (uint16_t)g_my_port);
                send_raw(g_relay_addr, T_RELAY, b);
            }
        }
    }
    for (auto it = g_direct.begin(); it != g_direct.end();)   // players who left
        if (!g_peers.count(it->first) || !same_group(it->first)) it = g_direct.erase(it); else ++it;
    conn_toasts(t);
    for (auto it = g_ping.begin(); it != g_ping.end();)   // forget players who left the group
        if (!g_peers.count(it->first) || !same_group(it->first)) it = g_ping.erase(it); else ++it;
    for (auto& [mac, p] : g_peers) {
        if (!same_group(mac)) continue;
        const char* who = p.nick[0] ? p.nick : "a player";
        const std::string id = mac_str(mac);
        if (t - p.last_seen > 2500000)
            psp_net_notice(("silent:" + id).c_str(), 15, "Network: nothing from %s for %d s (unstable connection)", who, (int)((t - p.last_seen) / 1000000));
        PingStat& st = g_ping[mac];
        for (auto o = st.outstanding.begin(); o != st.outstanding.end();)   // unanswered for 2 s: lost
            if (t - o->second > 2000000) {
                if (st.ever) { st.window.push_back(false); if (st.window.size() > 20) st.window.pop_front(); }
                o = st.outstanding.erase(o);
            } else ++o;
        std::vector<uint8_t> b(mac.begin(), mac.end());
        put16(b, PING_PORT); put16(b, PING_PORT);
        b.push_back(0); put32(b, ++st.seq);
        for (int i = 0; i < 8; i++) b.push_back(0);
        st.outstanding[st.seq] = t; if (st.ever) st.asked++;   // (counted once the player has answered a first time)
        send_to_mac(mac, T_PDP, b);
        if (!st.ever) continue;
        if (msg_old()) st.high = st.srtt_ms > 150 ? st.high + 1 : 0;
        if (msg_old()) { if (st.high >= 3) psp_net_notice(("lat:" + id).c_str(), 30, "Network: high latency to %s: %d ms", who, (int)st.srtt_ms); }
        else {   // over 120 ms for 3 s: the ping, which path it takes, and the Network Delay tip
            st.high = st.srtt_ms > 120 ? st.high + 1 : 0;
            if (st.high >= 3) {
                auto dit = g_direct.find(mac);
                const bool direct = !g_relay_mode || (dit != g_direct.end() && dit->second.state == 2);
                const std::string tip = delay_tip((int)st.srtt_ms, (int)(st.var_ms + 0.5));
                psp_net_notice(("lat:" + id).c_str(), 45, "Ping to %s is %d ms (%s). Fights may lag or wait for input.\n%s",
                               who, (int)st.srtt_ms, direct ? "direct" : g_relay_mode ? "through the relay" : "LAN", tip.c_str());
            }
        }
        if (st.window.size() >= 10) {
            int lost = 0;
            for (bool ok : st.window) lost += !ok;
            const int pct = lost * 100 / (int)st.window.size();
            if (pct >= 15) psp_net_notice(("loss:" + id).c_str(), 30, "Network: packet loss to %s: %d%%", who, pct);
        }
    }
}

void periodic() {
    int64_t t = now_us();
    std::lock_guard<std::mutex> lk(g_mu);
    // expire peers
    for (auto it = g_peers.begin(); it != g_peers.end();) {
        if (t - it->second.last_seen > 4000000) {
            ANOTE("peer %s left", mac_str(it->first).c_str());
            if (g_connected && same_group(it->first))
                psp_net_notice(("left:" + mac_str(it->first)).c_str(), 10, "Network: lost %s (nothing received for 4 s)", it->second.nick[0] ? it->second.nick : "a player");
            it = g_peers.erase(it);
        } else ++it;
    }
    if (g_scan_due && t >= g_scan_due) { g_scan_due = 0; queue_ctl(CTL_SCAN, 0); }
    if (t >= g_next_net_check) { g_next_net_check = t + 1000000; net_check(t); }
    for (auto& [mac, dp] : g_direct) {   // punch while trying, keep the path (and the routers' mappings) alive once up
        if (dp.state == 0 || t < dp.next_punch) continue;
        if (dp.state == 1 && t - dp.started > 8000000) {
            char pub[32], lan[32];
            std::snprintf(pub, sizeof pub, "%s:%u", inet_ntoa(dp.pub.sin_addr), ntohs(dp.pub.sin_port));
            std::snprintf(lan, sizeof lan, "%s:%u", inet_ntoa(dp.lan.sin_addr), ntohs(dp.lan.sin_port));
            (void)lan; dp.reason = std::string("no reply from ") + pub + " in 8 s (NAT/firewall, yours or theirs)";
            ANOTE("direct: no answer from %s; staying on the relay", mac_str(mac).c_str());
            dp.state = 0;
            continue;
        }
        if (dp.state == 2 && t - dp.last_rx > 3000000) {
            dp.reason = "direct path silent for 3 s (NAT mapping dropped?)";
            ANOTE("direct: path to %s went quiet; back to the relay", mac_str(mac).c_str());
            dp.state = 0;
            continue;
        }
        dp.next_punch = t + (dp.state == 1 ? 100000 : 500000);
        std::vector<uint8_t> b(mac.begin(), mac.end());
        if (dp.state == 2) send_raw(dp.active, T_PUNCH, b);
        else {
            if (dp.pub.sin_port) send_raw(dp.pub, T_PUNCH, b);
            if (dp.lan.sin_port && dp.lan.sin_addr.s_addr && dp.lan.sin_addr.s_addr != dp.pub.sin_addr.s_addr) send_raw(dp.lan, T_PUNCH, b);
            // A NAT that gives every destination its own port (symmetric) maps the other side to a port the relay never saw. Routers usually hand
            // those out in sequence, so after the first 1.5 s the punches also go to the ports next to the reported one (a few per round).
            if (dp.pub.sin_port && t - dp.started > 1500000) {
                static const int offs[] = {1, 2, 3, 4, 5, 6, 7, 8, -1, -2, -3, -4, 9, 10, 11, 12, 13, 14, 15, 16};
                for (int k = 0; k < 4; k++) {
                    const int o = offs[(dp.sweep * 4 + k) % (sizeof offs / sizeof offs[0])];
                    const int port = (int)ntohs(dp.pub.sin_port) + o;
                    if (port < 1024 || port > 65535) continue;
                    sockaddr_in a = dp.pub; a.sin_port = htons((uint16_t)port);
                    send_raw(a, T_PUNCH, b);
                }
                dp.sweep++;
            }
        }
    }
    for (auto it = g_reasm.begin(); it != g_reasm.end();)   // drop half-received messages
        if (t - it->second.first > 3000000) it = g_reasm.erase(it); else ++it;
    // matching timers
    for (auto& [id, m] : g_match) {
        if (!m.started) continue;
        if ((m.mode == 1 || m.mode == 3) && t >= m.next_hello) {
            m.next_hello = t + std::clamp<int64_t>(m.hello_us, 200000, 1000000);
            if (g_connected) match_send(m, Mac{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, MO_HELLO, m.hello_opt);
        }
        if (t >= m.next_ping) {
            // Keep-alive to every peer we have a request or a link with. A pending request (REQ_OF / REQ_BY / ACCEPTED) stays
            // until it is accepted or cancelled, as on a PSP, so both sides ping it too; extra = 1 marks "I hold you as pending".
            m.next_ping = t + 1000000;
            // Pending requests are pinged too: a room's host (Group Battle) leaves a join request open until it starts the
            // battle, and without traffic both sides dropped the request after the 6 s timeout below.
            for (auto& [mac, p] : m.peers)
                if (p.s != MPeer::HELLO) match_send(m, mac, MO_PING, {}, p.s == MPeer::ESTAB ? 0 : 1);
        }
        int64_t timeout = 6000000;  // the game's ping_delay is far longer than a LAN needs
        for (auto it = m.peers.begin(); it != m.peers.end();) {
            MPeer& p = it->second;
            bool drop = false;
            if (p.s == MPeer::HELLO) drop = t - p.last_seen > 6000000;  // host vanished
            else if (p.s != MPeer::ESTAB) {
                // A join request waiting for the host's answer (Group Battle: the host takes everyone in at once, often long after
                // they asked) sends nothing while it waits: it is dropped only when the player itself is gone (its beacons stop),
                // not after the established peers' silence timeout. Dropping it after 6 s made the host's accept fail and the game
                // delete the room ("connection error") whenever the room took longer than that to fill.
                drop = !g_peers.count(it->first);
            } else if (t - p.last_seen > timeout) {
                if (p.s == MPeer::ESTAB) queue_match(m, M_TIMEOUT, it->first, {});
                drop = true;
            }
            if (drop) it = m.peers.erase(it); else ++it;
        }
    }
    // ptp
    for (auto& [id, p] : g_ptp) {
        if (p.s == Ptp::SYN_SENT && t - p.last_syn > p.rexmt_us) {
            if (++p.retries > std::max(p.rexmt_cnt, 5)) { p.s = Ptp::REFUSED; continue; }
            p.last_syn = t;
            ptp_send_pkt(p, P_SYN, p.cid, 0);
        }
        if (p.s == Ptp::ESTAB) {
            uint32_t sent = p.snd_next - p.snd_una;
            if (!p.sendq.empty() && t - p.last_progress > p.rexmt_us) {
                p.snd_next = p.snd_una; sent = 0; p.last_progress = t;  // go back N
            }
            const uint32_t window = 32 * 1200;
            while (sent < p.sendq.size() && sent < window) {
                size_t n = std::min<size_t>(1200, p.sendq.size() - sent);
                std::vector<uint8_t> chunk(n);
                for (size_t i = 0; i < n; i++) chunk[i] = p.sendq[sent + i];
                ptp_send_pkt(p, P_DATA, p.snd_una + sent, 0, chunk.data(), n);
                sent += (uint32_t)n;
            }
            p.snd_next = p.snd_una + sent;
        }
    }
}

void apply_pending_config();
void relay_service();

void net_thread() {
    load_lag_cfg();
    int64_t next_beacon = 0, next_tick = 0;
    uint8_t buf[65536];
    while (!g_stop.load()) {
        pollfd pfd{}; pfd.fd = g_sock; pfd.events = POLLIN;
        int wait_ms = 5;
        if (!g_delayed.empty()) wait_ms = (int)std::clamp<int64_t>((g_delayed.front().due - now_us()) / 1000, 0, 5);
        int r = psp_plat::poll_(&pfd, 1, wait_ms);
        if (r > 0 && (pfd.revents & POLLIN)) {
            for (int i = 0; i < 64; i++) {
                sockaddr_in from{}; socklen_t fl = sizeof from;
                ssize_t n = recvfrom(g_sock, reinterpret_cast<char*>(buf), (int)sizeof buf, psp_plat::kMsgDontWait, (sockaddr*)&from, &fl);
                if (n <= 0) break;
                on_packet(buf, (size_t)n, from);
            }
        }
        flush_delayed();
        int64_t t = now_us();
        if (t >= next_beacon) {
            next_beacon = t + 500000;
            std::lock_guard<std::mutex> lk(g_mu);
            send_beacons();
        }
        { static const char* sw = std::getenv("PSPRECOMP_ADHOC_TEST_TOLAN"); static int64_t sw_at = sw ? t + (int64_t)(std::atof(sw) * 1e6) : -1;   // test aid: Settings > Network = LAN after <s> seconds
          if (sw_at >= 0 && t >= sw_at) { sw_at = -1; psp_adhoc_configure(false, "", "", ""); } }
        if (t >= next_tick) { next_tick = t + 10000; apply_pending_config(); relay_service(); periodic(); }
    }
}

void load_cfg_from_env() {
    std::call_once(g_cfg_once, [] {
        const char* m = std::getenv("PSPRECOMP_ADHOC_MODE");
        const char* pe = std::getenv("PSPRECOMP_ADHOC_PEERS");
        if (m && *m) g_relay_mode = !(m[0] == 'l' || m[0] == 'L');   // "lan" / "internet"
        else if (pe && *pe) g_relay_mode = false;                    // listing peers means a direct (LAN) setup
        if (const char* sv = std::getenv("PSPRECOMP_ADHOC_SERVER")) if (*sv) g_cfg_server = sv;
        if (const char* r = std::getenv("PSPRECOMP_ADHOC_ROOM")) g_cfg_room = r;
        if (pe) g_cfg_peers = pe;
    });
}

std::vector<sockaddr_in> parse_peers_list(const std::string& str);

// Network thread: applies a configuration change made from the settings menu.
void apply_pending_config() {
    std::string peers; bool relay;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_cfg_dirty) return;
        g_cfg_dirty = false;
        peers = g_cfg_peers; relay = g_relay_mode;
        if (g_relay_ok) {   // tell the old server we are leaving
            std::vector<uint8_t> bye(1, RO_BYE);
            send_raw(g_relay_addr, T_RELAY, bye);
        }
        g_peers.clear(); g_gossip.clear(); g_extra_peers.clear();
        g_relay_resolved = g_relay_resolve_failed = g_relay_ok = false;
        g_relay_cookie = 0; g_relay_players = 0; g_relay_hello_next = 0; g_relay_resolve_next = 0;
        for (auto& [id, m] : g_match) m.peers.clear();
        ANOTE("network: %s%s", relay ? "Internet via " : "LAN", relay ? g_cfg_server.c_str() : "");
    }
    // Internet -> LAN (23 Nov, PR #129 RP6 log: Settings > Network switched to LAN in a running game, "no peer ever found"): an Internet-mode socket is
    // bound to a port the OS picked (all traffic went to the server), but LAN beacons are sent to the well-known ports g_base_port.. only, so
    // nobody heard us and we heard nobody. The socket is bound again to the first free well-known port. PSPRECOMP_ADHOC_NOREBIND=1: old.
    if (!relay && g_sock >= 0 && (g_my_port < g_base_port || g_my_port >= g_base_port + g_span) && !std::getenv("PSPRECOMP_ADHOC_NOREBIND")) {
        const int ns = (int)socket(AF_INET, SOCK_DGRAM, 0);
        uint16_t np = 0;
        if (ns >= 0) {
            int one = 1;
            setsockopt(ns, SOL_SOCKET, SO_BROADCAST, PSP_SOCKOPT_CAST(&one), sizeof one);
            psp_plat::set_nonblocking(ns);
            for (int i = 0; i < g_span && !np; i++) {
                sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)(g_base_port + i));
                if (bind(ns, (sockaddr*)&a, sizeof a) == 0) np = (uint16_t)(g_base_port + i);
            }
        }
        if (np) {
            int old;
            { std::lock_guard<std::mutex> lk(g_mu); old = g_sock; g_sock = ns; ANOTE("LAN: socket moved from UDP %u to %u (the LAN ports %d-%d)", g_my_port, np, g_base_port, g_base_port + g_span - 1); g_my_port = np; }
            psp_plat::close_socket(old);
        } else {
            if (ns >= 0) psp_plat::close_socket(ns);
            ANOTE("LAN: could not bind any of UDP %d-%d; staying on %u (other players will not find this one by broadcast)", g_base_port, g_base_port + g_span - 1, g_my_port);
        }
    }
    std::vector<sockaddr_in> ps;
    if (!relay && !peers.empty()) ps = parse_peers_list(peers);   // DNS lookups happen without the lock
    psp_upnp_port(relay ? 0 : (uint16_t)g_my_port);
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_relay_mode) g_extra_peers = ps;
}

// Network thread: keeps the registration with the relay server alive (resolve, hello, cookie).
void relay_service() {
    std::string server, room; bool need_resolve = false;
    const int64_t t = now_us();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_relay_mode) return;
        server = g_cfg_server; room = g_cfg_room;
        need_resolve = (!g_relay_resolved && t >= g_relay_resolve_next) ||
                       (g_relay_resolved && t - g_relay_resolved_at > 300000000);
    }
    if (need_resolve) {
        std::string host = server, port = "27500";
        size_t c = server.rfind(':');
        if (c != std::string::npos) { host = server.substr(0, c); port = server.substr(c + 1); }
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);   // may block: no lock held
        std::lock_guard<std::mutex> lk(g_mu);
        if (rc == 0 && res) {
            sockaddr_in a = *(sockaddr_in*)res->ai_addr;
            if (!g_relay_resolved || a.sin_addr.s_addr != g_relay_addr.sin_addr.s_addr || a.sin_port != g_relay_addr.sin_port) {
                g_relay_addr = a; g_relay_ok = false; g_relay_cookie = 0;
                ANOTE("relay %s is %s:%u", host.c_str(), inet_ntoa(a.sin_addr), ntohs(a.sin_port));
            }
            g_relay_resolved = true; g_relay_resolve_failed = false;
            g_relay_resolved_at = now_us();
            g_relay_host_shown = host;
        } else {
            if (!g_relay_resolve_failed) ANOTE("cannot look up %s: %s", host.c_str(), gai_strerror(rc));
            g_relay_resolve_failed = true;
            g_relay_resolve_next = now_us() + 5000000;
        }
        if (res) freeaddrinfo(res);
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_relay_mode || !g_relay_resolved) return;
    if (g_relay_ok && t - g_relay_ok_at > 5000000) { g_relay_ok = false; ANOTE("relay stopped answering"); }
    if (t >= g_relay_hello_next) {
        g_relay_hello_next = t + 1000000;
        std::vector<uint8_t> h(1 + 32 + 8, 0);
        h[0] = RO_HELLO;
        std::memcpy(h.data() + 1, room.data(), std::min<size_t>(room.size(), 31));
        std::memcpy(h.data() + 33, &g_relay_cookie, 8);
        send_raw(g_relay_addr, T_RELAY, h);
    }
}

std::vector<sockaddr_in> parse_peers_list(const std::string& str) {
    std::vector<sockaddr_in> out;
    size_t pos = 0;
    while (pos < str.size()) {
        size_t e = str.find(',', pos);
        if (e == std::string::npos) e = str.size();
        std::string item = str.substr(pos, e - pos);
        pos = e + 1;
        if (item.empty()) continue;
        int port = -1;
        size_t c = item.rfind(':');
        std::string host = item;
        if (c != std::string::npos) { port = std::atoi(item.c_str() + c + 1); host = item.substr(0, c); }
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
            ANOTE("cannot resolve peer \"%s\"", host.c_str());
            continue;
        }
        sockaddr_in a = *(sockaddr_in*)res->ai_addr;
        freeaddrinfo(res);
        a.sin_family = AF_INET;
        if (port > 0) { a.sin_port = htons((uint16_t)port); out.push_back(a); }
        else for (int i = 0; i < g_span; i++) { a.sin_port = htons((uint16_t)(g_base_port + i)); out.push_back(a); }
        ANOTE("extra peer %s%s", host.c_str(), port > 0 ? "" : " (ports scan)");
    }
    return out;
}

// Starts the socket and thread on first use. Returns false when networking is off or failed.
bool ensure_net() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* en = std::getenv("PSPRECOMP_ADHOC");
        if (en && en[0] == '0') { g_enabled = false; return; }
        if (const char* dr = std::getenv("PSPRECOMP_ADHOC_DIRECT"); dr && dr[0] == '0') g_direct_off = true;
        const char* lg = std::getenv("PSPRECOMP_ADHOC_LOG");
        g_log = lg && (lg[0] == '1' || lg[0] == '2');
        g_trace = lg && lg[0] == '2';
        if (const char* bp = std::getenv("PSPRECOMP_ADHOC_PORT")) g_base_port = std::atoi(bp);
        load_cfg_from_env();
        psp_plat::net_init();
        g_sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
        if (g_sock < 0) { ANOTE("socket() failed; ad-hoc networking off"); g_off_reason = "Off: cannot open a socket (Android: INTERNET permission missing?)"; g_enabled = false; return; }
        int one = 1;
        setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, PSP_SOCKOPT_CAST(&one), sizeof one);
        psp_plat::set_nonblocking(g_sock);
#ifdef __ANDROID__
        psp_android_wifi_multicast_lock();   // LAN broadcasts reach us (Wi-Fi power save filters them otherwise)
        psp_android_wifi_low_latency_lock();   // and the radio does not sleep between packets (jitter)
#endif
        if (const char* sp = std::getenv("PSPRECOMP_ADHOC_SPAN")) g_span = std::clamp(std::atoi(sp), 1, 64);
        for (int i = 0; i < g_span && !g_my_port; i++) {
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY);
            // Internet mode needs no well-known port (all traffic goes to the server): let the OS pick.
            a.sin_port = g_relay_mode ? 0 : htons((uint16_t)(g_base_port + i));
            if (bind(g_sock, (sockaddr*)&a, sizeof a) == 0) {
                sockaddr_in got{}; socklen_t gl = sizeof got;
                getsockname(g_sock, (sockaddr*)&got, &gl);
                g_my_port = ntohs(got.sin_port);
            }
        }
        if (const char* up = std::getenv("PSPRECOMP_ADHOC_UPNP"); up && up[0] == '1') psp_upnp_set_enabled(true);
        if (g_my_port) psp_upnp_port(g_relay_mode ? 0 : (uint16_t)g_my_port);   // direct (LAN) play only: Internet goes through the server
        if (!g_my_port) { ANOTE("could not bind UDP ports %d-%d; ad-hoc networking off", g_base_port, g_base_port + g_span - 1); psp_plat::close_socket(g_sock); g_sock = -1; g_off_reason = "Off: could not bind a UDP port"; g_enabled = false; return; }

        const char* mc = std::getenv("PSPRECOMP_ADHOC_MAC");
        unsigned m[6];
        if (mc && std::sscanf(mc, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6)
            for (int i = 0; i < 6; i++) g_mac[i] = (uint8_t)m[i];
        else {
            // A random locally administered MAC per launch. (A host-name hash collides: every Steam
            // Deck is called "steamdeck", and two players with one MAC would kick each other off a relay.)
            std::random_device rd;
            uint32_t r1 = rd(), r2 = rd();
            g_mac = Mac{0x02, (uint8_t)(r1 >> 24), (uint8_t)(r1 >> 16), (uint8_t)(r1 >> 8), (uint8_t)r1, (uint8_t)r2};
        }
        const char* nick = std::getenv("PSPRECOMP_ADHOC_NICK");
        {
            std::lock_guard<std::mutex> lk(g_mu);
            std::snprintf(g_nick, sizeof g_nick, "%s", nick && *nick ? nick : (!g_game_nick.empty() ? g_game_nick.c_str() : "Player"));
        }
        g_cfg_dirty = true;   // the network thread resolves the server / peers
        g_net_ready = true;
        ANOTE("ready: UDP %u, MAC %s, nick \"%s\", %s", g_my_port, mac_str(g_mac).c_str(), g_nick,
              g_relay_mode ? "Internet" : "LAN");
        g_thread = std::thread(net_thread);
        g_thread.detach();
    });
    return g_net_ready;
}

// ---- wait helper for blocking calls ----
// timeout_us == 0 means wait forever. pred is evaluated with g_mu held.
// Returns true when pred became true; false on timeout or shutdown.
bool wait_for(uint32_t timeout_us, const std::function<bool()>& pred) {
    int64_t end = timeout_us ? now_us() + timeout_us : 0;
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (pred()) return true;
        }
        if (g_stop.load()) return false;
        if (end && now_us() >= end) return false;
        sched_yield_point();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

#define ARG(n) (static_cast<uint32_t>(ctx->r[(n) + 4]))
inline void ret(recomp_context* ctx, int32_t v) { ctx->r[2] = v; }

// ================= Wlan / Net helpers =================
void h_GetEtherAddr(uint8_t* rdram, recomp_context* ctx) {  // sceWlanGetEtherAddr / sceNetGetLocalEtherAddr
    ensure_net();
    uint32_t p = ARG(0);
    if (p) std::memcpy(gp(rdram, p), g_mac.data(), 6);
    ret(ctx, 0);
}
void h_WlanSwitch(uint8_t*, recomp_context* ctx) { ret(ctx, 1); }
void h_EtherNtostr(uint8_t* rdram, recomp_context* ctx) {
    uint32_t mp = ARG(0), out = ARG(1);
    if (!mp || !out) { ret(ctx, E_INVALID_ARG); return; }
    Mac m = read_mac(rdram, mp);
    std::string s = mac_str(m);
    std::memcpy(gp(rdram, out), s.c_str(), s.size() + 1);
    ret(ctx, 0);
}

// ================= sceNetAdhoc =================
void h_AdhocInit(uint8_t*, recomp_context* ctx) { ensure_net(); g_adhoc_init = true; ret(ctx, 0); }
void h_AdhocTerm(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_pdp.clear(); g_ptp.clear(); g_adhoc_init = false;
    ret(ctx, 0);
}

// ================= sceNetAdhocctl =================
void h_CtlInit(uint8_t*, recomp_context* ctx) { ensure_net(); if (!g_ctl_init) g_ctl_init_at = now_us(); g_ctl_init = true; ret(ctx, 0); }
void h_CtlTerm(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_ctl_init = false; g_connected = false; std::memset(g_group, 0, 8); g_handlers.clear();
    ret(ctx, 0);
}
void h_CtlAddHandler(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    int id = g_next_handler++;
    g_handlers[id] = {ARG(0), ARG(1)};
    ret(ctx, id);
}
void h_CtlDelHandler(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_handlers.erase((int)ARG(0));
    ret(ctx, 0);
}
void join_group(const char name[8]) {
    // caller holds g_mu
    if (g_connected) queue_ctl(CTL_DISCONNECT, 0);
    std::memcpy(g_group, name, 8);
    g_connected = true;
    ANOTE("joined group \"%.8s\"", g_group);
    queue_ctl(CTL_CONNECT, 0);
}
void h_CtlConnect(uint8_t* rdram, recomp_context* ctx) {  // also Create: (const char group[8])
    if (!ensure_net() || !ARG(0)) { ret(ctx, E_INVALID_ARG); return; }
    char name[8]; std::memcpy(name, gp(rdram, ARG(0)), 8);
    std::lock_guard<std::mutex> lk(g_mu);
    join_group(name);
    ret(ctx, 0);
}
void h_CtlJoin(uint8_t* rdram, recomp_context* ctx) {  // (scaninfo*) group name at +8
    if (!ensure_net() || !ARG(0)) { ret(ctx, E_INVALID_ARG); return; }
    char name[8]; std::memcpy(name, gp(rdram, ARG(0)) + 8, 8);
    std::lock_guard<std::mutex> lk(g_mu);
    join_group(name);
    ret(ctx, 0);
}
void h_CtlDisconnect(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_connected) {
        g_connected = false;
        std::memset(g_group, 0, 8);
        for (auto& [id, m] : g_match) m.peers.clear();
        ANOTE("left group");
    }
    queue_ctl(CTL_DISCONNECT, 0);  // always answer, even if we were not connected
    ret(ctx, 0);
}
void h_CtlScan(uint8_t*, recomp_context* ctx) {
    ensure_net();
    std::lock_guard<std::mutex> lk(g_mu);
    g_scan_due = now_us() + 700000;
    ret(ctx, 0);
}
void h_CtlGetScanInfo(uint8_t* rdram, recomp_context* ctx) {  // (int* buflen, void* buf)
    uint32_t lenp = ARG(0), buf = ARG(1);
    if (!lenp) { ret(ctx, E_INVALID_ARG); return; }
    std::lock_guard<std::mutex> lk(g_mu);
    struct G { char name[8]; Mac mac; };
    std::vector<G> gs;
    for (auto& [m, p] : g_peers) {
        if (!p.in_group) continue;
        bool dup = false;
        for (auto& g : gs) if (!std::strncmp(g.name, p.group, 8)) dup = true;
        if (dup) continue;
        G g; std::memcpy(g.name, p.group, 8); g.mac = m; gs.push_back(g);
    }
    const uint32_t SZ = 28;
    if (!buf) { w32(rdram, lenp, (uint32_t)(gs.size() * SZ)); ret(ctx, 0); return; }
    uint32_t cap = r32(rdram, lenp) / SZ;
    uint32_t n = std::min<uint32_t>(cap, (uint32_t)gs.size());
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = buf + i * SZ;
        std::memset(gp(rdram, a), 0, SZ);
        w32(rdram, a, i + 1 < n ? a + SZ : 0);
        w32(rdram, a + 4, 1);
        std::memcpy(gp(rdram, a + 8), gs[i].name, 8);
        std::memcpy(gp(rdram, a + 16), gs[i].mac.data(), 6);
        w32(rdram, a + 24, 0);
    }
    w32(rdram, lenp, n * SZ);
    ret(ctx, 0);
}
// last_seen_us is on now_us()'s clock. The game drops peers whose last_recv (game clock) is
// more than 20 s old, so a zero here would remove every peer at once.
void fill_peer(uint8_t* rdram, uint32_t a, const char* nick, const Mac& mac, uint32_t next, int64_t last_seen_us) {
    std::memset(gp(rdram, a), 0, 152);
    w32(rdram, a, next);
    std::strncpy((char*)gp(rdram, a + 4), nick, 127);
    std::memcpy(gp(rdram, a + 132), mac.data(), 6);
    w32(rdram, a + 140, 0x0400);
    uint64_t age = last_seen_us > 0 ? (uint64_t)std::max<int64_t>(0, now_us() - last_seen_us) : 0;
    uint64_t sys = psp_system_time_us();
    uint64_t lr = sys > age ? sys - age : sys;
    w32(rdram, a + 144, (uint32_t)(lr & 0xffffffffu));
    w32(rdram, a + 148, (uint32_t)(lr >> 32));
}
void h_CtlGetPeerList(uint8_t* rdram, recomp_context* ctx) {  // (int* buflen, void* buf)
    uint32_t lenp = ARG(0), buf = ARG(1);
    if (!lenp) { ret(ctx, E_INVALID_ARG); return; }
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<std::pair<Mac, Peer>> ps;
    if (g_connected)
        for (auto& [m, p] : g_peers)
            if (p.in_group && !std::strncmp(p.group, g_group, 8)) ps.push_back({m, p});
    const uint32_t SZ = 152;
    if (!buf) { w32(rdram, lenp, (uint32_t)(ps.size() * SZ)); ret(ctx, 0); return; }
    uint32_t n = std::min<uint32_t>(r32(rdram, lenp) / SZ, (uint32_t)ps.size());
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = buf + i * SZ;
        fill_peer(rdram, a, ps[i].second.nick, ps[i].first, i + 1 < n ? a + SZ : 0, ps[i].second.last_seen);
    }
    w32(rdram, lenp, n * SZ);
    ret(ctx, 0);
}
void h_CtlGetPeerInfo(uint8_t* rdram, recomp_context* ctx) {  // (mac*, size, buf*)
    uint32_t mp = ARG(0), buf = ARG(2);
    if (!mp || !buf) { ret(ctx, E_INVALID_ARG); return; }
    Mac m = read_mac(rdram, mp);
    std::lock_guard<std::mutex> lk(g_mu);
    if (m == g_mac) { fill_peer(rdram, buf, g_nick, g_mac, 0, now_us()); ret(ctx, 0); return; }
    auto it = g_peers.find(m);
    if (it == g_peers.end() || !same_group(m)) { ret(ctx, E_NO_ENTRY); return; }
    fill_peer(rdram, buf, it->second.nick, m, 0, it->second.last_seen);
    ret(ctx, 0);
}

// ================= sceNetAdhocMatching =================
Match* find_match(int id) { auto it = g_match.find(id); return it == g_match.end() ? nullptr : &it->second; }

void h_MInit(uint8_t*, recomp_context* ctx) { ensure_net(); g_match_init = true; ret(ctx, 0); }
void h_MTerm(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_match.clear(); g_match_init = false;
    ret(ctx, 0);
}
void h_MCreate(uint8_t* rdram, recomp_context* ctx) {
    // (mode, maxpeers, port, rxbuflen, hello_delay, ping_delay, init_count, msg_delay, callback)
    std::lock_guard<std::mutex> lk(g_mu);
    Match m;
    m.id = g_next_match++;
    m.mode = (int)ARG(0); m.maxpeers = (int)ARG(1); m.port = (int)ARG(2); m.rxbuf = (int)ARG(3);
    m.hello_us = ARG(4); m.ping_us = ARG(5); m.init_count = (int)ARG(6); m.msg_us = ARG(7);
    m.cb = r32(rdram, static_cast<uint32_t>(ctx->r[29]));  // 9th argument is on the stack
    ANOTE("matching %d created: mode=%d max=%d port=%d hello=%uus ping=%uus cb=0x%08X", m.id, m.mode, m.maxpeers,
          m.port, m.hello_us, m.ping_us, m.cb);
    int id = m.id;
    g_match[id] = std::move(m);
    ret(ctx, id);
}
void h_MStart(uint8_t* rdram, recomp_context* ctx) {
    // (id, evthpri, evthstack, inthpri, inthstack, optlen, optdata)
    std::lock_guard<std::mutex> lk(g_mu);
    Match* m = find_match((int)ARG(0));
    if (!m) { ret(ctx, E_INVALID_ARG); return; }
    uint32_t optlen = ARG(5), opt = ARG(6);
    m->hello_opt.clear();
    if (opt && optlen && optlen < 0x4000) m->hello_opt.assign(gp(rdram, opt), gp(rdram, opt) + optlen);
    m->started = true; m->next_hello = 0; m->next_ping = 0;
    ANOTE("matching %d started (hello opt %u bytes)", m->id, optlen);
    ret(ctx, 0);
}
void h_MStop(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    Match* m = find_match((int)ARG(0));
    if (!m) { ret(ctx, E_INVALID_ARG); return; }
    for (auto& [mac, p] : m->peers)
        if (p.s == MPeer::ESTAB) match_send(*m, mac, MO_BYE, {});
        else if (p.s == MPeer::REQ_BY || p.s == MPeer::REQ_OF || p.s == MPeer::ACCEPTED) match_send(*m, mac, MO_CANCEL, {});
    m->peers.clear(); m->started = false;
    ret(ctx, 0);
}
void h_MDelete(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    Match* m = find_match((int)ARG(0));
    if (m) {
        for (auto& [mac, p] : m->peers)
            if (p.s == MPeer::ESTAB) match_send(*m, mac, MO_BYE, {});
        g_match.erase((int)ARG(0));
    }
    // Drop queued events for the deleted id so no callback runs afterwards.
    g_events.erase(std::remove_if(g_events.begin(), g_events.end(),
                                  [&](const Ev& e) { return e.kind == 1 && e.a == (int)ARG(0); }),
                   g_events.end());
    ret(ctx, 0);
}
void h_MSelect(uint8_t* rdram, recomp_context* ctx) {  // (id, mac*, optlen, opt*)
    std::lock_guard<std::mutex> lk(g_mu);
    Match* m = find_match((int)ARG(0));
    if (!m || !ARG(1)) { ret(ctx, E_INVALID_ARG); return; }
    Mac mac = read_mac(rdram, ARG(1));
    uint32_t optlen = ARG(2), opt = ARG(3);
    std::vector<uint8_t> o;
    if (opt && optlen && optlen < 0x4000) o.assign(gp(rdram, opt), gp(rdram, opt) + optlen);
    auto it = m->peers.find(mac);
    int64_t t = now_us();
    if (it != m->peers.end() && it->second.s == MPeer::REQ_BY) {
        it->second.s = MPeer::ACCEPTED; it->second.last_seen = t;
        match_send(*m, mac, MO_ACCEPT, o);
        ALOG("accepted %s", mac_str(mac).c_str());
    } else if (m->mode != 1) {
        if (it == m->peers.end() && !same_group(mac)) { ret(ctx, E_NO_ENTRY); return; }
        MPeer& p = m->peers[mac];
        p.s = MPeer::REQ_OF; p.last_seen = t;
        match_send(*m, mac, MO_JOIN, o);
        ALOG("requested join to %s", mac_str(mac).c_str());
    } else { ret(ctx, E_NO_ENTRY); return; }
    ret(ctx, 0);
}
void h_MCancel(uint8_t* rdram, recomp_context* ctx) {  // (id, mac*)
    std::lock_guard<std::mutex> lk(g_mu);
    Match* m = find_match((int)ARG(0));
    if (!m || !ARG(1)) { ret(ctx, E_INVALID_ARG); return; }
    Mac mac = read_mac(rdram, ARG(1));
    auto it = m->peers.find(mac);
    if (it == m->peers.end()) { ret(ctx, 0); return; }
    if (it->second.s == MPeer::ESTAB) match_send(*m, mac, MO_BYE, {});
    else if (it->second.s != MPeer::HELLO) match_send(*m, mac, MO_CANCEL, {});
    if (it->second.s == MPeer::HELLO) it->second.last_event = 0;
    else m->peers.erase(it);
    ret(ctx, 0);
}
void h_MSendData(uint8_t* rdram, recomp_context* ctx) {  // (id, mac*, datalen, data*)
    std::lock_guard<std::mutex> lk(g_mu);
    Match* m = find_match((int)ARG(0));
    if (!m || !ARG(1)) { ret(ctx, E_INVALID_ARG); return; }
    Mac mac = read_mac(rdram, ARG(1));
    uint32_t len = ARG(2), data = ARG(3);
    auto it = m->peers.find(mac);
    if (it == m->peers.end() || it->second.s != MPeer::ESTAB) { ret(ctx, E_NO_ENTRY); return; }
    if (!data || len == 0 || len > 0x2800) { ret(ctx, E_INVALID_DATALEN); return; }
    std::vector<uint8_t> d(gp(rdram, data), gp(rdram, data) + len);
    match_send(*m, mac, MO_DATA, d, m->data_id++);
    ret(ctx, 0);
}
void h_MSetHelloOpt(uint8_t* rdram, recomp_context* ctx) {  // (id, optlen, opt*)
    std::lock_guard<std::mutex> lk(g_mu);
    Match* m = find_match((int)ARG(0));
    if (!m) { ret(ctx, E_INVALID_ARG); return; }
    uint32_t optlen = ARG(1), opt = ARG(2);
    m->hello_opt.clear();
    if (opt && optlen && optlen < 0x4000) m->hello_opt.assign(gp(rdram, opt), gp(rdram, opt) + optlen);
    ret(ctx, 0);
}

// ================= PDP =================
void h_PdpCreate(uint8_t* rdram, recomp_context* ctx) {  // (mac*, port, bufsize, unk)
    ensure_net();
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_adhoc_init) { ret(ctx, E_NOT_INIT); return; }
    Pdp s; s.id = g_next_pdp;
    s.mac = ARG(0) ? read_mac(rdram, ARG(0)) : g_mac;
    s.port = (uint16_t)ARG(1);
    s.bufsize = (int)ARG(2);
    if (s.port == 0) s.port = g_next_eph++;
    for (auto& [id, o] : g_pdp) if (o.port == s.port) { ret(ctx, E_PORT_IN_USE); return; }
    g_next_pdp++;
    int id = s.id;
    g_pdp[id] = std::move(s);
    ret(ctx, id);
}
void h_PdpDelete(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_pdp.erase((int)ARG(0));
    ret(ctx, 0);
}
void h_PdpSend(uint8_t* rdram, recomp_context* ctx) {  // (id, destmac*, port, data, len, timeout, nonblock)
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_pdp.find((int)ARG(0));
    if (it == g_pdp.end()) { ret(ctx, E_INVALID_SOCKET); return; }
    if (!ARG(1) || !ARG(3)) { ret(ctx, E_INVALID_ARG); return; }
    uint32_t len = ARG(4);
    if (len > 0x2000) { ret(ctx, E_INVALID_DATALEN); return; }
    Mac dst = read_mac(rdram, ARG(1));
    std::vector<uint8_t> b;
    b.insert(b.end(), dst.begin(), dst.end());
    put16(b, it->second.port); put16(b, (uint16_t)ARG(2));
    b.insert(b.end(), gp(rdram, ARG(3)), gp(rdram, ARG(3)) + len);
    psp_netplay_trace_ptp("pdp-send", (int)ARG(0), gp(rdram, ARG(3)), len);
    send_to_mac(dst, T_PDP, b);
    ret(ctx, 0);
}
void h_PdpRecv(uint8_t* rdram, recomp_context* ctx) {  // (id, srcmac*, port*, data, len*, timeout, nonblock)
    int id = (int)ARG(0);
    uint32_t macp = ARG(1), portp = ARG(2), data = ARG(3), lenp = ARG(4), timeout = ARG(5);
    bool nonblock = ARG(6) != 0;
    if (!data || !lenp) { ret(ctx, E_INVALID_ARG); return; }
    auto have = [&] { auto it = g_pdp.find(id); return it == g_pdp.end() || !it->second.q.empty(); };
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_pdp.count(id)) { ret(ctx, E_INVALID_SOCKET); return; }
    }
    if (nonblock) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!have()) { ret(ctx, E_WOULD_BLOCK); return; }
    } else if (!wait_for(timeout, have)) { ret(ctx, E_TIMEOUT); return; }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_pdp.find(id);
    if (it == g_pdp.end()) { ret(ctx, E_INVALID_SOCKET); return; }
    if (it->second.q.empty()) { ret(ctx, E_WOULD_BLOCK); return; }
    PdpMsg m = std::move(it->second.q.front());
    it->second.q.pop_front();
    uint32_t cap = r32(rdram, lenp);
    uint32_t n = std::min<uint32_t>(cap, (uint32_t)m.data.size());
    std::memcpy(gp(rdram, data), m.data.data(), n);
    psp_netplay_trace_ptp("pdp-recv", id, m.data.data(), n);
    w32(rdram, lenp, n);
    if (macp) std::memcpy(gp(rdram, macp), m.src.data(), 6);
    if (portp) w16(rdram, portp, m.sport);
    ret(ctx, 0);
}
void h_GetPdpStat(uint8_t* rdram, recomp_context* ctx) {  // (int* buflen, void* buf)
    uint32_t lenp = ARG(0), buf = ARG(1);
    if (!lenp) { ret(ctx, E_INVALID_ARG); return; }
    std::lock_guard<std::mutex> lk(g_mu);
    const uint32_t SZ = 20;
    if (!buf) { w32(rdram, lenp, (uint32_t)(g_pdp.size() * SZ)); ret(ctx, 0); return; }
    uint32_t n = std::min<uint32_t>(r32(rdram, lenp) / SZ, (uint32_t)g_pdp.size());
    uint32_t i = 0;
    for (auto& [id, s] : g_pdp) {
        if (i >= n) break;
        uint32_t a = buf + i * SZ;
        std::memset(gp(rdram, a), 0, SZ);
        w32(rdram, a, i + 1 < n ? a + SZ : 0);
        w32(rdram, a + 4, (uint32_t)id);
        std::memcpy(gp(rdram, a + 8), s.mac.data(), 6);
        w16(rdram, a + 14, s.port);
        uint32_t used = 0; for (auto& m : s.q) used += (uint32_t)m.data.size();
        w32(rdram, a + 16, used);
        i++;
    }
    w32(rdram, lenp, n * SZ);
    ret(ctx, 0);
}

// ================= PTP =================
void h_PtpListen(uint8_t* rdram, recomp_context* ctx) {  // (mac*, port, bufsize, rexmt_int, rexmt_cnt, backlog, unk)
    ensure_net();
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_adhoc_init) { ret(ctx, E_NOT_INIT); return; }
    Ptp p; p.id = g_next_ptp;
    p.s = Ptp::LISTEN;
    p.smac = ARG(0) ? read_mac(rdram, ARG(0)) : g_mac;
    p.sport = (uint16_t)ARG(1); p.bufsize = (int)ARG(2);
    p.rexmt_us = std::clamp<int64_t>(ARG(3), 50000, 1000000);
    p.rexmt_cnt = (int)ARG(4);
    for (auto& [id, o] : g_ptp) if (o.s == Ptp::LISTEN && o.sport == p.sport) { ret(ctx, E_PORT_IN_USE); return; }
    g_next_ptp++;
    int id = p.id;
    g_ptp[id] = std::move(p);
    ret(ctx, id);
}
void h_PtpOpen(uint8_t* rdram, recomp_context* ctx) {  // (smac*, sport, dmac*, dport, bufsize, rexmt_int, rexmt_cnt, unk)
    ensure_net();
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_adhoc_init) { ret(ctx, E_NOT_INIT); return; }
    if (!ARG(2)) { ret(ctx, E_INVALID_ARG); return; }
    Ptp p; p.id = g_next_ptp++;
    p.s = Ptp::SYN_SENT;
    p.smac = ARG(0) ? read_mac(rdram, ARG(0)) : g_mac;
    p.dmac = read_mac(rdram, ARG(2));
    p.sport = (uint16_t)ARG(1); if (!p.sport) p.sport = g_next_eph++;
    p.dport = (uint16_t)ARG(3);
    p.bufsize = (int)ARG(4);
    p.rexmt_us = std::clamp<int64_t>(ARG(5), 50000, 1000000);
    p.rexmt_cnt = (int)ARG(6);
    p.last_syn = now_us();
    p.cid = (uint32_t)(now_us() ^ ((uint64_t)p.id << 20) ^ 0x5bd1e995u);
    ptp_send_pkt(p, P_SYN, p.cid, 0);
    int id = p.id;
    g_ptp[id] = std::move(p);
    ret(ctx, id);
}
void h_PtpConnect(uint8_t*, recomp_context* ctx) {  // (id, timeout, nonblock)
    int id = (int)ARG(0); uint32_t timeout = ARG(1); bool nb = ARG(2) != 0;
    auto st = [&]() -> Ptp::S { auto it = g_ptp.find(id); return it == g_ptp.end() ? Ptp::CLOSED : it->second.s; };
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_ptp.count(id)) { ret(ctx, E_INVALID_SOCKET); return; }
    }
    // A non-blocking connect still waits for the handshake for a while: the game treats a would-block here as a failed
    // connect and opens a new socket, so over any real round trip (the SYNACK takes longer than the game's next poll)
    // it never got connected and "Preparing for online battle" hung with hundreds of sockets.
    wait_for(nb ? std::min<uint32_t>(timeout ? timeout : 1500000u, 1500000u) : timeout, [&] { return st() != Ptp::SYN_SENT; });
    std::lock_guard<std::mutex> lk(g_mu);
    Ptp::S s = st();
    if (s == Ptp::ESTAB) ret(ctx, 0);
    else if (s == Ptp::SYN_SENT) ret(ctx, nb ? E_WOULD_BLOCK : E_TIMEOUT);
    else ret(ctx, E_REFUSED);
}
void h_PtpAccept(uint8_t* rdram, recomp_context* ctx) {  // (id, mac*, port*, timeout, nonblock)
    int id = (int)ARG(0); uint32_t macp = ARG(1), portp = ARG(2), timeout = ARG(3); bool nb = ARG(4) != 0;
    auto ready = [&] { auto it = g_ptp.find(id); return it == g_ptp.end() || !it->second.accept_q.empty(); };
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_ptp.count(id)) { ret(ctx, E_INVALID_SOCKET); return; }
        if (nb && !ready()) { ret(ctx, E_WOULD_BLOCK); return; }
    }
    if (!nb && !wait_for(timeout, ready)) { ret(ctx, E_TIMEOUT); return; }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ptp.find(id);
    if (it == g_ptp.end()) { ret(ctx, E_INVALID_SOCKET); return; }
    if (it->second.accept_q.empty()) { ret(ctx, E_WOULD_BLOCK); return; }
    int cid = it->second.accept_q.front(); it->second.accept_q.pop_front();
    Ptp& c = g_ptp[cid];
    if (macp) std::memcpy(gp(rdram, macp), c.dmac.data(), 6);
    if (portp) w16(rdram, portp, c.dport);
    ret(ctx, cid);
}
void h_PtpSend(uint8_t* rdram, recomp_context* ctx) {  // (id, data, size*, timeout, nonblock)
    // Stream semantics like TCP send(): queue as many bytes as the send buffer has room for and
    // report that count in *size (0 on would-block/timeout - the game adds *size to what it has sent).
    int id = (int)ARG(0); uint32_t data = ARG(1), sizep = ARG(2), timeout = ARG(3); bool nb = ARG(4) != 0;
    if (!data || !sizep) { ret(ctx, E_INVALID_ARG); return; }
    uint32_t len = r32(rdram, sizep);
    auto space = [&]() -> size_t {
        auto it = g_ptp.find(id);
        if (it == g_ptp.end()) return 1;
        size_t cap = (size_t)std::max(it->second.bufsize, 0x2000), used = it->second.sendq.size();
        return cap > used ? cap - used : 0;
    };
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_ptp.find(id);
        if (it == g_ptp.end()) { ret(ctx, E_INVALID_SOCKET); return; }
        if (it->second.s != Ptp::ESTAB) { ret(ctx, E_NOT_CONNECTED); return; }
        if (nb && space() == 0) { w32(rdram, sizep, 0); ret(ctx, E_WOULD_BLOCK); return; }
    }
    if (!nb && !wait_for(timeout, [&] {
            auto it = g_ptp.find(id);
            return it == g_ptp.end() || it->second.s != Ptp::ESTAB || space() > 0; })) {
        w32(rdram, sizep, 0); ret(ctx, E_TIMEOUT); return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ptp.find(id);
    if (it == g_ptp.end() || it->second.s != Ptp::ESTAB) { ret(ctx, E_NOT_CONNECTED); return; }
    uint32_t n = (uint32_t)std::min<size_t>(len, space());
    it->second.sendq.insert(it->second.sendq.end(), gp(rdram, data), gp(rdram, data) + n);
    psp_netplay_trace_ptp("send", id, gp(rdram, data), n);
    w32(rdram, sizep, n);
    ret(ctx, 0);
}
void h_PtpRecv(uint8_t* rdram, recomp_context* ctx) {  // (id, buf, size*, timeout, nonblock)
    int id = (int)ARG(0); uint32_t buf = ARG(1), sizep = ARG(2), timeout = ARG(3); bool nb = ARG(4) != 0;
    if (!buf || !sizep) { ret(ctx, E_INVALID_ARG); return; }
    auto ready = [&] {
        auto it = g_ptp.find(id);
        return it == g_ptp.end() || !it->second.rxq.empty() || it->second.fin_rx || it->second.s != Ptp::ESTAB;
    };
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_ptp.count(id)) { ret(ctx, E_INVALID_SOCKET); return; }
        if (nb && !ready()) { w32(rdram, sizep, 0); ret(ctx, E_WOULD_BLOCK); return; }
    }
    if (!nb && !wait_for(timeout, ready)) { w32(rdram, sizep, 0); ret(ctx, E_TIMEOUT); return; }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ptp.find(id);
    if (it == g_ptp.end()) { ret(ctx, E_INVALID_SOCKET); return; }
    Ptp& p = it->second;
    if (p.rxq.empty()) {
        w32(rdram, sizep, 0); ret(ctx, (p.fin_rx || p.s != Ptp::ESTAB) ? E_DISCONNECTED : E_WOULD_BLOCK); return; }
    uint32_t n = std::min<uint32_t>(r32(rdram, sizep), (uint32_t)p.rxq.size());
    for (uint32_t i = 0; i < n; i++) { gp(rdram, buf)[i] = p.rxq.front(); p.rxq.pop_front(); }
    psp_netplay_trace_ptp("recv", id, gp(rdram, buf), n);
    w32(rdram, sizep, n);
    ret(ctx, 0);
}
void h_PtpFlush(uint8_t*, recomp_context* ctx) {  // (id, timeout, nonblock)
    int id = (int)ARG(0); uint32_t timeout = ARG(1); bool nb = ARG(2) != 0;
    auto done = [&] { auto it = g_ptp.find(id); return it == g_ptp.end() || it->second.s != Ptp::ESTAB || it->second.sendq.empty(); };
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_ptp.count(id)) { ret(ctx, E_INVALID_SOCKET); return; }
        if (nb && !done()) { ret(ctx, E_WOULD_BLOCK); return; }
    }
    if (!nb && !wait_for(timeout, done)) { ret(ctx, E_TIMEOUT); return; }
    ret(ctx, 0);
}
void h_PtpClose(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ptp.find((int)ARG(0));
    if (it == g_ptp.end()) { ret(ctx, E_INVALID_SOCKET); return; }
    if (it->second.s == Ptp::ESTAB) ptp_send_pkt(it->second, P_FIN, 0, 0);
    g_ptp.erase(it);
    ret(ctx, 0);
}
void h_GetPtpStat(uint8_t* rdram, recomp_context* ctx) {  // (int* buflen, void* buf)
    uint32_t lenp = ARG(0), buf = ARG(1);
    if (!lenp) { ret(ctx, E_INVALID_ARG); return; }
    std::lock_guard<std::mutex> lk(g_mu);
    const uint32_t SZ = 36;
    if (!buf) { w32(rdram, lenp, (uint32_t)(g_ptp.size() * SZ)); ret(ctx, 0); return; }
    uint32_t n = std::min<uint32_t>(r32(rdram, lenp) / SZ, (uint32_t)g_ptp.size());
    uint32_t i = 0;
    for (auto& [id, p] : g_ptp) {
        if (i >= n) break;
        uint32_t a = buf + i * SZ;
        std::memset(gp(rdram, a), 0, SZ);
        w32(rdram, a, i + 1 < n ? a + SZ : 0);
        w32(rdram, a + 4, (uint32_t)id);
        std::memcpy(gp(rdram, a + 8), p.smac.data(), 6);
        std::memcpy(gp(rdram, a + 14), p.dmac.data(), 6);
        w16(rdram, a + 20, p.sport); w16(rdram, a + 22, p.dport);
        w32(rdram, a + 24, (uint32_t)p.sendq.size());
        w32(rdram, a + 28, (uint32_t)p.rxq.size());
        w32(rdram, a + 32, p.s == Ptp::LISTEN ? 1 : p.s == Ptp::SYN_SENT ? 2 : p.s == Ptp::ESTAB ? 4 : 0);
        i++;
    }
    w32(rdram, lenp, n * SZ);
    ret(ctx, 0);
}

// Call tracing (PSPRECOMP_ADHOC_LOG=1): arguments, result and, for data calls, the first bytes.
// Polling results (would-block / timeout) are logged only the first few times per call.
#define TRACED(NAME, FN) \
    void t_##FN(uint8_t* rdram, recomp_context* ctx) { \
        static int quiet = 0; \
        uint32_t a0 = ARG(0), a1 = ARG(1), a2 = ARG(2), a3 = ARG(3), a4 = ARG(4); \
        FN(rdram, ctx); \
        if (!g_log) return; \
        int32_t rc = ctx->r[2]; \
        bool poll = rc == E_WOULD_BLOCK || rc == E_TIMEOUT; \
        if (poll && ++quiet > 8) return; \
        std::fprintf(stderr, "[ADHOC] " NAME "(0x%X, 0x%X, 0x%X, 0x%X, 0x%X) -> 0x%08X\n", a0, a1, a2, a3, a4, (uint32_t)rc); \
    }
// Same as TRACED, plus a hex dump of the first bytes of the message (first 300 messages per call).
// BUFARG is the argument holding the buffer address; LENARG the argument holding the length or a
// pointer to it (LENPTR=1 when it is a pointer, as in PtpSend/PtpRecv/PdpRecv).
#define TRACED_DATA(NAME, FN, BUFARG, LENARG, LENPTR) \
    void t_##FN(uint8_t* rdram, recomp_context* ctx) { \
        static int quiet = 0, shown = 0; \
        uint32_t a0 = ARG(0), a1 = ARG(1), a2 = ARG(2), a3 = ARG(3), a4 = ARG(4); \
        uint32_t args[5] = {a0, a1, a2, a3, a4}; \
        uint32_t pre_len = (LENPTR && args[LENARG]) ? r32(rdram, args[LENARG]) : 0; \
        FN(rdram, ctx); \
        if (!g_log) return; \
        int32_t rc = ctx->r[2]; \
        bool poll = rc == E_WOULD_BLOCK || rc == E_TIMEOUT; \
        if (poll && ++quiet > 8) return; \
        char hex[3 * 32 + 1] = {0}; \
        uint32_t n = 0; \
        if (rc == 0 && args[BUFARG]) { \
            n = LENPTR ? (args[LENARG] ? r32(rdram, args[LENARG]) : 0) : args[LENARG]; \
            if (n > 0x10000) n = 0; \
            for (uint32_t i = 0; i < n && i < 32; i++) std::snprintf(hex + i * 3, 4, "%02x ", gp(rdram, args[BUFARG])[i]); \
        } \
        if (rc == 0 && ++shown > 300) return; \
        std::fprintf(stderr, "[ADHOC] " NAME "(0x%X, 0x%X, 0x%X, 0x%X, 0x%X) -> 0x%08X len=%u (asked %u) %s\n", a0, a1, a2, a3, a4, (uint32_t)rc, n, pre_len, hex); \
    }
TRACED("PdpCreate", h_PdpCreate) TRACED("PdpDelete", h_PdpDelete)  
TRACED("PtpListen", h_PtpListen) TRACED("PtpOpen", h_PtpOpen) TRACED("PtpConnect", h_PtpConnect)
TRACED("PtpAccept", h_PtpAccept)  
TRACED("PtpFlush", h_PtpFlush) TRACED("PtpClose", h_PtpClose) TRACED("GetPtpStat", h_GetPtpStat)
TRACED("GetPdpStat", h_GetPdpStat)
TRACED_DATA("PtpSend", h_PtpSend, 1, 2, 1) TRACED_DATA("PtpRecv", h_PtpRecv, 1, 2, 1)
TRACED_DATA("PdpSend", h_PdpSend, 3, 4, 0) TRACED_DATA("PdpRecv", h_PdpRecv, 3, 4, 1)
TRACED_DATA("MatchingSendData", h_MSendData, 3, 2, 0)

TRACED("MatchingSelectTarget", h_MSelect) TRACED("MatchingCancelTarget", h_MCancel)
 TRACED("MatchingStop", h_MStop) TRACED("MatchingDelete", h_MDelete)
TRACED("AdhocctlDisconnect", h_CtlDisconnect)

}  // namespace

// ================= public =================
void psp_adhoc_send_aux(const uint8_t* data, size_t n) {
    if (!g_net_ready || n > 0x2000) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_connected) return;
    const Mac all{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    std::vector<uint8_t> b;
    b.insert(b.end(), all.begin(), all.end());
    put16(b, AUX_PORT); put16(b, AUX_PORT);
    b.insert(b.end(), data, data + n);
    send_to_mac(all, T_PDP, b);
}
void psp_adhoc_configure(bool internet, const char* server, const char* room, const char* peers) {
    load_cfg_from_env();   // consume the environment first so it cannot overwrite what the menu sets
    std::lock_guard<std::mutex> lk(g_mu);
    g_relay_mode = internet;
    g_cfg_server = (server && *server) ? server : DEFAULT_SERVER;
    g_cfg_room = room ? room : "";
    g_cfg_peers = peers ? peers : "";
    g_cfg_dirty = true;
}

void psp_adhoc_status(char* out, size_t n) {
    load_cfg_from_env();
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_enabled) { std::snprintf(out, n, "%s", g_off_reason); return; }
    if (!g_net_ready) { std::snprintf(out, n, "Starts in Communications mode"); return; }
    if (g_relay_mode) {
        if (g_relay_resolve_failed) std::snprintf(out, n, "Cannot find %s", g_cfg_server.c_str());
        else if (!g_relay_resolved) std::snprintf(out, n, "Looking up %s ...", g_cfg_server.c_str());
        else if (!g_relay_ok) std::snprintf(out, n, "No answer from the server yet");
        else std::snprintf(out, n, "Connected, %d online%s%s", g_relay_players, g_cfg_room.empty() ? "" : " in ", g_cfg_room.c_str());
    } else {
        std::snprintf(out, n, "LAN: %d nearby", (int)g_peers.size());
    }
}

void psp_adhoc_side_set_handler(void (*handler)(const uint8_t mac[6], const uint8_t* data, size_t len)) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_side_handler = handler;
}
void psp_adhoc_set_match_filter(bool (*allow)(const uint8_t mac[6])) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_match_filter = allow;
}
bool psp_adhoc_side_send(const uint8_t mac[6], const uint8_t* data, size_t len) {
    if (len > PSP_ADHOC_SIDE_MAX) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_net_ready || !g_connected) return false;
    Mac dst; std::memcpy(dst.data(), mac, 6);
    std::vector<uint8_t> b(dst.begin(), dst.end());
    put16(b, PSP_ADHOC_SIDE_PORT);
    put16(b, PSP_ADHOC_SIDE_PORT);
    b.insert(b.end(), data, data + len);
    send_to_mac(dst, T_PDP, b);
    return true;
}
void psp_adhoc_group_peers(uint8_t (*out)[6], size_t max, size_t* count, uint8_t own[6]) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (own) std::memcpy(own, g_mac.data(), 6);
    size_t n = 0;
    if (g_connected)
        for (auto& [m, p] : g_peers)
            if (n < max && same_group(m)) std::memcpy(out[n++], m.data(), 6);
    *count = n;
}
void psp_adhoc_set_local_fps(int fps) { std::lock_guard<std::mutex> lk(g_mu); g_local_fps = fps; }

// Up to 24 bytes of the game module's own (a mode it wants everyone in the group to run, a state checksum), sent in every beacon.
void psp_adhoc_set_game_data(const uint8_t* d, int n) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_game_len = (uint8_t)std::max(0, std::min<int>(n, (int)sizeof g_game_data));
    if (g_game_len) std::memcpy(g_game_data, d, g_game_len);
}
// The game bytes of the other players in this player's group (up to `max`): out[i] gets peer i's bytes, lens[i] their count; returns
// the number of peers in the group (0 when not in a group). Peers without game bytes are counted with length 0.
int psp_adhoc_group_game_data(uint8_t (*out)[48], int* lens, int max, uint8_t (*macs)[6]) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_connected) return 0;
    int k = 0;
    for (auto& [m, p] : g_peers)
        if (p.in_group && !std::strncmp(p.group, g_group, 8)) {
            if (k < max) { std::memcpy(out[k], p.game_data, sizeof p.game_data); lens[k] = p.game_len; if (macs) std::memcpy(macs[k], m.data(), 6); }
            k++;
        }
    return k;
}

int psp_adhoc_group_fps_cap() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_connected) return 0;
    int cap = 0;
    bool any = false;
    for (auto& [m, p] : g_peers)
        if (p.in_group && !std::strncmp(p.group, g_group, 8)) {
            any = true;
            if (p.fps) cap = cap ? std::min<int>(cap, p.fps) : p.fps;
        }
    if (!any) return 0;
    if (g_local_fps) cap = cap ? std::min(cap, g_local_fps) : g_local_fps;
    return cap;
}

bool psp_adhoc_connected() {   // the game has joined an ad-hoc group (lobby or match): pausing would stall the other players
    std::lock_guard<std::mutex> lk(g_mu);
    return g_connected;
}

bool psp_adhoc_in_session() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_connected) return false;
    for (auto& [m, p] : g_peers)
        if (p.in_group && !std::strncmp(p.group, g_group, 8)) return true;
    return false;
}

// UTF-8 cut to at most `max` bytes on a character boundary (a beacon carries 31 bytes of nickname).
static std::string utf8_cut(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}

void psp_adhoc_set_nickname(const char* utf8) {
    const char* forced = std::getenv("PSPRECOMP_ADHOC_NICK");
    std::lock_guard<std::mutex> lk(g_mu);
    g_game_nick = utf8_cut(utf8 ? utf8 : "", 31);
    if (forced && *forced) return;
    const std::string n = g_game_nick.empty() ? std::string("Player") : g_game_nick;
    if (n == g_nick) return;
    std::snprintf(g_nick, sizeof g_nick, "%s", n.c_str());
    if (g_net_ready) ANOTE("nickname \"%s\"", g_nick);
}

std::vector<std::string> psp_adhoc_group_nicks() {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_connected) return out;
    for (auto& [m, p] : g_peers)
        if (p.in_group && !std::strncmp(p.group, g_group, 8)) out.emplace_back(p.nick);
    return out;
}

std::string psp_adhoc_group_key() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_connected) return "";
    // the same for every member: where they meet (server + room, or LAN) and the game's group name
    std::string k = g_relay_mode ? g_cfg_server + "/" + g_cfg_room : std::string("lan");
    k += '/';
    k.append(g_group, strnlen(g_group, 8));
    return k;
}

int psp_adhoc_group_size() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_connected) return 0;
    int n = 1;
    for (auto& [m, p] : g_peers)
        if (p.in_group && !std::strncmp(p.group, g_group, 8)) n++;
    return n;
}

void (*g_pump_hook)(uint8_t* rdram) = nullptr;
void psp_adhoc_set_pump_hook(void (*hook)(uint8_t* rdram)) { g_pump_hook = hook; }
void psp_adhoc_pump(uint8_t* rdram, recomp_context* ctx) {
    if (!g_net_ready) return;
    if (g_pumping.exchange(true)) return;
    if (g_pump_hook) g_pump_hook(rdram);   // the game module's game-thread work, before the queued callbacks run
    for (;;) {
        Ev e;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_events.empty()) break;
            e = std::move(g_events.front());
            g_events.pop_front();
        }
        if (!e.func) continue;
        FuncPtr fn = RECOMP_LOOKUP(e.func);
        if (!fn) { ANOTE("callback 0x%08X not found", e.func); continue; }
        if (!g_scratch) g_scratch = psp_alloc_kernel_memory(SCRATCH_SIZE);
        if (!g_scratch) break;

        int32_t sv[10] = {ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[7], ctx->r[8], ctx->r[9], ctx->r[10], ctx->r[11], ctx->r[31], ctx->r[2]};
        if (e.kind == 0) {  // handler(flag, error, arg)
            ctx->r[4] = e.a; ctx->r[5] = e.b; ctx->r[6] = (int32_t)e.arg;
            ANOTE("adhocctl event %d -> handler 0x%08X", e.a, e.func);
        } else {  // callback(id, event, mac*, optlen, opt*)
            uint32_t macp = g_scratch, optp = g_scratch + 0x10;
            std::memcpy(gp(rdram, macp), e.mac.data(), 6);
            uint32_t n = (uint32_t)std::min<size_t>(e.data.size(), SCRATCH_SIZE - 0x20);
            if (n) std::memcpy(gp(rdram, optp), e.data.data(), n);
            ctx->r[4] = e.a; ctx->r[5] = e.b; ctx->r[6] = (int32_t)macp; ctx->r[7] = (int32_t)n;
            ctx->r[8] = (int32_t)optp;
        }
        ctx->r[31] = 0;
        fn(rdram, ctx);
        ctx->r[4] = sv[0]; ctx->r[5] = sv[1]; ctx->r[6] = sv[2]; ctx->r[7] = sv[3];
        ctx->r[8] = sv[4]; ctx->r[9] = sv[5]; ctx->r[10] = sv[6]; ctx->r[11] = sv[7];
        ctx->r[31] = sv[8]; ctx->r[2] = sv[9];
    }
    g_pumping.store(false);
}

void psp_hle_register_adhoc() {
    const char* en = std::getenv("PSPRECOMP_ADHOC");
    if (en && en[0] == '0') return;  // leave the old no-op stubs in place
    // helpers
    psp_hle_register("sceWlanGetEtherAddr", h_GetEtherAddr);
    psp_hle_register("sceNetGetLocalEtherAddr", h_GetEtherAddr);
    psp_hle_register("sceWlanGetSwitchState", h_WlanSwitch);
    psp_hle_register("sceNetEtherNtostr", h_EtherNtostr);
    // adhoc
    psp_hle_register("sceNetAdhocInit", h_AdhocInit);
    psp_hle_register("sceNetAdhocTerm", h_AdhocTerm);
    psp_hle_register("sceNetAdhocPdpCreate", t_h_PdpCreate);
    psp_hle_register("sceNetAdhocPdpDelete", t_h_PdpDelete);
    psp_hle_register("sceNetAdhocPdpSend", t_h_PdpSend);
    psp_hle_register("sceNetAdhocPdpRecv", t_h_PdpRecv);
    psp_hle_register("sceNetAdhocGetPdpStat", t_h_GetPdpStat);
    psp_hle_register("sceNetAdhocPtpListen", t_h_PtpListen);
    psp_hle_register("sceNetAdhocPtpOpen", t_h_PtpOpen);
    psp_hle_register("sceNetAdhocPtpConnect", t_h_PtpConnect);
    psp_hle_register("sceNetAdhocPtpAccept", t_h_PtpAccept);
    psp_hle_register("sceNetAdhocPtpSend", t_h_PtpSend);
    psp_hle_register("sceNetAdhocPtpRecv", t_h_PtpRecv);
    psp_hle_register("sceNetAdhocPtpFlush", t_h_PtpFlush);
    psp_hle_register("sceNetAdhocPtpClose", t_h_PtpClose);
    psp_hle_register("sceNetAdhocGetPtpStat", t_h_GetPtpStat);
    // adhocctl
    psp_hle_register("sceNetAdhocctlInit", h_CtlInit);
    psp_hle_register("sceNetAdhocctlTerm", h_CtlTerm);
    psp_hle_register("sceNetAdhocctlAddHandler", h_CtlAddHandler);
    psp_hle_register("sceNetAdhocctlDelHandler", h_CtlDelHandler);
    psp_hle_register("sceNetAdhocctlConnect", h_CtlConnect);
    psp_hle_register("sceNetAdhocctlCreate", h_CtlConnect);
    psp_hle_register("sceNetAdhocctlJoin", h_CtlJoin);
    psp_hle_register("sceNetAdhocctlDisconnect", t_h_CtlDisconnect);
    psp_hle_register("sceNetAdhocctlScan", h_CtlScan);
    psp_hle_register("sceNetAdhocctlGetScanInfo", h_CtlGetScanInfo);
    psp_hle_register("sceNetAdhocctlGetPeerList", h_CtlGetPeerList);
    psp_hle_register("sceNetAdhocctlGetPeerInfo", h_CtlGetPeerInfo);
    // matching
    psp_hle_register("sceNetAdhocMatchingInit", h_MInit);
    psp_hle_register("sceNetAdhocMatchingTerm", h_MTerm);
    psp_hle_register("sceNetAdhocMatchingCreate", h_MCreate);
    psp_hle_register("sceNetAdhocMatchingStart", h_MStart);
    psp_hle_register("sceNetAdhocMatchingStop", t_h_MStop);
    psp_hle_register("sceNetAdhocMatchingDelete", t_h_MDelete);
    psp_hle_register("sceNetAdhocMatchingSelectTarget", t_h_MSelect);
    psp_hle_register("sceNetAdhocMatchingCancelTarget", t_h_MCancel);
    psp_hle_register("sceNetAdhocMatchingSendData", t_h_MSendData);
    psp_hle_register("sceNetAdhocMatchingSetHelloOpt", h_MSetHelloOpt);
}

static std::atomic<int64_t> g_notice_at{-100000000};
// ---- network notices: one line on screen (and in the log), at most once per min_interval_s per key ----
void psp_net_notice(const char* key, int min_interval_s, const char* fmt, ...) {
    static std::mutex mu;
    static std::map<std::string, int64_t> last;
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::string msg = buf;
    if (!std::getenv("PSPRECOMP_ADHOC_MSG_OLD")) {   // the toast box does not wrap: break lines of more than 78 characters at a space
        std::string out; size_t col = 0;
        for (size_t i = 0; i < msg.size();) {
            size_t e = msg.find_first_of(" \n", i); if (e == std::string::npos) e = msg.size();
            const size_t w = e - i;
            if (col && col + 1 + w > 78) { out += '\n'; col = 0; } else if (col) { out += ' '; col++; }
            out.append(msg, i, w); col += w;
            if (e < msg.size() && msg[e] == '\n') { out += '\n'; col = 0; }
            i = e + 1;
        }
        msg = out;
    }
    const int64_t t = now_us();
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = last.find(key ? key : "");
        if (it != last.end() && t - it->second < (int64_t)min_interval_s * 1000000) return;
        last[key ? key : ""] = t;
    }
    std::fprintf(stderr, "[NET] %s\n", msg.c_str());
    ge_draw_toast(msg.c_str(), msg.find('\n') == std::string::npos ? 6000 : 9000);
    g_notice_at.store(t, std::memory_order_relaxed);
}
bool psp_net_notice_showing() { return now_us() - g_notice_at.load(std::memory_order_relaxed) < 6000000; }

int psp_adhoc_ping_ms() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_connected) return -1;
    double worst = -1;
    for (auto& [mac, st] : g_ping)
        if (st.ever && st.srtt_ms >= 0 && same_group(mac)) worst = std::max(worst, st.srtt_ms);
    return worst < 0 ? -1 : (int)(worst + 0.5);
}
// Lost pings of the worst path since those players joined, in 1/1000 (a ping is lost when either direction loses it); 0 with fewer than
// 30 pings sent (half a minute in the room) or no ping at all. The last two pings are not counted (their answers may be on the way).
int psp_adhoc_ping_loss_permille() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_connected) return 0;
    int worst = 0;
    for (auto& [mac, st] : g_ping)
        if (st.ever && same_group(mac) && st.asked >= 32) worst = std::max(worst, (int)(1000.0 * std::max(0.0, double(st.asked - 2) - st.answered) / (st.asked - 2)));
    return worst;
}
// The jitter of the slowest player's path: how far single round trips are from the smoothed one, on average (ms); -1 with no ping.
int psp_adhoc_ping_jitter_ms() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_connected) return -1;
    double worst = -1, var = -1;
    for (auto& [mac, st] : g_ping)
        if (st.ever && st.srtt_ms >= 0 && same_group(mac) && st.srtt_ms + 2 * st.var_ms > worst) { worst = st.srtt_ms + 2 * st.var_ms; var = st.var_ms; }
    return var < 0 ? -1 : (int)(var + 0.5);
}
// ---- side channel for game modules (see handle_pdp) ----
void psp_adhoc_local_mac(uint8_t out[6]) { std::lock_guard<std::mutex> lk(g_mu); std::memcpy(out, g_mac.data(), 6); }
int psp_adhoc_group_macs(uint8_t (*out)[6], char (*nicks)[32], int max) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_connected) return 0;
    int k = 0;
    for (auto& [m, p] : g_peers)
        if (p.in_group && !std::strncmp(p.group, g_group, 8)) {
            if (k < max) { std::memcpy(out[k], m.data(), 6); if (nicks) { std::strncpy(nicks[k], p.nick, 31); nicks[k][31] = 0; } }
            k++;
        }
    return k;
}
void psp_adhoc_side_send(const uint8_t mac[6], uint16_t chan, const uint8_t* d, int n) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_connected || n < 0 || n > 0x1000) return;
    Mac dst; std::memcpy(dst.data(), mac, 6);
    std::vector<uint8_t> b;
    b.insert(b.end(), dst.begin(), dst.end());
    put16(b, (uint16_t)(SIDE_PORT0 + chan)); put16(b, (uint16_t)(SIDE_PORT0 + chan));
    b.insert(b.end(), d, d + n);
    send_to_mac(dst, T_PDP, b);
}
int psp_adhoc_side_recv(uint16_t chan, uint8_t src[6], uint8_t* buf, int max) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto it = g_side_q.begin(); it != g_side_q.end(); ++it) {
        if (it->chan != chan) continue;
        const int n = std::min<int>(max, (int)it->data.size());
        std::memcpy(buf, it->data.data(), n); std::memcpy(src, it->src.data(), 6);
        g_side_q.erase(it);
        return n;
    }
    return -1;
}
