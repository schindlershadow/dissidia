// games/dissidia/runtime/comm_battle.cpp — Communications Mode battles on our own pad link.
//
// The game's own battle exchange is one function, the role-0 handler FUN_0890D664 (a0 = net = *(0x089D8364) + 0x10D8), run
// once a game frame by the network manager task. It samples this PSP's pad, packs it into a 16-bit word (12 buttons + 4 stick
// directions after the game's dead zone), trades words with the other fighter's PSP and, once both words for frame net+0x48
// are in, writes both into the raw pads' injected samples (pad manager + i * 0x60 + 0x40, player i in slot i on both PSPs)
// and returns 0; otherwise it returns 1 and the battle waits that frame. So online fighters only ever had 8 stick directions,
// its input delay grows to 2 frames and never shrinks, and every late word freezes the battle.
//
// This replaces that handler while a battle runs: each frame this copy's whole pad (buttons, the stick at full resolution) for
// frame f + delay goes to the other fighter's copy over the ad-hoc side channel (psp_adhoc_side_*, type byte 'B'), with this
// copy's last 8 frames in every packet (a lost packet is covered by the next), and the other copy's pad for frame f is injected
// next to ours. Every tenth frame the game's own check value (FUN_0890CD90, as the handler takes it) goes along and is compared
// with the other copy's; a difference ends the battle with the game's error, as the game does. When the other copy's pad for a
// frame is not there after a short wait the handler returns 1, as the game's does, so the game waits that frame (window and
// sound keep going) and the game's own timeout still ends a battle whose peer is gone.
//
// Watched fights (Group Battle's members who are not fighting, role 1, the game's FUN_0890DF58): the leading fighter (player 0)
// keeps both pads of every frame it played and streams them (STREAM, up to 36 frames a packet) to each watcher from the first
// frame that watcher lacks (WATCH says which); a watcher runs the fight from the stream, injecting both pads as the fighters do,
// and compares the game's check values with the leader's. The fighters only link once both saw every watcher announce itself.

// Both copies announce the link at the battle's first frame. A copy that hears nothing from the other within 5 s (an older
// build) keeps the game's own exchange for that battle, so mixed versions still play each other.
//
// Design and addresses from VCKing101's port (https://github.com/VCKing101/Dissidia012-pc, docs/port/communication-mode.md
// sections 4 and 11, port/host/d012_comm_battle.cpp, MIT); this file is our own code for our runtime and ad-hoc layer.
//
// PSPRECOMP_NET_LINK=0 keeps the game's exchange, PSPRECOMP_NET_DELAY=<frames> sets the input delay (default 2, 1..8),
// PSPRECOMP_NET_LINK_LOG=1 logs the link's progress, PSPRECOMP_NET_LINK_DROP=<percent> drops that share of the pad packets
// sent (tests).

#include "psp_game_module.h"
#include "psp_runtime.h"
#include "psp_netplay.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_adhoc.h"
#include "recomp.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

float dissidia_camera_turn();   // camera.cpp
void dissidia_input_turn_stick(uint8_t* lx, uint8_t* ly);   // input.cpp

namespace {

constexpr uint32_t HANDLER = 0x0890D664u;      // role 0's per-frame exchange (a0 = net)
constexpr uint32_t WATCH_HANDLER = 0x0890DF58u; // role 1's (a watcher's, Group Battle) per-frame handler (a0 = net)
constexpr uint32_t CHECK_STEP = 0x0890CD90u;   // the check value of every tenth frame (a0 = net)
constexpr uint32_t SESSION = 0x089D8364u;      // -> the lobby session; net is at +0x10D8
constexpr uint32_t SESSION_GROUP = 0x1664u;    // the battle group's name, 8 characters
constexpr uint32_t PAD_MGR = 0x08C15B90u;      // raw[i] = PAD_MGR + i * 0x60
constexpr uint32_t RAW_PAD = 0x60u, INJECTED = 0x40u;
// net fields
constexpr uint32_t N_QUIT_FRAME = 0x0Cu, N_FLAGS = 0x10u, N_CHECKS = 0x1Cu, N_CHECK_COUNT = 0x3Cu, N_ROLE = 0x40u,
                   N_LOCAL = 0x44u, N_FRAME = 0x48u, N_PEER_CONFIRMED = 0x4Cu + 0x50u, N_SAMPLED = 0x568u, N_SPECTATORS = 0x358u;
constexpr uint32_t F_OPEN = 1u, F_RUNNING = 2u, F_QUIT = 4u;

constexpr uint8_t MSG = 'B', VERSION = 2, K_PADS = 0, K_LEAVE = 1, K_WATCH = 2, K_STREAM = 3;
constexpr uint8_t WATCHER = 0xFF;   // the player byte of a watcher's packets
constexpr int HISTORY = 8;          // own pads repeated in every PADS packet
constexpr uint32_t STREAM_MAX = 36; // frames in one STREAM packet
constexpr uint32_t KEEP = 1800;     // frames the leader keeps for watchers (60 s)
// Wire (side channel type byte 'B', little endian): 'B' version kind player tag(4), then by kind
//   PADS   newest(4) n(1) n * {buttons(4) lx ly}   check frame(4) value(4)   watchers seen(1)
//   LEAVE  (as PADS)
//   WATCH  have(4): the watcher holds every frame below this
//   STREAM first(4) n(1) n * {player 0 pad(6), player 1 pad(6)}   check frame(4) value(4)
// Version 1 builds send version 1 and are ignored here (and ignore these): the link then falls back to the game's exchange.

FuncPtr g_orig = nullptr, g_orig_watch = nullptr, g_check = nullptr;
uint8_t* g_ram = nullptr;
bool g_enabled = true;
bool g_log = false;
uint32_t g_delay = 2;
uint32_t g_drop = 0;   // PSPRECOMP_NET_LINK_DROP=<percent>: drop that share of outgoing pad packets (tests)

uint32_t r32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
void w32(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &v, 4); }
void w8(uint32_t a, uint8_t v) { g_ram[a & 0x07FFFFFFu] = v; }
int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Pad { uint32_t buttons = 0; uint8_t lx = 128, ly = 128; };
using Mac = std::array<uint8_t, 6>;
const Mac BCAST{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---- inbox (filled on the network thread, read on the game thread) ----
std::mutex g_mu;
std::deque<std::pair<Mac, std::vector<uint8_t>>> g_inbox;

struct Watcher { uint32_t have = 0; int64_t heard = 0, sent = 0; };

// ---- the link of the battle in progress (a fighter's, or a watcher's) ----
struct Link {
    enum State { NONE, ANNOUNCING, LINKED, GAME } state = NONE;
    bool watcher = false;               // this copy watches (role 1): it runs the fight from the leader's stream
    Mac peer{};                         // fighter: the other fighter's copy; watcher: the leader's
    bool peer_known = false;
    uint32_t tag = 0;
    int local = 0;                      // fighter: this copy's player (0 leads: it streams to the watchers)
    uint32_t frame = 0;                 // the frame the game asks for next
    std::map<uint32_t, Pad> mine, theirs;
    std::map<uint32_t, uint32_t> my_checks, their_checks;
    int64_t started_us = 0, heard_us = 0, sent_us = 0;
    uint32_t newest_mine = 0;           // highest frame of mine filled (frames < delay are neutral)
    uint32_t waits = 0, agreed = 0;
    std::deque<int64_t> recent_waits;   // when the game last had to wait (warning)
    uint32_t latched = 0;               // buttons seen pressed between two logic frames (dissidia_comm_note_pad)
    // watchers
    uint32_t spectators = 0;            // fighter: how many the game says watch
    std::map<Mac, Watcher> watchers;    // fighter: the watchers that announced themselves
    bool their_watch_ok = false;        // fighter: the other fighter saw every watcher too
    std::map<uint32_t, std::array<Pad, 2>> history;   // leader: both pads of every played frame (for the stream)
    std::map<uint32_t, uint32_t> stream_checks;       // leader: the latest check values; watcher: the leader's
    std::map<uint32_t, std::array<Pad, 2>> stream;    // watcher: both pads by frame
    uint32_t have = 0;                  // watcher: every frame below this is in `stream` (or played)
};
Link g_link;
std::map<std::string, uint32_t> g_battles;   // battles begun per group (fighting or watching), for the tag

void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
void put_pad(std::vector<uint8_t>& b, const Pad& p) { put32(b, p.buttons); b.push_back(p.lx); b.push_back(p.ly); }
Pad get_pad(const uint8_t* p) { return Pad{get32(p), p[4], p[5]}; }

std::vector<uint8_t> header(uint8_t kind) {
    std::vector<uint8_t> b{MSG, VERSION, kind, g_link.watcher ? WATCHER : static_cast<uint8_t>(g_link.local)};
    put32(b, g_link.tag);
    return b;
}
bool watchers_ok() { return g_link.spectators == 0 || g_link.watchers.size() >= g_link.spectators; }

// A fighter's PADS (or LEAVE): to the other fighter once known, to the whole group while announcing.
void send(uint8_t kind) {
    std::vector<uint8_t> b = header(kind);
    const uint32_t newest = g_link.newest_mine;
    const uint32_t n = std::min<uint32_t>(HISTORY, newest + 1);
    put32(b, newest);
    b.push_back(static_cast<uint8_t>(n));
    for (uint32_t i = 0; i < n; i++) put_pad(b, g_link.mine[newest - i]);
    uint32_t cf = 0xFFFFFFFFu, cv = 0;
    if (!g_link.my_checks.empty()) { cf = g_link.my_checks.rbegin()->first; cv = g_link.my_checks.rbegin()->second; }
    put32(b, cf); put32(b, cv);
    b.push_back(watchers_ok() ? 1 : 0);
    g_link.sent_us = now_us();
    if (g_drop && kind == K_PADS && static_cast<uint32_t>(std::rand() % 100) < g_drop) return;   // test: a lossy network
    psp_adhoc_side_send(g_link.peer_known ? g_link.peer.data() : BCAST.data(), b.data(), b.size());
    if (kind == K_LEAVE && !g_link.watchers.empty()) psp_adhoc_side_send(BCAST.data(), b.data(), b.size());   // the watchers too
}

// The leader: frames a watcher lacks, as many as fit, while it lacks any (else every 100 ms).
void stream_to(const Mac& to, Watcher& w, bool force) {
    const int64_t t = now_us();
    const bool behind = !g_link.history.empty() && w.have <= g_link.history.rbegin()->first;
    if (!force && !behind && t - w.sent < 100000) return;
    std::vector<uint8_t> b = header(K_STREAM);
    uint32_t first = w.have;
    if (!g_link.history.empty() && first < g_link.history.begin()->first) first = g_link.history.begin()->first;   // older than kept
    put32(b, first);
    const size_t at = b.size();
    b.push_back(0);
    uint8_t n = 0;
    for (auto it = g_link.history.lower_bound(first); it != g_link.history.end() && it->first == first + n && n < STREAM_MAX; ++it, ++n) {
        put_pad(b, it->second[0]);
        put_pad(b, it->second[1]);
    }
    b[at] = n;
    uint32_t cf = 0xFFFFFFFFu, cv = 0;
    if (!g_link.stream_checks.empty()) { cf = g_link.stream_checks.rbegin()->first; cv = g_link.stream_checks.rbegin()->second; }
    put32(b, cf); put32(b, cv);
    w.sent = t;
    psp_adhoc_side_send(to.data(), b.data(), b.size());
}

void send_watch() {
    std::vector<uint8_t> b = header(K_WATCH);
    put32(b, g_link.have);
    g_link.sent_us = now_us();
    psp_adhoc_side_send(g_link.peer_known ? g_link.peer.data() : BCAST.data(), b.data(), b.size());
}

// Take what the others sent. Returns false when the copy this one depends on said it left.
bool drain() {
    std::deque<std::pair<Mac, std::vector<uint8_t>>> in;
    { std::lock_guard<std::mutex> lk(g_mu); in.swap(g_inbox); }
    bool alive = true;
    for (auto& [mac, b] : in) {
        if (b.size() < 8 || b[1] != VERSION || get32(&b[4]) != g_link.tag) continue;
        const uint8_t kind = b[2], from = b[3];
        if (g_link.watcher) {   // a watcher listens to the leader's STREAM (and its LEAVE)
            if (from != 0 || (kind != K_STREAM && kind != K_LEAVE)) continue;
            if (!g_link.peer_known) { g_link.peer = mac; g_link.peer_known = true; }
            if (mac != g_link.peer) continue;
            g_link.heard_us = now_us();
            if (kind == K_LEAVE) { alive = false; continue; }
            if (b.size() < 13) continue;
            const uint32_t first = get32(&b[8]), n = b[12];
            if (b.size() < 13 + 12u * n + 8u) continue;
            for (uint32_t i = 0; i < n; i++)
                if (first + i >= g_link.frame) g_link.stream[first + i] = {get_pad(&b[13 + 12 * i]), get_pad(&b[13 + 12 * i + 6])};
            while (g_link.stream.count(g_link.have) || g_link.have < g_link.frame) g_link.have++;
            const uint8_t* c = &b[13 + 12 * n];
            if (get32(c) != 0xFFFFFFFFu) g_link.stream_checks[get32(c)] = get32(c + 4);
            if (g_link.state == Link::ANNOUNCING) {
                g_link.state = Link::LINKED;
                if (g_log) std::fprintf(stderr, "[NETLINK] watching on the leader's stream (tag %06X)\n", g_link.tag);
            }
            continue;
        }
        // a fighter
        if (kind == K_WATCH && from == WATCHER && b.size() >= 12) {
            Watcher& w = g_link.watchers[mac];
            w.have = std::max(w.have, get32(&b[8]));
            w.heard = now_us();
            continue;
        }
        if ((kind != K_PADS && kind != K_LEAVE) || from != static_cast<uint8_t>(1 - g_link.local)) continue;
        if (!g_link.peer_known) { g_link.peer = mac; g_link.peer_known = true; }
        if (mac != g_link.peer || b.size() < 13) continue;
        g_link.heard_us = now_us();
        if (kind == K_LEAVE) alive = false;
        const uint32_t newest = get32(&b[8]);
        const uint32_t n = b[12];
        if (b.size() < 13 + 6u * n + 9u) continue;
        for (uint32_t i = 0; i < n && i <= newest; i++) g_link.theirs[newest - i] = get_pad(&b[13 + 6 * i]);
        const uint8_t* c = &b[13 + 6 * n];
        if (get32(c) != 0xFFFFFFFFu) g_link.their_checks[get32(c)] = get32(c + 4);
        g_link.their_watch_ok = c[8] != 0;
        if (g_link.state == Link::ANNOUNCING && g_link.their_watch_ok && watchers_ok()) {
            g_link.state = Link::LINKED;
            if (g_log)
                std::fprintf(stderr, "[NETLINK] linked with the other fighter's copy (tag %06X, delay %u, %zu watching)\n", g_link.tag, g_delay,
                             g_link.watchers.size());
        }
    }
    return alive;
}

void inject(int player, const Pad& p) {
    const uint32_t s = PAD_MGR + static_cast<uint32_t>(player) * RAW_PAD + INJECTED;
    w32(s, 0); w32(s + 4, p.buttons); w8(s + 8, p.lx); w8(s + 9, p.ly);
    for (uint32_t i = 10; i < 16; i++) w8(s + i, 0);
}

Pad sample_mine() {
    uint32_t buttons = 0, analog = 0x8080;
    psp_ctrl_sample_now(&buttons, &analog);
    Pad p;
    p.buttons = buttons | g_link.latched;
    g_link.latched = 0;
    p.lx = static_cast<uint8_t>(analog & 0xFF);
    p.ly = static_cast<uint8_t>((analog >> 8) & 0xFF);
    // The free camera's turn (camera.cpp) is this copy's view only: the stick is turned by it here, once, so every copy
    // injects the same bytes (input.cpp leaves injected sticks alone while the link runs).
    dissidia_input_turn_stick(&p.lx, &p.ly);
    return p;
}

uint32_t call_check(uint8_t* rdram, recomp_context* ctx, uint32_t net) {
    const recomp_context saved = *ctx;
    ctx->r[4] = static_cast<int32_t>(net);
    g_check(rdram, ctx);
    *ctx = saved;
    const uint32_t count = r32(net + N_CHECK_COUNT);
    if (count == 0 || count > 4) return 0xFFFFFFFFu;
    const uint32_t newest = net + N_CHECKS + (count - 1) * 8;
    return r32(newest) == g_link.frame ? r32(newest + 4) : 0xFFFFFFFFu;
}

void quit(recomp_context* ctx, uint32_t net, const char* why) {
    std::fprintf(stderr, "[NETLINK] battle ends at frame %u: %s\n", g_link.frame, why);
    psp_net_notice("link_end", 0, "Online: %s. The battle ends.", why);
    w32(net + N_FLAGS, r32(net + N_FLAGS) | F_QUIT);
    w32(net + N_QUIT_FRAME, g_link.frame);
    if (!g_link.watcher) send(K_LEAVE);
    g_link.state = Link::NONE;
    ctx->r[2] = 3;
}

uint32_t tag_for_battle() {
    const uint32_t session = r32(SESSION);
    std::string group;
    for (uint32_t i = 0; session && i < 8; i++) {
        const char c = static_cast<char>(g_ram[(session + SESSION_GROUP + i) & 0x07FFFFFFu]);
        if (!c) break;
        group += c;
    }
    const uint32_t ordinal = g_battles[group]++;
    uint32_t h = 2166136261u;
    for (char c : group) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return (h + ordinal * 0x9E3779B1u) & 0xFFFFFFu;
}

// A new battle at frame 0, fighting or watching: announce, and run on the link once the others answer.
void begin(uint32_t net, bool watcher) {
    g_link = Link{};
    g_link.state = Link::GAME;
    g_delay = static_cast<uint32_t>(psp_netplay_pick_delay());   // the Settings row may have changed since the last battle; Auto: from the ping now
    uint8_t peers[16][6]; size_t n = 0; uint8_t own[6];
    psp_adhoc_group_peers(peers, 16, &n, own);
    if (n == 0) return;
    g_link.watcher = watcher;
    g_link.tag = tag_for_battle();
    g_link.local = watcher ? 0 : static_cast<int>(r32(net + N_LOCAL)) & 1;
    g_link.spectators = watcher ? 0 : r32(net + N_SPECTATORS);
    g_link.state = Link::ANNOUNCING;
    g_link.started_us = g_link.heard_us = now_us();
    if (!watcher) {
        for (uint32_t f = 0; f < g_delay; f++) g_link.mine[f] = Pad{};   // the first frames: no input, on both copies
        g_link.newest_mine = g_delay - 1;
    }
    { std::lock_guard<std::mutex> lk(g_mu); g_inbox.clear(); }
    if (g_log)
        std::fprintf(stderr, "[NETLINK] battle starts: %s, announcing (tag %06X, %u watching)\n", watcher ? "watching" : (g_link.local ? "player 1" : "player 0"),
                     g_link.tag, g_link.spectators);
}

void note_wait() {
    g_link.waits++;
    // Waiting often: tell the player why the fight stutters (more than 5 waited frames within 2 s).
    const int64_t now = now_us();
    g_link.recent_waits.push_back(now);
    while (!g_link.recent_waits.empty() && now - g_link.recent_waits.front() > 2000000) g_link.recent_waits.pop_front();
    if (g_link.recent_waits.size() > 5)
        psp_net_notice("link_wait", 15, g_link.watcher ? "Online: waiting for the fighters (slow or lossy connection). Ping %d ms."
                                                       : "Online: waiting for the other player's input (slow or lossy connection). Ping %d ms.",
                       psp_adhoc_ping_ms());
}

// Both copies' check values for the same frame must agree; returns false (and ends the battle) on a difference.
bool compare_checks(recomp_context* ctx, uint32_t net, std::map<uint32_t, uint32_t>& mine, std::map<uint32_t, uint32_t>& theirs) {
    for (auto it = mine.begin(); it != mine.end();) {
        auto t = theirs.find(it->first);
        if (t == theirs.end()) { ++it; continue; }
        if (t->second != it->second) {
            char why[112];
            std::snprintf(why, sizeof why, "the battles differ (check at frame %u: %08X here, %08X there)", it->first, it->second, t->second);
            quit(ctx, net, why);
            return false;
        }
        theirs.erase(t);
        g_link.agreed++;
        it = mine.erase(it);
    }
    return true;
}

void handler_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t net = static_cast<uint32_t>(ctx->r[4]);
    const uint32_t flags = r32(net + N_FLAGS);
    const bool ours = g_enabled && (flags & (F_OPEN | F_RUNNING)) == (F_OPEN | F_RUNNING) && !(flags & F_QUIT) && r32(net + N_ROLE) == 0;
    const uint32_t frame = r32(net + N_FRAME);
    if (!ours) {
        if (!g_link.watcher && (g_link.state == Link::LINKED || g_link.state == Link::ANNOUNCING)) { send(K_LEAVE); g_link.state = Link::NONE; }
        g_orig(rdram, ctx);
        return;
    }
    if (frame == 0 && (g_link.state == Link::NONE || g_link.frame > 0 || g_link.watcher)) begin(net, false);   // a new battle
    g_link.frame = frame;
    if (g_link.state == Link::GAME || g_link.state == Link::NONE) { g_orig(rdram, ctx); return; }

    if (g_link.state == Link::ANNOUNCING) {
        send(K_PADS);
        drain();
        if (g_link.state == Link::ANNOUNCING) {
            if (now_us() - g_link.started_us > 5000000) {
                const bool others = g_link.peer_known;
                std::fprintf(stderr, "[NETLINK] %s in 5 s: the game's own exchange\n",
                             !others ? "no answer from the other fighter's copy (an older build?)" : "not every watcher answered (an older build?)");
                psp_net_notice("link_old", 30, others ? "Online: a watcher's game is an older version, so this fight uses the game's own netcode (8-way stick)."
                                                      : "Online: the other player's game is an older version, so this fight uses the game's own netcode (8-way stick).");
                g_link.state = Link::GAME;
                g_orig(rdram, ctx);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ctx->r[2] = 1;   // the game waits this frame
            return;
        }
    }

    // This copy's pad for frame + delay (once per frame: a retried frame keeps its sample).
    if (frame + g_delay > g_link.newest_mine) {
        g_link.mine[frame + g_delay] = sample_mine();
        g_link.newest_mine = frame + g_delay;
    }
    // The game's check value for this frame, before its input goes in, as the handler takes it.
    if (frame > 0 && frame % 10 == 0 && !g_link.my_checks.count(frame)) {
        const uint32_t v = call_check(rdram, ctx, net);
        if (v != 0xFFFFFFFFu) {
            g_link.my_checks[frame] = v;
            if (g_link.local == 0) {   // the leader passes its check values on to the watchers
                g_link.stream_checks[frame] = v;
                while (g_link.stream_checks.size() > 8) g_link.stream_checks.erase(g_link.stream_checks.begin());
            }
        }
    }
    send(K_PADS);

    const int64_t t0 = now_us();
    bool alive = true;
    for (;;) {
        alive = drain();
        if (!alive || g_link.theirs.count(frame)) break;
        if (now_us() - t0 > 25000) break;                                // let the game wait this frame
        if (now_us() - g_link.sent_us > 8000) send(K_PADS);             // resend while waiting
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (!alive) return quit(ctx, net, "the other player left the battle");
    if (!compare_checks(ctx, net, g_link.my_checks, g_link.their_checks)) return;
    if (!g_link.theirs.count(frame)) {
        note_wait();
        if (now_us() - g_link.heard_us > 10000000) return quit(ctx, net, "nothing from the other player for 10 s");
        ctx->r[2] = 1;
        return;
    }
    inject(g_link.local, g_link.mine[frame]);
    inject(1 - g_link.local, g_link.theirs[frame]);
    if (g_link.local == 0 && !g_link.watchers.empty()) {   // the leader: keep the frame for the watchers and send them what they lack
        g_link.history[frame] = g_link.local == 0 ? std::array<Pad, 2>{g_link.mine[frame], g_link.theirs[frame]}
                                                  : std::array<Pad, 2>{g_link.theirs[frame], g_link.mine[frame]};
        while (!g_link.history.empty() && g_link.history.begin()->first + KEEP < frame) g_link.history.erase(g_link.history.begin());
        for (auto& [mac, w] : g_link.watchers) stream_to(mac, w, false);
    }
    g_link.mine.erase(g_link.mine.begin(), g_link.mine.lower_bound(frame > HISTORY ? frame - HISTORY : 0));
    g_link.theirs.erase(g_link.theirs.begin(), g_link.theirs.lower_bound(frame));
    w32(net + N_FRAME, frame + 1);
    w32(net + N_SAMPLED, frame + 1);
    w32(net + N_PEER_CONFIRMED, frame);
    if (g_log && frame % 900 == 0 && frame)
        std::fprintf(stderr, "[NETLINK] frame %u: %u checks agreed, %u frames waited, %zu watching\n", frame, g_link.agreed, g_link.waits,
                     g_link.watchers.size());
    ctx->r[2] = 0;
}

// A watcher's frame (role 1, the game's FUN_0890DF58): both pads from the leader's stream, as the fighters inject them; the
// game's check value compared with the leader's; the game's wait (return 1) while the frame has not come.
void watch_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t net = static_cast<uint32_t>(ctx->r[4]);
    const uint32_t flags = r32(net + N_FLAGS);
    const bool ours = g_enabled && (flags & (F_OPEN | F_RUNNING)) == (F_OPEN | F_RUNNING) && !(flags & F_QUIT) && r32(net + N_ROLE) == 1;
    const uint32_t frame = r32(net + N_FRAME);
    if (!ours) {
        if (g_link.watcher) g_link.state = Link::NONE;
        g_orig_watch(rdram, ctx);
        return;
    }
    if (frame == 0 && (g_link.state == Link::NONE || g_link.frame > 0 || !g_link.watcher)) begin(net, true);
    g_link.frame = frame;
    if (g_link.state == Link::GAME || g_link.state == Link::NONE) { g_orig_watch(rdram, ctx); return; }
    if (now_us() - g_link.sent_us > 30000) send_watch();
    const int64_t t0 = now_us();
    bool alive = true;
    for (;;) {
        alive = drain();
        if (!alive || g_link.stream.count(frame)) break;
        if (g_link.state == Link::ANNOUNCING || now_us() - t0 > 25000) break;
        if (now_us() - g_link.sent_us > 30000) send_watch();
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (g_link.state == Link::ANNOUNCING) {
        if (now_us() - g_link.started_us > 5000000) {
            std::fprintf(stderr, "[NETLINK] no stream from the fighters in 5 s: the game's own exchange\n");
            g_link.state = Link::GAME;
            g_orig_watch(rdram, ctx);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ctx->r[2] = 1;
        return;
    }
    if (!alive) return quit(ctx, net, "the fighters left the battle");
    if (!g_link.stream.count(frame)) {
        note_wait();
        if (now_us() - g_link.heard_us > 10000000) return quit(ctx, net, "nothing from the fighters for 10 s");
        ctx->r[2] = 1;
        return;
    }
    if (frame > 0 && frame % 10 == 0 && !g_link.my_checks.count(frame)) {
        const uint32_t v = call_check(rdram, ctx, net);
        if (v != 0xFFFFFFFFu) g_link.my_checks[frame] = v;
    }
    if (!compare_checks(ctx, net, g_link.my_checks, g_link.stream_checks)) return;
    inject(0, g_link.stream[frame][0]);
    inject(1, g_link.stream[frame][1]);
    g_link.stream.erase(g_link.stream.begin(), g_link.stream.upper_bound(frame));
    w32(net + N_FRAME, frame + 1);
    if (g_log && frame % 900 == 0 && frame)
        std::fprintf(stderr, "[NETLINK] watching frame %u: %u checks agreed, %u frames waited\n", frame, g_link.agreed, g_link.waits);
    ctx->r[2] = 0;
}

}  // namespace

// The ad-hoc side channel's 'B' messages (net_packs.cpp owns the handler; network thread, lock held: copy only).
void dissidia_comm_on_side(const uint8_t mac[6], const uint8_t* data, size_t len) {
    if (len < 8 || data[0] != MSG) return;
    Mac m; std::memcpy(m.data(), mac, 6);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_inbox.size() > 512) g_inbox.pop_front();
    g_inbox.emplace_back(m, std::vector<uint8_t>(data, data + len));
}

// Whether this copy's battle runs on the link (input.cpp: injected sticks are then left as sent, with fixed constants).
bool dissidia_comm_link_active() { return g_link.state == Link::LINKED || g_link.state == Link::ANNOUNCING; }

// Called on drawn-only frames (framerate.cpp, 30 Hz logic): a press shorter than a logic frame still reaches the next sample.
void dissidia_comm_note_pad() {
    if (!dissidia_comm_link_active() || g_link.watcher) return;
    uint32_t buttons = 0, analog = 0;
    psp_ctrl_sample_now(&buttons, &analog);
    g_link.latched |= buttons;
}

void dissidia_comm_battle_install(uint8_t* rdram) {
    g_ram = rdram;
    const char* e = std::getenv("PSPRECOMP_NET_LINK");
    if (e && e[0] == '0') { std::fprintf(stderr, "[NETLINK] off: online battles use the game's own exchange\n"); return; }
    g_log = (e = std::getenv("PSPRECOMP_NET_LINK_LOG")) && e[0] && e[0] != '0';
    g_delay = static_cast<uint32_t>(psp_netplay_input_delay());   // the one network delay (env / Settings), read again at every battle start
    if ((e = std::getenv("PSPRECOMP_NET_LINK_DROP"))) g_drop = static_cast<uint32_t>(std::min(90, std::max(0, std::atoi(e))));
    g_orig = psp_dispatch_probe_lookup(HANDLER);
    g_orig_watch = psp_dispatch_probe_lookup(WATCH_HANDLER);
    g_check = psp_dispatch_probe_lookup(CHECK_STEP);
    if (!g_orig || !g_orig_watch || !g_check) { std::fprintf(stderr, "[NETLINK] NOT installed\n"); return; }
    psp_dispatch_register(HANDLER, handler_hook);
    psp_dispatch_register(WATCH_HANDLER, watch_hook);
    std::fprintf(stderr, "[NETLINK] online battles on our pad link (input delay %u frames)\n", g_delay);
}
