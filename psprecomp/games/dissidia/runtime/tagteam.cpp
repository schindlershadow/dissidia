// games/dissidia/runtime/tagteam.cpp - tag team battles (2 v 2). See docs/tag-team.md.
//
// Step 1 (spike): after the battle setup FUN_0888BA0C has built the two fighters, make a third one with the same
// constructor, cloning the CPU fighter's setup data (so no new assets are needed), and see whether the engine runs it.
//   PSPRECOMP_TAGTEAM=1        turn the spike on
//   PSPRECOMP_TAGTEAM_LOG=1    log what the hook does
#include "psp_game_module.h"
#include "psp_runtime.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);

namespace {

constexpr uint32_t BATTLE_SETUP = 0x0888BA0Cu;   // EBOOT: FUN_0888BA0C(bs), builds the fighters
constexpr uint32_t ALLOC = 0x08831320u;          // EBOOT: guest allocator (a0 = size)
constexpr uint32_t SIDE_REC = 0x088729A0u;       // EBOOT: per-side record (a0 = side)
constexpr uint32_t CHAR_INFO = 0x088470B8u;      // EBOOT: character info from a setup record (a0 = bs+0x7D8 / +0x7DC)
constexpr uint32_t FIGHTER_CTOR = 0x08B39E5Cu;   // battle overlay: fighter constructor (mem, siderec, info, side, 0)
constexpr uint32_t SET_FOE = 0x08836DA4u;        // EBOOT: a->+0x300 = b
constexpr uint32_t BRAIN_MODE = 0x08A64078u;     // battle overlay: brain (actor +0x5C4) mode (a1 = 2: CPU)
constexpr uint32_t BRAIN_LEVEL = 0x08A64100u;    // battle overlay: brain level (a1 = 15)
constexpr uint32_t ACT_BRAIN = 0x5C4u;
constexpr uint32_t REC_INIT = 0x08B01090u;       // battle overlay: fills the 40-byte init block (a0 = buf, a1 = side)
constexpr uint32_t LOCK_INIT = 0x08B3B1B8u;      // battle overlay: lock-on block init (a0 = *(actor + 0x5AC), a1 = 3 words)
constexpr uint32_t PLACE = 0x088359A8u;          // EBOOT: place an actor (a0 = actor, a1 = vec4, a2 = 1)
constexpr uint32_t REC_BASE = 0x09AFB8A0u;       // global per-side records (stride 0x1060), 0x09B00000 - 18272
constexpr uint32_t ACT_VT = 0x544u, ACT_FLAGS = 0x5B0u, ACT_LOCKBLK = 0x5ACu;

FuncPtr g_orig_setup = nullptr;
bool g_log = false;
uint8_t* g_ram = nullptr;
recomp_context* g_ctx = nullptr;

uint32_t rd(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
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

float rdf(uint32_t a) { float f; std::memcpy(&f, g_ram + (a & 0x07FFFFFFu), 4); return f; }
void wr(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &v, 4); }
void wrf(uint32_t a, float f) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &f, 4); }

uint32_t call_ptr(FuncPtr f, uint32_t a0, uint32_t a1, uint32_t a2) {
    const recomp_context saved = *g_ctx;
    g_ctx->r[4] = static_cast<int32_t>(a0); g_ctx->r[5] = static_cast<int32_t>(a1); g_ctx->r[6] = static_cast<int32_t>(a2);
    f(g_ram, g_ctx);
    const uint32_t v = static_cast<uint32_t>(g_ctx->r[2]);
    *g_ctx = saved;
    return v;
}

void setup_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t bs = static_cast<uint32_t>(ctx->r[4]);
    g_orig_setup(rdram, ctx);
    g_ram = rdram; g_ctx = ctx;
    if (!ok(bs, 0x2800)) return;
    const uint32_t a0 = rd(bs + 56), a1 = rd(bs + 60), mode = rd(bs + 76), human = rd(bs + 84);
    if (g_log) std::fprintf(stderr, "[TAGTEAM] setup bs=%08X mode=%u slots %08X %08X human slot %u\n", bs, mode, a0, a1, human);
    if (!ok(a0, 0x12D0) || !ok(a1, 0x12D0)) return;

    // Scratch space below the stack pointer, kept clear of the callees' own frames.
    const uint32_t sp0 = static_cast<uint32_t>(ctx->r[29]);
    ctx->r[29] = static_cast<int32_t>(sp0 - 0x200u);
    const uint32_t buf = sp0 - 0x1C0u, lockargs = sp0 - 0x180u, vec = sp0 - 0x160u;

    // The third fighter: the CPU slot's setup data again (slot 1, side 1), the same steps as the setup's own.
    const uint32_t mem = call(ALLOC, 0x12D0);
    if (!ok(mem, 0x12D0)) { std::fprintf(stderr, "[TAGTEAM] alloc failed\n"); ctx->r[29] = static_cast<int32_t>(sp0); return; }
    const uint32_t rec = call(SIDE_REC, 1);
    const uint32_t info = call(CHAR_INFO, rd(bs + 2012));
    call(FIGHTER_CTOR, mem, rec, info, 1, 0);
    call(SET_FOE, mem, a0);                                   // his foe: the fighter in slot 0

    // Per-fighter init (the second loop of the setup, side 1): the actor's own init through its vtable, flags, lock block.
    call(REC_INIT, buf, 1);
    const uint32_t vt = rd(mem + 1348) + 256u;
    const uint32_t voff = static_cast<uint32_t>(static_cast<int16_t>(rd(vt) & 0xFFFFu)), vfn = rd(vt + 4);
    if (FuncPtr f = fn(vfn)) call_ptr(f, mem + voff, REC_BASE + 4192u, buf);
    if (rd(REC_BASE + 10208u + 4u) & 1u) g_ram[(mem + 1456u + 7u) & 0x07FFFFFFu] |= 0x10;
    if (rd(REC_BASE + 10216u + 2u) & 0x1u) g_ram[(mem + 1456u + 8u) & 0x07FFFFFFu] |= 0x20;
    {
        const uint32_t w = rd(REC_BASE + 8384u + 4u) & 0xFFFFu;
        wr(lockargs, static_cast<uint32_t>(static_cast<int8_t>(g_ram[(REC_BASE + 8384u + 4u + 2u) & 0x07FFFFFFu])));
        g_ram[(lockargs + 4u) & 0x07FFFFFFu] = (w & 4u) ? 1 : 0;
        g_ram[(lockargs + 5u) & 0x07FFFFFFu] = (w & 0x10u) ? 1 : 0;
        // (the words at lockargs are laid out as the setup does: +0 word, +4 byte, +5 byte)
        call(LOCK_INIT, rd(mem + ACT_LOCKBLK), lockargs);
    }
    // Stand next to the CPU fighter.
    wrf(vec, rdf(a1 + 0x80u) + 3.0f); wrf(vec + 4, rdf(a1 + 0x84u)); wrf(vec + 8, rdf(a1 + 0x88u)); wrf(vec + 12, 1.0f);
    call(PLACE, mem, vec, 1);
    const uint32_t brain = rd(mem + ACT_BRAIN);
    if (g_log) std::fprintf(stderr, "[TAGTEAM] third fighter %08X (rec %08X info %08X brain %08X)\n", mem, rec, info, brain);
    if (ok(brain, 0x40)) { call(BRAIN_MODE, brain, 2); call(BRAIN_LEVEL, brain, 15); }
    ctx->r[29] = static_cast<int32_t>(sp0);
}

}  // namespace

void dissidia_tagteam_install(uint8_t* rdram) {
    const char* e = std::getenv("PSPRECOMP_TAGTEAM");
    if (!e || e[0] == '0') return;
    g_log = std::getenv("PSPRECOMP_TAGTEAM_LOG") != nullptr;
    g_ram = rdram;
    g_orig_setup = psp_dispatch_probe_lookup(BATTLE_SETUP);
    if (!g_orig_setup) { std::fprintf(stderr, "[TAGTEAM] setup function not found\n"); return; }
    psp_dispatch_register(BATTLE_SETUP, setup_hook);
    std::fprintf(stderr, "[TAGTEAM] spike installed\n");
}
