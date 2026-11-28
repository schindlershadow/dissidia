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
#include <atomic>
#include <chrono>
#include <thread>
#include "psp_memory.h"
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

uint32_t dissidia_tagteam_fighter_sig();   // tagteam.cpp
int dissidia_tagteam_extra_actors(uint32_t out[4], uint32_t flags[4]);   // tagteam.cpp
uint32_t dissidia_camera_manager();   // camera.cpp
void psp_mem_watch_set(uint32_t addr);   // runtime psp_dispatch.cpp (diagnostic watch)
extern uint32_t g_mem_watch_tag;
uint32_t dissidia_libm_fingerprint();
uint32_t dissidia_simmath_fingerprint();   // net_packs.cpp
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
// PSPRECOMP_NETSYNC_WIDE=1 (diagnostic, both players): also the game's two random sequences and the tag team's extras (up to 4: flags, a few
// fields, the struct hashed in four pieces), which the slot-fighter fields only show once an extra has touched a slot fighter. Off = zeros.
constexpr Field k_extra_fields[] = {{"pos.x", 0x80}, {"pos.y", 0x84}, {"pos.z", 0x88}, {"script", 0x540}, {"motion_time", 0x1A8}, {"hp", 0x10B0}};
constexpr int k_nxf = sizeof k_extra_fields / sizeof k_extra_fields[0];
constexpr int XPARTS = 4, PER_EXTRA = 1 + k_nxf + XPARTS + 2, NEXTRA = 4;   // (+ the extra's CPU brain and its lock block, hashed)
constexpr uint32_t ACT_BRAIN = 0x5C4u, BRAIN_LEN = 0x800u, ACT_LOCKBLK = 0x5ACu, LB_LEN = 0x600u;
constexpr uint32_t RNG_WORDS[2] = {0x09AF84D0u, 0x09AFB89Cu};   // rand() LCG, effect LCG (framerate.cpp)
constexpr int NVALUES = 1 + 2 * PER_FIGHTER + 1 + 2 + 4 + NEXTRA * PER_EXTRA;   // + the tag team's fighters on the stage (count and characters) + the wide fields
bool wide_on() { static const bool on = [] { const char* e = std::getenv("PSPRECOMP_NETSYNC_WIDE"); return e && e[0] == '1'; }(); return on; }
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
    store[k++] = "tag_fighters";
    store[k++] = "rng"; store[k++] = "fighter0.brain"; store[k++] = "fighter1.brain"; store[k++] = "fighter0.lockblk"; store[k++] = "fighter1.lockblk"; store[k++] = "rng_fx";
    for (int x = 0; x < NEXTRA; x++) {
        const std::string p = "extra" + std::to_string(x) + ".";
        store[k++] = p + "flags";
        for (const Field& fl : k_extra_fields) store[k++] = p + fl.name;
        for (int b = 0; b < XPARTS; b++) store[k++] = p + "part" + std::to_string(b);
        store[k++] = p + "brain"; store[k++] = p + "lockblk";
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
    default: {
        // +0xC40..+0xCF3 (final10: 5-device 5v5, "fighter1.blockC00" differing for 1 to 300 frames at a time between the team-1 humans (B, D) and the team-0 machines, nothing else of the 124 fields
        // differing: two arrays of 5 floats at +0xC40 and +0xC68, the same values on both sides but shifted by two entries, i.e. a history the update pushes one sample per call into, here two calls
        // more on one side): writer / readers NOT yet found, so NOT proven view-side: only with PSPRECOMP_NETSYNC_MASK_C40=1 (diagnostic), default compared.
        static const bool c40_mask = std::getenv("PSPRECOMP_NETSYNC_MASK_C40") != nullptr;
        if (c40_mask && off >= 0xC40 && off < 0xCF4) return 0;
        return ((off >= 0x700 && off < 0x768) || (off >= 0xCF4 && off < 0xD00)) ? 0 : ~0u;
    }
    }
}
// The first frames after a slot fighter was rebuilt (the mate's call-in): word +0x3C4 holds, for one frame, a float a vector routine of the first update writes from FPU registers /
// stack the new fighter's constructor has not initialised (found 26 Nov, docs "Batch eighty-two": 0x781200F0 vs 0xF81200F0, only bit 31 differs, the same guest code path on both
// machines, back to the real value the next frame, about 1 run in 10). Not simulation state: left out of the check for the rebuild frame and the next two.
// PSPRECOMP_NETSYNC_REBUILD_OLD=1: compared.
uint32_t g_rb_addr[2] = {0, 0}, g_rb_until[2] = {0, 0}; uint32_t g_hw_frame = 0;
uint32_t hashed_word(uint32_t a, uint32_t off) {
    static const bool rb_old = std::getenv("PSPRECOMP_NETSYNC_REBUILD_OLD") != nullptr;
    if (off == 0x3C4u && !rb_old) for (int i = 0; i < 2; i++) if (a && a == g_rb_addr[i] && g_hw_frame <= g_rb_until[i]) return 0;
    const uint32_t w = r32(a + off);
    // +0x3C4 of EVERY fighter holds a float whose upper 16 bits are leftovers of FPU registers / stack the vector routine of the update does not initialise (final10: 5-device 5v5, extra2
    // D5C39301 vs CC8AF333 for two frames, back in line the next: the only part0 word not tied to the motion counters; the same word as the rebuild case above, here on a parked extra).
    // NOT proven non-simulation (writer / readers not found): off by default, PSPRECOMP_NETSYNC_MASK_3C4=1 compares only the low 16 bits (diagnostic).
    static const bool w3c4_mask = std::getenv("PSPRECOMP_NETSYNC_MASK_3C4") != nullptr;
    if (off == 0x3C4u && w3c4_mask) return (ok(w) || psp_guest_user_ok(w, 4)) ? 0 : (w & 0xFFFFu);
    return (ok(w) || psp_guest_user_ok(w, 4)) ? 0 : w & viewer_mask(off);   // a pointer (also into the high RAM, where a rebuilt fighter's files live) is not compared
}
// The lock block's two cursor fade values (+0x444 and +0x584, 0..0xFF, FF = fully shown; the lock-on cursors' alpha) are drawing state: the UI update steps them on
// every game-loop pass, also on the passes where the lock-step waits for a late packet, so a machine that stalled shows one more fade step for a frame or two than the
// other (30 Nov: mp_div_team3_lan desktop runs, "fighter1.lockblk" E5 vs FF at the frame the lock moved, back in line two frames later). PSPRECOMP_NETSYNC_LBFADE_OLD=1: compared.
bool lb_fade_word(uint32_t i) {
    static const bool old = std::getenv("PSPRECOMP_NETSYNC_LBFADE_OLD") != nullptr;
    return !old && (i == 0x444u || i == 0x584u);
}
uint32_t fnv_plain(uint32_t a, uint32_t n, bool lockblk = false) {   // a block outside the fighter struct (the CPU brain, the lock block): guest pointers left out only
    if (!ok(a) || !ok(a + n)) return 0;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i += 4) { uint32_t w = r32(a + i); if (ok(w) || psp_guest_user_ok(w, 4) || (lockblk && (i == 0x124u || lb_fade_word(i)))) w = 0; for (int k = 0; k < 4; k++) { h ^= (w >> (8 * k)) & 0xFFu; h *= 16777619u; } }   // (lock block +0x125: the local player's own block)
    return h;
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
    v[k++] = dissidia_tagteam_fighter_sig();   // a different number of fighters (or characters) is a desync too
    const bool w = wide_on();
    if (!w) return k;   // (the wide fields are sent only when asked for: both players set PSPRECOMP_NETSYNC_WIDE=1)
    v[k++] = r32(RNG_WORDS[0]);
    for (int f = 0; f < 2; f++) { const uint32_t a = (w && ok(battle)) ? r32(battle + 56 + 4 * f) : 0; v[k++] = ok(a) ? fnv_plain(r32(a + ACT_BRAIN), BRAIN_LEN) : 0; }
    for (int f = 0; f < 2; f++) { const uint32_t a = (w && ok(battle)) ? r32(battle + 56 + 4 * f) : 0; v[k++] = ok(a) ? fnv_plain(r32(a + ACT_LOCKBLK), LB_LEN, true) : 0; }
    v[k++] = 0;   // (the effect LCG is per machine until tagteam_net.cpp reseeds it right after this: not compared)
    uint32_t xa[4] = {}, xf[4] = {};
    const int nx = w ? dissidia_tagteam_extra_actors(xa, xf) : 0;
    for (int x = 0; x < NEXTRA; x++) {
        const uint32_t a = x < nx ? xa[x] : 0;
        if (!ok(a)) { for (int i = 0; i < PER_EXTRA; i++) v[k++] = 0; continue; }
        v[k++] = xf[x] | 0x100u;
        for (const Field& fl : k_extra_fields) v[k++] = r32(a + fl.off);
        for (int b = 0; b < XPARTS; b++) v[k++] = fnv(a, b * (BLOCK_END / XPARTS), BLOCK_END / XPARTS);
        v[k++] = fnv_plain(r32(a + ACT_BRAIN), BRAIN_LEN);
        v[k++] = fnv_plain(r32(a + ACT_LOCKBLK), LB_LEN, true);
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
    uint32_t xa[4] = {}, xf[4] = {};
    const int nx = wide_on() ? dissidia_tagteam_extra_actors(xa, xf) : 0;   // wide: the extras too (fighter=2.. in spawn order)
    if (wide_on()) std::fprintf(stderr, "[NETDUMP] frame=%u rng %08X %08X\n", frame, r32(RNG_WORDS[0]), r32(RNG_WORDS[1]));
    for (int f = 0; f < 2 + nx; f++) {
        const uint32_t a = f < 2 ? (ok(battle) ? r32(battle + 56 + 4 * f) : 0) : xa[f - 2];
        if (!ok(a)) continue;
        for (uint32_t off : g_dump_list) {
            std::string line;
            char w[12];
            for (uint32_t i = 0; i < BLOCK; i += 4) { std::snprintf(w, sizeof w, " %08X", hashed_word(a, off + i)); line += w; }
            std::fprintf(stderr, "[NETDUMP] frame=%u fighter=%d block=%03X%s\n", frame, f, off, line.c_str());
        }
    }
}

// The first two delivered frames of every battle: this machine's build (does the compiler fuse a * b + c?) and every word of both
// fighters (pointers and viewer-only bits left out, as in the hashes), [NETDUMP0] lines. A DESYNC at frame 0 / 1 can then be pinned
// to the words by diffing the two players' logs (grep NETDUMP0), also when only one of the two logs is at hand later.
// PSPRECOMP_NETSYNC_F0DUMP=0 turns it off.
bool fma_contracted() {   // true when this build fuses a * b - p into one FMA (the exact low part of a * b is left over)
    volatile float xv = 1.0f + 1.0f / 4096.0f;
    volatile float pv = xv * xv;   // rounded product
    const float a = xv, b = xv;
    const float r = a * b - pv;
    return r != 0.0f;
}
void dump_start(uint32_t frame) {
    static const bool off = [] { const char* e = std::getenv("PSPRECOMP_NETSYNC_F0DUMP"); return e && e[0] == '0'; }();
    if (off || frame > 1) return;
    if (frame == 0) {
#if defined(__aarch64__)
        const char* arch = "arm64";
#elif defined(__x86_64__)
        const char* arch = "x86_64";
#else
        const char* arch = "other";
#endif
        std::fprintf(stderr, "[NETSYNC] this build: %s, fused multiply-add %s, libm (log only) %08X, sim maths %08X, %s\n", arch, fma_contracted() ? "ON (differs from a PC build)" : "off", dissidia_libm_fingerprint(), dissidia_simmath_fingerprint(),
#ifdef __clang__
                     "clang " __clang_version__
#else
                     "gcc/other"
#endif
        );
    }
    const uint32_t battle = r32(BATTLE_PTR);
    for (int f = 0; f < 2; f++) {
        const uint32_t a = ok(battle) ? r32(battle + 56 + 4 * f) : 0;
        if (!ok(a)) { std::fprintf(stderr, "[NETDUMP0] frame=%u fighter=%d none (battle %08X)\n", frame, f, battle); continue; }
        for (uint32_t off2 = BLOCK_START; off2 < BLOCK_END; off2 += BLOCK) {
            std::string line; char w[12];
            for (uint32_t i = 0; i < BLOCK; i += 4) { std::snprintf(w, sizeof w, " %08X", hashed_word(a, off2 + i)); line += w; }
            std::fprintf(stderr, "[NETDUMP0] frame=%u fighter=%d block=%03X%s\n", frame, f, off2, line.c_str());
        }
    }
}

// PSPRECOMP_NETSYNC_WIDE=1: the last 64 frames' words of every fighter (slot fighters, then the extras) are kept; once the desync check
// names the first mismatching frame, that frame's words are logged ([NETDUMPX] lines), so diffing the two players' logs names the words.
constexpr uint32_t CAM_LEN = 0x1000u;
struct Snap { uint32_t frame = 0xFFFFFFFFu; uint32_t rng[2] = {}; int n = 0; uint32_t addr[6] = {}, baddr[6] = {}; uint32_t w[6][BLOCK_END / 4]; uint32_t b[6][BRAIN_LEN / 4]; uint32_t laddr[6] = {}; uint32_t l[6][LB_LEN / 4]; uint32_t cam_at = 0; uint32_t cam[CAM_LEN / 4]; };
Snap g_ring[64];
void dump_snap(uint32_t bad, uint32_t frame);
bool g_ring_dumped = false;
void ring_frame(uint32_t frame) {
    if (!wide_on()) return;
    ::g_mem_watch_tag = frame;
    if (const char* lw = std::getenv("PSPRECOMP_NETSYNC_LBWATCH")) {   // "<slot>:<hex offset>": watch that word of the slot fighter's lock block (re-armed when the block changes)
        static uint32_t armed = 0; const int sl = std::atoi(lw) & 1; const char* c = std::strchr(lw, ':'); const uint32_t off = c ? static_cast<uint32_t>(std::strtoul(c + 1, nullptr, 16)) : 0x280u;
        const uint32_t bt = r32(BATTLE_PTR), a = ok(bt) ? r32(bt + 56 + 4 * sl) : 0, lb = ok(a) ? r32(a + ACT_LOCKBLK) : 0;
        if (ok(lb) && lb + off != armed) { armed = lb + off; ::psp_mem_watch_set(armed); std::fprintf(stderr, "[LBWATCH] frame %u: watching %08X (slot %d lock block %08X +%X)\n", frame, armed, sl, lb, off); }
    }
    if (const char* aw = std::getenv("PSPRECOMP_NETSYNC_ACTWATCH")) {   // "<slot>:<hex offset>:<from frame>": watch that word of the slot fighter from that lock-step frame on (re-armed when its address changes)
        static uint32_t armed = 0; const int sl = std::atoi(aw) & 1; const char* c = std::strchr(aw, ':'); const uint32_t off = c ? static_cast<uint32_t>(std::strtoul(c + 1, nullptr, 16)) : 0x3C4u;
        const char* c2 = c ? std::strchr(c + 1, ':') : nullptr; const uint32_t from = c2 ? static_cast<uint32_t>(std::strtoul(c2 + 1, nullptr, 0)) : 0u;
        const uint32_t bt = r32(BATTLE_PTR), a = ok(bt) ? r32(bt + 56 + 4 * sl) : 0;
        if (frame >= from && ok(a) && a + off != armed) { armed = a + off; ::psp_mem_watch_set(armed); std::fprintf(stderr, "[ACTWATCH] frame %u: watching %08X (slot %d fighter %08X +%X)\n", frame, armed, sl, a, off); }
    }
    if (frame == 0) g_ring_dumped = false;
    Snap& r = g_ring[frame % 64];
    r.frame = frame; r.n = 0;
    for (int i = 0; i < 2; i++) r.rng[i] = r32(RNG_WORDS[i]);
    const uint32_t battle = r32(BATTLE_PTR);
    uint32_t xa[4] = {}, xf[4] = {};
    const int nx = dissidia_tagteam_extra_actors(xa, xf);
    for (int f = 0; f < 2 + nx && f < 6; f++) {
        const uint32_t a = f < 2 ? (ok(battle) ? r32(battle + 56 + 4 * f) : 0) : xa[f - 2];
        r.addr[r.n] = a;
        for (uint32_t o = 0; o < BLOCK_END; o += 4) r.w[r.n][o / 4] = ok(a) ? r32(a + o) : 0;   // (raw: pointers and viewer-only words included, the diff tells them apart)
        const uint32_t br = ok(a) ? r32(a + ACT_BRAIN) : 0; r.baddr[r.n] = br;
        for (uint32_t o = 0; o < BRAIN_LEN; o += 4) r.b[r.n][o / 4] = (ok(br) && ok(br + BRAIN_LEN)) ? r32(br + o) : 0;
        const uint32_t lb = ok(a) ? r32(a + ACT_LOCKBLK) : 0; r.laddr[r.n] = lb;
        for (uint32_t o = 0; o < LB_LEN; o += 4) r.l[r.n][o / 4] = (ok(lb) && ok(lb + LB_LEN)) ? r32(lb + o) : 0;
        r.n++;
    }
    r.cam_at = dissidia_camera_manager();
    for (uint32_t o = 0; o < CAM_LEN; o += 4) r.cam[o / 4] = (ok(r.cam_at) && ok(r.cam_at + CAM_LEN)) ? r32(r.cam_at + o) : 0;
    const uint32_t bad = psp_netplay_first_mismatch();
    if (g_ring_dumped || bad == 0xFFFFFFFFu) return;
    g_ring_dumped = true;
    for (uint32_t back = 3; back-- > 0;) {   // the first mismatching frame and the two before it
    const uint32_t fr = bad >= back ? bad - back : bad;
    if (back && fr == bad) continue;
    dump_snap(fr, frame);
    }
}
void dump_snap(uint32_t bad, uint32_t frame) {
    const Snap& b = g_ring[bad % 64];
    if (b.frame != bad) { std::fprintf(stderr, "[NETDUMPX] frame=%u not in the ring any more (now %u)\n", bad, frame); return; }
    std::fprintf(stderr, "[NETDUMPX] frame=%u rng %08X %08X fighters %d\n", bad, b.rng[0], b.rng[1], b.n);
    for (int f = 0; f < b.n; f++)
        for (uint32_t o = 0; o < BLOCK_END; o += BLOCK) {
            std::string line; char t[12];
            for (uint32_t i = 0; i < BLOCK; i += 4) { std::snprintf(t, sizeof t, " %08X", b.w[f][(o + i) / 4]); line += t; }
            std::fprintf(stderr, "[NETDUMPX] frame=%u fighter=%d addr=%08X block=%03X%s\n", bad, f, b.addr[f], o, line.c_str());
        }
    for (int f = 0; f < b.n; f++)
        for (uint32_t o = 0; o < BRAIN_LEN; o += BLOCK) {
            std::string line; char t[12];
            for (uint32_t i = 0; i < BLOCK; i += 4) { std::snprintf(t, sizeof t, " %08X", b.b[f][(o + i) / 4]); line += t; }
            std::fprintf(stderr, "[NETDUMPX] frame=%u fighter=%d addr=%08X block=B%03X%s\n", bad, f, b.baddr[f], o, line.c_str());
        }
    for (int f = 0; f < b.n; f++)
        for (uint32_t o = 0; o < LB_LEN; o += BLOCK) {
            std::string line; char t[12];
            for (uint32_t i = 0; i < BLOCK; i += 4) { std::snprintf(t, sizeof t, " %08X", b.l[f][(o + i) / 4]); line += t; }
            std::fprintf(stderr, "[NETDUMPX] frame=%u fighter=%d addr=%08X block=A%03X%s\n", bad, f, b.laddr[f], o, line.c_str());
        }
    for (uint32_t o = 0; o < CAM_LEN; o += BLOCK) {   // the camera manager (fighter=9)
        std::string line; char t[12];
        for (uint32_t i = 0; i < BLOCK; i += 4) { std::snprintf(t, sizeof t, " %08X", b.cam[(o + i) / 4]); line += t; }
        std::fprintf(stderr, "[NETDUMPX] frame=%u fighter=9 addr=%08X block=C%03X%s\n", bad, b.cam_at, o, line.c_str());
    }
}

}  // namespace

// tagteam_net.cpp (the tag battle's own lock-step, which replaces FUN_0890D664 there): a frame was delivered (state = the start of it).
static uint32_t g_ls_frame = 0;
uint32_t dissidia_netplay_ls_frame() { return g_ls_frame; }   // the last delivered lock-step frame (0 = none yet); tagteam.cpp's KOLIST test aid keys on it with PSPRECOMP_TAGTEAM_KOLIST_LSFRAME=1
void dissidia_netplay_note_rebuild(int slot, uint32_t frame) { const uint32_t bt = r32(BATTLE_PTR), a = ok(bt) ? r32(bt + 56 + 4 * (slot & 1)) : 0; g_rb_addr[slot & 1] = a; g_rb_until[slot & 1] = frame + 2; }
void dissidia_netplay_ls_reset() { g_ls_frame = 0; g_rb_addr[0] = g_rb_addr[1] = 0; }   // a new lock-step battle starts (tagteam_net.cpp): no delivered frame yet
int psp_dispatch_get_recent_funcs(uint32_t* out, int max);   // runtime psp_dispatch.cpp (needs PSPRECOMP_FUNC_TRACE=1)
// PSPRECOMP_STALL_DUMP=1 (diagnostic, final10b: all five copies stopped delivering frames at 9811 for 35-100 s): a thread logs, once a second while no lock-step frame has been
// delivered for 3 s, the game threads' last function entries (with PSPRECOMP_FUNC_TRACE=1), so the log says where the main thread sits.
static std::atomic<int64_t> g_wd_ms{0}; static std::atomic<uint32_t> g_wd_frame{0};
static int64_t wd_now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void stall_watch_start() {
    static bool started = false; if (started || !std::getenv("PSPRECOMP_STALL_DUMP")) return; started = true;
    std::thread([] {
        int64_t last_dump = 0; int dumps = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const int64_t t = g_wd_ms.load(); if (!t) continue;
            const int64_t el = wd_now() - t;
            if (el < 3000) { last_dump = 0; dumps = 0; continue; }
            if (wd_now() - last_dump < 2000 || dumps >= 12) continue;
            last_dump = wd_now(); dumps++;
            uint32_t f[64]; const int n = psp_dispatch_get_recent_funcs(f, 64);
            std::string l; char b[16]; for (int i = std::max(0, n - 40); i < n; i++) { std::snprintf(b, sizeof b, " %08X", f[i]); l += b; }
            std::fprintf(stderr, "[STALL] no lock-step frame for %.1f s after frame %u; recent functions (oldest first):%s\n", el / 1000.0, g_wd_frame.load(), l.c_str());
        }
    }).detach();
}
void dissidia_netplay_frame_delivered(uint32_t frame) {
    g_ls_frame = frame; g_hw_frame = frame;
    stall_watch_start(); g_wd_frame = frame; g_wd_ms = wd_now();
    if (!g_ram) return;
    // The check's hashes (2 fighters x 4352 words, byte-wise FNV, plus a ~640 B packet) are taken every PSPRECOMP_NETSYNC_EVERY-th frame (default 1 = every frame; measured ~0.1% of a frame, so the saving is not worth halving detection; 2+ was for a phone
    // that is the online-only work of every logic frame; a difference is found at most one frame later). Every machine uses the frame number, so the compared
    // frames agree (an older build sends every frame: its extra frames are ignored). PSPRECOMP_NETSYNC_EVERY=1: every frame (the old behaviour).
    static const uint32_t every = [] { const char* e = std::getenv("PSPRECOMP_NETSYNC_EVERY"); const int v = e ? std::atoi(e) : 1; return static_cast<uint32_t>(v < 1 ? 1 : v > 30 ? 30 : v); }();
    static double cost_us = 0; static uint32_t cost_n = 0;
    if (psp_netplay_sync_check() && (frame % every) == 0) {
        const auto t0 = std::chrono::steady_clock::now();
        uint32_t vals[NVALUES]; const int n = collect(vals); psp_netplay_submit_state(frame, vals, n); dump_start(frame);
        cost_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        if (++cost_n >= 300) { std::fprintf(stderr, "[NETSYNC] cost: %.0f us per checked frame (every %u frame(s), %u checked)\n", cost_us / cost_n, every, cost_n); cost_us = 0; cost_n = 0; }
    }
    if (g_dump_blocks) dump_blocks(frame);
    ring_frame(frame);
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
