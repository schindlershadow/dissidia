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
#include "tagteam_hud.h"
#include "hle/psp_hle.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
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
constexpr uint32_t START_ACTION = 0x0882FA10u;   // EBOOT: start an action (a0 = actor + 304, a1 = id, a2 = 0, f12 = f13 = 0)
constexpr uint32_t REC_INIT = 0x08B01090u;       // battle overlay: fills the 40-byte init block (a0 = buf, a1 = side)
constexpr uint32_t LOCK_INIT = 0x08B3B1B8u;      // battle overlay: lock-on block init (a0 = *(actor + 0x5AC), a1 = 3 words)
constexpr uint32_t PLACE = 0x088359A8u;          // EBOOT: place an actor (a0 = actor, a1 = vec4, a2 = 1)
constexpr uint32_t REC_BASE = 0x09AFB8A0u;       // global per-side records (stride 0x1060), 0x09B00000 - 18272
constexpr uint32_t REG_PAIR0 = 0x08BAE9FCu, REG_PAIR1 = 0x08BAEA08u;   // ctor writes the new fighter here (see spawn_fighter)
constexpr uint32_t LAST_FIGHTER_OBJ = 0x089D4774u;   // EBOOT global the fighter constructor sets to its own controller-side object
constexpr uint32_t ACT_VT = 0x544u, ACT_FLAGS = 0x5B0u, ACT_LOCKBLK = 0x5ACu;

FuncPtr g_orig_setup = nullptr, g_orig_flow = nullptr, g_orig_tick = nullptr, g_orig_lock_other = nullptr, g_orig_refresh = nullptr, g_orig_rel1 = nullptr, g_orig_rel2 = nullptr;
bool g_in_ctor = false;   // inside the extra's constructor: the resource releases are skipped (see rel_hook)
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
struct Late { bool pending = false; uint32_t side = 0, foe = 0, cpu_side = 0, info_slot = 0, slot_actor = 0; } g_late_p;   // PSPRECOMP_TAGTEAM_EXTRAS: 1 = enemy side fighter, 2 = partner on the human's side

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
uint32_t spawn_fighter(uint32_t bs, uint32_t side, uint32_t foe, uint32_t cpu_side, uint32_t info_slot, uint32_t slot_actor) {
    const uint32_t sp0 = static_cast<uint32_t>(g_ctx->r[29]);
    g_ctx->r[29] = static_cast<int32_t>(sp0 - 0x200u);   // scratch below the stack pointer, clear of the callees' frames
    const uint32_t buf = sp0 - 0x1C0u, lockargs = sp0 - 0x180u;
    uint32_t mem = call(ALLOC, 0x12D0);
    if (!ok(mem, 0x12D0)) { std::fprintf(stderr, "[TAGTEAM] alloc failed\n"); g_ctx->r[29] = static_cast<int32_t>(sp0); return 0; }
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
        undo.push_back({REG_PAIR0, t0}); undo.push_back({REG_PAIR1, t1});
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
    g_orig_setup(rdram, ctx);
    g_ram = rdram; g_ctx = ctx;
    g_nextra = 0; g_bs = 0;
    if (!ok(bs, 0x2800)) return;
    const uint32_t a0 = rd(bs + 56), a1 = rd(bs + 60), mode = rd(bs + 76), human = rd(bs + 84);
    if (g_log) std::fprintf(stderr, "[TAGTEAM] setup bs=%08X mode=%u slots %08X %08X human slot %u\n", bs, mode, a0, a1, human);
    if (!ok(a0, 0x12D0) || !ok(a1, 0x12D0)) return;
    if (!g_orig_lock_other) {
        g_orig_lock_other = fn(LOCK_OTHER); g_orig_refresh = fn(TARGET_REFRESH);
        if (g_orig_lock_other && g_orig_refresh) { psp_dispatch_register(LOCK_OTHER, lock_other_hook); psp_dispatch_register(TARGET_REFRESH, refresh_hook); }
        else std::fprintf(stderr, "[TAGTEAM] lock cycle hooks not installed\n");
    }
    if (!g_orig_tick) { g_orig_tick = fn(BRAIN_TICK); if (g_orig_tick) psp_dispatch_register(BRAIN_TICK, tick_hook); else std::fprintf(stderr, "[TAGTEAM] brain tick not found\n"); }
    const uint32_t hum = human == 0 ? a0 : a1, cpu = human == 0 ? a1 : a0;
    if (const char* w = std::getenv("PSPRECOMP_TAGTEAM_WATCH")) {   // arm the write watch on <human actor> + hex offset
        const uint32_t off = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
        psp_mem_watch_set(hum + off);
        std::fprintf(stderr, "[TAGTEAM] watching %08X\n", hum + off);
    }
    if (std::getenv("PSPRECOMP_TAGTEAM_WATCH_SEL")) {   // watch the human's lock cycle selector (retail mode handler [1] + 28)
        const uint32_t L = rd(hum + 0x5ACu), h1 = ok(L, 0x200) ? rd(L + 304u) : 0u;
        if (ok(h1, 0x40)) { psp_mem_watch_set(h1 + 28u); std::fprintf(stderr, "[TAGTEAM] watching the lock selector at %08X\n", h1 + 28u); }
    }
    // Slot 0 is side 0 and slot 1 side 1 in the setup, whoever the human is; the CPU brain settings are the CPU slot's.
    const uint32_t cpu_side = human == 0 ? 1 : 0, hum_side = human;
    if (g_mask & 1) {   // the enemy team's second fighter: a copy of the CPU fighter, foe = the human
        Extra& e = g_extra[g_nextra];
        e.actor = spawn_fighter(bs, cpu_side, hum, cpu_side, cpu_side, cpu); e.side = cpu_side; e.foe = hum; e.slot_actor = cpu; e.started = false;
        if (e.actor) { e.undo = g_last_undo; g_nextra++; }
    }
    if (g_mask & 2) {   // the human's partner: a copy of the human fighter on the CPU brain, foe = the CPU fighter
        const uint32_t P = rd(0x089D6A10u);
        Snap sb, sp, sg;
        if (g_log) { sb = take(bs, 0x2800); if (ok(P, 0x400)) sp = take(P, 0x400); sg = take(0x089D6000u, 0x1000); }
        if (g_late) { g_late_p = {true, hum_side, cpu, cpu_side, std::getenv("PSPRECOMP_TAGTEAM_PARTNER_CPU_CHAR") ? cpu_side : hum_side, hum}; g_bs = bs; goto after_partner; }
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
    e.started = true;
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
        f.hp = static_cast<int>(rd(e.actor + 0x10B0u)); e.hp_max = std::max(e.hp_max, f.hp); f.hp_max = std::max(e.hp_max, 1);
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

Extra* extra_of(uint32_t actor) {
    for (int i = 0; i < g_nextra; i++) if (g_extra[i].actor == actor) return &g_extra[i];
    return nullptr;
}

void tick_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t brain = static_cast<uint32_t>(ctx->r[4]);
    g_ram = rdram; g_ctx = ctx;
    const uint32_t owner = rd(brain + 4);
    if (g_nextra && owner == g_extra[0].actor && g_extra[0].started) hud_update();
    if (g_log && g_bs) {   // the human's lock cycle selector (retail mode handler [1], +28) and its target
        static int last_sel = -99; static uint32_t last_t = 0, last_mode = 99;
        const uint32_t hum = rd(g_bs + 56u + 4u * rd(g_bs + 84u)), L = ok(hum, 0x12D0) ? rd(hum + 0x5ACu) : 0u;
        if (ok(L, 0x200) && owner == rd(hum + 0x5C4u + 0u) + 0u) {}
        if (ok(L, 0x200)) {
            const uint32_t h1 = rd(L + 304u);
            const int sel = ok(h1, 0x40) ? static_cast<int>(rd(h1 + 28u)) : -99; const uint32_t t = rd(L + 252u);
            if (sel != last_sel || t != last_t || rd(L + 264u) != last_mode) { last_mode = rd(L + 264u); std::fprintf(stderr, "[TAGTEAM] human lock: sel %d -> %d, mode %u, target %08X -> %08X (+0x2FC %08X foe %08X)\n", last_sel, sel, rd(L + 264u), last_t, t, rd(hum + 0x2FCu), rd(hum + 0x300u)); last_sel = sel; last_t = t; }
        }
    }
    static bool intro_seen = false, intro_done = false;
    if (g_late_p.pending && g_bs && rd(g_bs + 72) != 0 && !intro_done) {   // the intro scripts (ids >= 1000) have run and the human is idle again
        const int cur = static_cast<int16_t>(rd(g_late_p.slot_actor + 0x180u) & 0xFFFFu);
        if (cur >= 1000) intro_seen = true;
        else if (intro_seen && (cur == 1 || cur == 2)) intro_done = true;
    }
    if (g_late_p.pending && g_bs && rd(g_bs + 72) != 0 && (g_late_wait ? intro_done : true)) {   // the battle has started: make the late partner now
        g_late_p.pending = false;
        Extra& e = g_extra[g_nextra];
        e.actor = spawn_fighter(g_bs, g_late_p.side, g_late_p.foe, g_late_p.cpu_side, g_late_p.info_slot, g_late_p.slot_actor); e.side = g_late_p.side; e.foe = g_late_p.foe; e.slot_actor = g_late_p.slot_actor; e.started = false;
        if (e.actor) { e.undo = g_last_undo; g_nextra++; }
    }
    for (int i = 0; i < g_nextra; i++) if (!g_extra[i].started && g_bs && rd(g_bs + 72) != 0 && call(FIGHTER_READY, g_extra[i].actor)) start_extra(g_extra[i]);
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

void dissidia_tagteam_install(uint8_t* rdram) {
    const char* e = std::getenv("PSPRECOMP_TAGTEAM");
    if (!e || e[0] == '0') return;
    g_log = std::getenv("PSPRECOMP_TAGTEAM_LOG") != nullptr;
    if (const char* m = std::getenv("PSPRECOMP_TAGTEAM_EXTRAS")) g_mask = std::atoi(m);
    g_late = std::getenv("PSPRECOMP_TAGTEAM_LATE") != nullptr;
    g_hud_on = !(std::getenv("PSPRECOMP_TAGTEAM_HUD") && std::getenv("PSPRECOMP_TAGTEAM_HUD")[0] == '0');
    g_late_wait = g_late && std::atoi(std::getenv("PSPRECOMP_TAGTEAM_LATE")) >= 2;
    if (const char* m = std::getenv("PSPRECOMP_TAGTEAM_STOP")) g_stop = std::atoi(m);
    g_ram = rdram;
    g_orig_setup = psp_dispatch_probe_lookup(BATTLE_SETUP);
    if (!g_orig_setup) { std::fprintf(stderr, "[TAGTEAM] setup function not found\n"); return; }
    g_orig_flow = psp_dispatch_probe_lookup(FLOW_STEP);
    if (!g_orig_flow) { std::fprintf(stderr, "[TAGTEAM] flow step not found\n"); return; }
    psp_dispatch_register(BATTLE_SETUP, setup_hook);
    psp_dispatch_register(FLOW_STEP, flow_hook);
    g_orig_rel1 = psp_dispatch_probe_lookup(0x0886CAA0u); g_orig_rel2 = psp_dispatch_probe_lookup(0x0886A140u);
    if (g_orig_rel1 && g_orig_rel2) { psp_dispatch_register(0x0886CAA0u, rel1_hook); psp_dispatch_register(0x0886A140u, rel2_hook); }
    std::fprintf(stderr, "[TAGTEAM] spike installed\n");
}
