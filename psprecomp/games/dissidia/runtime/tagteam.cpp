// games/dissidia/runtime/tagteam.cpp - tag team battles (2 v 2). See docs/tag-team.md.
//
// Spike: after the battle setup FUN_0888BA0C has built the two fighters, make more with the same constructor, cloning
// the existing fighters' setup data (so no new assets are needed): a second enemy and a partner for the human.
//   PSPRECOMP_TAGTEAM=1        turn the spike on
//   PSPRECOMP_TAGTEAM_EXTRAS=n 1 = the enemy's second fighter, 2 = the human's partner, 3 = both (default)
//   PSPRECOMP_TAGTEAM_LOG=1    log what the hook does
#include "psp_game_module.h"
#include "psp_runtime.h"
#include "recomp.h"
#include "psp_settings.h"
#include "tagteam_hud.h"
#include "hle/psp_hle.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <map>
#include <vector>

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
float dissidia_battle_age();   // framerate.cpp: ticks (1/60 s) since the battle started running
uint32_t dissidia_camera_manager();   // camera.cpp
void psp_mem_watch_set(uint32_t addr);   // runtime/src/psp_dispatch.cpp

namespace {

constexpr uint32_t BATTLE_SETUP = 0x0888BA0Cu;   // EBOOT: FUN_0888BA0C(bs), builds the fighters
constexpr uint32_t ALLOC = 0x08831320u;          // EBOOT: guest allocator (a0 = size)
constexpr uint32_t SIDE_REC = 0x088729A0u;       // EBOOT: per-side record (a0 = side)
constexpr uint32_t CHAR_INFO = 0x088470B8u;      // EBOOT: character info from a setup record (a0 = bs+0x7D8 / +0x7DC)
constexpr uint32_t FIGHTER_CTOR = 0x08B39E5Cu;   // battle overlay: fighter constructor (mem, siderec, info, side, 0)
constexpr uint32_t SET_FOE = 0x08836DA4u;        // EBOOT: a->+0x300 = b
constexpr uint32_t CPU_MODE = 0x08B2F2E4u;       // battle overlay: FUN_08B2F2E4(actor, 0), the setup's CPU-control call
constexpr uint32_t BRAIN_MODE = 0x08A640CCu;     // battle overlay: brain (actor +0x5C4) parameter 1 (record +3894)
constexpr uint32_t BRAIN_LEVEL = 0x08A64100u;    // battle overlay: brain parameter 2 (record +3896)
constexpr uint32_t ACT_BRAIN = 0x5C4u;
constexpr uint32_t FLOW_STEP = 0x088887A4u;      // EBOOT: battle flow step (a0 = bs): waits for both fighters, starts action 1 on each, bs+72 = 1
constexpr uint32_t FIGHTER_READY = 0x08B2F9E0u;  // battle overlay: bit 0x10 of byte actor+1456 (the fighter is ready)
constexpr uint32_t LOCK_SET = 0x08B3B0B4u;       // battle overlay: lock block: set the target (a0 = block, a1 = actor)
constexpr uint32_t BRAIN_TICK = 0x08A64400u;     // battle overlay: the CPU brain tick (a0 = brain, actor +0x5C4)
constexpr uint32_t TARGET_REFRESH = 0x08B2F17Cu; // battle overlay: copy the lock block's result into actor +0x2FC (a0 = actor)
constexpr uint32_t LOCK_OTHER = 0x08B40994u;     // battle overlay: the lock cycle's 2nd candidate, retail = the foe's assist (a0 = handler, a1 = lock block)
constexpr uint32_t LOCK_GET = 0x08B3B280u;       // battle overlay: the lock block's current target (a0 = block)
constexpr uint32_t LOCK_MODE = 0x08B3B3ECu;      // battle overlay: change the lock block's mode (a0 = block, a1 = mode, a2 = flag): L toggles 1 <-> 0 through it
constexpr uint32_t START_ACTION = 0x0882FA10u;   // EBOOT: start an action (a0 = actor + 304, a1 = id, a2 = 0, f12 = f13 = 0)
constexpr uint32_t REC_INIT = 0x08B01090u;       // battle overlay: fills the 40-byte init block (a0 = buf, a1 = side)
constexpr uint32_t LOCK_INIT = 0x08B3B1B8u;      // battle overlay: lock-on block init (a0 = *(actor + 0x5AC), a1 = 3 words)
constexpr uint32_t PLACE = 0x088359A8u;          // EBOOT: place an actor (a0 = actor, a1 = vec4, a2 = 1)
constexpr uint32_t REC_BASE = 0x09AFB8A0u;       // global per-side records (stride 0x1060), 0x09B00000 - 18272
constexpr uint32_t REG_PAIR0 = 0x08BAE9FCu, REG_PAIR1 = 0x08BAEA08u;   // ctor writes the new fighter here (see spawn_fighter)
constexpr uint32_t LAST_FIGHTER_OBJ = 0x089D4774u;   // EBOOT global the fighter constructor sets to its own controller-side object
constexpr uint32_t ACT_VT = 0x544u, ACT_FLAGS = 0x5B0u, ACT_LOCKBLK = 0x5ACu;

FuncPtr g_orig_setup = nullptr, g_orig_flow = nullptr, g_orig_tick = nullptr, g_orig_lock_other = nullptr, g_orig_refresh = nullptr, g_orig_lock_mode = nullptr, g_orig_rel1 = nullptr, g_orig_rel2 = nullptr;
bool g_in_ctor = false;   // inside the extra's constructor: the resource releases are skipped (see rel_hook)
bool g_enabled = false;   // PSPRECOMP_TAGTEAM=1 or Settings > Debug > Tag Team (2v2): the next battle is a tag battle
bool g_log = false;
uint8_t* g_ram = nullptr;
recomp_context* g_ctx = nullptr;

// The extra fighters of the battle being set up. side 1 = the CPU's team (the "third"), side 0 = the human's (the partner).
struct Extra { uint32_t actor = 0, side = 0, foe = 0, slot_actor = 0; bool started = false; int hp_max = 0; std::vector<std::pair<uint32_t, uint32_t>> undo; };   // undo: (address, value) writes that take back what the constructor registered
Extra g_extra[2];
std::vector<std::pair<uint32_t, uint32_t>> g_last_undo;
int g_nextra = 0;
uint32_t g_bs = 0;
int g_mask = 3;
int g_stop = 99;   // PSPRECOMP_TAGTEAM_STOP=n: debugging, spawn steps up to n only (1 alloc, 2 ctor, 3 foe, 4 init, 5 brain, 6 start/place)
bool g_late_wait = false;   // PSPRECOMP_TAGTEAM_LATE=2: the late partner waits until the intro is over
bool g_late = false;   // PSPRECOMP_TAGTEAM_LATE=1: the partner is made once the battle has started (not during the intro setup)
struct Late { uint32_t side = 0, foe = 0, cpu_side = 0, info_slot = 0, slot_actor = 0; };
std::vector<Late> g_late_q;   // extras still to be made (after the intro)
bool g_late_ready = false;    // the intro is over, the queue may spawn   // PSPRECOMP_TAGTEAM_EXTRAS: 1 = enemy side fighter, 2 = partner on the human's side

uint32_t rd(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
uint32_t rdh(uint32_t a) { return static_cast<uint32_t>(g_ram[a & 0x07FFFFFFu]) | (static_cast<uint32_t>(g_ram[(a + 1) & 0x07FFFFFFu]) << 8); }
float rdf(uint32_t a) { float f; std::memcpy(&f, g_ram + (a & 0x07FFFFFFu), 4); return f; }
void wr(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &v, 4); }
void wrf(uint32_t a, float f) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &f, 4); }
bool ok(uint32_t a, uint32_t n) { return a >= 0x08800000u && a + n <= 0x0C000000u; }

FuncPtr fn(uint32_t addr) { return addr >= 0x089DD4C0u ? ovl_battle_lookup(addr) : psp_dispatch_probe_lookup(addr); }

// Calls a guest function with up to 5 integer arguments and returns v0; the registers are restored afterwards.
uint32_t call(uint32_t addr, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0) {
    FuncPtr f = fn(addr);
    if (!f) { std::fprintf(stderr, "[TAGTEAM] no function at %08X\n", addr); return 0; }
    const recomp_context saved = *g_ctx;
    g_ctx->r[4] = static_cast<int32_t>(a0); g_ctx->r[5] = static_cast<int32_t>(a1); g_ctx->r[6] = static_cast<int32_t>(a2);
    g_ctx->r[7] = static_cast<int32_t>(a3); g_ctx->r[8] = static_cast<int32_t>(a4);
    f(g_ram, g_ctx);
    const uint32_t v = static_cast<uint32_t>(g_ctx->r[2]);
    *g_ctx = saved;
    return v;
}

void tick_hook(uint8_t* rdram, recomp_context* ctx);
void lock_other_hook(uint8_t* rdram, recomp_context* ctx);
void refresh_hook(uint8_t* rdram, recomp_context* ctx);
void lock_mode_hook(uint8_t* rdram, recomp_context* ctx);

// The fighter constructor first releases the resource handles of the side's slot object (FUN_0886CAA0: two handles at +288,
// FUN_0886A140: one at +268), the intro / outro camera data the retail fighter of that side is still using, and loads its own.
// For an extra on a side that already has a fighter that frees the retail fighter's data (its intro camera then reads
// nothing: eye and target 0). While an extra is being constructed the releases are skipped.
void rel1_hook(uint8_t* rdram, recomp_context* ctx) { if (g_in_ctor) { if (g_log) std::fprintf(stderr, "[TAGTEAM] release (two handles) of %08X skipped\n", static_cast<uint32_t>(ctx->r[4])); return; } g_orig_rel1(rdram, ctx); }
void rel2_hook(uint8_t* rdram, recomp_context* ctx) { if (g_in_ctor) { if (g_log) std::fprintf(stderr, "[TAGTEAM] release (one handle) of %08X skipped\n", static_cast<uint32_t>(ctx->r[4])); return; } g_orig_rel2(rdram, ctx); }

// Debug: every guest word outside the actor itself that holds the actor's address (who registered it).
void scan_refs(uint32_t actor, const char* tag) {
    int n = 0;
    for (uint32_t a = 0x08800000u; a < 0x0BF00000u && n < 60; a += 4) {
        if (a >= actor && a < actor + 0x12D0u) continue;
        if (rd(a) == actor) {
            std::fprintf(stderr, "[TAGTEAM] %s: %08X holds the actor\n", tag, a); n++;
            if (a < 0x09000000u) { std::fprintf(stderr, "[TAGTEAM]   around:"); for (int k = -6; k <= 6; k++) std::fprintf(stderr, " %08X", rd(a + 4 * k)); std::fprintf(stderr, "\n"); }
        }
    }
}

// One more fighter, the same steps as the setup's own for `side` (its character is the one in that side's setup data).
// `cpu_side` is the side whose record holds the CPU brain settings (record +3892 control type, +3894 / +3896 parameters).
// ---- ownership of a fighter's heap objects (for the content exchange of the tag swap) ----
// Every guest allocation made while a fighter is being built (its constructor FUN_08B39E5C, its per-fighter init, the actor block
// itself) is recorded against that actor, in order: clones allocate the same sizes in the same order, so the k-th block of one is the
// counterpart of the k-th block of the other.
struct Blk { uint32_t addr, size; };
std::map<uint32_t, std::vector<Blk>> g_owned;
uint32_t g_own_ctx = 0;
FuncPtr g_orig_alloc = nullptr, g_orig_ctor = nullptr, g_orig_init = nullptr;
constexpr uint32_t ALLOC_CORE = 0x0885C6CCu;       // EBOOT: the heap allocator core (a0 = heap, a1 = size), v0 = block
constexpr uint32_t FIGHTER_VT = 0x08B3A9A8u;       // the fighter class's vtable (the constructor stores it in actor+1348)
void alloc_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t size = static_cast<uint32_t>(ctx->r[5]);
    g_orig_alloc(rdram, ctx);
    if (g_own_ctx) { const uint32_t p = static_cast<uint32_t>(ctx->r[2]); if (p) g_owned[g_own_ctx].push_back({p, size}); }
}
void ctor_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t mem = static_cast<uint32_t>(ctx->r[4]), prev = g_own_ctx;
    g_ram = rdram;
    if (!g_owned.count(mem)) g_owned[mem].push_back({mem, 0x12D0u});   // the actor block (allocated by the caller)
    g_own_ctx = mem;
    g_orig_ctor(rdram, ctx);
    g_own_ctx = prev;
}
void init_hook(uint8_t* rdram, recomp_context* ctx) {   // the per-fighter init: a0 = actor + a small offset
    const uint32_t a0 = static_cast<uint32_t>(ctx->r[4]), prev = g_own_ctx;
    for (auto& kv : g_owned) if (a0 >= kv.first && a0 < kv.first + 0x12D0u) { g_own_ctx = kv.first; break; }
    g_orig_init(rdram, ctx);
    g_own_ctx = prev;
}

uint32_t spawn_fighter(uint32_t bs, uint32_t side, uint32_t foe, uint32_t cpu_side, uint32_t info_slot, uint32_t slot_actor) {
    const uint32_t sp0 = static_cast<uint32_t>(g_ctx->r[29]);
    g_ctx->r[29] = static_cast<int32_t>(sp0 - 0x200u);   // scratch below the stack pointer, clear of the callees' frames
    const uint32_t buf = sp0 - 0x1C0u, lockargs = sp0 - 0x180u;
    uint32_t mem = call(ALLOC, 0x12D0);
    if (!ok(mem, 0x12D0)) { std::fprintf(stderr, "[TAGTEAM] alloc failed\n"); g_ctx->r[29] = static_cast<int32_t>(sp0); return 0; }
    g_owned[mem].clear(); g_owned[mem].push_back({mem, 0x12D0u}); g_own_ctx = mem;   // everything allocated from here on belongs to this fighter
    if (g_stop < 2) { g_ctx->r[29] = static_cast<int32_t>(sp0); return mem; }
    const uint32_t rec = call(SIDE_REC, side);
    uint32_t before[24]; if (g_log) for (int k = 0; k < 24; k++) before[k] = rd(rec + 4u * k);
    const uint32_t info = call(CHAR_INFO, rd(bs + 2008u + 4u * info_slot));
    std::vector<uint32_t> reg;
    const uint32_t RB = 0x08B64000u, RN = (0x09000000u - RB) / 4;
    const bool regdump = g_log && std::getenv("PSPRECOMP_TAGTEAM_REGDUMP");
    if (regdump) { reg.resize(RN); std::memcpy(reg.data(), g_ram + (RB & 0x07FFFFFFu), RN * 4); }
    // The constructor stores itself in two words of a table of the retail pair (0x08BAE9FC / 0x08BAEA08: the CPU fighter,
    // twice, with the human between), overwriting the CPU's entries. The game camera reads them, so they are put back.
    const uint32_t t0 = rd(REG_PAIR0), t1 = rd(REG_PAIR1);
    if (const char* w = std::getenv("PSPRECOMP_TAGTEAM_WATCHRES")) { const uint32_t a = static_cast<uint32_t>(std::strtoul(w, nullptr, 16)); psp_mem_watch_set(a); std::fprintf(stderr, "[TAGTEAM] watching %08X (before the constructor)\n", a); }
    // The constructor also registers the new fighter's lock block in the camera manager's slot for its side (mgr + 0x6D0 +
    // 4 * side: the camera follows the lock blocks of the retail pair) and resets a few words of the old one. The camera
    // then loses the human (eye and target 0). Snapshot the guest RAM so exactly those writes can be taken back.
    const uint32_t old_lb = rd(slot_actor + ACT_LOCKBLK);
    constexpr uint32_t SB = 0x08800000u, SN = (0x0A000000u - SB) / 4;
    std::vector<uint32_t> snap;
    std::vector<std::pair<uint32_t, uint32_t>> undo;   // handed to the caller (g_last_undo)
    const bool guard = !std::getenv("PSPRECOMP_TAGTEAM_NOFIX");
    if (guard) { snap.resize(SN); std::memcpy(snap.data(), g_ram + (SB & 0x07FFFFFFu), SN * 4); }
    std::vector<uint32_t> cam;   // debug: the camera manager and its mode objects around the constructor
    const uint32_t mgr = dissidia_camera_manager();
    std::vector<uint32_t> modes;
    if (regdump && ok(mgr, 0x2400)) { cam.resize(0x2400 / 4); std::memcpy(cam.data(), g_ram + (mgr & 0x07FFFFFFu), 0x2400); for (int k = 0; k < 8; k++) modes.push_back(rd(mgr + 1744u + 4u * k)); }
    std::vector<std::vector<uint32_t>> msnap;
    for (uint32_t m : modes) { std::vector<uint32_t> v(0x400 / 4, 0); if (ok(m, 0x400)) std::memcpy(v.data(), g_ram + (m & 0x07FFFFFFu), 0x400); msnap.push_back(v); }
    g_in_ctor = !std::getenv("PSPRECOMP_TAGTEAM_FREE");
    call(FIGHTER_CTOR, mem, rec, info, side, 0);
    g_in_ctor = false;
    if (!cam.empty()) {
        std::fprintf(stderr, "[TAGTEAM] camera manager %08X, modes:", mgr); for (uint32_t m : modes) std::fprintf(stderr, " %08X", m); std::fprintf(stderr, "\n");
        int n = 0;
        for (uint32_t i = 0; i < cam.size() && n < 120; i++) if (rd(mgr + 4 * i) != cam[i]) { std::fprintf(stderr, "[TAGTEAM] mgr +%04X: %08X -> %08X\n", 4 * i, cam[i], rd(mgr + 4 * i)); n++; }
        for (size_t k = 0; k < modes.size(); k++) for (uint32_t i = 0; i < 0x100 && ok(modes[k], 0x400); i++) if (rd(modes[k] + 4 * i) != msnap[k][i]) std::fprintf(stderr, "[TAGTEAM] mode%zu %08X +%03X: %08X -> %08X\n", k, modes[k], 4 * i, msnap[k][i], rd(modes[k] + 4 * i));
    }
    if (guard && g_log && std::getenv("PSPRECOMP_TAGTEAM_PTRDIFF")) {   // debug: every overwritten pointer (old value a live-looking pointer, not zero)
        int n = 0;
        for (uint32_t i = 0; i < SN && n < 400; i++) {
            const uint32_t a = SB + 4 * i, ov = snap[i], nv = rd(a);
            if (ov != nv && ov >= 0x08800000u && ov < 0x0A000000u && !(a >= mem && a < mem + 0x12D0u)) { std::fprintf(stderr, "[TAGTEAM] ptr %08X: %08X -> %08X\n", a, ov, nv); n++; }
        }
    }
    if (guard) {
        if (std::getenv("PSPRECOMP_TAGTEAM_UNDO_PAIR")) { undo.push_back({REG_PAIR0, t0}); undo.push_back({REG_PAIR1, t1}); }   // (kept by default: the CPU brain tick of the extra needs its place in this table)
        if (!std::getenv("PSPRECOMP_TAGTEAM_KEEP_LAST")) undo.push_back({LAST_FIGHTER_OBJ, snap[(LAST_FIGHTER_OBJ - SB) / 4]});   // global: the last created fighter's object
        const uint32_t new_lb = rd(mem + ACT_LOCKBLK);
        uint32_t slot_at = 0;
        for (uint32_t i = 0; i < SN; i++) {
            const uint32_t a = SB + 4 * i;
            if (snap[i] == old_lb && rd(a) == new_lb && !(a >= mem && a < mem + 0x12D0u)) { undo.push_back({a, old_lb}); slot_at = a; if (g_log) std::fprintf(stderr, "[TAGTEAM] camera slot %08X: %08X to go back to the retail lock block %08X when the extra starts\n", a, new_lb, old_lb); }
        }
        if (slot_at) {
            const uint32_t m = slot_at - 0x6D0u - 4u * side;   // the camera manager
            int n = 0;
            for (uint32_t o = 0; o < 0xD00u; o += 4) { const uint32_t a = m + o, ov = snap[(a - SB) / 4]; if (rd(a) != ov) { undo.push_back({a, ov}); n++; } }
            int k = 0;
            for (uint32_t o = 0; o < 0x440u; o += 4) { const uint32_t a = old_lb - 0x40u + o, ov = snap[(a - SB) / 4]; if (rd(a) != ov) { undo.push_back({a, ov}); k++; if (g_log) std::fprintf(stderr, "[TAGTEAM]   lock block %+d: now %08X, was %08X\n", int(o) - 0x40, rd(a), ov); } }
            if (g_log) std::fprintf(stderr, "[TAGTEAM] camera manager %08X: %d words and %d words of the old lock block to put back when the extra starts\n", m, n, k);
        } else if (g_log) std::fprintf(stderr, "[TAGTEAM] no camera slot found for lock block %08X\n", new_lb);
    }
    if (regdump) {
        int n = 0;
        for (uint32_t i = 0; i < RN && n < 300; i++) {
            const uint32_t a = RB + 4 * i, v = rd(a);
            if (v != reg[i] && !(a >= mem && a < mem + 0x12D0u)) { std::fprintf(stderr, "[TAGTEAM] reg side %u %08X: %08X -> %08X\n", side, a, reg[i], v); n++; }
        }
    }
    if (g_stop < 3) { g_ctx->r[29] = static_cast<int32_t>(sp0); return mem; }
    call(SET_FOE, mem, foe);
    if (g_stop < 4) { g_ctx->r[29] = static_cast<int32_t>(sp0); return mem; }
    if (g_log) for (int k = 0; k < 24; k++) if (rd(rec + 4u * k) != before[k]) std::fprintf(stderr, "[TAGTEAM] side %u record %08X +%02X: %08X -> %08X (after ctor)\n", side, rec, 4 * k, before[k], rd(rec + 4u * k));

    // The setup's per-fighter init: the actor's own init through its vtable, two flag bits, the lock block.
    const uint32_t G = REC_BASE + 4192u * side;
    call(REC_INIT, buf, side);
    const uint32_t vt = rd(mem + 1348) + 256u;
    const uint32_t voff = static_cast<uint32_t>(static_cast<int16_t>(rd(vt) & 0xFFFFu)), vfn = rd(vt + 4);
    if (FuncPtr f = fn(vfn)) { const recomp_context sv = *g_ctx; g_ctx->r[4] = static_cast<int32_t>(mem + voff); g_ctx->r[5] = static_cast<int32_t>(G); g_ctx->r[6] = static_cast<int32_t>(buf); f(g_ram, g_ctx); *g_ctx = sv; }
    if (rd(REC_BASE + 10208u + 4u * side) & 1u) g_ram[(mem + 1456u + 7u) & 0x07FFFFFFu] |= 0x10;
    if (rdh(REC_BASE + 10216u + 2u * side) & 1u) g_ram[(mem + 1456u + 8u) & 0x07FFFFFFu] |= 0x20;
    {
        const uint32_t lr = REC_BASE + 8384u + 4u * side, w = rdh(lr);
        wr(lockargs, static_cast<uint32_t>(static_cast<int8_t>(g_ram[(lr + 2u) & 0x07FFFFFFu])));
        g_ram[(lockargs + 4u) & 0x07FFFFFFu] = (w & 4u) ? 1 : 0;
        g_ram[(lockargs + 5u) & 0x07FFFFFFu] = (w & 0x10u) ? 1 : 0;
        call(LOCK_INIT, rd(mem + ACT_LOCKBLK), lockargs);
    }
    if (g_log) for (int k = 0; k < 24; k++) if (rd(rec + 4u * k) != before[k]) std::fprintf(stderr, "[TAGTEAM] side %u record +%02X now %08X (after init)\n", side, 4 * k, rd(rec + 4u * k));
    if (g_stop < 5) { g_ctx->r[29] = static_cast<int32_t>(sp0); return mem; }
    // The CPU brain, as the setup gives it to a fighter whose control type is not 0: FUN_08B2F2E4(actor, 0), then, for
    // types 1 and 2, its two parameters.
    const uint32_t brain = rd(mem + ACT_BRAIN), cr = REC_BASE + 4192u * cpu_side, type = rdh(cr + 3892u);
    if (g_log) std::fprintf(stderr, "[TAGTEAM] spawn side %u: actor %08X (info %08X brain %08X) foe %08X, cpu record side %u type %u params %u %u\n", side, mem, info, brain, foe, cpu_side, type, rdh(cr + 3894u), rdh(cr + 3896u));
    if (type != 0) {
        call(CPU_MODE, mem, 0);
        if (type < 3 && ok(brain, 0x40)) { call(BRAIN_MODE, brain, rdh(cr + 3894u)); call(BRAIN_LEVEL, brain, rdh(cr + 3896u)); }
    }
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_SCAN")) { std::fprintf(stderr, "[TAGTEAM] slots: %08X %08X human slot %u\n", rd(bs + 56), rd(bs + 60), rd(bs + 84)); scan_refs(mem, side ? "extra side 1" : "extra side 0"); }
    g_last_undo = undo;
    g_own_ctx = 0;
    g_ctx->r[29] = static_cast<int32_t>(sp0);
    return mem;
}

struct Snap { uint32_t base = 0, n = 0; std::vector<uint32_t> w; };
Snap take(uint32_t base, uint32_t bytes) { Snap s; s.base = base; s.n = bytes / 4; for (uint32_t i = 0; i < s.n; i++) s.w.push_back(rd(base + 4 * i)); return s; }
void diff(const char* tag, const Snap& s) {
    for (uint32_t i = 0; i < s.n; i++) { const uint32_t v = rd(s.base + 4 * i); if (v != s.w[i]) std::fprintf(stderr, "[TAGTEAM] %s %08X+%04X: %08X -> %08X\n", tag, s.base, 4 * i, s.w[i], v); }
}

void setup_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t bs = static_cast<uint32_t>(ctx->r[4]);
    g_ram = rdram; g_ctx = ctx;
    if (!g_orig_ctor) {   // ownership recording for the retail pair (the constructor and the init run inside the original setup)
        g_orig_ctor = fn(FIGHTER_CTOR);
        const uint32_t vfn0 = rd(FIGHTER_VT + 256u + 4u);
        g_orig_init = fn(vfn0);
        if (g_orig_ctor && g_orig_init) { psp_dispatch_register(FIGHTER_CTOR, ctor_hook); psp_dispatch_register(vfn0, init_hook); if (g_log) std::fprintf(stderr, "[TAGTEAM] ownership hooks: ctor %08X init %08X\n", FIGHTER_CTOR, vfn0); }
        else std::fprintf(stderr, "[TAGTEAM] ownership hooks NOT installed\n");
    }
    g_owned.clear(); g_own_ctx = 0;
    g_orig_setup(rdram, ctx);
    g_ram = rdram; g_ctx = ctx;
    g_nextra = 0; g_bs = 0; g_late_q.clear(); g_late_ready = false;
    if (!g_enabled) return;
    if (!ok(bs, 0x2800)) return;
    const uint32_t a0 = rd(bs + 56), a1 = rd(bs + 60), mode = rd(bs + 76), human = rd(bs + 84);
    if (g_log) std::fprintf(stderr, "[TAGTEAM] setup bs=%08X mode=%u slots %08X %08X human slot %u\n", bs, mode, a0, a1, human);
    if (!ok(a0, 0x12D0) || !ok(a1, 0x12D0)) return;
    if (!g_orig_lock_other) {
        g_orig_lock_other = fn(LOCK_OTHER); g_orig_refresh = fn(TARGET_REFRESH); g_orig_lock_mode = fn(LOCK_MODE);
        if (g_orig_lock_other && g_orig_refresh && g_orig_lock_mode) { psp_dispatch_register(LOCK_OTHER, lock_other_hook); psp_dispatch_register(TARGET_REFRESH, refresh_hook); psp_dispatch_register(LOCK_MODE, lock_mode_hook); }
        else std::fprintf(stderr, "[TAGTEAM] lock cycle hooks not installed\n");
    }
    if (!g_orig_tick) { g_orig_tick = fn(BRAIN_TICK); if (g_orig_tick) psp_dispatch_register(BRAIN_TICK, tick_hook); else std::fprintf(stderr, "[TAGTEAM] brain tick not found\n"); }
    const uint32_t hum = human == 0 ? a0 : a1, cpu = human == 0 ? a1 : a0;
    if (const char* w = std::getenv("PSPRECOMP_TAGTEAM_WATCH_BS")) {   // arm the write watch on bs + hex offset
        const uint32_t off = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
        psp_mem_watch_set(bs + off);
        std::fprintf(stderr, "[TAGTEAM] watching bs+%X = %08X\n", off, bs + off);
    }
    if (const char* w = std::getenv("PSPRECOMP_TAGTEAM_WATCH")) {   // arm the write watch on <human actor> + hex offset
        const uint32_t off = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
        psp_mem_watch_set(hum + off);
        std::fprintf(stderr, "[TAGTEAM] watching %08X\n", hum + off);
    }
    if (const char* w = std::getenv("PSPRECOMP_TAGTEAM_WATCH_STATUS")) {   // watch the human's status object (actor+0x52C) + hex offset
        const uint32_t st = rd(hum + 0x52Cu), off = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
        psp_mem_watch_set(st + off); std::fprintf(stderr, "[TAGTEAM] watching status %08X + %X\n", st, off);
    }
    if (std::getenv("PSPRECOMP_TAGTEAM_WATCH_SEL")) {   // watch the human's lock cycle selector (retail mode handler [1] + 28)
        const uint32_t L = rd(hum + 0x5ACu), h1 = ok(L, 0x200) ? rd(L + 304u) : 0u;
        if (ok(h1, 0x40)) { psp_mem_watch_set(h1 + 28u); std::fprintf(stderr, "[TAGTEAM] watching the lock selector at %08X\n", h1 + 28u); }
    }
    // Slot 0 is side 0 and slot 1 side 1 in the setup, whoever the human is; the CPU brain settings are the CPU slot's.
    const uint32_t cpu_side = human == 0 ? 1 : 0, hum_side = human;
    if ((g_mask & 1) && g_late) { g_late_q.push_back({cpu_side, hum, cpu_side, cpu_side, cpu}); g_bs = bs; }
    else if (g_mask & 1) {   // the enemy team's second fighter: a copy of the CPU fighter, foe = the human
        Extra& e = g_extra[g_nextra];
        e.actor = spawn_fighter(bs, cpu_side, hum, cpu_side, cpu_side, cpu); e.side = cpu_side; e.foe = hum; e.slot_actor = cpu; e.started = false;
        if (e.actor) { e.undo = g_last_undo; g_nextra++; }
    }
    if (g_mask & 2) {   // the human's partner: a copy of the human fighter on the CPU brain, foe = the CPU fighter
        const uint32_t P = rd(0x089D6A10u);
        Snap sb, sp, sg;
        if (g_log) { sb = take(bs, 0x2800); if (ok(P, 0x400)) sp = take(P, 0x400); sg = take(0x089D6000u, 0x1000); }
        if (g_late) { g_late_q.push_back({hum_side, cpu, cpu_side, std::getenv("PSPRECOMP_TAGTEAM_PARTNER_CPU_CHAR") ? cpu_side : hum_side, hum}); g_bs = bs; goto after_partner; }
        Extra& e = g_extra[g_nextra];
        e.actor = spawn_fighter(bs, hum_side, cpu, cpu_side, std::getenv("PSPRECOMP_TAGTEAM_PARTNER_CPU_CHAR") ? cpu_side : hum_side, hum);
        if (g_log) { diff("bs", sb); if (!sp.w.empty()) diff("P", sp); diff("glob", sg); } e.side = hum_side; e.foe = cpu; e.slot_actor = hum; e.started = false;
        if (e.actor) { e.undo = g_last_undo; g_nextra++; }
    }
    after_partner:
    if (g_nextra) g_bs = bs;
}

// Take back the constructor's registrations (camera slot, lock block list, pair table). They are needed while the fighter
// loads (it never becomes ready without them), but with them in place the game camera follows the new fighter, not the human.
void apply_undo(Extra& e) {
    for (auto& u : e.undo) wr(u.first, u.second);
    if (g_log && !e.undo.empty()) std::fprintf(stderr, "[TAGTEAM] extra %08X: %zu registration words taken back\n", e.actor, e.undo.size());
    e.undo.clear();
}

void start_extra(Extra& e) {
    if (g_stop < 6) { e.started = true; return; }
    apply_undo(e);
    const uint32_t sp0 = static_cast<uint32_t>(g_ctx->r[29]);
    g_ctx->r[29] = static_cast<int32_t>(sp0 - 0x100u);
    const uint32_t vec = sp0 - 0xC0u;
    // Stand 3 m beside the fighter it copies (the retail pair is placed by the stage at this point).
    wrf(vec, rdf(e.slot_actor + 0x80u) + 3.0f); wrf(vec + 4, rdf(e.slot_actor + 0x84u)); wrf(vec + 8, rdf(e.slot_actor + 0x88u)); wrf(vec + 12, 1.0f);
    call(PLACE, e.actor, vec, 1);
    g_ctx->f[12] = 0.0f; g_ctx->f[13] = 0.0f;
    call(START_ACTION, e.actor + 304u, 1, 0);
    call(LOCK_SET, rd(e.actor + 0x5ACu), e.foe);          // lock on to the foe: the AI and the turn-to-target ops read it
    {   // Bravery: the retail pair gets its base bravery (actor +0x10AC) in both halves of status +0x14 at battle start; the extras start at 0 (broken).
        const uint32_t st = rd(e.actor + 0x52Cu), b = rd(e.actor + 0x10ACu) & 0xFFFFu;
        if (ok(st, 0x40) && rd(st + 0x14u) == 0) wr(st + 0x14u, (b << 16) | b);
    }
    e.started = true;
    if (g_log) for (uint32_t a : {e.slot_actor, e.actor}) { const uint32_t st = rd(a + 0x52Cu); std::fprintf(stderr, "[TAGTEAM] stats %08X: +10A8 %u %u %u %u %u status %08X +0x14 %08X +0x18 %08X +0x1C %08X\n", a, rd(a + 0x10A8u), rd(a + 0x10ACu), rd(a + 0x10B0u), rd(a + 0x10B4u), rd(a + 0x10B8u), st, rd(st + 0x14u), rd(st + 0x18u), rd(st + 0x1Cu)); }
    if (g_log) std::fprintf(stderr, "[TAGTEAM] extra %08X (side %u) started, target %08X foe %08X\n", e.actor, e.side, rd(e.actor + 0x2FCu), e.foe);
    g_ctx->r[29] = static_cast<int32_t>(sp0);
}

// The flow step runs every frame until both fighters are ready, then starts them (action 1) and sets bs+72. The extras
// are held to the same rule and started right after the original has started the others.
void flow_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t bs = static_cast<uint32_t>(ctx->r[4]);
    g_ram = rdram; g_ctx = ctx;
    bool pending = false;
    for (int i = 0; i < g_nextra; i++) if (!g_extra[i].started) pending = true;
    if (!pending || bs != g_bs || rd(bs + 72) != 0) { g_orig_flow(rdram, ctx); return; }
    static int n = 0;
    bool ready = true;
    for (int i = 0; i < g_nextra; i++) if (!call(FIGHTER_READY, g_extra[i].actor)) ready = false;
    if (g_log && (n++ % 60) == 0) std::fprintf(stderr, "[TAGTEAM] flow: extras ready=%d\n", ready);
    if (!ready) return;                                      // wait: the original would start the others without them
    for (int i = g_nextra - 1; i >= 0; i--) apply_undo(g_extra[i]);   // (last spawned first) before the original starts the retail pair and the camera
    g_orig_flow(rdram, ctx);
    if (rd(bs + 72) == 0) return;                            // the others were not ready yet
    for (int i = 0; i < g_nextra; i++) if (!g_extra[i].started) start_extra(g_extra[i]);
}

// ---- HUD (tagteam_hud.cpp) ----
std::vector<TagHudReserve> g_reserve;   // waiting fighters per side (placeholder list until the roster call-ins exist)
bool g_hud_on = true;

// A character's label. Retail charIds 1..33 have a name table (the order of the roster; 15 = Squall and 31 = Lightning are confirmed
// in runs, the rest follows the roster order and the series codes of the table at 0x089D6CA8); other ids (mods) show the first
// letters of their code string ("fiv100" = Bartz -> "FIV1"), else "#id".
struct CharName { const char* full; const char* brief; };
const CharName kNames[34] = {
    {"", ""}, {"WARRIOR OF LIGHT", "WOL"}, {"GARLAND", "GARL"}, {"FIRION", "FIRI"}, {"EMPEROR", "EMPE"}, {"ONION KNIGHT", "ONIO"},
    {"CLOUD OF DARKNESS", "COD"}, {"CECIL", "CECI"}, {"GOLBEZ", "GOLB"}, {"BARTZ", "BART"}, {"EXDEATH", "EXDE"}, {"TERRA", "TERR"},
    {"KEFKA", "KEFK"}, {"CLOUD", "CLOU"}, {"SEPHIROTH", "SEPH"}, {"SQUALL", "SQUA"}, {"ULTIMECIA", "ULTI"}, {"ZIDANE", "ZIDA"},
    {"KUJA", "KUJA"}, {"TIDUS", "TIDU"}, {"JECHT", "JECH"}, {"SHANTOTTO", "SHAN"}, {"GABRANTH", "GABR"}, {"CHAOS", "CHAO"},
    {"KAIN", "KAIN"}, {"GILGAMESH", "GILG"}, {"TIFA", "TIFA"}, {"LAGUNA", "LAGU"}, {"YUNA", "YUNA"}, {"VAAN", "VAAN"},
    {"PRISHE", "PRIS"}, {"LIGHTNING", "LIGH"}, {"FERAL CHAOS", "FCHA"}, {"YUFFIE", "YUFF"},
};
std::string char_label(int id, bool full) {
    if (id >= 1 && id <= 33) return full ? kNames[id].full : kNames[id].brief;
    if (id <= 0 || id > 60) return "#" + std::to_string(id);
    const uint32_t p = rd(0x089D6CA8u + 4u * static_cast<uint32_t>(id));
    std::string out;
    if (ok(p, 8)) for (int i = 0; i < 4; i++) { const char c = static_cast<char>(g_ram[(p + i) & 0x07FFFFFFu]); if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) break; out += c; }
    return out.size() >= 2 ? out : "#" + std::to_string(id);
}
int actor_char(uint32_t actor) {
    const uint32_t info = rd(actor + 0x2F8u);
    return ok(info, 8) ? static_cast<int8_t>(g_ram[(info + 6u) & 0x07FFFFFFu]) : -1;
}

void init_reserve() {
    g_reserve.clear();
    g_reserve.resize(2);
    g_reserve[0].side = 0; g_reserve[1].side = 1;
    const char* e = std::getenv("PSPRECOMP_TAGTEAM_RESERVE");   // "id,id;id,id": charIds waiting on the human's side ; the enemy's side
    std::string spec = e ? e : "5,9;20,24";                      // placeholder roster for the HUD until the party call-ins exist
    int side = 0; int v = -1;
    auto flush = [&]() { if (v >= 0) g_reserve[side].names.push_back(char_label(v, false)); v = -1; };
    for (char c : spec) { if (c >= '0' && c <= '9') v = (v < 0 ? 0 : v * 10) + (c - '0'); else if (c == ',') flush(); else if (c == ';') { flush(); side = 1; } }
    flush();
}

void hud_update() {
    if (!g_hud_on) return;
    static bool inited = false;
    if (!inited) { inited = true; init_reserve(); if (g_log) { std::fprintf(stderr, "[TAGTEAM] labels:"); for (int i = 0; i <= 36; i++) { const uint32_t p = rd(0x089D6CA8u + 4u * static_cast<uint32_t>(i)); char b[9] = {}; if (ok(p, 8)) for (int k = 0; k < 8; k++) b[k] = static_cast<char>(g_ram[(p + k) & 0x07FFFFFFu]); std::fprintf(stderr, " %d=%s", i, b); } std::fprintf(stderr, "\n"); } }
    std::vector<TagHudFighter> fs;
    for (int i = 0; i < g_nextra; i++) {
        Extra& e = g_extra[i];
        TagHudFighter f;
        f.name = char_label(actor_char(e.actor), true); f.side = static_cast<int>(e.side == g_extra[0].side ? e.side : e.side);
        { const uint32_t st = rd(e.actor + 0x52Cu); f.hp = ok(st, 0x40) ? static_cast<int>(static_cast<int32_t>(rd(st + 0xCu))) : 0; }   // live HP (the status object word the retail HUD reads); +0x10B0 is the maximum
        f.hp_max = std::max(static_cast<int>(rd(e.actor + 0x10B0u)), std::max(f.hp, 1));
        fs.push_back(f);
    }
    // sides in the HUD: the human's team on the left (side value of the human's slot), the other on the right
    const uint32_t hum_side = g_bs ? rd(g_bs + 84u) : 0u;
    for (TagHudFighter& f : fs) f.side = (static_cast<uint32_t>(f.side) == hum_side) ? 0 : 1;
    tagteam_hud_draw(fs, g_reserve);
}

// ---- lock-on cycling between the enemy team's fighters ----
// Every fighter of the battle: the two retail slots and the extras. Side = halfword actor+0x10CC (the ctor's side argument).
int all_fighters(uint32_t out[6]) {
    int n = 0;
    if (g_bs) { for (int k = 0; k < 2; k++) { const uint32_t a = rd(g_bs + 56u + 4u * k); if (ok(a, 0x12D0)) out[n++] = a; } }
    for (int i = 0; i < g_nextra && n < 6; i++) if (g_extra[i].started) out[n++] = g_extra[i].actor;
    return n;
}
uint32_t side_of(uint32_t a) { return rdh(a + 0x10CCu); }
bool alive(uint32_t a) { return static_cast<int32_t>(rd(a + 0x10B0u)) > 0; }

// The enemy fighter to cycle to from `foe` for `owner`: another living fighter of the other team, 0 if none.
uint32_t other_enemy(uint32_t owner, uint32_t foe) {
    uint32_t f[6];
    const int n = all_fighters(f);
    for (int i = 0; i < n; i++) if (f[i] != owner && f[i] != foe && side_of(f[i]) != side_of(owner) && alive(f[i])) return f[i];
    return 0;
}

// FUN_08B40994: the cycle's second candidate (retail: the foe's assist). In a tag battle it is the foe's team mate, so the retail cycle
// (FUN_08B3FE3C advances through the candidates, asking this function whether each exists) walks foe -> other enemy -> ...
void lock_other_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram; g_ctx = ctx;
    const uint32_t L = static_cast<uint32_t>(ctx->r[5]);
    if (g_nextra && ok(L, 0x200)) {
        const uint32_t owner = rd(L + 248u);
        if (ok(owner, 0x12D0) && g_ram[(owner + 0x542u) & 0x07FFFFFFu] == 0) {
            const uint32_t o = other_enemy(owner, rd(owner + 0x300u));
            if (o) { ctx->r[2] = static_cast<int32_t>(o); return; }
        }
    }
    g_orig_lock_other(rdram, ctx);
}

// FUN_08B2F17C copies the lock result into actor+0x2FC but, for a fighter that is not an assist, always writes the foe (+0x300). Afterwards
// a locked enemy fighter other than the foe is written back.
void refresh_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]);
    g_ram = rdram; g_ctx = ctx;
    g_orig_refresh(rdram, ctx);
    if (!g_nextra || !ok(actor, 0x12D0)) return;
    const uint32_t L = rd(actor + 0x5ACu);
    if (!ok(L, 0x200)) return;
    const uint32_t t = call(LOCK_GET, L);
    if (t && t != actor && ok(t, 0x12D0) && g_ram[(t + 0x542u) & 0x07FFFFFFu] == 0 && side_of(t) != side_of(actor) && alive(t)) {
        if (g_log && rd(actor + 0x2FCu) != t) std::fprintf(stderr, "[TAGTEAM] %08X targets %08X (foe %08X)\n", actor, t, rd(actor + 0x300u));
        wr(actor + 0x2FCu, t);
    }
}

// FUN_08B3B3EC(block, mode, flag): L toggles the human's lock through it. Tag battle: with a second enemy alive, the press that would
// release the lock first moves the lock to that enemy (the retail cycle selector, handler [1] +28 = 1); the next press releases it.
void lock_mode_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram; g_ctx = ctx;
    const uint32_t L = static_cast<uint32_t>(ctx->r[4]), mode = static_cast<uint32_t>(ctx->r[5]);
    if (g_nextra && ok(L, 0x200)) {
        const uint32_t owner = rd(L + 248u), h1 = rd(L + 304u);
        if (g_log && (owner == rd(g_bs + 56u + 4u * rd(g_bs + 84u)))) { static int n = 0; if (n++ < 40) std::fprintf(stderr, "[TAGTEAM] t=%.1fs lock mode call: block %08X mode %u -> %u flag %u (sel %d)\n", dissidia_battle_age() / 60.0f, L, rd(L + 264u), mode, static_cast<uint32_t>(ctx->r[6]), ok(h1, 0x40) ? static_cast<int>(rd(h1 + 28u)) : -99); }
        if (mode == 0 && rd(L + 264u) == 1 && ok(h1, 0x40) && ok(owner, 0x12D0) && g_ram[(owner + 0x542u) & 0x07FFFFFFu] == 0 && owner == rd(g_bs + 56u + 4u * rd(g_bs + 84u))) {
            static int cyc = 0;   // 0: locked on the foe, 1: on the other enemy (the retail selector is reset by the press itself, so the state is kept here)
            const uint32_t other = other_enemy(owner, rd(owner + 0x300u));
            if (cyc == 0 && other) {
                cyc = 1; wr(h1 + 28u, 1);           // cycle to the other enemy instead of releasing the lock
                if (g_log) std::fprintf(stderr, "[TAGTEAM] lock cycle: %08X -> the other enemy %08X\n", rd(owner + 0x300u), other);
                ctx->r[2] = 1; return;
            }
            cyc = 0; wr(h1 + 28u, 0);               // released: the next lock starts on the foe again
        }
    }
    g_orig_lock_mode(rdram, ctx);
}

// ---- tag swap: SELECT (PSPRECOMP_TAGTEAM_SWAP_BUTTON=<hex psp mask>) hands the pad to the partner ----
uint32_t g_swap_mask = 0;         // off until the swap keeps the camera and HUD intact; PSPRECOMP_TAGTEAM_SWAP_BUTTON=1 (hex PSP mask, 1 = SELECT) turns it on
bool g_swap_req = false;
Extra* extra_of(uint32_t actor) {
    for (int i = 0; i < g_nextra; i++) if (g_extra[i].actor == actor) return &g_extra[i];
    return nullptr;
}

// Hand the pad to the partner: the two fighters trade brains (the pad is read by the human brain, +0x54 != 0) and trade places in
// the battle's slot table, which the retail HUD, the camera and the lock cycle read. The partner becomes an extra on the CPU brain.
// Debug (PSPRECOMP_TAGTEAM_SWAPSCAN=1): every word in guest RAM equal to `v`, outside the two actors' own blocks.
void scan_val(uint32_t v, const char* tag, uint32_t a0, uint32_t a1) {
    int n = 0;
    for (uint32_t a = 0x08800000u; a < 0x0BF00000u && n < 150; a += 4) {
        if ((a >= a0 && a < a0 + 0x12D0u) || (a >= a1 && a < a1 + 0x12D0u)) continue;
        if (rd(a) == v) { std::fprintf(stderr, "[SWAPSCAN] %s %08X held at %08X\n", tag, v, a); n++; }
    }
}

void dump_win(uint32_t at, const char* tag, uint32_t H, uint32_t P, uint32_t C, uint32_t lbH, uint32_t lbP) {
    std::fprintf(stderr, "[SWAPSCAN] window %s @%08X:", tag, at);
    for (int k = -6; k <= 6; k++) {
        const uint32_t v = rd(at + 4 * k);
        const char* n = v == H ? "H" : v == P ? "P" : v == C ? "C" : v == lbH ? "lbH" : v == lbP ? "lbP" : nullptr;
        if (n) std::fprintf(stderr, " [%s]", n); else std::fprintf(stderr, " %X", v);
    }
    std::fprintf(stderr, "\n");
}

// Debug: the retail HUD's accessor functions (battle overlay 0x089E95E0..0x089E9EF4, a0 = side) before / after the swap.
void hud_probe(const char* tag) {
    static const uint32_t fns[] = {0x089E95B0u, 0x089E95E0u, 0x089E9628u, 0x089E968Cu, 0x089E96DCu, 0x089E973Cu, 0x089E978Cu, 0x089E97FCu, 0x089E9858u, 0x089E98F0u, 0x089E9964u, 0x089E99C0u, 0x089E9A4Cu, 0x089E9AA8u, 0x089E9B18u, 0x089E9C24u, 0x089E9C64u, 0x089E9D84u, 0x089E9E10u, 0x089E9E68u, 0x089E9EF4u};
    for (uint32_t f : fns) {
        std::fprintf(stderr, "[HUDPROBE] %s %08X:", tag, f);
        for (uint32_t side = 0; side < 2; side++) {
            FuncPtr fp = fn(f); if (!fp) continue;
            const recomp_context sv = *g_ctx; g_ctx->r[4] = static_cast<int32_t>(side); g_ctx->r[5] = 0;
            fp(g_ram, g_ctx);
            std::fprintf(stderr, "  s%u v0=%08X f0=%g", side, static_cast<uint32_t>(g_ctx->r[2]), static_cast<double>(g_ctx->f[0]));
            *g_ctx = sv;
        }
        std::fprintf(stderr, "\n");
    }
}

// ---- tag swap by content exchange ----
// The retail HUD, camera, pad, lock block, pair table and the enemies' targets all refer to the two slot fighters by ADDRESS. So the swap
// leaves every address alone and exchanges the CONTENT of the human's fighter (H) and the partner (P): the actor block and the heap
// objects each owns (recorded by alloc_hook), word by word, with
//   - plain numbers exchanged (position, hp, bravery, action state, animation ...),
//   - pointers into either fighter's owned blocks translated to the counterpart block (offset kept), so the sub-objects of the content
//     that moved point at the content's new home,
//   - pointers to anything else (shared resources, list neighbours, registrations, targets) left where they are: they belong to the
//     ADDRESS (identity), not to the character,
//   - the human brain and the lock block left in place whole (the pad and the camera slot belong to the address).
bool isptr(uint32_t w) { return (w & 3u) == 0 && w >= 0x08800000u && w < 0x0C000000u; }
struct BlkPair { Blk h, p; bool keep; };
std::vector<BlkPair> g_pairs;
uint32_t trans(uint32_t v) {
    for (const BlkPair& b : g_pairs) {
        if (v >= b.h.addr && v < b.h.addr + b.h.size) return v - b.h.addr + b.p.addr;
        if (v >= b.p.addr && v < b.p.addr + b.p.size) return v - b.p.addr + b.h.addr;
    }
    return v;
}
bool content_swap(uint32_t H, uint32_t P) {
    auto ih = g_owned.find(H), ip = g_owned.find(P);
    if (ih == g_owned.end() || ip == g_owned.end()) { std::fprintf(stderr, "[TAGTEAM] swap: no ownership record (H %zu, P %zu)\n", ih == g_owned.end() ? 0u : ih->second.size(), ip == g_owned.end() ? 0u : ip->second.size()); return false; }
    const std::vector<Blk>& bh = ih->second; const std::vector<Blk>& bp = ip->second;
    if (bh.size() != bp.size()) { std::fprintf(stderr, "[TAGTEAM] swap: H owns %zu blocks, P %zu: not clones\n", bh.size(), bp.size()); return false; }
    for (size_t k = 0; k < bh.size(); k++) if (bh[k].size != bp[k].size) { std::fprintf(stderr, "[TAGTEAM] swap: block %zu size %u vs %u\n", k, bh[k].size, bp[k].size); return false; }
    const uint32_t keepH[2] = {rd(H + ACT_BRAIN), rd(H + ACT_LOCKBLK)}, keepP[2] = {rd(P + ACT_BRAIN), rd(P + ACT_LOCKBLK)};
    g_pairs.clear();
    int kept = 0;
    for (size_t k = 0; k < bh.size(); k++) {
        bool keep = false;
        for (int i = 0; i < 2; i++) if (bh[k].addr == keepH[i]) { keep = true; if (bp[k].addr != keepP[i]) { std::fprintf(stderr, "[TAGTEAM] swap: kept block %zu pairs %08X with %08X, not %08X\n", k, bh[k].addr, bp[k].addr, keepP[i]); return false; } }
        if (keep) kept++;
        g_pairs.push_back({bh[k], bp[k], keep});
    }
    if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: %zu block pairs (%d kept: brain %08X/%08X, lock block %08X/%08X)\n", g_pairs.size(), kept, keepH[0], keepP[0], keepH[1], keepP[1]);
    struct W { uint32_t a, v; };
    std::vector<W> writes;
    size_t nnum = 0, ntrans = 0, next = 0;
    auto f = [&](uint32_t w, bool* ext) { *ext = false; if (!isptr(w)) return w; const uint32_t t = trans(w); if (t != w) return t; *ext = true; return w; };
    for (const BlkPair& b : g_pairs) {
        if (b.keep) continue;
        for (uint32_t o = 0; o < b.h.size; o += 4) {
            const uint32_t a = b.h.addr + o, c = b.p.addr + o, x = rd(a), y = rd(c);
            if (x == y) continue;
            bool ex = false, ey = false;
            const uint32_t fx = f(x, &ex), fy = f(y, &ey);   // what x / y become after moving to the other block
            const uint32_t na = ey ? x : fy;                  // an external pointer stays with its address
            const uint32_t nc = ex ? y : fx;
            if (ex || ey) next++; else if (isptr(x) || isptr(y)) ntrans++; else nnum++;
            if (na != x) writes.push_back({a, na});
            if (nc != y) writes.push_back({c, nc});
        }
    }
    for (const W& w : writes) wr(w.a, w.v);
    if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: content exchanged: %zu numeric, %zu translated pointer, %zu external-pointer words left with their address, %zu writes\n", nnum, ntrans, next, writes.size());
    return true;
}

void swap_content() {
    if (!g_bs) return;
    const uint32_t hs = rd(g_bs + 84u), H = rd(g_bs + 56u + 4u * hs);
    Extra* pe = nullptr;
    for (int i = 0; i < g_nextra; i++) if (g_extra[i].started && g_extra[i].side == side_of(H) && alive(g_extra[i].actor)) pe = &g_extra[i];
    if (!pe || !ok(H, 0x12D0) || !alive(H)) { if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: no living partner\n"); return; }
    const uint32_t P = pe->actor;
    if (content_swap(H, P)) {
        if (rd(rd(H + ACT_LOCKBLK) + 264u) != 1) call(LOCK_MODE, rd(H + ACT_LOCKBLK), 1, 0);
        if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: pad character is now the one that stood at %08X (H %08X, P %08X)\n", P, H, P);
    }
}

void swap_roles() {
    static const int mode0 = std::getenv("PSPRECOMP_TAGTEAM_SWAP_MODE") ? std::atoi(std::getenv("PSPRECOMP_TAGTEAM_SWAP_MODE")) : 4;
    if (mode0 == 4) { swap_content(); return; }
    if (!g_bs) return;
    const uint32_t hs = rd(g_bs + 84u), slot = g_bs + 56u + 4u * hs, H = rd(slot);
    Extra* pe = nullptr;
    for (int i = 0; i < g_nextra; i++) if (g_extra[i].started && g_extra[i].side == side_of(H) && alive(g_extra[i].actor)) pe = &g_extra[i];
    if (!pe || !ok(H, 0x12D0) || !alive(H)) { if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: no living partner\n"); return; }
    const uint32_t P = pe->actor, bh = rd(H + ACT_BRAIN), bp = rd(P + ACT_BRAIN);
    if (g_log) { const uint32_t T = rd(0x089D6A10u); std::fprintf(stderr, "[TAGTEAM] swap: bs %08X slots %08X %08X human slot %u; [0x089D6A10] = %08X slots %08X %08X\n", g_bs, rd(g_bs + 56u), rd(g_bs + 60u), hs, T, ok(T, 0x80) ? rd(T + 56u) : 0u, ok(T, 0x80) ? rd(T + 60u) : 0u); }
    if (!ok(bh, 0x80) || !ok(bp, 0x80)) return;
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_SWAPSCAN")) {
        std::fprintf(stderr, "[SWAPSCAN] mgr %08X bs %08X H %08X P %08X\n", dissidia_camera_manager(), g_bs, H, P);
        scan_val(H, "H", H, P); scan_val(P, "P", H, P);
        scan_val(rd(H + ACT_LOCKBLK), "lbH", H, P); scan_val(rd(P + ACT_LOCKBLK), "lbP", H, P);
        scan_val(bh, "brainH", H, P); scan_val(bp, "brainP", H, P);
        const uint32_t C = rd(g_bs + 56u + 4u * (1u - hs)), lbH_ = rd(H + ACT_LOCKBLK), lbP_ = rd(P + ACT_LOCKBLK);
        const uint32_t ws[] = {0x08B63310u, 0x08B876A0u, 0x08BAEA04u, 0x08C00720u, 0x09AEEFD0u, 0x089D4890u, 0x08BAE204u, 0x08BAE9FCu, 0x08C00AD4u, 0x09785AECu, 0x09785AFCu, 0x09AE9340u};
        for (uint32_t w : ws) dump_win(w, "w", H, P, C, lbH_, lbP_);
    }
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_HUDPROBE")) hud_probe("before");
    static const int mode = std::getenv("PSPRECOMP_TAGTEAM_SWAP_MODE") ? std::atoi(std::getenv("PSPRECOMP_TAGTEAM_SWAP_MODE")) : 3;   // 1 brains only, 2 slots only, 3 both (debugging)
    if (mode & 1) { wr(H + ACT_BRAIN, bp); wr(P + ACT_BRAIN, bh); wr(bh + 4u, P); wr(bp + 4u, H); }   // the pad (human brain) goes to the partner, the AI brain to the old human, each brain's owner
    if (mode & 2) { wr(slot, P); pe->actor = H; pe->slot_actor = P; }   // slot table: the retail HUD / camera / lock cycle follow the new human
    // the camera manager keeps one lock block per side (mgr + 0x6D0 + 4 * side)
    const uint32_t mgr = dissidia_camera_manager(), side = side_of(H), lbH = rd(H + ACT_LOCKBLK), lbP = rd(P + ACT_LOCKBLK);
    if (mgr) { const uint32_t at = mgr + 0x6D0u + 4u * side; if (rd(at) == lbH) wr(at, lbP); else if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: camera slot %08X holds %08X, expected %08X\n", at, rd(at), lbH); }
    // Other shared references to the human fighter (found by scanning RAM for H at swap time): the camera manager's fighter list
    // (mgr + 0x6360: [CPU, human]), an EBOOT fighter list (0x089D4890: [CPU, human]) and 0x08B63310. H is replaced by P there.
    static const uint32_t mask = std::getenv("PSPRECOMP_TAGTEAM_SWAP_LISTS") ? static_cast<uint32_t>(std::strtoul(std::getenv("PSPRECOMP_TAGTEAM_SWAP_LISTS"), nullptr, 16)) : 7u;
    const uint32_t lists[3] = {mgr ? mgr + 0x6360u : 0u, 0x089D4890u, 0x08B63310u};
    for (int i = 0; i < 3; i++) if ((mask & (1u << i)) && lists[i]) { const uint32_t v = rd(lists[i]); if (v == H) wr(lists[i], P); else if (v == P) wr(lists[i], H); else if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: list %08X holds %08X (H %08X P %08X), left alone\n", lists[i], v, H, P); }
    // the new human locks on to the foe, the old one keeps the foe he had
    if (rd(lbP + 264u) != 1) call(LOCK_MODE, lbP, 1, 0);
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_HUDPROBE")) hud_probe("after");
    if (g_log) std::fprintf(stderr, "[TAGTEAM] swap: pad %08X -> %08X (brains %08X / %08X)\n", H, P, bh, bp);
}

void tick_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t brain = static_cast<uint32_t>(ctx->r[4]);
    g_ram = rdram; g_ctx = ctx;
    const uint32_t owner = rd(brain + 4);
    if (g_nextra && g_extra[0].started && owner == g_extra[0].actor) hud_update();
    if (g_log && g_bs) {   // the human's lock cycle selector (retail mode handler [1], +28) and its target
        static int last_sel = -99; static uint32_t last_t = 0, last_mode = 99;
        const uint32_t hum = rd(g_bs + 56u + 4u * rd(g_bs + 84u)), L = ok(hum, 0x12D0) ? rd(hum + 0x5ACu) : 0u;
        if (ok(L, 0x200) && owner == rd(hum + 0x5C4u + 0u) + 0u) {}
        if (ok(L, 0x200)) {
            const uint32_t h1 = rd(L + 304u);
            const int sel = ok(h1, 0x40) ? static_cast<int>(rd(h1 + 28u)) : -99; const uint32_t t = rd(L + 252u);
            if (sel != last_sel || t != last_t || rd(L + 264u) != last_mode) { last_mode = rd(L + 264u); std::fprintf(stderr, "[TAGTEAM] t=%.1fs human lock: sel %d -> %d, mode %u, target %08X -> %08X (+0x2FC %08X foe %08X)\n", dissidia_battle_age() / 60.0f, last_sel, sel, rd(L + 264u), last_t, t, rd(hum + 0x2FCu), rd(hum + 0x300u)); last_sel = sel; last_t = t; }
        }
    }
    if (g_log && g_bs && std::getenv("PSPRECOMP_TAGTEAM_KO")) {   // debugging: "<who>:<seconds>" puts a fighter on 1 HP and 0 bravery so the next hit KOs it (who = human, cpu, enemy2, partner)
        static bool done = false; static uint32_t target = 0; static int lasthp = -1;
        char who[16] = {}; float at = 0; std::sscanf(std::getenv("PSPRECOMP_TAGTEAM_KO"), "%15[a-z0-9]:%f", who, &at);
        if (!done && dissidia_battle_age() / 60.0f >= at && g_nextra) {
            uint32_t a = 0;
            const uint32_t hs = rd(g_bs + 84u);
            if (!std::strcmp(who, "human")) a = rd(g_bs + 56u + 4u * hs);
            else if (!std::strcmp(who, "cpu")) a = rd(g_bs + 56u + 4u * (1u - hs));
            else for (int i = 0; i < g_nextra; i++) if (g_extra[i].started && ((!std::strcmp(who, "enemy2") && g_extra[i].side != side_of(rd(g_bs + 56u + 4u * hs))) || (!std::strcmp(who, "partner") && g_extra[i].side == side_of(rd(g_bs + 56u + 4u * hs))))) a = g_extra[i].actor;
            if (ok(a, 0x12D0)) {
                const uint32_t st = rd(a + 0x52Cu);
                const uint32_t hp0 = std::getenv("PSPRECOMP_TAGTEAM_KO_HP0") ? 0u : 1u;   // HP0=1: the live HP goes to 0 at once (the game's own update has to notice)
                if (ok(st, 0x40)) { wr(st + 0xCu, hp0); g_ram[(st + 0x14u) & 0x07FFFFFFu] = 0; g_ram[(st + 0x15u) & 0x07FFFFFFu] = 0; }
                target = a; done = true;
                std::fprintf(stderr, "[TAGTEAM] t=%.1fs KO prepared on %s %08X (hp 1, brv 0)\n", dissidia_battle_age() / 60.0f, who, a);
            }
        }
        if (target && owner == target) { const uint32_t st2 = rd(target + 0x52Cu); const int hp = ok(st2, 0x40) ? static_cast<int32_t>(rd(st2 + 0xCu)) : -1; if (hp != lasthp) { std::fprintf(stderr, "[TAGTEAM] t=%.1fs KO target %08X live hp %d (max %u) script %d flags %08X\n", dissidia_battle_age() / 60.0f, target, hp, rd(target + 0x10B0u), static_cast<int16_t>(rd(target + 0x180u) & 0xFFFF), rd(target + 0x380u)); lasthp = hp; } }
    }
    static bool intro_seen = false;
    if (!g_late_q.empty() && g_bs && rd(g_bs + 72) != 0 && !g_late_ready) {   // the intro scripts (ids >= 1000) have run and the human is idle again
        const int cur = static_cast<int16_t>(rd(g_late_q[0].slot_actor + 0x180u) & 0xFFFFu);
        if (cur >= 1000) intro_seen = true;
        else if (intro_seen && (cur == 1 || cur == 2)) g_late_ready = true;
        if (!g_late_wait) g_late_ready = true;
    }
    if (!g_late_q.empty() && g_late_ready) {   // the intro is over: make the late extras now
        for (const Late& l : g_late_q) {
            Extra& e = g_extra[g_nextra];
            e.actor = spawn_fighter(g_bs, l.side, l.foe, l.cpu_side, l.info_slot, l.slot_actor); e.side = l.side; e.foe = l.foe; e.slot_actor = l.slot_actor; e.started = false;
            if (e.actor) { e.undo = g_last_undo; g_nextra++; }
        }
        g_late_q.clear(); intro_seen = false;
    }
    {   // start the extras once all of them are loaded; the registrations are taken back in reverse spawn order
        bool pending = false, all_ready = true;
        for (int i = 0; i < g_nextra; i++) if (!g_extra[i].started) { pending = true; if (!call(FIGHTER_READY, g_extra[i].actor)) all_ready = false; }
        if (pending && all_ready && g_bs && rd(g_bs + 72) != 0) {
            for (int i = g_nextra - 1; i >= 0; i--) if (!g_extra[i].started) apply_undo(g_extra[i]);
            for (int i = 0; i < g_nextra; i++) if (!g_extra[i].started) start_extra(g_extra[i]);
        }
    }
    if (Extra* e = extra_of(owner); e && e->started) {
        if (rd(owner + 0x2FCu) == 0 && g_orig_refresh) { const recomp_context sv = *g_ctx; g_ctx->r[4] = static_cast<int32_t>(owner); g_orig_refresh(g_ram, g_ctx); *g_ctx = sv; }
        wr(owner + 0x380u, rd(owner + 0x380u) & ~2u);   // bit 2 of +0x380 makes the CPU brain skip the actor; the retail two lose it after the intro
    }
    static uint32_t seen[8] = {}; static int cnt[8] = {}; static int n = 0;
    int i = 0; while (i < n && seen[i] != brain) i++;
    if (i == n && n < 8) { seen[n++] = brain; }
    if (g_log && i < 8 && (cnt[i]++ % 300) == 0 && ok(brain, 0x40))
        std::fprintf(stderr, "[TAGTEAM] brain tick %08X (owner %08X) enabled=%u state=%u call#%d tgt2FC=%08X foe300=%08X partner1108=%08X lockT=%08X act380=%08X%s\n", brain, owner, g_ram[(brain) & 0x07FFFFFFu], g_ram[(brain + 12) & 0x07FFFFFFu], cnt[i], rd(owner + 0x2FCu), rd(owner + 0x300u), rd(owner + 0x1108u), rd(rd(owner + 0x5ACu) + 252u), rd(owner + 0x380u), extra_of(owner) ? " <- EXTRA" : "");
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_STATUS") && i < 8 && (cnt[i] % 300) == 2) {
        const uint32_t st = rd(owner + 0x52Cu);
        std::fprintf(stderr, "[TAGTEAM] status %08X owner %08X%s hp10B0=%u:", st, owner, extra_of(owner) ? " EXTRA" : "", rd(owner + 0x10B0u));
        if (ok(st, 0x80)) for (uint32_t o = 0; o < 0x60; o += 4) std::fprintf(stderr, " %08X", rd(st + o));
        std::fprintf(stderr, "\n");
    }
    if (g_log && std::getenv("PSPRECOMP_TAGTEAM_LOCKDUMP") && i < 8 && cnt[i] == 302 && g_bs) {
        for (int side = 0; side < 2; side++) {
            const uint32_t act = rd(g_bs + 56u + 4u * side), L = rd(act + 0x5ACu);
            if (!ok(L, 0x200)) continue;
            std::fprintf(stderr, "[TAGTEAM] lock block %08X of %08X (slot %d): owner=%08X target=%08X mode=%u prev=%u idx=%u idx2=%u\n", L, act, side, rd(L + 248), rd(L + 252), rd(L + 264), rd(L + 268), rd(L + 272), rd(L + 276));
            for (uint32_t o = 240; o < 400; o += 4) { const uint32_t v = rd(L + o); if (v) std::fprintf(stderr, "[TAGTEAM]   +%u = %08X\n", o, v); }
            for (uint32_t k = 0; k < 6; k++) {
                const uint32_t h = rd(L + 300 + 4 * k);
                if (!ok(h, 16)) continue;
                const uint32_t vt = rd(h), e = vt + 112;
                std::fprintf(stderr, "[TAGTEAM]   handler[%u] %08X vtable %08X getter(+112) off=%d fn=%08X  +8: fn=%08X +16: fn=%08X\n", k, h, vt, static_cast<int16_t>(rd(e) & 0xFFFF), rd(e + 4), rd(vt + 8 + 4), rd(vt + 16 + 4));
            }
        }
    }
    g_orig_tick(rdram, ctx);
}

}  // namespace

// Called by the scene tick (hooks_main.cpp) before every frame's update: between frames no fighter is mid-update, the safe point for the swap.
void dissidia_tagteam_frame(uint8_t* rdram, recomp_context* ctx) {
    if (!g_swap_req) return;
    g_swap_req = false; g_ram = rdram; g_ctx = ctx;
    swap_roles();
}

// Pad filter (framerate.cpp calls it with the buttons of every poll): a rising edge of the swap button becomes a swap request and is
// taken out of the input.
uint32_t dissidia_tagteam_pad(uint32_t raw) {
    static uint32_t prev = 0;
    const uint32_t edge = raw & ~prev & g_swap_mask;
    prev = raw;
    if (g_nextra == 0 || g_swap_mask == 0) return raw;
    if (edge) g_swap_req = true;
    return raw & ~g_swap_mask;
}

void dissidia_tagteam_install(uint8_t* rdram) {
    const char* e = std::getenv("PSPRECOMP_TAGTEAM");
    g_enabled = e && e[0] && e[0] != '0';
    g_log = std::getenv("PSPRECOMP_TAGTEAM_LOG") != nullptr;
    if (const char* m = std::getenv("PSPRECOMP_TAGTEAM_EXTRAS")) g_mask = std::atoi(m);
    { const char* l = std::getenv("PSPRECOMP_TAGTEAM_LATE"); g_late = !(l && l[0] == '0'); g_late_wait = !(l && l[0] == '1'); }   // default: spawn after the intro; LATE=1 right when the battle starts, LATE=0 during the intro setup
    g_hud_on = !(std::getenv("PSPRECOMP_TAGTEAM_HUD") && std::getenv("PSPRECOMP_TAGTEAM_HUD")[0] == '0');
    if (const char* b = std::getenv("PSPRECOMP_TAGTEAM_SWAP_BUTTON")) g_swap_mask = static_cast<uint32_t>(std::strtoul(b, nullptr, 16));
    if (const char* m = std::getenv("PSPRECOMP_TAGTEAM_STOP")) g_stop = std::atoi(m);
    g_ram = rdram;
    g_orig_alloc = psp_dispatch_probe_lookup(ALLOC_CORE);
    if (g_orig_alloc) psp_dispatch_register(ALLOC_CORE, alloc_hook);
    g_orig_setup = psp_dispatch_probe_lookup(BATTLE_SETUP);
    if (!g_orig_setup) { std::fprintf(stderr, "[TAGTEAM] setup function not found\n"); return; }
    g_orig_flow = psp_dispatch_probe_lookup(FLOW_STEP);
    if (!g_orig_flow) { std::fprintf(stderr, "[TAGTEAM] flow step not found\n"); return; }
    psp_dispatch_register(BATTLE_SETUP, setup_hook);
    psp_dispatch_register(FLOW_STEP, flow_hook);
    g_orig_rel1 = psp_dispatch_probe_lookup(0x0886CAA0u); g_orig_rel2 = psp_dispatch_probe_lookup(0x0886A140u);
    if (g_orig_rel1 && g_orig_rel2) { psp_dispatch_register(0x0886CAA0u, rel1_hook); psp_dispatch_register(0x0886A140u, rel2_hook); }
    psp_settings_add_debug_toggle({"Tag Team (2v2)", "Tag team battle (experimental)\nOn: the next battle adds a partner for you and a second fighter to the CPU's team (2 v 2), L cycles the lock between the two enemies.\nAffects the next battle that starts.",
                                   +[]() { return g_enabled; }, +[](bool on) { g_enabled = on; }});
    std::fprintf(stderr, "[TAGTEAM] installed (%s)\n", g_enabled ? "on" : "off, Settings > Debug > Tag Team (2v2)");
}
