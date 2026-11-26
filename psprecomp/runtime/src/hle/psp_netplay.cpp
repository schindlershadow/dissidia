// runtime/src/hle/psp_netplay.cpp - netplay helpers on top of the ad-hoc emulation (issue #66). See include/psp_netplay.h.
#include "psp_netplay.h"
#include "hle/psp_hle_adhoc.h"
#include "psp_ge_draw.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {
std::atomic<uint64_t> g_frame{0};
std::atomic<bool> g_battle{false};
const bool g_trace = [] { const char* e = std::getenv("PSPRECOMP_NET_TRACE"); return e && e[0] && e[0] != '0'; }();

std::atomic<int> g_delay{-1};        // -1: not read from the environment yet
std::atomic<int> g_check{-1};

// ---- desync check ----
// Message: "NSYN", version, value count, 2 spare bytes, frame (u32 LE), then the values (u32 LE each).
constexpr uint8_t MSG_VERSION = 1;
constexpr size_t MSG_HDR = 12, MAX_VALUES = 160;
constexpr uint32_t KEEP_FRAMES = 600;   // frames kept waiting for the other side's values
using Mac = std::array<uint8_t, 6>;
using Values = std::vector<uint32_t>;

std::mutex g_mu;
const char* const* g_names = nullptr;
int g_name_count = 0;
std::map<uint32_t, Values> g_local;            // frame -> values not compared yet
std::map<Mac, std::map<uint32_t, Values>> g_remote;
uint32_t g_last_local = 0;
bool g_have_local = false;
uint64_t g_compared = 0, g_mismatched = 0;
int g_reports = 0;                             // mismatching frames reported in full so far (the first few: tells a fixed difference, e.g. leftover memory, from a growing one, e.g. float drift)
int64_t g_next_summary = 0;
std::atomic<uint32_t> g_first_bad{0xFFFFFFFFu};   // the first mismatching frame of this battle (psp_netplay_first_mismatch)

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string mac_str(const Mac& m) {
    char b[24];
    std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}
const char* field_name(int i) { return (g_names && i < g_name_count && g_names[i]) ? g_names[i] : "?"; }

void summary(const char* why) {   // caller holds g_mu
    if (g_compared || g_mismatched)
        std::fprintf(stderr, "[NETSYNC] %s: frames compared %llu, mismatched %llu\n", why, (unsigned long long)g_compared,
                     (unsigned long long)g_mismatched);
}

void compare(const Mac& peer, uint32_t frame, const Values& mine, const Values& theirs) {   // caller holds g_mu
    g_compared++;
    int differ = 0, first = -1;
    const size_t n = std::min(mine.size(), theirs.size());
    for (size_t i = 0; i < n; i++)
        if (mine[i] != theirs[i]) { if (first < 0) first = (int)i; differ++; }
    if (mine.size() != theirs.size()) differ++;
    if (!differ) return;
    g_mismatched++;
    if (g_reports >= 3 && (g_mismatched % 150) == 0) {   // later on: which fields still differ (a fixed word or a spreading split)
        std::string names;
        for (size_t i = 0; i < n; i++) if (mine[i] != theirs[i]) { names += ' '; names += field_name((int)i); }
        std::fprintf(stderr, "[NETSYNC] frame %u: %d fields differ:%s\n", frame, differ, names.c_str());
    }
    if (frame < g_first_bad.load(std::memory_order_relaxed)) g_first_bad.store(frame, std::memory_order_relaxed);
    if (g_reports >= 3) return;
    g_reports++;
    char msg[160];
    if (first >= 0) {
        std::fprintf(stderr, "[NETSYNC] DESYNC at frame %u with %s: %d of %zu fields differ, first %s local %08X remote %08X\n", frame,
                     mac_str(peer).c_str(), differ, n, field_name(first), mine[first], theirs[first]);
        for (size_t i = 0; i < n; i++)
            if (mine[i] != theirs[i]) std::fprintf(stderr, "[NETSYNC]   %-24s local %08X remote %08X\n", field_name((int)i), mine[i], theirs[i]);
        std::snprintf(msg, sizeof msg, "Desync at frame %u (%s)", frame, field_name(first));
    } else {
        std::fprintf(stderr, "[NETSYNC] DESYNC at frame %u with %s: %zu values here, %zu there (different builds?)\n", frame,
                     mac_str(peer).c_str(), mine.size(), theirs.size());
        std::snprintf(msg, sizeof msg, "Desync check: the other player's build differs");
    }
    ge_draw_toast(msg, 6000, 40);
}

void prune(uint32_t newest) {   // caller holds g_mu
    auto old = [&](uint32_t f) { return f + KEEP_FRAMES < newest || f > newest + KEEP_FRAMES; };
    for (auto it = g_local.begin(); it != g_local.end();) it = old(it->first) ? g_local.erase(it) : std::next(it);
    for (auto& [mac, m] : g_remote)
        for (auto it = m.begin(); it != m.end();) it = old(it->first) ? m.erase(it) : std::next(it);
}
}  // namespace

void psp_netplay_frame_end(bool battle) {
    g_frame.fetch_add(1, std::memory_order_relaxed);
    g_battle.store(battle, std::memory_order_relaxed);
}
uint32_t psp_netplay_first_mismatch() { return g_first_bad.load(std::memory_order_relaxed); }
uint64_t psp_netplay_frame() { return g_frame.load(std::memory_order_relaxed); }

int psp_netplay_input_delay() {
    int d = g_delay.load(std::memory_order_relaxed);
    if (d < 0) {
        const char* e = std::getenv("PSPRECOMP_NET_DELAY");
        if (!e || !*e) e = std::getenv("PSPRECOMP_NET_INPUT_DELAY");   // the old name
        d = std::clamp(e ? std::atoi(e) : PSP_NETPLAY_DEFAULT_DELAY, PSP_NETPLAY_MIN_DELAY, PSP_NETPLAY_MAX_DELAY);
        g_delay.store(d, std::memory_order_relaxed);
    }
    return d;
}
void psp_netplay_set_input_delay(int frames) { g_delay.store(std::clamp(frames, PSP_NETPLAY_MIN_DELAY, PSP_NETPLAY_MAX_DELAY), std::memory_order_relaxed); }

bool psp_netplay_sync_check() {
    int c = g_check.load(std::memory_order_relaxed);
    if (c < 0) {
        const char* e = std::getenv("PSPRECOMP_NET_SYNCCHECK");
        c = (e && e[0] == '0') ? 0 : 1;
        g_check.store(c, std::memory_order_relaxed);
    }
    return c != 0;
}
void psp_netplay_set_sync_check(bool on) { g_check.store(on ? 1 : 0, std::memory_order_relaxed); }

void psp_netplay_set_fields(const char* const* names, int count) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_names = names;
    g_name_count = count;
}

void psp_netplay_submit_state(uint32_t frame, const uint32_t* values, int count) {
    if (!psp_netplay_sync_check() || count <= 0) return;
    count = std::min<int>(count, (int)MAX_VALUES);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_have_local && frame < g_last_local) {   // a new battle
            summary("battle over");
            g_local.clear();
            for (auto& [mac, m] : g_remote)
                for (auto it = m.begin(); it != m.end();) it = it->first >= frame + KEEP_FRAMES ? m.erase(it) : std::next(it);
            g_compared = g_mismatched = 0;
            g_reports = 0;
            g_first_bad.store(0xFFFFFFFFu, std::memory_order_relaxed);
        }
        g_have_local = true;
        g_last_local = frame;
        Values v(values, values + count);
        bool matched = false;
        for (auto& [mac, m] : g_remote) {
            auto it = m.find(frame);
            if (it == m.end()) continue;
            compare(mac, frame, v, it->second);
            m.erase(it);
            matched = true;
        }
        if (!matched) g_local[frame] = std::move(v);
        prune(frame);
        const int64_t t = now_ms();
        if (t >= g_next_summary) {
            if (g_next_summary) summary("so far");
            g_next_summary = t + 10000;
        }
    }
    std::vector<uint8_t> msg(MSG_HDR + 4 * (size_t)count);
    std::memcpy(msg.data(), "NSYN", 4);
    msg[4] = MSG_VERSION;
    msg[5] = (uint8_t)count;
    std::memcpy(&msg[8], &frame, 4);
    std::memcpy(&msg[MSG_HDR], values, 4 * (size_t)count);
    psp_adhoc_send_aux(msg.data(), msg.size());
}

void psp_netplay_on_message(const uint8_t src_mac[6], const uint8_t* data, size_t n) {
    if (n < MSG_HDR || std::memcmp(data, "NSYN", 4) != 0 || data[4] != MSG_VERSION) return;
    const size_t count = data[5];
    if (n < MSG_HDR + 4 * count || !psp_netplay_sync_check()) return;
    uint32_t frame;
    std::memcpy(&frame, data + 8, 4);
    Values v(count);
    std::memcpy(v.data(), data + MSG_HDR, 4 * count);
    Mac mac;
    std::memcpy(mac.data(), src_mac, 6);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_local.find(frame);
    if (it != g_local.end()) {
        compare(mac, frame, it->second, v);
        g_local.erase(it);   // (with more than one other player the first answer is the one compared)
        return;
    }
    if (g_have_local && (frame + KEEP_FRAMES < g_last_local)) return;   // too old: this side has moved on
    g_remote[mac][frame] = std::move(v);
}

bool psp_netplay_trace_on() { return g_trace; }
void psp_netplay_trace_ptp(const char* what, int sock, const uint8_t* data, uint32_t n) {
    if (!g_trace) return;
    char hex[3 * 96 + 1] = {0};
    for (uint32_t i = 0; i < n && i < 96; i++) std::snprintf(hex + i * 3, 4, "%02x ", data[i]);
    std::fprintf(stderr, "[NETTRACE] f=%llu b=%d %s sock=%d n=%u %s\n", (unsigned long long)psp_netplay_frame(), g_battle.load() ? 1 : 0, what, sock, n, hex);
}
