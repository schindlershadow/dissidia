// games/dissidia/runtime/net_packs.cpp — online play with a player who uses a retail-moves mod (Bartz's Dissidia 1
// moves) when this player doesn't have it: the other player sends what this game needs, as verified packs.
//
// Trust: none in the peer. The packs that may be received are compiled in (net_packs.inc, from
// games/dissidia/net_packs.json via tools/netpack.py): every file with its size and SHA-256, the SHA-256 of the retail
// file its delta applies to, and the abilities. A peer only ever sends delta bytes. This game
//   * applies a delta (bounds-checked: every copy inside its own retail file, the output never past the listed size)
//     to its OWN retail file, and keeps the result only when size and SHA-256 are the listed ones;
//   * stores it as <mods>/.net_cache/<sha256>.bin, a name made here from the list, never from what the peer sent;
//   * makes the ability records itself, from the list (a copy of its own record the list names, the listed command);
//   * refuses to start a match with a player who uses a pack it does not know (another version, an edited mod, a pack
//     the player cannot share) or has not received and verified yet, and with a player who has not confirmed having
//     the packs this player uses (an older build never confirms: no match with it while a pack is in use).
// So a peer can make this game use exactly the listed files and abilities, or nothing.
//
// Wire (the ad-hoc side channel, psp_adhoc_side_*; little endian):
//   'M' 1 nuse {name[24] u16 version u8 state(1 = shareable, 2 = in use, not shareable)}*  nready {name[24] u16 version}*
//   'R' sha256[32] u32 offset u16 length          request a piece of a delta (at most kChunk bytes)
//   'D' sha256[32] u32 total u32 offset bytes     a piece of a delta (total = the delta's size)
//   'B' ...                                        the battle pad link, handed to comm_battle.cpp
// The manifest ends with the build id (26 Nov): 'V' u16 netcode-protocol u8 len commit-string. Older builds stop reading after the
// pack list, so the trailer is ignored by them (their players show up as "unknown build"). A different protocol = no match (refusal()).
// A sender serves only the deltas tools/netpack.py wrote for packs whose files it has installed byte for byte
// (<mods>/.net/<sha256 of the result>.dlt).
#include "psp_game_module.h"
#include "hle/psp_hle_adhoc.h"

void dissidia_comm_on_side(const uint8_t mac[6], const uint8_t* data, size_t len);   // comm_battle.cpp
#include "package_mods.h"
#include "net_delta.h"
#include "voice_merge.h"
#include "recomp.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

std::string psp_git_commit();   // psp_report.cpp
namespace psp_cloud { std::string sha256_hex(const std::vector<uint8_t>& data); }
std::string dissidia_mods_dir();
std::string dissidia_package_dir();
std::string dissidia_voice_jp_dir();
void dissidia_mods_net_possible();
bool dissidia_mods_add_override(uint8_t* rdram, const std::string& asset, const std::string& file, uint32_t size);
int dissidia_retail_moves_state(const std::string& folder);
bool dissidia_ablt_ensure(uint8_t* rdram, uint16_t id, uint16_t copy, uint16_t cmd, uint8_t type, uint8_t phase, uint8_t owner, int cp, int cp_mastered, int ap);
bool dissidia_chrl_set_row(uint8_t* rdram, int charId, const std::vector<uint8_t>& entries);
void dissidia_ablt_dump(uint8_t* rdram, const char* tag, const uint16_t* ids, size_t nids);

namespace {

struct NetFile { const char* path; uint32_t size; const char* sha; uint32_t base_size; const char* base_sha; };
struct NetAbil { uint16_t id, copy, cmd; uint8_t type, phase; int16_t cp, cp_mastered, ap; };   // costs -1: the copied record's own
struct NetPack { const char* name; int version; int charId; std::vector<NetFile> files; std::vector<NetAbil> abilities; const char* chrl; };   // chrl: the character's 'chrl' row (hex of 8-byte entries), "" = none
const std::vector<NetPack>& packs() {
    static const std::vector<NetPack> v = {
#include "net_packs.inc"
    };
    return v;
}

using Mac = std::array<uint8_t, 6>;
constexpr size_t kName = 24, kChunk = 1024, kWindow = 24;
constexpr uint32_t kMaxDelta = 8u << 20;
constexpr int64_t kResendUs = 400000, kGiveUpUs = 90000000, kPeerGoneUs = 10000000, kManifestUs = 1000000;
const Mac kBcast{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string mac_str(const Mac& m) {
    char b[24];
    std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}
bool read_file(const std::string& p, std::vector<uint8_t>* out, uint64_t max) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0 || (uint64_t)st.st_size > max) return false;
    std::ifstream f(p, std::ios::binary);
    out->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return f.good() || f.eof();
}
std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; i++) { std::snprintf(b, sizeof b, "%02x", p[i]); s += b; }
    return s;
}
bool unhex(const char* h, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        if (std::sscanf(h + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

// ---- build id: the netcode protocol (bump when lock-step / simulation-visible behaviour changes in a way old builds cannot follow) and the
// git commit this build was made from. Exchanged in the manifest (lobby) and the tag roster (tagteam_net.cpp). ----
constexpr uint16_t kNetProtocolReal = 1;
// PSPRECOMP_NETBUILD_FAKE=<id>[:<protocol>]: test aid, announces another build id (and netcode version) than this build's
const char* fake_build() { static const char* e = std::getenv("PSPRECOMP_NETBUILD_FAKE"); return e && *e ? e : nullptr; }
// A fingerprint of this machine's C math library: a hash of what sinf / cosf / tanf / atan2f / powf / expf / logf / fmodf / sqrtf / floorf give for a fixed set
// of inputs (volatile: computed at run time, not folded by the compiler). The simulation itself does not call libm (the VFPU is table code, the generated code
// is plain float operations), so every build gives the same value today; if a platform's math (msvcrt / ucrt / glibc / bionic) or float codegen ever differs,
// the id below differs and both players are warned like for a build mismatch. PSPRECOMP_LIBM_FP_OFF=1: not part of the build id.
uint32_t libm_fingerprint() {
    static const uint32_t fp = [] {
        volatile float in[] = {0.0f, 0.1f, 0.5f, 0.7853982f, 1.0f, 1.5707964f, 2.0f, 3.1415927f, 4.7f, 6.2831855f, 10.0f, 100.0f, 1e-3f, 1234.5678f, -0.3f, -2.5f, -17.25f, 0.999f};
        uint32_t h = 2166136261u;
        auto mix = [&](float v) { uint32_t u; std::memcpy(&u, &v, 4); for (int k = 0; k < 4; k++) { h ^= (u >> (8 * k)) & 0xFFu; h *= 16777619u; } };
        for (volatile float x : in) {
            const float a = x;
            mix(sinf(a)); mix(cosf(a)); mix(tanf(a)); mix(atanf(a)); mix(expf(a * 0.1f)); mix(sqrtf(a < 0 ? -a : a)); mix(floorf(a)); mix(truncf(a * 7.7f));
            if (a > 0) { mix(logf(a)); mix(log2f(a)); mix(exp2f(a * 0.05f)); }
            for (volatile float y : in) { const float b = y; mix(atan2f(a, b)); mix(fmodf(a, b == 0 ? 1.0f : b)); if (a > 0) mix(powf(a, b > 8 ? 8.0f : b)); }
        }
        return h;
    }();
    return fp;
}
std::string local_commit() {
    static const std::string c = [] {
        std::string s = fake_build() ? std::string(fake_build(), std::strcspn(fake_build(), ":")) : psp_git_commit();
        if (!std::getenv("PSPRECOMP_LIBM_FP_OFF") && !fake_build()) { char b[16]; std::snprintf(b, sizeof b, "+m%06X", libm_fingerprint() & 0xFFFFFFu); s += b; }
        return s;
    }();
    return c;
}
uint16_t local_proto() {
    static const uint16_t p = [] { const char* f = fake_build(); const char* c = f ? std::strchr(f, ':') : nullptr; return c ? (uint16_t)std::atoi(c + 1) : kNetProtocolReal; }();
    return p;
}
#define kNetProtocol local_proto()
struct PeerBuild { bool known = false; uint16_t proto = 0; std::string commit; };

// ---- state (g_mu; never held while calling into the ad-hoc layer, which calls the handler and filter with its own lock) ----
struct Peer {
    int64_t seen = 0;
    bool manifest = false;
    std::vector<std::pair<int, int>> uses;   // (pack index or -1 = unknown to this build, state)
    std::set<int> ready;                     // packs the peer says it has
};
struct Download {
    int pack = -1, file = -1;
    Mac from{};
    std::vector<uint8_t> data;
    uint32_t total = 0;                      // 0 = not known yet (first piece not received)
    std::vector<uint8_t> got;                // per piece
    std::map<uint32_t, int64_t> asked;       // piece -> when requested
    int64_t started = 0;
};
std::mutex g_mu;
std::map<Mac, Peer> g_peers;
std::deque<std::pair<Mac, std::vector<uint8_t>>> g_inbox;
std::vector<int> g_file_ok;                  // per (pack, file) flattened: -1 unknown, 0 no, 1 installed, 2 cached
std::vector<int> g_pack_state;               // per pack: 0 not ready, 1 ready, 2 failed (a received file did not verify)
std::vector<bool> g_want_active, g_active;   // a peer in the group uses it / applied in this session
std::vector<bool> g_share;                   // this player uses it and can send it
std::vector<int> g_use;                      // this player uses it: 1 shareable, 2 not shareable, 0 no
std::vector<bool> g_use_exact;               // this player uses it AND its installed files are the listed ones byte for byte
std::map<Mac, PeerBuild> g_builds;   // the build id every player sent (kept after the player left the manifest list: the tag battle reads it)
std::map<Mac, std::pair<std::string, int64_t>> g_refuse_why;   // allow_match's last verdict per player ("" = allowed) and when, shown by the worker
std::map<std::string, std::vector<uint8_t>> g_serve;   // sha -> delta bytes this player sends
std::vector<Download> g_dl;
std::atomic<bool> g_started{false};
std::atomic<bool> g_activate_pending{false};
Mac g_own{};

size_t file_slot(int p, int f) {
    size_t n = 0;
    for (int i = 0; i < p; i++) n += packs()[i].files.size();
    return n + (size_t)f;
}
std::string cache_dir() { return dissidia_mods_dir() + "/.net_cache"; }
std::string cache_path(const NetFile& f) { return cache_dir() + "/" + f.sha + ".bin"; }
std::string installed_path(const NetFile& f) { return dissidia_mods_dir() + "/" + f.path; }

// a file is available when this player's mods dir has it byte for byte, or a verified copy is in the cache (hashed
// again on first use: the cache is a plain folder)
int check_file(const NetFile& f) {
    std::vector<uint8_t> b;
    if (read_file(installed_path(f), &b, f.size) && b.size() == f.size && psp_cloud::sha256_hex(b) == f.sha) return 1;
    if (read_file(cache_path(f), &b, f.size) && b.size() == f.size && psp_cloud::sha256_hex(b) == f.sha) return 2;
    return 0;
}

void refresh_local() {   // worker, once: what this player has and uses
    const auto& P = packs();
    size_t nf = 0;
    for (const auto& p : P) nf += p.files.size();
    std::vector<int> ok(nf, 0), st(P.size(), 0), use(P.size(), 0);
    std::vector<bool> share(P.size(), false), exact(P.size(), false);
    std::map<std::string, std::vector<uint8_t>> serve;
    for (size_t i = 0; i < P.size(); i++) {
        bool all = true, installed = true;
        for (size_t f = 0; f < P[i].files.size(); f++) {
            const int c = check_file(P[i].files[f]);
            ok[file_slot((int)i, (int)f)] = c;
            all = all && c != 0;
            installed = installed && c == 1;
        }
        st[i] = all ? 1 : 0;
        if (dissidia_retail_moves_state(P[i].name) != 1) continue;     // this player does not use it
        bool deltas = installed;
        for (const auto& f : P[i].files) {
            std::vector<uint8_t> d;
            if (!deltas) break;
            if (!read_file(dissidia_mods_dir() + "/.net/" + f.sha + ".dlt", &d, kMaxDelta) || d.size() < 12) { deltas = false; break; }
            serve[f.sha] = std::move(d);
        }
        share[i] = deltas;
        exact[i] = installed;
        use[i] = deltas ? 1 : 2;
        // (23 Nov: one "NOT shareable" line covered both cases below; the first one made every other player that uses the pack refuse
        // this one silently - "players cannot join", desktop + RP6)
        std::fprintf(stderr, "[NETPACK] %s v%d: in use here, %s\n", P[i].name, P[i].version,
                     deltas ? "can be sent to players who don't have it"
                     : installed ? "the known build, but NOT shareable: its online copies (<mods>/.net/*.dlt) are missing (tools/netpack.py "
                                   "deltas <mods> --disc <disc0> makes them): players who have it can play this player, players without it cannot"
                                 : "but its files are NOT the known build (another version or an edited mod): NO online match with any "
                                   "player while it is on (Settings > Mods turns it off, or reinstall it)");
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_file_ok = ok; g_pack_state = st; g_use = use; g_use_exact = exact; g_share = share; g_serve = std::move(serve);
    g_want_active.assign(P.size(), false);
    g_active.assign(P.size(), false);
}

int pack_by_name(const uint8_t* name, uint16_t ver) {
    char n[kName + 1];
    std::memcpy(n, name, kName);
    n[kName] = 0;
    for (size_t i = 0; i < packs().size(); i++)
        if (std::strcmp(packs()[i].name, n) == 0 && packs()[i].version == ver) return (int)i;
    return -1;
}
void put_name(std::vector<uint8_t>& b, const char* name, int ver) {
    uint8_t n[kName] = {0};
    std::strncpy(reinterpret_cast<char*>(n), name, kName - 1);
    b.insert(b.end(), n, n + kName);
    b.push_back(ver & 0xff); b.push_back((ver >> 8) & 0xff);
}
template <class T> void put(std::vector<uint8_t>& b, T v) { const uint8_t* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + sizeof v); }

std::vector<uint8_t> manifest() {   // g_mu held
    std::vector<uint8_t> b{'M', 1};
    std::vector<int> uses;
    for (size_t i = 0; i < g_use.size(); i++) if (g_use[i]) uses.push_back((int)i);
    b.push_back((uint8_t)uses.size());
    for (int i : uses) { put_name(b, packs()[i].name, packs()[i].version); b.push_back((uint8_t)g_use[i]); }
    std::vector<int> ready;
    // ready = this game runs the listed files: received and applied, or (a pack in use here) its own install byte for byte. A
    // verified copy in the cache does not count for a pack in use here: the game runs the installed files.
    for (size_t i = 0; i < g_pack_state.size(); i++)
        if (g_use[i] ? (bool)g_use_exact[i] : g_pack_state[i] == 1 && g_active[i]) ready.push_back((int)i);
    b.push_back((uint8_t)ready.size());
    for (int i : ready) put_name(b, packs()[i].name, packs()[i].version);
    const std::string c = local_commit();   // build id trailer (older builds ignore what follows the pack list)
    b.push_back('V'); b.push_back(kNetProtocol & 0xff); b.push_back(kNetProtocol >> 8); b.push_back((uint8_t)std::min<size_t>(c.size(), 32));
    b.insert(b.end(), c.begin(), c.begin() + std::min<size_t>(c.size(), 32));
    return b;
}

std::string build_mismatch_text(const PeerBuild& pb) {
    if (!pb.known) return "Online: another player runs an older build (no build id; this game: build " + local_commit() + ", netcode " + std::to_string(kNetProtocol) + "). Fights may desync.";
    return "Online: another player runs build " + pb.commit + " (netcode " + std::to_string(pb.proto) + "), this game build " + local_commit() + " (netcode " + std::to_string(kNetProtocol) +
           (pb.proto != kNetProtocol ? "): no match possible." : "). Fights may desync.");
}
void note_build(const Mac& from, uint16_t pr, std::string c) {   // g_mu held
    for (char& ch : c) if (ch < 32 || ch > 126) ch = '?';
    PeerBuild& pb = g_builds[from];
    if (!pb.known || pb.proto != pr || pb.commit != c)
        std::fprintf(stderr, "[BUILD] %s runs build %s, netcode version %u (this game: %s, version %u)\n", mac_str(from).c_str(), c.c_str(), pr, local_commit().c_str(), kNetProtocol);
    pb = {true, pr, c};
}

// ---- network thread: copy and queue only ----
void on_side(const uint8_t mac[6], const uint8_t* data, size_t len) {
    if (!len || len > PSP_ADHOC_SIDE_MAX) return;
    if (data[0] == 'B') { dissidia_comm_on_side(mac, data, len); return; }   // the battle pad link (comm_battle.cpp)
    Mac m;
    std::memcpy(m.data(), mac, 6);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_inbox.size() > 4096) return;           // a flood: drop
    g_inbox.emplace_back(m, std::vector<uint8_t>(data, data + len));
}

// may a match with this player start? (network thread, called with the ad-hoc lock held: only reads state)
// "" = yes, else why not. Both players decide from the same facts (each one's packs in use, shareable or not, and whether
// it runs the listed files), so they agree: a match refused on one side only was a lobby where one player sees the other and
// a battle request that does nothing (23 Nov, desktop + RP6 both using bartz-dff, the desktop's "not shareable").
std::string refusal(const Mac& m) {   // g_mu held
    for (size_t i = 0; i < g_use.size(); i++)   // a pack in use here whose files are not the listed ones: every player refuses us
        if (g_use[i] && !(i < g_use_exact.size() && g_use_exact[i]))
            return std::string("this game's ") + packs()[i].name + " is not the known build";
    bool i_use = false;
    for (int u : g_use) i_use = i_use || u != 0;
    if (auto bi = g_builds.find(m); bi != g_builds.end() && bi->second.known && bi->second.proto != kNetProtocol)
        return "that player's netcode version " + std::to_string(bi->second.proto) + " (build " + bi->second.commit + ") differs from this game's version " +
               std::to_string(kNetProtocol) + " (build " + local_commit() + "): both need the same build";
    auto it = g_peers.find(m);
    if (it == g_peers.end() || !it->second.manifest) return i_use ? "no pack list from that player yet (or an older build)" : "";
    const Peer& p = it->second;
    for (auto [idx, state] : p.uses) {
        if (idx < 0) return "that player uses a pack this build does not know";
        // runs the listed files: shareable (state 1) or not (it listed the pack as ready: installed byte for byte)
        if (state != 1 && !p.ready.count(idx)) return std::string("that player's ") + packs()[idx].name + " is not the known build";
        if (!g_use[idx] && !(g_pack_state[idx] == 1 && g_active[idx]))
            return std::string("that player's ") + packs()[idx].name + (state == 1 ? " is not received yet" : " cannot be sent (and this game does not have it)");
    }
    // A pack applied from an earlier lobby of this session stays in the game's tables and files (it cannot be taken out again): a player who does not use it
    // any more (turned the mod off, or never had it) fights with other tables, so the two would desync at the first Bartz move. Say so instead of starting.
    static const bool stale_old = std::getenv("PSPRECOMP_NETPACK_STALE_OLD") != nullptr;
    if (!stale_old)
        for (size_t i = 0; i < g_active.size(); i++) {
            if (!g_active[i] || g_use[i]) continue;
            bool theirs = false;
            for (auto [idx, state] : p.uses) theirs = theirs || idx == (int)i;
            if (!theirs) return std::string("this game still has ") + packs()[i].name + " from an earlier lobby (restart the game: that player does not use it)";
        }
    for (size_t i = 0; i < g_use.size(); i++)
        if (g_use[i] && !p.ready.count((int)i))
            return std::string("that player does not have ") + packs()[i].name + (g_use[i] == 1 ? " yet" : " (and this game cannot send it)");
    return "";
}

bool allow_match(const uint8_t mac[6]) {
    Mac m;
    std::memcpy(m.data(), mac, 6);
    std::lock_guard<std::mutex> lk(g_mu);
    std::string why = refusal(m);
    const bool ok = why.empty();
    g_refuse_why[m] = {std::move(why), now_us()};
    return ok;
}

// ---- worker ----
void handle(const Mac& from, const std::vector<uint8_t>& b, std::vector<std::pair<Mac, std::vector<uint8_t>>>& out) {   // g_mu held
    const int64_t t = now_us();
    if (b[0] == 'M' && b.size() >= 4 && b[1] == 1) {
        Peer& p = g_peers[from];
        p.seen = t;
        p.manifest = true;
        p.uses.clear();
        p.ready.clear();
        size_t o = 2;
        const size_t nuse = b[o++];
        for (size_t k = 0; k < nuse; k++) {
            if (o + kName + 3 > b.size()) return;
            uint16_t ver = b[o + kName] | (b[o + kName + 1] << 8);
            p.uses.emplace_back(pack_by_name(&b[o], ver), b[o + kName + 2]);
            o += kName + 3;
        }
        if (o >= b.size()) return;
        const size_t nready = b[o++];
        for (size_t k = 0; k < nready; k++) {
            if (o + kName + 2 > b.size()) return;
            const int idx = pack_by_name(&b[o], (uint16_t)(b[o + kName] | (b[o + kName + 1] << 8)));
            if (idx >= 0) p.ready.insert(idx);
            o += kName + 2;
        }
        if (o + 4 <= b.size() && b[o] == 'V' && o + 4 + b[o + 3] <= b.size()) {   // the build id (newer builds)
            const uint16_t pr = (uint16_t)(b[o + 1] | (b[o + 2] << 8));
            note_build(from, pr, std::string(reinterpret_cast<const char*>(&b[o + 4]), b[o + 3]));
        } else if (!g_builds.count(from)) {
            g_builds[from] = PeerBuild{};
            std::fprintf(stderr, "[BUILD] %s sends no build id: an older build (this game: %s, version %u)\n", mac_str(from).c_str(), local_commit().c_str(), kNetProtocol);
        }
        for (auto [idx, state] : p.uses) {
            if (idx < 0) continue;
            g_want_active[idx] = true;
            if (g_pack_state[idx] == 0 && state == 1 && !g_use[idx]) {   // fetch from this player what is missing (a pack in use here runs
                                                                         // its installed files: a fetched copy would not be used)
                for (size_t f = 0; f < packs()[idx].files.size(); f++) {
                    if (g_file_ok[file_slot(idx, (int)f)] != 0) continue;
                    bool have = false;
                    for (auto& d : g_dl) have = have || (d.pack == idx && d.file == (int)f);
                    if (have) continue;
                    Download d;
                    d.pack = idx; d.file = (int)f; d.from = from; d.started = t;
                    g_dl.push_back(d);
                    std::fprintf(stderr, "[NETPACK] %s: fetching %s from %s\n", packs()[idx].name, packs()[idx].files[f].path, mac_str(from).c_str());
                }
            } else if (g_pack_state[idx] == 0 && state != 1 && !g_use[idx]) {
                std::fprintf(stderr, "[NETPACK] %s uses %s but cannot send it: no match with that player\n", mac_str(from).c_str(), packs()[idx].name);
            }
        }
        if (g_pack_state.size()) for (auto [idx, state] : p.uses) if (idx < 0) {
            std::fprintf(stderr, "[NETPACK] %s uses a pack this build does not know: no match with that player\n", mac_str(from).c_str());
            break;
        }
        if (g_activate_pending.load() == false) for (size_t i = 0; i < g_want_active.size(); i++)
            if (g_want_active[i] && g_pack_state[i] == 1 && !g_active[i] && !g_use[i]) g_activate_pending = true;
        g_refuse_why[from] = {refusal(from), t};   // known from the list alone (the other player may never send us a matching message)
        return;
    }
    if (b[0] == 'R' && b.size() == 1 + 32 + 4 + 2) {
        const std::string sha = hex(&b[1], 32);
        auto it = g_serve.find(sha);
        if (it == g_serve.end()) return;                                 // not something this player sends
        uint32_t off; uint16_t n;
        std::memcpy(&off, &b[33], 4);
        std::memcpy(&n, &b[37], 2);
        const std::vector<uint8_t>& d = it->second;
        if (off >= d.size()) return;
        n = (uint16_t)std::min<size_t>({(size_t)n, kChunk, d.size() - off});
        std::vector<uint8_t> r{'D'};
        r.insert(r.end(), &b[1], &b[33]);
        put<uint32_t>(r, (uint32_t)d.size());
        put<uint32_t>(r, off);
        r.insert(r.end(), d.begin() + off, d.begin() + off + n);
        out.emplace_back(from, std::move(r));
        return;
    }
    if (b[0] == 'D' && b.size() > 1 + 32 + 8) {
        uint8_t want[32];
        for (auto& dl : g_dl) {
            if (dl.from != from) continue;
            if (!unhex(packs()[dl.pack].files[dl.file].sha, want) || std::memcmp(want, &b[1], 32) != 0) continue;
            uint32_t total, off;
            std::memcpy(&total, &b[33], 4);
            std::memcpy(&off, &b[37], 4);
            const size_t n = b.size() - 41;
            if (total < 12 || total > kMaxDelta || (dl.total && total != dl.total) || off % kChunk || n > kChunk ||
                (uint64_t)off + n > total || (off + n != total && n != kChunk)) return;   // not the shape a sender makes
            if (!dl.total) {
                dl.total = total;
                dl.data.assign(total, 0);
                dl.got.assign((total + kChunk - 1) / kChunk, 0);
            }
            const uint32_t piece = off / kChunk;
            std::memcpy(dl.data.data() + off, &b[41], n);
            dl.got[piece] = 1;
            dl.asked.erase(piece);
            return;
        }
    }
}

// a finished delta: rebuild the file from this player's own retail file and keep it only if it is the listed one
int finish(Download& dl) {   // g_mu NOT held (reads files)
    const NetFile& f = packs()[dl.pack].files[dl.file];
    std::vector<uint8_t> base, out;
    const std::string dir = dissidia_package_dir();
    if (dir.empty() || !dissidia::read_asset(dir, "", f.path, &base)) {
        std::fprintf(stderr, "[NETPACK] %s: no retail %s to apply the delta to\n", packs()[dl.pack].name, f.path);
        return -1;
    }
    if (base.size() != f.base_size || psp_cloud::sha256_hex(base) != f.base_sha) {
        std::fprintf(stderr, "[NETPACK] %s: this disc's %s is not the known retail file: cannot use the pack\n", packs()[dl.pack].name, f.path);
        return -1;
    }
    if (!dissidia::apply_delta(dl.data, base, f.size, &out) || psp_cloud::sha256_hex(out) != f.sha) {
        std::fprintf(stderr, "[NETPACK] %s: %s from %s did NOT verify: thrown away, no match with that player\n",
                     packs()[dl.pack].name, f.path, mac_str(dl.from).c_str());
        return -1;
    }
    {
        std::error_code ec;                 // std::filesystem: Windows has no two-argument mkdir
        std::filesystem::create_directories(cache_dir(), ec);
    }
    const std::string tmp = cache_path(f) + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
        if (!o) return -1;
    }
    if (std::rename(tmp.c_str(), cache_path(f).c_str()) != 0) return -1;
    std::fprintf(stderr, "[NETPACK] %s: %s verified (%u B from a %u B delta), cached\n", packs()[dl.pack].name, f.path, f.size, dl.total);
    return 2;
}

void worker() {
    refresh_local();
    int64_t next_manifest = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const int64_t t = now_us();
        std::vector<std::pair<Mac, std::vector<uint8_t>>> out;
        std::vector<Download> done;
        std::vector<std::string> toast, buildwarn;
        uint8_t peers[32][6];
        size_t npeers = 0;
        psp_adhoc_group_peers(peers, 32, &npeers, g_own.data());
        {
            std::lock_guard<std::mutex> lk(g_mu);
            while (!g_inbox.empty()) {
                auto [m, b] = std::move(g_inbox.front());
                g_inbox.pop_front();
                if (!b.empty()) handle(m, b, out);
            }
            for (auto it = g_peers.begin(); it != g_peers.end();)
                if (t - it->second.seen > kPeerGoneUs) it = g_peers.erase(it); else ++it;
            // why a player in the lobby cannot be matched: in the log when it changes, on screen when it lasts (a refused match
            // used to be silent: the other player just never showed up / a request did nothing)
            static std::map<Mac, std::pair<std::string, int64_t>> shown;   // player -> (reason, since)
            for (auto it = g_refuse_why.begin(); it != g_refuse_why.end();)
                if (t - it->second.second > kPeerGoneUs) { shown.erase(it->first); it = g_refuse_why.erase(it); } else ++it;
            for (auto& [m, v] : g_refuse_why) {
                const std::string& why = v.first;
                auto& [last, since] = shown[m];
                if (why != last) {
                    if (!why.empty()) std::fprintf(stderr, "[NETPACK] no match with %s: %s\n", mac_str(m).c_str(), why.c_str());
                    else if (!last.empty()) std::fprintf(stderr, "[NETPACK] %s: match allowed now\n", mac_str(m).c_str());
                    last = why; since = t;
                } else if (!why.empty() && since >= 0 && t - since > 3000000) {
                    toast.push_back(why);
                    since = -1;
                }
            }
            static std::map<Mac, std::string> warned;   // player -> the build we warned about
            for (size_t i = 0; i < npeers && i < 32; i++) {
                Mac pm; std::memcpy(pm.data(), peers[i], 6);
                auto bi = g_builds.find(pm);
                if (bi == g_builds.end()) continue;   // no manifest yet
                const PeerBuild& pb = bi->second;
                if (pb.known && pb.commit == local_commit() && pb.proto == kNetProtocol) { warned.erase(pm); continue; }
                const std::string key = pb.known ? pb.commit + "/" + std::to_string(pb.proto) : "old";
                if (warned[pm] == key) continue;
                warned[pm] = key;
                buildwarn.push_back(build_mismatch_text(pb));
            }
            if (npeers && t >= next_manifest) { next_manifest = t + kManifestUs; out.emplace_back(kBcast, manifest()); }
            for (auto it = g_dl.begin(); it != g_dl.end();) {
                Download& d = *it;
                const bool complete = d.total && std::all_of(d.got.begin(), d.got.end(), [](uint8_t g) { return g != 0; });
                if (complete) { done.push_back(std::move(d)); it = g_dl.erase(it); continue; }
                if (t - d.started > kGiveUpUs || !g_peers.count(d.from)) {
                    std::fprintf(stderr, "[NETPACK] %s: gave up on %s (the player left or stopped sending)\n", packs()[d.pack].name, packs()[d.pack].files[d.file].path);
                    it = g_dl.erase(it);
                    continue;
                }
                uint8_t sha[32];
                if (!unhex(packs()[d.pack].files[d.file].sha, sha)) { it = g_dl.erase(it); continue; }
                auto ask = [&](uint32_t piece) {
                    std::vector<uint8_t> r{'R'};
                    r.insert(r.end(), sha, sha + 32);
                    put<uint32_t>(r, piece * (uint32_t)kChunk);
                    put<uint16_t>(r, (uint16_t)kChunk);
                    out.emplace_back(d.from, std::move(r));
                    d.asked[piece] = t;
                };
                if (!d.total) { if (!d.asked.count(0) || t - d.asked[0] > kResendUs) ask(0); }
                else {
                    for (auto& [piece, when] : d.asked) if (t - when > kResendUs) when = 0;   // resend lost ones
                    for (auto& [piece, when] : std::map<uint32_t, int64_t>(d.asked)) if (!when) ask(piece);
                    for (uint32_t piece = 0; piece < d.got.size() && d.asked.size() < kWindow; piece++)
                        if (!d.got[piece] && !d.asked.count(piece)) ask(piece);
                }
                ++it;
            }
        }
        for (auto& [m, b] : out) psp_adhoc_side_send(m.data(), b.data(), b.size());
        for (const std::string& w : buildwarn) { std::fprintf(stderr, "[BUILD] %s\n", w.c_str()); psp_net_notice("netbuild", 15, "%s", w.c_str()); }
        for (const std::string& w : toast) psp_net_notice("netpack_refuse", 20, "Online: no match with a player in the lobby: %s.", w.c_str());
        for (Download& d : done) {
            const int r = finish(d);
            std::lock_guard<std::mutex> lk(g_mu);
            g_file_ok[file_slot(d.pack, d.file)] = r > 0 ? r : 0;
            if (r < 0) { g_pack_state[d.pack] = 2; continue; }
            bool all = true;
            for (size_t f = 0; f < packs()[d.pack].files.size(); f++) all = all && g_file_ok[file_slot(d.pack, (int)f)] != 0;
            if (all && g_pack_state[d.pack] == 0) {
                g_pack_state[d.pack] = 1;
                g_activate_pending = true;
                std::fprintf(stderr, "[NETPACK] %s: every file received and verified\n", packs()[d.pack].name);
            }
        }
    }
}

// ---- game thread (psp_adhoc_pump): apply what a player in the group uses and this player now has ----
void on_pump(uint8_t* rdram) {
    if (!g_started.exchange(true)) std::thread(worker).detach();
    if (!g_activate_pending.exchange(false)) return;
    std::vector<int> todo;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (size_t i = 0; i < g_want_active.size(); i++)
            if (g_want_active[i] && g_pack_state[i] == 1 && !g_active[i] && !g_use[i]) todo.push_back((int)i);
    }
    for (int i : todo) {
        const NetPack& p = packs()[i];
        bool ok = true;
        for (size_t f = 0; f < p.files.size() && ok; f++) {
            int src;
            { std::lock_guard<std::mutex> lk(g_mu); src = g_file_ok[file_slot(i, (int)f)]; }
            if (src == 2) {
                std::string file = cache_path(p.files[f]);
                uint32_t size = p.files[f].size;
                const std::string jp = dissidia_voice_jp_dir();
                std::vector<uint8_t> jb, mb, out;
                int added = 0;
                if (!jp.empty() && std::strncmp(p.files[f].path, "voice/", 6) == 0 &&   // Japanese voices: their bank + the pack's added clips
                    read_file(jp + "/" + p.files[f].path, &jb, 64u << 20) && read_file(file, &mb, 64u << 20) &&
                    dissidia::merge_voice_bank(jb, mb, &out, &added)) {
                    const std::string mf = file + ".jp";
                    std::ofstream o(mf, std::ios::binary | std::ios::trunc);
                    o.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
                    if (o) { file = mf; size = (uint32_t)out.size(); }
                }
                ok = dissidia_mods_add_override(rdram, p.files[f].path, file, size);
            }
            // src 1: this player's own install of the pack, served since boot
        }
        for (const NetAbil& a : p.abilities)
            ok = ok && dissidia_ablt_ensure(rdram, a.id, a.copy, a.cmd, a.type, a.phase, (uint8_t)p.charId, a.cp, a.cp_mastered, a.ap);
        if (ok && p.chrl[0] && !std::getenv("PSPRECOMP_NETPACK_COSTS_OLD")) {   // the learned-by-level row a native install has through its main.bin
            std::vector<uint8_t> row;
            for (const char* c = p.chrl; c[0] && c[1]; c += 2) { char h[3] = {c[0], c[1], 0}; row.push_back((uint8_t)std::strtoul(h, nullptr, 16)); }
            ok = dissidia_chrl_set_row(rdram, p.charId, row);
        }
        {
            std::vector<uint16_t> ids;
            for (const NetAbil& a : p.abilities) ids.push_back(a.id);
            dissidia_ablt_dump(rdram, "after the online pack", ids.data(), ids.size());
        }
        std::lock_guard<std::mutex> lk(g_mu);
        if (ok) g_active[i] = true; else g_pack_state[i] = 2;
        std::fprintf(stderr, "[NETPACK] %s v%d %s for this session\n", p.name, p.version, ok ? "applied" : "could NOT be applied (no match with players who use it)");
    }
}

}  // namespace

// Called from dissidia_register_hooks.
void dissidia_net_packs_install() {
    if (packs().empty()) return;
    dissidia_mods_net_possible();
    psp_adhoc_side_set_handler(on_side);
    psp_adhoc_set_match_filter(allow_match);
    psp_adhoc_set_pump_hook(on_pump);
    std::fprintf(stderr, "[NETPACK] %zu known pack(s); online players can send them to this game (verified)\n", packs().size());
}

// ---- build id for tagteam_net.cpp (the tag roster exchange) ----
uint32_t dissidia_libm_fingerprint() { return libm_fingerprint(); }   // for netplay.cpp's "[NETSYNC] this build" line
void dissidia_netbuild_local(char* out, size_t n, int* proto) {
    std::snprintf(out, n, "%s", local_commit().c_str());
    if (proto) *proto = kNetProtocol;
}
void dissidia_netbuild_note(const uint8_t mac[6], int proto, const char* commit) {   // a player's build id from the roster exchange
    Mac m; std::memcpy(m.data(), mac, 6);
    std::lock_guard<std::mutex> lk(g_mu);
    note_build(m, (uint16_t)proto, commit ? commit : "");
}
// 0 = no information (no manifest / roster message seen), 1 = an older build (no build id), 2 = known (out, proto set)
int dissidia_netbuild_peer(const uint8_t mac[6], char* out, size_t n, int* proto) {
    Mac m; std::memcpy(m.data(), mac, 6);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_builds.find(m);
    if (it == g_builds.end()) return 0;
    if (!it->second.known) return 1;
    std::snprintf(out, n, "%s", it->second.commit.c_str());
    if (proto) *proto = it->second.proto;
    return 2;
}
std::string dissidia_netbuild_text(const uint8_t mac[6]) {
    Mac m; std::memcpy(m.data(), mac, 6);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_builds.find(m);
    if (it == g_builds.end() || (it->second.known && it->second.commit == local_commit() && it->second.proto == kNetProtocol)) return "";
    return build_mismatch_text(it->second);
}
