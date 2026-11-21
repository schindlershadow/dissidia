// games/dissidia/runtime/netplay.cpp - Dissidia's own lock-step battle protocol, seen from the runtime (issue #66).
//
// What the game exchanges in a Communications (Group Battle) fight (found with PSPRECOMP_NET_TRACE=1, see
// docs/multiplayer.md "Battle protocol"): no state, only pad input. Every frame the battle's net object (EBOOT
// FUN_0890D664, a0 = the object) reads the pad itself (sceCtrlReadBufferPositive), packs it into a 16-bit input word
// and sends one 36-byte PDP datagram (port 0x67) to every peer with the newest 8 input words (redundancy against loss),
// then waits until every player's input for the frame it is about to run has arrived, and copies the inputs into
// the pad manager's per-player slots. It returns 0 when it delivered a frame (the battle then runs one update) and
// 1 when an input is missing (the battle waits). Every 10 frames it also sends a checksum of a few fighter fields.
//
// (This file used to hook that function for an input delay; main's comm_battle.cpp now owns it. What stays here is the
// desync check + state dumps that the tag team's lock-step (tagteam_net.cpp) uses.)
// Net object (heap): +0 PDP socket id, +16 flags (bit 0 active), +20 checksum callback (ovl FUN_08B136D4), +28 four
// (frame, checksum) pairs, +68 local player index, +72 the frame being delivered next, +76 peer records (84 bytes:
// MAC at +0, +8 the received input window, +80 the last frame delivered with its input), +852 peer count, +860 the
// local record (+8 its input window, +524 the newest local input frame = the send sequence). Pad slots: [0x089D6008] + 96 x player, words +64..+79 (SceCtrlData).
#include "psp_game_module.h"
#include "psp_netplay.h"
#include "hle/psp_hle.h"
#include "recomp.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t BATTLE_PTR = 0x089D6A10u;          // battle object: +56 + 4 x side = fighter, +96 the battle clock (float)

uint8_t* g_ram = nullptr;
uint32_t g_perturb_frame = 0xFFFFFFFFu;   // PSPRECOMP_NET_PERTURB=<frame>: nudge fighter 0 on this side only (desync-check test)
int g_shown_delay = -1;

bool ok(uint32_t a) { return a >= 0x08800000u && a < 0x0A000000u; }
uint32_t r32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
float rdf(uint32_t a) { float f; std::memcpy(&f, g_ram + (a & 0x07FFFFFFu), 4); return f; }
void wrf(uint32_t a, float f) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &f, 4); }

// ---- the state hashed for the desync check ----
// Per fighter: the fields the game's own checksum uses (origin position) and the ones a desync shows in first (body
// position, current script, motion time, the HP / brave block, the assist gauge), then FNV-1a hashes of the fighter
// struct in 256-byte pieces, so a report says roughly where the difference is. Plus the battle clock.
constexpr uint32_t BLOCK_START = 0x000u, BLOCK_END = 0x1100u, BLOCK = 0x100u;
constexpr int BLOCKS = (BLOCK_END - BLOCK_START) / BLOCK;
struct Field { const char* name; uint32_t off; };
constexpr Field k_fighter_fields[] = {
    {"pos.x", 0x80}, {"pos.y", 0x84}, {"pos.z", 0x88}, {"body.x", 0x110}, {"body.y", 0x114}, {"body.z", 0x118},
    {"script", 0x540}, {"motion_time", 0x1A8}, {"stat10A8", 0x10A8}, {"stat10AC", 0x10AC}, {"hp", 0x10B0}, {"stat10B4", 0x10B4}, {"stat10B8", 0x10B8},
};
constexpr int k_nf = sizeof k_fighter_fields / sizeof k_fighter_fields[0];
constexpr int PER_FIGHTER = k_nf + 2 + BLOCKS;   // + assist gauge (2) + blocks
constexpr int NVALUES = 1 + 2 * PER_FIGHTER;
const char* g_names[NVALUES];

void build_names() {
    static std::string store[NVALUES];
    int k = 0;
    store[k++] = "battle_clock";
    for (int f = 0; f < 2; f++) {
        const std::string p = "fighter" + std::to_string(f) + ".";
        for (const Field& fl : k_fighter_fields) store[k++] = p + fl.name;
        store[k++] = p + "assist_gauge";
        store[k++] = p + "assist_bar";
        for (int b = 0; b < BLOCKS; b++) {
            char t[32];
            std::snprintf(t, sizeof t, "block%03X", BLOCK_START + b * BLOCK);
            store[k++] = p + t;
        }
    }
    for (int i = 0; i < NVALUES; i++) g_names[i] = store[i].c_str();
}

// Fighter bits that differ between the two players by design (who is the local player), left out of the hashes. Found by
// diffing both fighters on the two sides of normal matches (PSPRECOMP_NETSYNC_DUMP); in those matches every gameplay field
// (positions, scripts, HP, gauges) stayed equal, so these are viewer-side only:
//   +0x2F8 a pointer (the two fighters' values swapped), +0x380 bit 21 and +0x490 (0.9 instead of 1.0) on the local
//   fighter only, +0x388 bit 29, +0x4A4 (garbage in the first frame), +0x510 / +0x514 (1.0 on the local fighter for a
//   while), +0x5B0 bit 2, +0x700..+0x767 data only the local fighter fills (camera / lock-on), +0xCF4..+0xCFF a
//   (pointer, id, pointer) slot of a view-side object, +0xD08 (local fighter), +0x1078 byte 1 (pad number).
// +0xD00: a float frame counter (+2 per frame) that the local player's lock-on resets on its target; with the tag team's lock
// cycle (L onto a fighter other than the slot opponent) it reset on one machine only (21 Nov, FFA online, play2: 45 frames of
// fighter1 +0xD00 only, every other word of the block and every hit / KO the same on both copies over 12726 frames).
// Words holding a guest pointer (0x08800000..0x09FFFFFF) are left out as well: effect and HUD objects are allocated for the
// local view, so the heap addresses (and whether a slot holds one at all) differ between the players (+0xCF4..+0xCFC).
uint32_t viewer_mask(uint32_t off) {   // bits of the word at `off` that are hashed
    switch (off) {
    case 0x2F8: case 0x490: case 0x4A4: case 0x510: case 0x514: case 0x5B0: case 0xD00: case 0xD08: case 0x1078: return 0;
    case 0x380: return ~0x00200000u;
    case 0x388: return ~0x20000000u;
    default: return ((off >= 0x700 && off < 0x768) || (off >= 0xCF4 && off < 0xD00)) ? 0 : ~0u;
    }
}
uint32_t hashed_word(uint32_t a, uint32_t off) {
    const uint32_t w = r32(a + off);
    return ok(w) ? 0 : w & viewer_mask(off);
}
uint32_t fnv(uint32_t a, uint32_t off, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i += 4) {
        const uint32_t w = hashed_word(a, off + i);
        for (int k = 0; k < 4; k++) { h ^= (w >> (8 * k)) & 0xFFu; h *= 16777619u; }
    }
    return h;
}

int collect(uint32_t* v) {
    const uint32_t battle = r32(BATTLE_PTR);
    int k = 0;
    v[k++] = ok(battle) ? r32(battle + 96) : 0;
    for (int f = 0; f < 2; f++) {
        const uint32_t a = ok(battle) ? r32(battle + 56 + 4 * f) : 0;
        if (!ok(a)) { for (int i = 0; i < PER_FIGHTER; i++) v[k++] = 0; continue; }
        for (const Field& fl : k_fighter_fields) v[k++] = r32(a + fl.off);
        const uint32_t g = r32(a + 0x52C);
        v[k++] = ok(g) ? r32(g + 0x18) : 0;
        v[k++] = ok(g) ? r32(g + 0x1C) : 0;
        for (int b = 0; b < BLOCKS; b++) v[k++] = fnv(a, BLOCK_START + b * BLOCK, BLOCK);
    }
    return k;
}

void perturb() {
    const uint32_t battle = r32(BATTLE_PTR);
    const uint32_t a = ok(battle) ? r32(battle + 56) : 0;
    if (!ok(a)) return;
    wrf(a + 0x80, rdf(a + 0x80) + 0.25f);
    wrf(a + 0x110, rdf(a + 0x110) + 0.25f);
    std::fprintf(stderr, "[NETPLAY] PSPRECOMP_NET_PERTURB: moved fighter 0 by 0.25 on x after frame %u (this side only)\n", g_perturb_frame);
}

// PSPRECOMP_NETSYNC_DUMP=300,500 (fighter block offsets, hex): log those blocks of both fighters, word by word, for every
// frame ([NETDUMP] lines), to find the field behind a block mismatch by diffing the two players' logs.
std::vector<uint32_t> g_dump_list;
bool g_dump_blocks = false;
void dump_blocks(uint32_t frame) {
    const uint32_t battle = r32(BATTLE_PTR);
    for (int f = 0; f < 2; f++) {
        const uint32_t a = ok(battle) ? r32(battle + 56 + 4 * f) : 0;
        if (!ok(a)) continue;
        for (uint32_t off : g_dump_list) {
            std::string line;
            char w[12];
            for (uint32_t i = 0; i < BLOCK; i += 4) { std::snprintf(w, sizeof w, " %08X", hashed_word(a, off + i)); line += w; }
            std::fprintf(stderr, "[NETDUMP] frame=%u fighter=%d block=%03X%s\n", frame, f, off, line.c_str());
        }
    }
}

}  // namespace

// tagteam_net.cpp (the tag battle's own lock-step, which replaces FUN_0890D664 there): a frame was delivered (state = the start of it).
void dissidia_netplay_frame_delivered(uint32_t frame) {
    if (!g_ram) return;
    if (psp_netplay_sync_check()) { uint32_t vals[NVALUES]; const int n = collect(vals); psp_netplay_submit_state(frame, vals, n); }
    if (g_dump_blocks) dump_blocks(frame);
    if (frame == g_perturb_frame) perturb();
}
void dissidia_netplay_note_delay(int delay) {
    if (delay == g_shown_delay) return;
    g_shown_delay = delay;
    std::fprintf(stderr, "[NETPLAY] tag lock-step battle: network delay %d frame(s)\n", delay);
}

void dissidia_netplay_install(uint8_t* rdram) {
    g_ram = rdram;
    if (const char* e = std::getenv("PSPRECOMP_NETSYNC_DUMP"); e && *e) {
        for (const char* p = e; *p;) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 16);
            if (end == p) break;
            if (v < BLOCK_END) g_dump_list.push_back(static_cast<uint32_t>(v) & ~(BLOCK - 1));
            p = *end ? end + 1 : end;
        }
        g_dump_blocks = !g_dump_list.empty();
    }
    if (const char* e = std::getenv("PSPRECOMP_NET_PERTURB"); e && *e) g_perturb_frame = static_cast<uint32_t>(std::strtoul(e, nullptr, 0));
    // No hook on the game's lock-step (FUN_0890D664) any more: plain online battles run on comm_battle.cpp's pad link; this module
    // only keeps the desync check and the state dumps for the tag team's own lock-step (tagteam_net.cpp).
    build_names();
    psp_netplay_set_fields(g_names, NVALUES);
}
