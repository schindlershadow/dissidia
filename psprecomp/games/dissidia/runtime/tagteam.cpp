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
#include "hle/psp_hle.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
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
constexpr uint32_t START_ACTION = 0x0882FA10u;   // EBOOT: start an action (a0 = actor + 304, a1 = id, a2 = 0, f12 = f13 = 0)
constexpr uint32_t REC_INIT = 0x08B01090u;       // battle overlay: fills the 40-byte init block (a0 = buf, a1 = side)
constexpr uint32_t LOCK_INIT = 0x08B3B1B8u;      // battle overlay: lock-on block init (a0 = *(actor + 0x5AC), a1 = 3 words)
constexpr uint32_t PLACE = 0x088359A8u;          // EBOOT: place an actor (a0 = actor, a1 = vec4, a2 = 1)
constexpr uint32_t REC_BASE = 0x09AFB8A0u;       // global per-side records (stride 0x1060), 0x09B00000 - 18272
constexpr uint32_t ACT_VT = 0x544u, ACT_FLAGS = 0x5B0u, ACT_LOCKBLK = 0x5ACu;

FuncPtr g_orig_setup = nullptr, g_orig_flow = nullptr, g_orig_tick = nullptr;
bool g_log = false;
uint8_t* g_ram = nullptr;
recomp_context* g_ctx = nullptr;

// The extra fighters of the battle being set up. side 1 = the CPU's team (the "third"), side 0 = the human's (the partner).
struct Extra { uint32_t actor = 0, side = 0, foe = 0, slot_actor = 0; bool started = false; };
Extra g_extra[2];
int g_nextra = 0;
uint32_t g_bs = 0;
int g_mask = 3;
int g_stop = 99;   // PSPRECOMP_TAGTEAM_STOP=n: debugging, spawn steps up to n only (1 alloc, 2 ctor, 3 foe, 4 init, 5 brain, 6 start/place)
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
uint32_t spawn_fighter(uint32_t bs, uint32_t side, uint32_t foe, uint32_t cpu_side, uint32_t info_slot) {
    const uint32_t sp0 = static_cast<uint32_t>(g_ctx->r[29]);
    g_ctx->r[29] = static_cast<int32_t>(sp0 - 0x200u);   // scratch below the stack pointer, clear of the callees' frames
    const uint32_t buf = sp0 - 0x1C0u, lockargs = sp0 - 0x180u;
    uint32_t mem = call(ALLOC, 0x12D0);
    if (!ok(mem, 0x12D0)) { std::fprintf(stderr, "[TAGTEAM] alloc failed\n"); g_ctx->r[29] = static_cast<int32_t>(sp0); return 0; }
    if (g_stop < 2) { g_ctx->r[29] = static_cast<int32_t>(sp0); return mem; }
    const uint32_t rec = call(SIDE_REC, side);
    uint32_t before[24]; if (g_log) for (int k = 0; k < 24; k++) before[k] = rd(rec + 4u * k);
    const uint32_t info = call(CHAR_INFO, rd(bs + 2008u + 4u * info_slot));
    call(FIGHTER_CTOR, mem, rec, info, side, 0);
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
    if (!g_orig_tick) { g_orig_tick = fn(BRAIN_TICK); if (g_orig_tick) psp_dispatch_register(BRAIN_TICK, tick_hook); else std::fprintf(stderr, "[TAGTEAM] brain tick not found\n"); }
    const uint32_t hum = human == 0 ? a0 : a1, cpu = human == 0 ? a1 : a0;
    if (const char* w = std::getenv("PSPRECOMP_TAGTEAM_WATCH")) {   // arm the write watch on <human actor> + hex offset
        const uint32_t off = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
        psp_mem_watch_set(hum + off);
        std::fprintf(stderr, "[TAGTEAM] watching %08X\n", hum + off);
    }
    // Slot 0 is side 0 and slot 1 side 1 in the setup, whoever the human is; the CPU brain settings are the CPU slot's.
    const uint32_t cpu_side = human == 0 ? 1 : 0, hum_side = human;
    if (g_mask & 1) {   // the enemy team's second fighter: a copy of the CPU fighter, foe = the human
        Extra& e = g_extra[g_nextra];
        e.actor = spawn_fighter(bs, cpu_side, hum, cpu_side, cpu_side); e.side = cpu_side; e.foe = hum; e.slot_actor = cpu; e.started = false;
        if (e.actor) g_nextra++;
    }
    if (g_mask & 2) {   // the human's partner: a copy of the human fighter on the CPU brain, foe = the CPU fighter
        const uint32_t P = rd(0x089D6A10u);
        Snap sb, sp, sg;
        if (g_log) { sb = take(bs, 0x2800); if (ok(P, 0x400)) sp = take(P, 0x400); sg = take(0x089D6000u, 0x1000); }
        if (g_late) { g_late_p = {true, hum_side, cpu, cpu_side, std::getenv("PSPRECOMP_TAGTEAM_PARTNER_CPU_CHAR") ? cpu_side : hum_side, hum}; g_bs = bs; goto after_partner; }
        Extra& e = g_extra[g_nextra];
        e.actor = spawn_fighter(bs, hum_side, cpu, cpu_side, std::getenv("PSPRECOMP_TAGTEAM_PARTNER_CPU_CHAR") ? cpu_side : hum_side);
        if (g_log) { diff("bs", sb); if (!sp.w.empty()) diff("P", sp); diff("glob", sg); } e.side = hum_side; e.foe = cpu; e.slot_actor = hum; e.started = false;
        if (e.actor) g_nextra++;
    }
    after_partner:
    if (g_nextra) g_bs = bs;
}

void start_extra(Extra& e) {
    if (g_stop < 6) { e.started = true; return; }
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
    g_orig_flow(rdram, ctx);
    if (rd(bs + 72) == 0) return;                            // the others were not ready yet
    for (int i = 0; i < g_nextra; i++) if (!g_extra[i].started) start_extra(g_extra[i]);
}

Extra* extra_of(uint32_t actor) {
    for (int i = 0; i < g_nextra; i++) if (g_extra[i].actor == actor) return &g_extra[i];
    return nullptr;
}

void tick_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t brain = static_cast<uint32_t>(ctx->r[4]);
    g_ram = rdram; g_ctx = ctx;
    const uint32_t owner = rd(brain + 4);
    if (g_late_p.pending && g_bs && rd(g_bs + 72) != 0) {   // the battle has started: make the late partner now
        g_late_p.pending = false;
        Extra& e = g_extra[g_nextra];
        e.actor = spawn_fighter(g_bs, g_late_p.side, g_late_p.foe, g_late_p.cpu_side, g_late_p.info_slot); e.side = g_late_p.side; e.foe = g_late_p.foe; e.slot_actor = g_late_p.slot_actor; e.started = false;
        if (e.actor) g_nextra++;
    }
    for (int i = 0; i < g_nextra; i++) if (!g_extra[i].started && g_bs && rd(g_bs + 72) != 0 && call(FIGHTER_READY, g_extra[i].actor)) start_extra(g_extra[i]);
    if (Extra* e = extra_of(owner); e && e->started) {
        if (rd(owner + 0x2FCu) == 0) call(TARGET_REFRESH, owner);
        wr(owner + 0x380u, rd(owner + 0x380u) & ~2u);   // bit 2 of +0x380 makes the CPU brain skip the actor; the retail two lose it after the intro
    }
    static uint32_t seen[8] = {}; static int cnt[8] = {}; static int n = 0;
    int i = 0; while (i < n && seen[i] != brain) i++;
    if (i == n && n < 8) { seen[n++] = brain; }
    if (g_log && i < 8 && (cnt[i]++ % 300) == 0 && ok(brain, 0x40))
        std::fprintf(stderr, "[TAGTEAM] brain tick %08X (owner %08X) enabled=%u state=%u call#%d tgt2FC=%08X foe300=%08X partner1108=%08X lockT=%08X act380=%08X%s\n", brain, owner, g_ram[(brain) & 0x07FFFFFFu], g_ram[(brain + 12) & 0x07FFFFFFu], cnt[i], rd(owner + 0x2FCu), rd(owner + 0x300u), rd(owner + 0x1108u), rd(rd(owner + 0x5ACu) + 252u), rd(owner + 0x380u), extra_of(owner) ? " <- EXTRA" : "");
    g_orig_tick(rdram, ctx);
}

}  // namespace

void dissidia_tagteam_install(uint8_t* rdram) {
    const char* e = std::getenv("PSPRECOMP_TAGTEAM");
    if (!e || e[0] == '0') return;
    g_log = std::getenv("PSPRECOMP_TAGTEAM_LOG") != nullptr;
    if (const char* m = std::getenv("PSPRECOMP_TAGTEAM_EXTRAS")) g_mask = std::atoi(m);
    g_late = std::getenv("PSPRECOMP_TAGTEAM_LATE") != nullptr;
    if (const char* m = std::getenv("PSPRECOMP_TAGTEAM_STOP")) g_stop = std::atoi(m);
    g_ram = rdram;
    g_orig_setup = psp_dispatch_probe_lookup(BATTLE_SETUP);
    if (!g_orig_setup) { std::fprintf(stderr, "[TAGTEAM] setup function not found\n"); return; }
    g_orig_flow = psp_dispatch_probe_lookup(FLOW_STEP);
    if (!g_orig_flow) { std::fprintf(stderr, "[TAGTEAM] flow step not found\n"); return; }
    psp_dispatch_register(BATTLE_SETUP, setup_hook);
    psp_dispatch_register(FLOW_STEP, flow_hook);
    std::fprintf(stderr, "[TAGTEAM] spike installed\n");
}
