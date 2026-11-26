// games/dissidia/runtime/training.cpp - Training mode (Battle Mode > Training), a standalone mode that has nothing to do with the tag team code.
//
// Menu: menu_settings.cpp adds a "Training" row under the Battle Mode list; confirming it sets dissidia_training_set(true) and lets the game start
// Quick Battle (1-on-1): the retail Player Select (LOOK / SET), opponent and stage pages and the retail 1v1 battle. Everything here is the layer on
// top of that battle, active only while the flag is set, the battle overlay runs, no tag team battle is on and the session is not online:
//   - no KO end: a lethal HP hit is capped at HP - 1 (the HP step FUN_08AEFF40 runs with extra HP, as the tag code's KO hold did), a fighter whose HP is
//     nearly gone is refilled; "Infinite" keeps the HP full;
//   - the options panel (Select while the retail pause menu is up) sets CPU action (normal AI / stand / guard), HP, Bravery, EX and assist
//     gauges, summon, last-hit overlay and "reset positions"; the pause menu's own rows (Retry / Leave ...) are the way out;
//   - the overlay at the top left shows the last Bravery hit and the last HP hit.
// PSPRECOMP_TRAINING=1 (+ PSPRECOMP_TRAINING_OPTS="v0,v1,...", option ids as in kRows) turns it on without the menu (test aid); PSPRECOMP_TRAINING_OFF=1: no
// Training row, never active.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "menu_layer.h"
#include "psp_memory.h"
#include "psp_runtime.h"
#include "recomp.h"
#include "hle/psp_hle.h"

extern FuncPtr dissidia_ovl_battle_lookup(uint32_t vaddr);
extern FuncPtr dissidia_ovl_menu_lookup(uint32_t vaddr);
bool dissidia_tagteam_menu_mode_on();   // tagteam_menu.cpp: a tag team battle was chosen in the menus
bool psp_adhoc_in_session();
void dissidia_menu_style_list(const std::string& title, const std::string& sub, const std::vector<std::pair<std::string, std::string>>& rows, int sel, const std::string& help, const std::string& footer);   // tagteam_menu.cpp (the menu style)
void dissidia_menu_style_clear();
void dissidia_menu_play_se(uint8_t* rdram, recomp_context* ctx, int id);   // tagteam_menu.cpp

namespace {

using dissidia::LayerColor;
uint8_t* g_ram = nullptr; recomp_context* g_ctx = nullptr;
uint32_t rd(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
uint32_t rdh(uint32_t a) { return static_cast<uint32_t>(g_ram[a & 0x07FFFFFFu]) | (static_cast<uint32_t>(g_ram[(a + 1) & 0x07FFFFFFu]) << 8); }
void wr(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &v, 4); }
float rdf(uint32_t a) { float v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
void wrf(uint32_t a, float v) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &v, 4); }
bool ok(uint32_t a, uint32_t n) { return psp_guest_user_ok(a, n); }

constexpr uint32_t HP_STEP = 0x08AEFF40u;     // battle overlay: the HP damage step (a1 = message: +0 the victim, +4 -> (+0 the attacker))
constexpr uint32_t PAUSE_OPEN = 0x08B1DE34u;  // battle overlay: opens the pause list
constexpr uint32_t PLACE = 0x088359A8u;       // EBOOT: place an actor (a0 = actor, a1 = vec4, a2 = 1)
constexpr uint32_t OVL_BASE = 0x089DD4C0u;    // (its word = the resident overlay id: 1 = battle, 2 = menu)
constexpr uint32_t BTN_SELECT = 0x1u, BTN_UP = 0x10u, BTN_RIGHT = 0x20u, BTN_DOWN = 0x40u, BTN_LEFT = 0x80u, BTN_CIRCLE = 0x2000u, BTN_CROSS = 0x4000u, BTN_R = 0x200u;

enum Opt { O_CPU, O_HP_YOU, O_HP_CPU, O_BRV_YOU, O_BRV_CPU, O_EX, O_ASSIST, O_SUMMON, O_INFO, O_COUNT };
struct Row { const char* label; int n; const char* vals[5]; const char* help; };
const Row kRows[] = {
    {"CPU ACTION", 3, {"Normal AI", "Stand", "Guard"}, "What the opponent does: his normal AI, stand still, or hold Guard. (CPU strength and level: set on the opponent page)"},
    {"YOUR HP", 2, {"Refill at 0", "Infinite"}, "Your HP is refilled when it would reach 0 (no KO), or never drops"},
    {"CPU HP", 2, {"Refill at 0", "Infinite"}, "The opponent's HP is refilled when it would reach 0 (no KO), or never drops"},
    {"YOUR BRV", 5, {"Normal", "Base value", "0", "3000", "9999"}, "Your Bravery: normal, held at your base value, or fixed"},
    {"CPU BRV", 5, {"Normal", "Base value", "0", "3000", "9999"}, "The opponent's Bravery: normal, held at his base value, or fixed (0 = always broken)"},
    {"EX GAUGE", 2, {"Normal", "Always full"}, "Your EX gauge is kept full (EX Mode and EX Burst any time)"},
    {"ASSIST GAUGE", 2, {"Normal", "Always full"}, "Your assist gauge is kept full: test assist calls one after the other"},
    {"SUMMON", 2, {"Normal", "Always ready"}, "Your summon is ready again after each use"},
    {"HIT INFO", 2, {"Off", "On"}, "Shows the last hit's Bravery and HP damage (who hit whom) at the top left"},
};
constexpr int kRowCount = static_cast<int>(sizeof kRows / sizeof kRows[0]) + 1;   // + RESET POSITIONS

bool g_chosen = false, g_env_done = false;
int g_opt[O_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0, 1};
std::atomic<uint32_t> g_raw{0};
std::atomic<bool> g_panel{false}, g_battle_seen{false};
std::atomic<int> g_inject_cross{0};   // after choosing the row: one Cross for the retail list (the confirm that chose it was consumed)
int g_row = 0; uint32_t g_prev_pad = 0; bool g_panel_clear = false;
bool g_paused = false, g_hooked = false, g_reset_req = false; uint32_t g_pc = 0;
uint32_t g_gen = 0; bool g_spots_ok = false; float g_spot[2][3] = {};
uint32_t g_seen_pair[2] = {0, 0};
int g_brv_prev[2] = {-1, -1}; int g_refills = 0;
struct Hit { std::string who; int n = 0; bool have = false; } g_hit_brv, g_hit_hp;
FuncPtr g_prev_hp = nullptr, g_prev_pause = nullptr; FuncPtr g_orig_hp = nullptr, g_orig_pause = nullptr;

bool env_on() {
    if (!g_env_done) { g_env_done = true;
        if (std::getenv("PSPRECOMP_TRAINING")) { g_chosen = true; if (const char* o = std::getenv("PSPRECOMP_TRAINING_OPTS")) { const char* q = o; for (int i = 0; i < O_COUNT && *q; i++) { g_opt[i] = std::atoi(q); const char* c = std::strchr(q, ','); if (!c) break; q = c + 1; } } } }
    return g_chosen;
}
bool battle_overlay() { return g_ram && rd(OVL_BASE) == 1; }
bool active() {
    static const bool off = std::getenv("PSPRECOMP_TRAINING_OFF") != nullptr;
    return !off && env_on() && g_ram && battle_overlay() && !dissidia_tagteam_menu_mode_on() && !psp_adhoc_in_session();
}
bool is_cpu(uint32_t a) { const uint32_t br = rd(a + 0x5C4u); return ok(br, 0x60) && static_cast<int32_t>(rd(br + 0x54u)) == 0; }
int cpu_index(const uint32_t f[4]) { for (int i = 0; i < 4; i++) if (f[i] && ok(f[i], 0x12D0) && is_cpu(f[i])) return i; return -1; }
int hp_eff(uint32_t a) { const uint32_t st = rd(a + 0x52Cu); if (!ok(st, 0x40)) return 0; const int32_t d = static_cast<int32_t>(rd(st + 0x8u)); return static_cast<int32_t>(rd(st + 0xCu)) - (d > 0 ? d : 0); }
void refill(uint32_t a) { const uint32_t st = rd(a + 0x52Cu); if (!ok(st, 0x40)) return; wr(st + 0xCu, rd(a + 0x10B0u)); wr(st + 0x8u, 0); wr(st + 0x20u, 0); wr(st + 0x0u, rd(st + 0x0u) & 0xFFFFu); }

// The HP damage step: nothing is lethal (the retail KO latch is inside this step); the damage is capped at HP - 1 and booked for the overlay.
void hp_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram; g_ctx = ctx;
    const uint32_t m = static_cast<uint32_t>(ctx->r[5]);
    const uint32_t a = ok(m, 8) ? rd(m) : 0u, st = ok(a, 0x12D0) ? rd(a + 0x52Cu) : 0u;
    if (!active() || !ok(st, 0x40)) { g_orig_hp(rdram, ctx); return; }
    const int hp0 = static_cast<int>(rd(st + 0xCu)), pd0 = static_cast<int>(rd(st + 0x8u));
    wr(st + 0xCu, static_cast<uint32_t>(hp0 + 1000000));
    g_orig_hp(rdram, ctx);
    wr(st + 0xCu, static_cast<uint32_t>(hp0));
    const int taken = static_cast<int>(rd(st + 0x8u));
    if (taken >= hp0) { const uint32_t keep = static_cast<uint32_t>(hp0 > 1 ? hp0 - 1 : 0); wr(st + 0x8u, keep); wr(st + 0x20u, keep); wr(st + 0x0u, (rd(st + 0x0u) & 0xFFFFu) | (keep << 16)); }
    const int dmg = std::min(taken, hp0) - std::max(pd0, 0);
    if (dmg > 0) { g_hit_hp.who = is_cpu(a) ? "YOU > CPU" : "CPU > YOU"; g_hit_hp.n = dmg; g_hit_hp.have = true; }
}
void pause_hook(uint8_t* rdram, recomp_context* ctx) {
    g_ram = rdram; g_ctx = ctx;
    if (active()) { g_paused = true; g_pc = static_cast<uint32_t>(ctx->r[4]); std::fprintf(stderr, "[TRAIN] pause opened (control %08X)\n", g_pc); }
    g_orig_pause(rdram, ctx);
}
// The hooks sit on battle overlay addresses (one hook per address; the tag team code installs its own when a tag battle starts): installed when a Training battle runs and taken
// out (the previous entry back) when it is over.
void hooks_install() {
    if (g_hooked) return;
    g_orig_hp = dissidia_ovl_battle_lookup(HP_STEP); g_orig_pause = dissidia_ovl_battle_lookup(PAUSE_OPEN);
    if (!g_orig_hp || !g_orig_pause) { std::fprintf(stderr, "[TRAIN] battle functions not found\n"); return; }
    g_prev_hp = psp_dispatch_probe_lookup(HP_STEP); g_prev_pause = psp_dispatch_probe_lookup(PAUSE_OPEN);
    psp_dispatch_register(HP_STEP, hp_hook); psp_dispatch_register(PAUSE_OPEN, pause_hook);
    g_hooked = true; std::fprintf(stderr, "[TRAIN] hooks installed\n");
}
void hooks_remove() {
    if (!g_hooked) return;
    if (g_prev_hp) psp_dispatch_register(HP_STEP, g_prev_hp); else psp_dispatch_register(HP_STEP, g_orig_hp);
    if (g_prev_pause) psp_dispatch_register(PAUSE_OPEN, g_prev_pause); else psp_dispatch_register(PAUSE_OPEN, g_orig_pause);
    g_hooked = false; std::fprintf(stderr, "[TRAIN] hooks removed\n");
}

void panel_rows(std::vector<std::pair<std::string, std::string>>& rows) {
    rows.clear();
    for (int i = 0; i < kRowCount - 1; i++) rows.push_back({kRows[i].label, kRows[i].vals[std::clamp(g_opt[i], 0, kRows[i].n - 1)]});
    rows.push_back({"RESET POSITIONS", ""});
}

}  // namespace

// ---- API ----
void dissidia_training_set(bool on) { g_chosen = on; g_env_done = true; if (on) std::fprintf(stderr, "[TRAIN] Training chosen\n"); }
bool dissidia_training_chosen() { return env_on(); }
void dissidia_training_inject_cross() { g_inject_cross.store(9, std::memory_order_relaxed); }

// The pad filter (framerate.cpp, every poll): the panel takes the pad while it is open; the row's confirm is replayed once as a fresh Cross for the retail list.
uint32_t dissidia_training_pad(uint32_t raw) {
    g_raw.store(raw, std::memory_order_relaxed);
    static int hold = 0;
    if (const int il = g_inject_cross.load(std::memory_order_relaxed); il > 0) { g_inject_cross.store(il - 1, std::memory_order_relaxed); raw = (il >= 3 && il <= 5) ? (raw | BTN_CROSS) : (raw & ~BTN_CROSS); }
    if (g_panel.load(std::memory_order_relaxed)) { hold = 12; return 0u; }
    if (hold > 0) { hold--; return 0u; }
    return raw;
}

// the CPU brain tick (framerate.cpp): true = skip it, the opponent thinks no more (Guard is started in the frame hook instead)
bool dissidia_training_ai_skip(uint32_t brain) {
    if (!g_ram || !g_battle_seen.load(std::memory_order_relaxed) || g_opt[O_CPU] == 0 || !active()) return false;
    const uint32_t owner = rd(brain + 4u);
    const bool skip = ok(owner, 0x12D0) && is_cpu(owner) && ok(brain, 0x60) && static_cast<int32_t>(rd(brain + 0x54u)) == 0;
    return skip;
}

// every frame end (framerate.cpp): f = the fighters of the running fight, idle = frames since a fighter last stepped (the pause menu), age = ticks the fight has run
void dissidia_training_frame_end(uint8_t* rdram, recomp_context* ctx, const uint32_t f[4], int idle, float age) {
    g_ram = rdram; g_ctx = ctx;
    if (!env_on() && !g_hooked) return;
    const bool act = active() && age > 0.f;
    if (!act && !(g_paused && active())) {
        if (g_hooked && !battle_overlay()) hooks_remove();
        if (g_panel.load()) { g_panel = false; }
        if (g_battle_seen.load() && !active()) { g_battle_seen = false; g_paused = false; g_spots_ok = false; }
        if (g_panel_clear) { g_panel_clear = false; dissidia_menu_style_clear(); }
        return;
    }
    if (!g_hooked) hooks_install();
    if (g_paused && (!ok(g_pc, 16) || rd(g_pc + 4u) == 0 || rd(g_pc + 12u) == 0)) { g_paused = false; std::fprintf(stderr, "[TRAIN] pause closed\n"); }   // the pause control: +4 open, +12 the owner (both 0 when it is closed)
    (void)idle;
    const int ci = cpu_index(f);
    if (ci < 0) return;
    int hi = -1; for (int i = 0; i < 4; i++) if (f[i] && f[i] != f[ci] && ok(f[i], 0x12D0)) { hi = i; break; }
    if (hi < 0) return;
    const uint32_t H = f[hi], C = f[ci];
    if (!g_battle_seen.load()) { g_battle_seen = true; g_spots_ok = false; g_refills = 0; g_hit_brv = Hit(); g_hit_hp = Hit(); g_brv_prev[0] = g_brv_prev[1] = -1; std::fprintf(stderr, "[TRAIN] battle: you %08X, CPU %08X\n", H, C); }
    // ---- the options panel: Select in the pause menu ----
    const uint32_t b = g_raw.load(std::memory_order_relaxed), pressed = b & ~g_prev_pad; g_prev_pad = b;
    bool drew = false;
    if (g_paused && !g_panel.load() && (pressed & BTN_SELECT)) { g_panel = true; g_row = 0; dissidia_menu_play_se(rdram, ctx, 2); std::fprintf(stderr, "[TRAIN] options panel open\n"); }
    if (g_panel.load()) {
        if (!g_paused) { g_panel = false; g_panel_clear = true; }
        else {
            if (pressed & BTN_UP) { g_row = (g_row + kRowCount - 1) % kRowCount; dissidia_menu_play_se(rdram, ctx, 1); }
            if (pressed & BTN_DOWN) { g_row = (g_row + 1) % kRowCount; dissidia_menu_play_se(rdram, ctx, 1); }
            if (g_row < kRowCount - 1) {
                const Row& r = kRows[g_row]; int v = g_opt[g_row];
                if (pressed & (BTN_RIGHT | BTN_CROSS)) { v = (v + 1) % r.n; g_opt[g_row] = v; dissidia_menu_play_se(rdram, ctx, 1); std::fprintf(stderr, "[TRAIN] %s = %s\n", r.label, r.vals[v]); }
                if (pressed & BTN_LEFT) { v = (v + r.n - 1) % r.n; g_opt[g_row] = v; dissidia_menu_play_se(rdram, ctx, 1); std::fprintf(stderr, "[TRAIN] %s = %s\n", r.label, r.vals[v]); }
            } else if (pressed & BTN_CROSS) { g_reset_req = true; g_panel = false; g_panel_clear = true; dissidia_menu_play_se(rdram, ctx, 2); std::fprintf(stderr, "[TRAIN] reset positions requested\n"); }
            if (pressed & BTN_CIRCLE) { g_panel = false; g_panel_clear = true; dissidia_menu_play_se(rdram, ctx, 1); std::fprintf(stderr, "[TRAIN] options panel closed\n"); }
            if (g_panel.load()) {
                std::vector<std::pair<std::string, std::string>> rows; panel_rows(rows);
                dissidia_menu_style_list("TRAINING", "Options", rows, g_row, g_row < kRowCount - 1 ? kRows[g_row].help : "Both fighters go back to the start spots of this fight", "Up/Down: row   Left/Right: change   Cross: choose   Circle: back");
                drew = true;
            }
        }
    }
    if (!drew && g_panel_clear && !g_panel.load()) { g_panel_clear = false; dissidia_menu_style_clear(); }
    if (g_reset_req && g_spots_ok) {
        g_reset_req = false;
        for (int i = 0; i < 2; i++) {
            const uint32_t a = i == 0 ? H : C; FuncPtr place = dissidia_ovl_battle_lookup(PLACE);
            if (!place) place = psp_dispatch_probe_lookup(PLACE);
            if (!place) continue;
            const recomp_context saved = *ctx; const uint32_t sp0 = static_cast<uint32_t>(ctx->r[29]); ctx->r[29] = static_cast<int32_t>(sp0 - 0x100u); const uint32_t vec = sp0 - 0xC0u;
            wrf(vec, g_spot[i][0]); wrf(vec + 4, g_spot[i][1]); wrf(vec + 8, g_spot[i][2]); wrf(vec + 12, 1.0f);
            ctx->r[4] = static_cast<int32_t>(a); ctx->r[5] = static_cast<int32_t>(vec); ctx->r[6] = 1; place(rdram, ctx); *ctx = saved;
        }
        std::fprintf(stderr, "[TRAIN] positions reset\n");
    }
    if (g_paused) return;   // (nothing else runs in the pause)

    // ---- per frame: gauges, HP, Bravery, the numbers of the last hit ----
    for (int i = 0; i < 2; i++) {
        const uint32_t a = i == 0 ? H : C, st = rd(a + 0x52Cu); if (!ok(st, 0x40)) continue;
        const bool you = i == 0;
        const int hpmax = std::max(1, static_cast<int>(rd(a + 0x10B0u)));
        const int cur = static_cast<int>(rdh(st + 0x14u));
        if (g_brv_prev[i] >= 0 && g_brv_prev[i] > cur) { const uint32_t o = you ? C : H; const uint32_t os = rd(o + 0x52Cu); const int on = ok(os, 0x40) ? static_cast<int>(rdh(os + 0x14u)) : 0;
            if (on > g_brv_prev[1 - i] && g_brv_prev[1 - i] >= 0) { g_hit_brv.who = you ? "CPU > YOU" : "YOU > CPU"; g_hit_brv.n = g_brv_prev[i] - cur; g_hit_brv.have = true; } }
        const int hm = g_opt[you ? O_HP_YOU : O_HP_CPU];
        if (hm == 1) { if (hp_eff(a) < hpmax || static_cast<int32_t>(rd(st + 0x8u)) != 0) refill(a); }
        else if (hp_eff(a) <= std::max(1, hpmax / 50)) { refill(a); g_refills++; }
        const int bm = g_opt[you ? O_BRV_YOU : O_BRV_CPU];
        if (bm > 0) { const uint32_t v = bm == 1 ? (rd(st + 0x14u) >> 16) : bm == 2 ? 0u : bm == 3 ? 3000u : 9999u; wr(st + 0x14u, (rd(st + 0x14u) & 0xFFFF0000u) | (v & 0xFFFFu)); }
        g_brv_prev[i] = static_cast<int>(rdh(st + 0x14u));
        if (you) {
            if (g_opt[O_EX]) wrf(st + 0x18u, 10000.f);
            if (g_opt[O_ASSIST] && rd(a + 0x1108u)) wrf(st + 0x1Cu, 10000.f);
            if (g_opt[O_SUMMON]) { g_ram[(a + 0x10CEu) & 0x07FFFFFFu] = 0; g_ram[(a + 0x10CFu) & 0x07FFFFFFu] = 0; }
        }
    }
    // ---- CPU action Guard: his brain is skipped (dissidia_training_ai_skip); the guard action (script 470, the one R starts for the player) is started here whenever he is not guarding.
    // (Jump was tried the same way: action 560 only reaches the take-off script 102 and falls back, the launch needs the pad input the brain no longer gives: not offered.) ----
    if (g_opt[O_CPU] == 2) {
        static uint32_t n = 0; n++;
        const int sc = static_cast<int16_t>(rd(C + 0x180u) & 0xFFFFu);
        if (sc != 470 && (sc == 1 || n % 20u == 0)) {
            FuncPtr st = psp_dispatch_probe_lookup(0x0882FA10u);
            if (st) { const recomp_context saved = *ctx; const uint32_t sp0 = static_cast<uint32_t>(ctx->r[29]); ctx->r[29] = static_cast<int32_t>(sp0 - 0x40u); ctx->f[12] = 0.f; ctx->f[13] = 0.f;
                ctx->r[4] = static_cast<int32_t>(C + 304u); ctx->r[5] = 470; ctx->r[6] = 0; st(rdram, ctx); *ctx = saved; }
        }
    }
    // ---- start spots and "reset positions" ----
    // (taken when both fighters stand idle after the intro: script 1; at the latest 7 s into the fight)
    if (!g_spots_ok && age > 30.f && ((static_cast<int16_t>(rd(H + 0x180u) & 0xFFFFu) == 1 && static_cast<int16_t>(rd(C + 0x180u) & 0xFFFFu) == 1) || age > 420.f)) {
        for (int i = 0; i < 2; i++) { const uint32_t a = i == 0 ? H : C; g_spot[i][0] = rdf(a + 0x80u); g_spot[i][1] = rdf(a + 0x84u); g_spot[i][2] = rdf(a + 0x88u); }
        g_spots_ok = true; std::fprintf(stderr, "[TRAIN] start spots: you %.1f %.1f %.1f, CPU %.1f %.1f %.1f\n", g_spot[0][0], g_spot[0][1], g_spot[0][2], g_spot[1][0], g_spot[1][1], g_spot[1][2]);
    }
}

// ---- the overlay (top left): called every frame end after the above; drawn on its own game layer ----
namespace {
dissidia::MenuLayer g_layer("training", true); bool g_layer_up = false;
}
void dissidia_training_overlay(bool paused_hint) {
    const bool show = active() && g_battle_seen.load() && !g_panel.load();
    if (!show) { if (g_layer_up) { g_layer.begin(2); g_layer.publish(); g_layer_up = false; } return; }
    g_layer.begin(2);
    std::vector<std::string> lines;
    if (g_opt[O_INFO]) {
        lines.push_back(g_hit_brv.have ? "BRV HIT  " + g_hit_brv.who + "  " + std::to_string(g_hit_brv.n) : "BRV HIT  -");
        lines.push_back(g_hit_hp.have ? "HP HIT  " + g_hit_hp.who + "  " + std::to_string(g_hit_hp.n) : "HP HIT  -");
        lines.push_back("REFILLED  " + std::to_string(g_refills));
    }
    if (paused_hint) lines.push_back("Select: Training options");
    const float px = 6.f, py = 44.f; const int nl = static_cast<int>(lines.size());
    g_layer.band(px, py, px + 126.f, py + 11.f + 9.5f * float(nl), [&](float, float) { return LayerColor{14, 18, 50, 0.62f}; });
    g_layer.text("TRAINING", true, 5.4f, px + 4.f, py + 8.f, LayerColor{110, 232, 240, 1.f});
    for (int i = 0; i < nl; i++) g_layer.text(lines[static_cast<size_t>(i)], true, 5.2f, px + 4.f, py + 17.5f + 9.5f * float(i), LayerColor{255, 255, 255, 1.f});
    g_layer.publish(); g_layer_up = true;
}
bool dissidia_training_paused() { return g_paused; }
