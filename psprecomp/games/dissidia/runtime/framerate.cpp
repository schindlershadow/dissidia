// games/dissidia/runtime/framerate.cpp — native 60 Hz battle logic for Dissidia 012.
//
// How the game paces itself (EBOOT, byte-identical US/EU):
//   * One frame timer object, pointer at 0x089D69E0, built by FUN_08883E74: +0x04 mode (0), +0x0C vcount of the last
//     flip, +0x10 frame interval in vblanks (2), +0x14 vblanks this frame, +0x18 dt (float), +0x24/+0x25 dt enables.
//   * FUN_08883F9C(timer, useNonCB, flag) ends every frame of every loop (battle, menus, loading, movies): it waits
//     for vblanks until `interval` have passed since the last flip, swaps, and stores dt = vblanks x 1.0 in +0x18.
//     In mode 0 the vblank count is clamped to the interval, so dt is a fixed step: 2.0 at 30 Hz, in 1/60 s ticks.
//   * Nothing but the constructor writes the interval, and only FUN_08883F54 (inside the frame end) reads it.
//   * FUN_08864DD8 copies dt into the global frame step 0x089D5EAC (or a slow-motion value derived from it).
//   * The battle code is written against that step: 85 reads of timer+0x18 and 146 of 0x089D5EAC in the battle
//     overlay, plus FUN_08838300 (an actor's step = actor clock x global step, 217 call sites). Motion time,
//     timeline events ((prev, now] windows), movement (speed x dt), decays (pow(a, dt/2)), effect lifetimes and the
//     camera all advance by it. So interval 1 gives dt 1.0 and the same game at 60 updates a second.
//
// What does NOT follow the step (each fixed here, only while running at 60):
//   * Pad auto-repeat (FUN_08871BD0): the repeat timer counts down by max(step, 2.0). Its reload values
//     (pad manager *0x089D6008 +0x25C delay 30, +0x260 interval 4) are scaled by 2/step for the call.
//   * Actors on their own clock (actor+0x3CC, used while the rest of the world is slowed/stopped): the battle
//     overlay sets it to a constant 2.0 (0x08A374B0, 0x08A37868, 0x08A37960, 0x08B46BE0). FUN_08838300's result
//     for such an actor is scaled by interval/2.
//   * Battle camera mode 2 (FUN_08B3B4FC, camera +0x100 == 2) uses camera +0x104, a constant 2.0 from its
//     constructor: scaled by interval/2.
//   * Effect components that step once per update instead of x dt (docs/archive/open-questions/fx.md item 7): the
//     ch61 envelope, the ch95/100/102/103 point and line emitters, the ch68/70 trail paths, the ch105 locus UV scroll
//     and the ch106 locus history (weapon trails). See "Effect components" below.
// Mod code that counts per call rather than per tick uses dissidia_frame_ticks() (roster.cpp warp-dash link).
//
// Scope: the battle overlay only by default. Menus mostly count frames (4 dt reads in the menu overlay), so they
// keep 30 Hz. PSPRECOMP_FPS_SCOPE=all applies 60 everywhere (for testing; menus then run fast).
//
// Options: PSPRECOMP_FPS=30|60 (default 30; the settings menu changes it live and saves it),
// PSPRECOMP_FPS_SCOPE=battle|all, PSPRECOMP_FPS_LOG=1 (logic rate, dt, step and how often body poses change, every
// 5 s, plus each switch), PSPRECOMP_FX60=0 (leave the effect components below as they are, for A/B checks).

#include "psp_game_module.h"
#include "psp_runtime.h"
#include "psp_settings.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_adhoc.h"
#include "psp_ge_draw.h"
#include <algorithm>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iterator>
#include <unordered_map>

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
extern FuncPtr ovl_menu_lookup(uint32_t vaddr);
extern FuncPtr ovl_extra_lookup(uint32_t vaddr);

int dissidia_test_action(uint8_t* rdram, recomp_context* ctx, uint32_t actor, int id);   // roster.cpp

void psp_mem_watch_set(uint32_t addr);   // runtime/src/psp_dispatch.cpp
namespace {

constexpr uint32_t OVL_BASE = 0x089DD4C0u;
constexpr uint32_t FRAME_END = 0x08883F9Cu;     // EBOOT: per-frame end (a0 = frame timer)
constexpr uint32_t TIMER_PTR = 0x089D69E0u;     // EBOOT: frame timer object pointer
constexpr uint32_t FRAME_STEP = 0x089D5EACu;    // EBOOT: global frame step (float, 1/60 s ticks)
constexpr uint32_t PRESSED = 0x08AB399Cu;       // battle overlay: virtual pad 'pressed this update' test (obj, bit) = state & ~prev
constexpr uint32_t PAD_SET = 0x08871BD0u;       // EBOOT: pad button state + auto-repeat
constexpr uint32_t PAD_MGR_PTR = 0x089D6008u;   // EBOOT: pad manager pointer (+0x25C delay, +0x260 interval)
constexpr uint32_t ACTOR_DT = 0x08838300u;      // EBOOT: an actor's time step
constexpr uint32_t SLOWMO_FRAME = 0x08864DD8u;  // EBOOT: per-frame update of the slow-motion object, writes the global step
constexpr uint32_t SLOWMO_PTR = 0x089D5EA8u;    // EBOOT: pointer to that object (+0 step value, +4 time left, +8 flags)
constexpr uint32_t ROOT_MOTION = 0x0882F784u;   // EBOOT: adds (bone pose now - pose last tick) to the motion player's accumulator
constexpr uint32_t CPU_AI = 0x08A75D38u;        // battle overlay: CPU AI action chooser (returns -1 / -4: no action)
constexpr uint32_t STEER_MOVE = 0x08A2C960u;   // battle overlay: ground steered move controller (Squall's Revolver Drive rush): a0 = out vector (metres to move this update)
constexpr uint32_t UI_PARAM_STEP = 0x088DA520u; // EBOOT: UI animation parameter stepper (value += rate x UI step toward a target)
constexpr uint32_t UI_MGR_PTR = 0x089D7830u;    // EBOOT: pointer to the UI manager (+33: use the frame dt as the UI step, +34: paused)
constexpr uint32_t UI_STEP = 0x089D7834u;       // EBOOT: the UI step FUN_088CF550 sets every update (0 paused, 1.0 fixed, or the frame dt)
constexpr uint32_t OWN_MASK0 = 0x089C7FD8u, OWN_MASK1 = 0x089C7FDCu;
constexpr uint32_t EXB_PRESSED = 0x08B326C4u;   // battle overlay: 'virtual button pressed this update' test of an actor (r4), used by the EX Burst code
constexpr uint32_t EXB_PROMPT = 0x08A3D1C4u;    // battle overlay: EX Burst button prompt update (state machine with a timer at +28; log only)
constexpr uint32_t EXB_UI = 0x08A40E60u;        // battle overlay: EX Burst quicktime UI update (calls the 08A41558 / 08A41774 states)
constexpr uint32_t EXB_CTRL = 0x08A3FE10u;      // battle overlay: EX Burst controller update (prompt, release gauge, slow motion)
constexpr uint32_t CAM_DT = 0x08B3B4FCu;        // battle overlay: a camera's time step

uint8_t* g_ram = nullptr;
FuncPtr g_orig_ui_param = nullptr, g_orig_steer_move = nullptr, g_orig_pressed = nullptr, g_orig_rootmotion = nullptr, g_orig_slowmo = nullptr, g_orig_frame_end = nullptr, g_orig_pad = nullptr, g_orig_actor_dt = nullptr, g_orig_cam_dt = nullptr, g_orig_exb_ctrl = nullptr, g_orig_exb_pressed = nullptr, g_orig_exb_prompt = nullptr, g_orig_exb_ui = nullptr;
std::atomic<int> g_target{30};     // requested rate (settings / env)
int g_interval = 2;                // interval written on the last frame end (2 = 30 Hz, 1 = 60 Hz and above)
int g_rate = 30;                   // logic rate in effect (30, 60, 90, ... one frame per vblank above 30)
float g_ticks = 2.0f;              // 1/60 s ticks one frame advances: 60 / g_rate
int g_fx_period = 1;               // updates per effect-component step at 30 Hz pacing: g_rate / 30
bool g_scope_all = false, g_log = false;
int g_switch_logs = 0;
int g_rm_log_left = 300;
bool g_fighter_log = false;   // PSPRECOMP_FIGHTER_LOG (see fighter_log_frame)
uint32_t g_press_calls = 0, g_press_edges = 0, g_press_latched = 0, g_press_dropped = 0;   // press latch counters for the [FPS] log
uint64_t g_tick = 1;               // frame ends seen (the effect system updates once per frame)
// logic-rate log
int64_t g_win_t0 = 0;
int g_win_frames = 0;

uint32_t r32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
float rdf(uint32_t a) { float f; std::memcpy(&f, g_ram + (a & 0x07FFFFFFu), 4); return f; }
void w32(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &v, 4); }
void wrf(uint32_t a, float f) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &f, 4); }
bool ok(uint32_t a) { return a >= 0x08800000u && a < 0x0BFFFF00u; }
uint32_t overlay_id() { return r32(OVL_BASE); }

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// PSPRECOMP_FPS_LOG: does body animation change every frame? FUN_089B2974 builds a skeleton's matrices from the
// local poses the motion evaluator wrote (bone i at node+0x0C base + 256 i: scale +80, rotation quaternion +96,
// translation +112). Per skeleton seen in a frame, the local poses are hashed and compared with the previous
// frame's. If the evaluator samples motions at fractional frames, poses change on (nearly) every 60 Hz frame; if
// it rounds to whole 30 fps frames, on about half of them. Logged with the logic rate every 5 s.
constexpr uint32_t SKEL_BUILD = 0x089B2974u;
FuncPtr g_orig_skel = nullptr;
struct SkelSeen { uint32_t node = 0, hash = 0; int64_t frame = -1; };
SkelSeen g_skel[16];
int64_t g_frame_no = 0;
int g_pose_changed = 0, g_pose_same = 0;

void skel_build_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t node = static_cast<uint32_t>(ctx->r[4]);
    if (ok(node) && overlay_id() == 1) {
        const uint32_t base = r32(node + 0x0Cu);
        uint32_t n = 0;
        std::memcpy(&n, g_ram + ((node + 32u) & 0x07FFFFFFu), 2);
        n &= 0xFFFFu;
        if (ok(base) && n >= 8 && n <= 200) {   // a character skeleton (weapons / props have few bones)
            uint32_t h = 2166136261u;
            for (uint32_t i = 0; i < n; i++)
                for (uint32_t o = 80; o < 128; o += 4) { h ^= r32(base + 256u * i + o); h *= 16777619u; }
            SkelSeen* slot = nullptr;
            for (auto& sk : g_skel) if (sk.node == node) { slot = &sk; break; }
            if (!slot) for (auto& sk : g_skel) if (sk.frame < g_frame_no - 30) { slot = &sk; sk = SkelSeen{}; sk.node = node; break; }
            if (slot && slot->frame != g_frame_no) {   // first build of this skeleton this frame
                if (slot->frame == g_frame_no - 1) (h != slot->hash ? g_pose_changed : g_pose_same)++;
                slot->hash = h;
                slot->frame = g_frame_no;
            }
        }
    }
    g_orig_skel(rdram, ctx);
}

void fx_log_window();uint32_t g_fighters[4] = {};
uint64_t g_fighter_seen[4] = {};   // g_tick when the fighter's step was last asked for (stale = left the battle)

// ---- Test driver (PSPRECOMP_TEST_SCRIPT / PSPRECOMP_INPUT_RECORD): scripted pad input in GAME time ----
// A script is a text file, one event per line:  <ms> set <BUTTONS|-> [ax ay]   hold exactly that pad state (and stick, 128 = centre)
//                                               <ms> tap <BUTTONS> [ms]         press, release after ms (default 100)
//                                               <ms> analog <ax> <ay>           stick only
//                                               <ms> mark <text>                 [TEST] line in the log
//                                               <ms> quit                        exit the game
// BUTTONS: CROSS CIRCLE SQUARE TRIANGLE L R UP DOWN LEFT RIGHT START SELECT joined by '+', or a 0x hex mask. The clock is game
// time in ms since the battle began (the first fighter in an action), advanced by 1/60 s x dt per update, so the same script
// plays identically at 30 and 120 fps and the [FIGHTER] logs (gt=) of two rates can be compared line by line (tools/rate_test.py).
// The scripted pad replaces every other input while a script runs in battle. PSPRECOMP_INPUT_RECORD=<file> writes the pad
// state the game was given as such a script (play the move once by hand, replay it at any rate).
struct TestEvent { double t; int kind; uint32_t a, b; std::string text; };   // kind 0 set, 1 analog, 2 mark, 3 quit, 4 action
std::vector<TestEvent> g_test_events;
size_t g_test_next = 0;
double g_gt_ms = 0;                     // game time in the battle, ms
bool g_gt_started = false, g_test_on = false;
uint32_t g_test_buttons = 0, g_test_analog = 0x8080;
bool g_test_freeze_cpu = false;   // PSPRECOMP_TEST_FREEZE_CPU=1: every fighter but the first one seen gets time step 0 while a test runs (a standing dummy)
int g_player_char = -1;   // PSPRECOMP_TEST_PLAYER_CHAR=<charId>: the fighter under test (default: the first one seen)
uint32_t g_human_vt = 0;   // vtable of the human's controller object (found at the first battle: the one a scripted press reaches)
FILE* g_record = nullptr;
char g_record_path[512] = {};
uint32_t g_rec_buttons = 0xFFFFFFFFu, g_rec_analog = 0;

uint32_t parse_buttons(const std::string& s) {
    static const struct { const char* n; uint32_t m; } k[] = {{"SELECT", 0x1}, {"START", 0x8}, {"UP", 0x10}, {"RIGHT", 0x20}, {"DOWN", 0x40}, {"LEFT", 0x80},
        {"L", 0x100}, {"R", 0x200}, {"TRIANGLE", 0x1000}, {"CIRCLE", 0x2000}, {"CROSS", 0x4000}, {"SQUARE", 0x8000}};
    if (s == "-" || s == "NONE") return 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 16));
    uint32_t m = 0;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('+', i);
        if (j == std::string::npos) j = s.size();
        const std::string w = s.substr(i, j - i);
        bool hit = false;
        for (const auto& e : k) if (w == e.n) { m |= e.m; hit = true; }
        if (!hit && !w.empty()) std::fprintf(stderr, "[TEST] unknown button '%s'\n", w.c_str());
        i = j + 1;
    }
    return m;
}

void testdrive_load(const char* path) {
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "[TEST] cannot read script %s\n", path); return; }
    std::string line;
    while (std::getline(in, line)) {
        const size_t h = line.find('#');
        if (h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        double t; std::string cmd;
        if (!(ss >> t >> cmd)) continue;
        if (cmd == "set" || cmd == "tap") {
            std::string b; ss >> b;
            const uint32_t m = parse_buttons(b);
            if (cmd == "tap") {
                double d = 100; ss >> d;
                g_test_events.push_back({t, 0, m, 0x8080, ""});
                g_test_events.push_back({t + d, 0, 0, 0x8080, "tap"});   // 'tap' release keeps the stick: marked by text
            } else {
                unsigned ax = 128, ay = 128; ss >> ax >> ay;
                g_test_events.push_back({t, 0, m, (ay & 255u) << 8 | (ax & 255u), "set"});
            }
        } else if (cmd == "analog") {
            unsigned ax = 128, ay = 128; ss >> ax >> ay;
            g_test_events.push_back({t, 1, 0, (ay & 255u) << 8 | (ax & 255u), ""});
        } else if (cmd == "mark") {
            std::string rest; std::getline(ss, rest);
            g_test_events.push_back({t, 2, 0, 0, rest});
        } else if (cmd == "action") {   // action <id> [who]: start the action; who = player (default) | cpu | <slot>; id -1 lists them
            int id = -1; std::string who = "player"; ss >> id >> who;
            g_test_events.push_back({t, 4, static_cast<uint32_t>(id), who == "cpu" ? 1u : (who == "player" ? 0u : 2u + static_cast<uint32_t>(std::atoi(who.c_str()))), ""});
        } else if (cmd == "gauge") {   // gauge <side> <0..10000>: set a side's assist gauge (side 0 / 1 = the controller table order of the HUD)
            unsigned side = 1; double v = 0; ss >> side >> v;
            g_test_events.push_back({t, 5, side, 0, std::to_string(v)});
        } else if (cmd == "quit") g_test_events.push_back({t, 3, 0, 0, ""});
        else std::fprintf(stderr, "[TEST] unknown command '%s'\n", cmd.c_str());
    }
    std::stable_sort(g_test_events.begin(), g_test_events.end(), [](const TestEvent& x, const TestEvent& y) { return x.t < y.t; });
    std::fprintf(stderr, "[TEST] script %s: %zu events\n", path, g_test_events.size());
}

uint32_t assist_gauge_addr(int side);
bool battle_running() {
    if (overlay_id() != 1) return false;
    for (int i = 0; i < 4; i++) {
        const uint32_t a = g_fighters[i];
        if (a && ok(a) && g_tick - g_fighter_seen[i] < 60) return true;
    }
    return false;
}

// PSPRECOMP_TEST_GO=<file>: the clock and the scripted pad wait until this file exists (the harness creates it when the fight's HUD
// is on screen: the loading and opponent pages before it are in the battle overlay too and need real button presses).
const char* g_test_go = nullptr;
bool g_go_seen = false;
bool test_go() {
    if (!g_test_go || g_go_seen) return true;
    if (g_tick % 10 == 0) {
        if (FILE* f = std::fopen(g_test_go, "r")) { std::fclose(f); g_go_seen = true; std::fprintf(stderr, "[TEST] go file seen (tick %llu)\n", (unsigned long long)g_tick); }
    }
    return g_go_seen;
}

// the fighter a script command means: 0 = the player's, 1 = the CPU's, 2+n = the n-th known fighter. The human's is the one whose
// controller object (actor +0x5C4) has the virtual-pad owner flag... found by looking at what [TEST] fighters prints (see below).
int fighter_char(uint32_t a) {
    const uint32_t info = r32(a + 0x2F8u);
    return ok(info) ? static_cast<int8_t>(g_ram[(info + 6u) & 0x07FFFFFFu]) : -99;
}
uint32_t test_player() {   // the fighter under test
    uint32_t first = 0;
    for (int i = 0; i < 4; i++) {
        const uint32_t a = g_fighters[i];
        if (!a || !ok(a) || g_tick - g_fighter_seen[i] > 60) continue;
        if (!first) first = a;
        if (g_player_char >= 0 && fighter_char(a) == g_player_char) return a;
    }
    return g_player_char >= 0 ? 0u : first;
}
uint32_t test_fighter(uint32_t who) {
    uint32_t list[4]; int n = 0;
    for (int i = 0; i < 4; i++) {
        const uint32_t a = g_fighters[i];
        if (a && ok(a) && g_tick - g_fighter_seen[i] < 60) list[n++] = a;
    }
    if (who >= 2) return static_cast<int>(who - 2) < n ? list[who - 2] : 0u;
    const uint32_t pl = test_player();
    for (int i = 0; i < n; i++) if ((list[i] == pl) == (who == 0)) return list[i];
    return 0u;
}
void test_describe_fighters() {
    for (int i = 0; i < 4; i++) {
        const uint32_t a = g_fighters[i];
        if (!a || !ok(a) || g_tick - g_fighter_seen[i] > 60) continue;
        const uint32_t ctrl = r32(a + 0x5C4u), info = r32(a + 0x1108u - 0x1108u + 0x0u);
        (void)info;
        std::fprintf(stderr, "[TEST] fighter %08X char=%d ctrl=%08X vt=%08X scr=%u pos=%.2f,%.2f\n", a, fighter_char(a), ctrl, ok(ctrl) ? r32(ctrl) : 0u, r32(a + 0x540u) & 0xFFFFu, rdf(a + 0x80u), rdf(a + 0x88u));
    }
}

// once per logic update, after the frame end
void testdrive_frame(uint8_t* rdram, recomp_context* ctx) {
    if (!g_test_on) return;
    const bool run = battle_running() && test_go();
    if (!run) {
        if (g_gt_started) {
            std::fprintf(stderr, "[TEST] battle ended at gt=%.0f ms\n", g_gt_ms);
            g_gt_started = false;
            psp_ctrl_set_script(false, 0, 0x8080);
            if (g_record) { std::fclose(g_record); g_record = nullptr; }
        }
        return;
    }
    if (!g_gt_started) {
        g_gt_started = true; g_gt_ms = 0; g_test_next = 0; g_test_buttons = 0; g_test_analog = 0x8080;
        std::fprintf(stderr, "[TEST] battle started (tick %llu)\n", (unsigned long long)g_tick);
        if (g_record_path[0]) {   // one file per fight: <file>, <file>.2, <file>.3 ...
            static int fights = 0;
            fights++;
            char path[600];
            if (fights == 1) std::snprintf(path, sizeof path, "%s", g_record_path); else std::snprintf(path, sizeof path, "%s.%d", g_record_path, fights);
            if (g_record) std::fclose(g_record);
            g_record = std::fopen(path, "w");
            if (g_record) std::fprintf(g_record, "# recorded pad input, game-time ms (PSPRECOMP_TEST_SCRIPT=this file)\n");
            g_rec_buttons = 0xFFFFFFFFu; g_rec_analog = 0;
            std::fprintf(stderr, "[TEST] recording to %s\n", path);
        }
    } else {
        g_gt_ms += g_ticks * (1000.0 / 60.0);
    }
    if (g_record) {
        uint32_t b, a; psp_ctrl_last_sample(&b, &a);
        if (b != g_rec_buttons || a != g_rec_analog) {
            std::fprintf(g_record, "%.0f set 0x%X %u %u\n", g_gt_ms, b, a & 255u, (a >> 8) & 255u);
            std::fflush(g_record);
            g_rec_buttons = b; g_rec_analog = a;
        }
    }
    static bool described = false;
    if (!g_gt_started) described = false;
    if (!described && g_gt_ms >= 1000) { described = true; test_describe_fighters(); }
    if (g_test_freeze_cpu) {   // the dummy keeps the HP it started with (actor +0x10B0): a hit never ends the fight
        static uint32_t keep_actor = 0, keep_hp = 0;
        for (int i = 0; i < 4; i++) {
            const uint32_t a = g_fighters[i];
            if (!a || !ok(a) || g_tick - g_fighter_seen[i] > 60 || a == test_player()) continue;
            if (a != keep_actor || !g_gt_started || g_gt_ms < 34) { keep_actor = a; keep_hp = r32(a + 0x10B0u); }
            else if (keep_hp) w32(a + 0x10B0u, keep_hp);
        }
    }
    if (g_test_events.empty()) return;
    while (g_test_next < g_test_events.size() && g_test_events[g_test_next].t <= g_gt_ms) {
        const TestEvent& e = g_test_events[g_test_next++];
        if (e.kind == 0) { g_test_buttons = e.a; if (e.text == "set") g_test_analog = e.b; }
        else if (e.kind == 1) g_test_analog = e.b;
        else if (e.kind == 2) std::fprintf(stderr, "[TEST] gt=%.0f mark%s\n", g_gt_ms, e.text.c_str());
        else if (e.kind == 4) {
            const uint32_t a = test_fighter(e.b);
            const int r = a ? dissidia_test_action(rdram, ctx, a, static_cast<int>(e.a)) : -2;
            std::fprintf(stderr, "[TEST] gt=%.0f action %d on %08X (%s): %s\n", g_gt_ms, static_cast<int>(e.a), a, e.b == 1 ? "cpu" : "player",
                         r == 1 ? "started" : r == 0 ? "NOT started" : r == -1 ? "no such record" : "no such fighter");
        }
        else if (e.kind == 5) {
            const uint32_t ga = assist_gauge_addr(static_cast<int>(e.a));
            if (ga) wrf(ga, static_cast<float>(std::atof(e.text.c_str())));
            std::fprintf(stderr, "[TEST] gt=%.0f gauge side %u = %s (%s)\n", g_gt_ms, e.a, e.text.c_str(), ga ? "set" : "no gauge");
        }
        else if (e.kind == 3) { std::fprintf(stderr, "[TEST] quit at gt=%.0f ms\n", g_gt_ms); g_should_exit.store(true); }
    }
    psp_ctrl_set_script(true, g_test_buttons, g_test_analog);
}

void testdrive_install() {
#ifdef __linux__
    // PSPRECOMP_ALLOW_PTRACE=1: let any process of this user attach a debugger (Yama ptrace_scope 1 only allows a parent); for
    // `gdb -p` write traces ("which function stores this guest word") on an unstripped build
    if (const char* ap = std::getenv("PSPRECOMP_ALLOW_PTRACE"); ap && ap[0] && ap[0] != '0') prctl(PR_SET_PTRACER, (unsigned long)-1 /* PR_SET_PTRACER_ANY */, 0, 0, 0);
#endif
    if (const char* fz = std::getenv("PSPRECOMP_TEST_FREEZE_CPU"); fz && fz[0] && fz[0] != '0') g_test_freeze_cpu = true;
    if (const char* pc = std::getenv("PSPRECOMP_TEST_PLAYER_CHAR"); pc && pc[0]) g_player_char = std::atoi(pc);
    g_test_go = std::getenv("PSPRECOMP_TEST_GO");
    if (g_test_go && !g_test_go[0]) g_test_go = nullptr;
    const char* e = std::getenv("PSPRECOMP_TEST_SCRIPT");
    if (e && e[0]) { testdrive_load(e); g_test_on = true; }
    if ((e = std::getenv("PSPRECOMP_INPUT_RECORD")) && e[0]) {
        std::snprintf(g_record_path, sizeof g_record_path, "%s", e);   // opened at each fight's start
        g_test_on = true;
    }
}

void fighter_log_frame();
void fighter_dump_frame();

// A multiplayer group agrees on a frame rate: tell the player (a few seconds, top centre), and again when it
// changes or the group ends.
void note_negotiated_rate(int cap) {
    static int shown = 0;
    if (cap == shown) return;
    const int own = g_target.load(std::memory_order_relaxed);
    char msg[96];
    if (cap) {
        std::snprintf(msg, sizeof msg, cap < own ? "Online: %d fps (lowest player's setting)" : "Online: %d fps (all players agree)", cap);
    } else {
        std::snprintf(msg, sizeof msg, "Online session ended: back to %d fps", own);
    }
    std::fprintf(stderr, "[FPS] negotiated frame rate %d (own setting %d)\n", cap, own);
    ge_draw_toast(msg, 5000);
    shown = cap;
}

void frame_end_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t timer = static_cast<uint32_t>(ctx->r[4]);
    g_tick++;
    if (ok(timer)) {
        // In an ad-hoc session every player runs at the lowest rate any of them asked for, so the
        // lock-step battle stays in sync (psp_adhoc_group_fps_cap() is 0 when not connected).
        const int cap = psp_adhoc_group_fps_cap();
        note_negotiated_rate(cap);
        int rate = 30;
        if (g_scope_all || overlay_id() == 1) {
            rate = g_target.load(std::memory_order_relaxed);
            if (cap > 0 && cap < rate) rate = cap;   // never above what the slowest player asked for
        }
        if (rate < 30) rate = 30;
        const int interval = rate > 30 ? 1 : 2;      // vblanks per frame: the vblank clock itself runs faster above 60
        psp_display_set_vblank_hz(rate > 60 ? rate : 60);
        if (r32(timer + 0x10u) != static_cast<uint32_t>(interval) || g_rate != rate) {
            w32(timer + 0x10u, static_cast<uint32_t>(interval));
            if (g_log || g_switch_logs < 12) {
                g_switch_logs++;
                std::fprintf(stderr, "[FPS] game logic now %d Hz (frame interval %d, overlay %u)\n", rate, interval, overlay_id());
            }
        }
        g_interval = interval;
        g_rate = rate;
        g_ticks = 60.0f / static_cast<float>(rate);
        g_fx_period = rate / 30;
    }
    g_orig_frame_end(rdram, ctx);
    testdrive_frame(rdram, ctx);
    fighter_log_frame();
    if (g_rate > 60 && ok(timer)) wrf(timer + 0x18u, g_ticks);   // the frame waited for one (short) vblank: dt = 60 / rate
    if (g_log && ok(timer)) {
        const int64_t t = now_ms();
        if (!g_win_t0) g_win_t0 = t;
        g_win_frames++;
        g_frame_no++;
        if (t - g_win_t0 >= 5000) {
            const int pose_n = g_pose_changed + g_pose_same;
            std::fprintf(stderr, "[FPS] logic %.1f Hz, interval %d, dt %.2f, step %.2f, overlay %u; body poses changed on %d of %d "
                         "frame-to-frame checks (%.0f%%); press latch: %u calls, %u edges, %u latched, %u dropped\n",
                         g_win_frames * 1000.0 / double(t - g_win_t0), g_interval, rdf(timer + 0x18u), rdf(FRAME_STEP),
                         overlay_id(), g_pose_changed, pose_n, pose_n ? 100.0 * g_pose_changed / pose_n : 0.0,
                         g_press_calls, g_press_edges, g_press_latched, g_press_dropped);
            g_win_t0 = t;
            g_win_frames = 0;
            g_pose_changed = g_pose_same = 0;
            fx_log_window();
        }
    }
}

// Slow motion (hit stops, assist and summon cut-ins, EX bursts). While it is on, the global step is not the frame's
// dt but the slow-motion object's own value (+0: 0.4, 0.15 ...), in the PSP's units where a normal 30 Hz frame is
// 2.0; the same value is also what the time left (+4) drops by every frame. Left alone at 60 Hz and above the world
// is twice as fast as meant (0.4 against a normal 1.0, not against 2.0) and the slow motion is over in half the time.
// Worst for an assist: it runs on a clock of 5.0 (5 x 0.4 = 2.0, the normal PSP speed), so it ran at twice normal
// speed while everything else crawled. The value is scaled by ticks / 2 for the update, then put back, so the
// code that reads it elsewhere (the setters) sees what it stored.
void slowmo_frame_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t obj = r32(SLOWMO_PTR);
    if (g_ticks == 2.0f || !ok(obj)) { g_orig_slowmo(rdram, ctx); return; }
    const float was = rdf(obj);
    wrf(obj, was * g_ticks * 0.5f);
    g_orig_slowmo(rdram, ctx);
    wrf(obj, was);
}

// UI animation step. FUN_088CF550 (the UI update) stores the step the UI animations advance by (0x089D7834) before it runs
// them: 0 while the UI is paused, the frame dt when the UI manager's flag +33 is set, else a FIXED 1.0 per update. The fixed
// ones were written for 30 updates a second, so at 60 and above they ran twice (or more) as fast: the 'VS <name>' banner that
// opens a fight flashed by in 0.3 s instead of 0.6 s. The stepper FUN_088DA520 is the only reader (value += rate x step), so
// the fixed step is scaled by ticks / 2 for the call and put back. The dt-driven ones already follow the frame time.
// PSPRECOMP_UI_STEP=0 turns it off (A/B).
bool g_ui_step = true;
void ui_param_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t mgr = r32(UI_MGR_PTR);
    if (!g_ui_step || g_ticks == 2.0f || overlay_id() != 1 || !ok(mgr) || g_ram[(mgr + 33u) & 0x07FFFFFFu] != 0 || g_ram[(mgr + 34u) & 0x07FFFFFFu] != 0) { g_orig_ui_param(rdram, ctx); return; }
    const float was = rdf(UI_STEP);
    wrf(UI_STEP, was * g_ticks * 0.5f);
    g_orig_ui_param(rdram, ctx);
    wrf(UI_STEP, was);
}

// Root motion across a loop. A looping motion (Kain's Sky Spear dive, Sky Rave: a 12-tick loop of travel) moves the
// fighter by the change of the root bone's pose each tick. On the last tick before the loop wraps (time 31 of a loop
// that ends at 32, with the player's end at +112) the change is minus the whole loop's travel (5.3 against 0.48 on
// every other tick): every 12 ticks the fighter was thrown back by what he had just covered, so the move hardly
// moved. The player's own timing at 30 Hz steps by 2 and never lands on that tick; at 60 Hz and above it does. On that
// tick a change far larger than the previous tick's is dropped (that tick's travel is lost, about 1 in 12; the pose is
// still taken as the new reference, so the next tick continues normally). Player layout (FUN_08830444): +112 end of
// the played range, +116 previous time, +120 time, +16..31 accumulated root motion (actor+0x140, cleared every tick).
void root_motion_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t player = static_cast<uint32_t>(ctx->r[4]);
    if (!ok(player)) { g_orig_rootmotion(rdram, ctx); return; }
    static struct { uint32_t player; float mag; } last[8];
    static int next = 0;
    auto slot = [&]() -> float& {
        for (auto& l : last) if (l.player == player) return l.mag;
        last[next & 7] = {player, 0.f};
        return last[next++ & 7].mag;
    };
    const float t = rdf(player + 120u), pt = rdf(player + 116u), end = rdf(player + 112u);
    uint8_t saved[16];
    std::memcpy(saved, g_ram + ((player + 16u) & 0x07FFFFFFu), 16);
    g_orig_rootmotion(rdram, ctx);
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = rdf(player + 16u + 4u * i) - [&] { float f; std::memcpy(&f, saved + 4 * i, 4); return f; }();
    const float mag = std::max(std::fabs(d[0]), std::max(std::fabs(d[1]), std::fabs(d[2])));
    float& prev_mag = slot();
    const bool last_before_wrap = g_ticks != 2.0f && t >= pt && t + g_ticks >= end - 0.001f && end > 0.f;
    const bool spike = last_before_wrap && mag > 0.5f && mag > 3.f * std::max(prev_mag, 0.05f);
    if (g_fighter_log && g_rm_log_left > 0 && (spike || mag > 2.f)) {
        g_rm_log_left--;
        std::fprintf(stderr, "[ROOTMOTION] player %08X tick=%llu time=%.3f prev=%.3f end=%.3f delta=(%.3f,%.3f,%.3f) last_mag=%.3f spike=%d\n", player,
                     (unsigned long long)g_tick, t, pt, end, d[0], d[1], d[2], prev_mag, spike ? 1 : 0);
    }
    if (spike) std::memcpy(g_ram + ((player + 16u) & 0x07FFFFFFu), saved, 16);   // keep what was accumulated before
    else prev_mag = mag;
}


// FUN_08A2C960(out, ctrl, actor): the movement of a steered ground move (Squall's Revolver Drive rush: timeline op 0x0225, actor
// controller +0x3A4). It writes how far the actor moves THIS UPDATE (0.595 m at full speed) and never reads the frame time: on the PSP
// that was 30 updates a second, so at 120 the rush went 4 times as fast (found with a write trace, tools/battle_test.py). The vector it
// returns is scaled by the frame time (dt 2 = 30 Hz).
void steer_move_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t out = static_cast<uint32_t>(ctx->r[4]);
    g_orig_steer_move(rdram, ctx);
    const uint32_t timer = r32(TIMER_PTR);
    const float dt = ok(timer) ? rdf(timer + 0x18u) : 2.f;
    if (dt > 0.f && dt < 2.f && ok(out)) for (uint32_t i = 0; i < 3; i++) wrf(out + 4 * i, rdf(out + 4 * i) * dt * 0.5f);
}

void pad_set_hook(uint8_t* rdram, recomp_context* ctx) {
    const float step = rdf(FRAME_STEP);
    const uint32_t mgr = r32(PAD_MGR_PTR);
    if (step > 0.f && step < 2.f && ok(mgr)) {   // the repeat timer always counts down >= 2 per call
        const float delay = rdf(mgr + 0x25Cu), every = rdf(mgr + 0x260u), k = 2.f / step;
        wrf(mgr + 0x25Cu, delay * k);
        wrf(mgr + 0x260u, every * k);
        g_orig_pad(rdram, ctx);
        wrf(mgr + 0x25Cu, delay);
        wrf(mgr + 0x260u, every);
        return;
    }
    g_orig_pad(rdram, ctx);
}

// PSPRECOMP_ASSIST_LOG=1: log each actor's time step whenever it changes (debug aid for actors that run at the
// wrong speed): address, kind (actor+0x542, roster.cpp ACT_KIND: 0 fighter, 1 assist, 3 summoned), own-clock flag, both clocks, the step the game
// gets and the global step. Capped, so a long battle does not flood the log.
bool g_actor_log = false;
// PSPRECOMP_FIGHTER_LOG=1: one line per frame for each fighter that is in an action (script id != 0xFFFF): height,
// the floor height the game keeps for it (+0x2E4), flags (+0x380) and a few fields that moved while an attack
// misbehaved. For debugging attacks that behave differently above 30 Hz (Kain's Sky Spear).
int g_fighter_log_left = 40000;

// Press latch (PSPRECOMP_PRESS_LATCH=0 turns it off). The fighter code asks the virtual pad "was bit B pressed this update"
// (FUN_08AB399C: state & ~previous state). The PSP ran 30 updates a second, so a press was seen for 33 ms; at 120 it is seen for
// 8 ms, and the timed chain inputs (Jecht: 3 to 5 frame windows) are four times harder to hit. A press is therefore reported as
// pressed for 2 / step updates in total (4 at 120 Hz), as long as the fighter has not started another action (script id) since:
// the press that starts a move must not count again for that move's own early-press lockout (event 0x004C).
bool g_press_latch = true;
float g_press_ms = 33.4f;   // PSPRECOMP_PRESS_LATCH=<ms>: how long a press counts (default one PSP update; 0 = off)
struct PressEntry { uint32_t obj = 0, bit = 0, actor = 0, scr = 0; uint64_t tick = 0; };
PressEntry g_press[48];
uint32_t actor_of_pad(uint32_t pad) {   // the fighter whose controller (actor+0x5C4) owns this virtual pad (controller+0x53C)
    for (uint32_t a : g_fighters) {
        if (!a || !ok(a)) continue;
        const uint32_t ctrl = r32(a + 0x5C4u);
        if (ok(ctrl) && ctrl + 0x53Cu == pad) return a;
    }
    return 0;
}
void pressed_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t obj = static_cast<uint32_t>(ctx->r[4]), bit = static_cast<uint32_t>(ctx->r[5]);
    g_orig_pressed(rdram, ctx);
    g_press_calls++;
    if (!g_press_latch || g_ticks >= 2.0f || overlay_id() != 1) return;
    const uint64_t window = static_cast<uint64_t>(g_press_ms * 0.001f * 60.0f / g_ticks + 0.5f);   // updates a press stays "pressed"
    PressEntry* free_slot = nullptr;
    PressEntry* mine = nullptr;
    for (auto& e : g_press) {
        if (e.obj == obj && e.bit == bit) { mine = &e; break; }
        if (!free_slot && (!e.obj || g_tick - e.tick > 16)) free_slot = &e;
    }
    if (ctx->r[2] & 1) {   // a real edge: remember it
        PressEntry* e = mine ? mine : free_slot;
        if (!e) return;
        const uint32_t a = actor_of_pad(obj);
        *e = PressEntry{obj, bit, a, a ? (r32(a + 0x540u) & 0xFFFFu) : 0u, g_tick};
        g_press_edges++;
        return;
    }
    if (!mine || g_tick == mine->tick || g_tick - mine->tick >= window) return;
    if (mine->actor && (!ok(mine->actor) || (r32(mine->actor + 0x540u) & 0xFFFFu) != mine->scr)) { mine->obj = 0; g_press_dropped++; return; }   // a new action began
    ctx->r[2] = 1;
    g_press_latched++;
}


uint32_t g_objects[12] = {};      // assists (kind 1), summoned / thrown objects (3, 6 ...): PSPRECOMP_FIGHTER_LOG logs their motion time
float g_object_time[12] = {};
int g_object_log_left = 30000;
// PSPRECOMP_FIGHTER_DUMP=1: every update, each fighter's state words that changed since the last update (offset, old -> new, as
// hex and float) over the position, input-flag and action-state ranges. Finds counters that tick once per update instead of by dt
// (input windows that shrink above 30 Hz) and the fields behind a move's speed. Needs PSPRECOMP_FIGHTER_LOG=1 as well.
bool g_fighter_dump = false;
int g_fighter_dump_left = 400000;
uint32_t g_dump_prev[4][0x1400 / 4] = {};
uint32_t g_dump_prev_actor[4] = {};
void fighter_dump_frame() {
    if (!g_fighter_dump) return;
    static uint32_t ranges[][2] = {{0x20, 0x140}, {0x180, 0x200}, {0x380, 0x3A0}, {0x540, 0x600}, {0x1A0, 0x1C0}};
    static bool ranged = false;
    if (!ranged) {   // PSPRECOMP_FIGHTER_DUMP_RANGE=<lo>-<hi> (hex actor offsets, below 0x700): dump only that range instead
        ranged = true;
        unsigned lo = 0, hi = 0;
        const char* e = std::getenv("PSPRECOMP_FIGHTER_DUMP_RANGE");
        if (e && std::sscanf(e, "%x-%x", &lo, &hi) == 2 && lo < hi && hi <= 0x1400) { ranges[0][0] = lo; ranges[0][1] = hi; for (int k = 1; k < 5; k++) ranges[k][0] = ranges[k][1] = 0; }
    }
    for (int i = 0; i < 4 && g_fighter_dump_left > 0; i++) {
        const uint32_t a = g_fighters[i];
        if (!a || !ok(a)) continue;
        const uint32_t scr = r32(a + 0x540u) & 0xFFFFu;
        if (scr == 0xFFFFu && !g_test_on) continue;
        const bool fresh = g_dump_prev_actor[i] != a;
        g_dump_prev_actor[i] = a;
        for (const auto& rg : ranges)
            for (uint32_t off = rg[0]; off < rg[1] && g_fighter_dump_left > 0; off += 4) {
                const uint32_t v = r32(a + off);
                uint32_t& prev = g_dump_prev[i][off / 4];
                if (!fresh && v != prev) {
                    float fo, fn; const uint32_t po = prev; std::memcpy(&fo, &po, 4); std::memcpy(&fn, &v, 4);
                    std::fprintf(stderr, "[FDUMP] t=%llu %08X scr=%u +%03X %08X -> %08X (%g -> %g)\n", (unsigned long long)g_tick, a, scr, off, po, v, fo, fn);
                    g_fighter_dump_left--;
                }
                prev = v;
            }
    }
}
// PSPRECOMP_ASSIST_GAUGE=1 (with FIGHTER_LOG): logs each fighter's assist gauge when it changes. The gauge is a float 0..10000 at
// [ctrl+0x52C]+0x18 (ctrl = [[0x089D6A10]+56+4*side]; the HUD reads it in ovl FUN_089E97FC: value x 100 / 10000 = percent). With =2 the write watch is armed on the
// first gauge word that becomes non-zero ([MEM_WATCH] lines: who changes it).

uint32_t assist_gauge_addr(int side) {   // guest address of a side's assist gauge float, 0 if not there
    const uint32_t base = r32(0x089D6A10u);
    if (!base || !ok(base) || side < 0 || side > 1) return 0;
    const uint32_t pc = r32(base + 56u + 4u * static_cast<uint32_t>(side));
    if (!pc || !ok(pc)) return 0;
    const uint32_t gp = r32(pc + 0x52Cu);
    return gp && ok(gp) ? gp + 0x18u : 0;
}
void assist_gauge_log() {
    static const char* e = std::getenv("PSPRECOMP_ASSIST_GAUGE");
    if (!e || !e[0] || e[0] == '0') return;
    static float last[2] = {-1, -1};
    static int armed = 0;
    const uint32_t base = r32(0x089D6A10u);   // per-side controller table (ovl FUN_089E95B0(side) = [base + 56 + 4 * side])
    if (!base || !ok(base)) return;
    for (int i = 0; i < 2; i++) {
        const uint32_t pc = r32(base + 56u + 4u * i);
        if (!pc || !ok(pc)) continue;
        const uint32_t gp = r32(pc + 0x52Cu);
        if (!gp || !ok(gp)) continue;
        const float v = rdf(gp + 0x18u);
        if (v == last[i]) continue;
        const bool changed = last[i] >= 0;
        last[i] = v;
        std::fprintf(stderr, "[ASSIST] gt=%.0f side=%d ctrl=%08X gauge=%.2f addr=%08X step=%.3f\n", g_gt_ms, i, pc, v, gp + 0x18u, rdf(FRAME_STEP));
        if (e[0] == '2' && !armed && g_test_on && changed && g_gt_ms > 500) { armed = 1; psp_mem_watch_set(gp + 0x18u); }
    }
}
void fighter_log_frame() {
    if (!g_fighter_log) return;
    for (int i = 0; i < 12 && g_object_log_left > 0; i++) {   // motion time of the other actors, whenever it changes
        const uint32_t a = g_objects[i];
        if (!a || !ok(a)) continue;
        const float t = rdf(a + 0x1A8u);   // motion player (actor+0x130) time
        if (t == g_object_time[i]) continue;
        g_object_time[i] = t;
        g_object_log_left--;
        std::fprintf(stderr, "[MOTION] gt=%.0f pos=%.3f,%.3f,%.3f t=%llu %08X kind=%u step=%.3f time=%.3f prev=%.3f end=%.3f len=%.3f scr=%u\n", g_gt_ms, rdf(a + 0x110u), rdf(a + 0x114u), rdf(a + 0x118u), (unsigned long long)g_tick, a,
                     (unsigned)g_ram[(a + 0x542u) & 0x07FFFFFFu], rdf(FRAME_STEP), t, rdf(a + 0x1A4u), rdf(a + 0x1A0u), rdf(a + 0x1BCu),
                     r32(a + 0x540u) & 0xFFFFu);
    }
    fighter_dump_frame();
    assist_gauge_log();
    if (g_fighter_log_left <= 0) return;
    for (uint32_t a : g_fighters) {
        if (!a || !ok(a)) continue;
        const uint32_t scr = r32(a + 0x540u) & 0xFFFFu;
        if (scr == 0xFFFFu && !g_test_on) continue;   // test runs log every frame (the rush of a move can run with no action script id)
        g_fighter_log_left--;
        std::fprintf(stderr, "[FIGHTER] gt=%.0f t=%llu %08X scr=%u cur=%d mt=%.3f step=%.3f pos=%.3f,%.3f y=%.3f floor=%.3f fl=%08X f110=%.3f f114=%.3f f134=%.3f f0F4=%.3f f104=%.3f f144=%.3f vel=%.3f,%.3f,%.3f\n",
                     g_gt_ms, (unsigned long long)g_tick, a, scr, (int)(int16_t)(r32(a + 0x180u) & 0xFFFFu), rdf(a + 0x1A8u), rdf(FRAME_STEP), rdf(a + 0x110u), rdf(a + 0x118u), rdf(a + 0x34u), rdf(a + 0x2E4u), r32(a + 0x380u), rdf(a + 0x110u),
                     rdf(a + 0x114u), rdf(a + 0x134u), rdf(a + 0xF4u), rdf(a + 0x104u), rdf(a + 0x144u), rdf(a + 0xE0u), rdf(a + 0xE4u),
                     rdf(a + 0xE8u));
    }
}
int g_actor_log_left = 4000;
struct ActorSeen { uint32_t actor; float res; int own; };
ActorSeen g_actor_seen[24];

void actor_dt_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]);
    g_orig_actor_dt(rdram, ctx);
    bool own = false;
    if (ok(actor)) own = ((r32(actor + 0x388u) & r32(OWN_MASK0)) | (r32(actor + 0x38Cu) & r32(OWN_MASK1))) != 0;
    if (ok(actor)) {   // remember the fighters (kind 0: camera.cpp pivots on one) and, for the log, the other actors
        const bool fighter = g_ram[(actor + 0x542u) & 0x07FFFFFFu] == 0;
        if (fighter) {
            int slot = -1;
            for (int i = 0; i < 4; i++) if (g_fighters[i] == actor) slot = i;
            if (slot < 0) for (int i = 0; i < 4 && slot < 0; i++) if (!g_fighters[i] || g_tick - g_fighter_seen[i] > 120) slot = i;   // free or stale
            if (slot >= 0) { g_fighters[slot] = actor; g_fighter_seen[slot] = g_tick; }
        } else if (g_fighter_log) {
            bool known = false;
            for (uint32_t f : g_objects) known |= f == actor;
            for (uint32_t& f : g_objects) if (!known && !f) { f = actor; known = true; }
        }
    }
    const float before = ctx->f[0];
    if (g_test_freeze_cpu && g_gt_started && ok(actor) && g_ram[(actor + 0x542u) & 0x07FFFFFFu] == 0 && actor != test_player()) { ctx->f[0] = 0.f; return; }
    if (g_ticks != 2.0f && ok(actor) && own) ctx->f[0] *= g_ticks * 0.5f;   // own clock: a fixed 2.0 per frame
    if (g_actor_log && g_actor_log_left > 0 && ok(actor)) {
        ActorSeen* slot = nullptr;
        for (auto& a : g_actor_seen) if (a.actor == actor) { slot = &a; break; }
        if (!slot) for (auto& a : g_actor_seen) if (!a.actor) { slot = &a; a.res = -1.f; a.own = -1; a.actor = actor; break; }
        if (slot && (slot->res != ctx->f[0] || slot->own != (own ? 1 : 0))) {
            slot->res = ctx->f[0]; slot->own = own ? 1 : 0;
            g_actor_log_left--;
            std::fprintf(stderr, "[ACTOR] %08X kind=%u master=%08X own=%d clock=%.3f ownclock=%.3f game=%.3f ours=%.3f step=%.3f ticks=%.3f overlay=%u\n",
                         actor, (unsigned)(g_ram[(actor + 0x542u) & 0x07FFFFFFu]), r32(actor + 0x1108u), own ? 1 : 0, rdf(actor + 0x3C8u),
                         rdf(actor + 0x3CCu), before, ctx->f[0], rdf(FRAME_STEP), g_ticks, overlay_id());
        }
    }
}

void cam_dt_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t id = overlay_id();
    if (id != 1) {   // the same address in another overlay is another function
        FuncPtr f = id == 2 ? ovl_menu_lookup(CAM_DT) : id == 3 ? ovl_extra_lookup(CAM_DT) : nullptr;
        if (f) f(rdram, ctx);
        return;
    }
    const uint32_t cam = static_cast<uint32_t>(ctx->r[4]);
    g_orig_cam_dt(rdram, ctx);
    if (g_ticks != 2.0f && ok(cam) && r32(cam + 0x100u) == 2u) ctx->f[0] *= g_ticks * 0.5f;
}


// EX Burst controller (FUN_08A3FE10, called once per update from FUN_088413D0): every phase of the burst runs from it, the
// button prompt (quicktime) and the release gauge (FUN_08A3E4A8: position += velocity per call) among them, with fixed per-call
// steps and integer counters that do not scale with dt, so at 60 / 120 Hz the prompt and the pointer ran 2x / 4x too fast.
// Run it once per 30 Hz frame's worth of time (an accumulator of ticks / 2 per update, held updates skipped) with the global
// step scaled to what a 30 Hz frame has (2.0 normally, the slow-motion value otherwise) for the call.
// PSPRECOMP_EXB30=0 turns it off.
bool g_exb30 = true;
// The controller reads 'pressed this update' edges (FUN_08B326C4(actor)), which last one update. With the controller run on one
// update in 2 / 4, an edge on a held update would be lost (the burst waited for a press it never saw, and timing felt off).
// While held, every actor the controller asked about is polled and the edge kept until the next controller run.
bool g_exb_in = false;
uint32_t g_exb_args[8] = {};
bool g_exb_sticky[8] = {};
int g_exb_nargs = 0;
void exb_pressed_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t id = overlay_id();
    if (id != 1) {
        FuncPtr f = id == 2 ? ovl_menu_lookup(EXB_PRESSED) : id == 3 ? ovl_extra_lookup(EXB_PRESSED) : nullptr;
        if (f) f(rdram, ctx);
        return;
    }
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]);
    g_orig_exb_pressed(rdram, ctx);
    if (!g_exb_in) return;
    int i = 0;
    while (i < g_exb_nargs && g_exb_args[i] != actor) i++;
    if (i == g_exb_nargs && i < 8) { g_exb_args[i] = actor; g_exb_sticky[i] = false; g_exb_nargs++; }
    if (i < 8) {
        if (g_exb_sticky[i]) ctx->r[2] = 1;
        g_exb_sticky[i] = false;
    }
}
void exb_poll_held(uint8_t* rdram, recomp_context* ctx) {
    for (int i = 0; i < g_exb_nargs; i++) {
        recomp_context tmp = *ctx;
        tmp.r[4] = static_cast<int32_t>(g_exb_args[i]);
        g_orig_exb_pressed(rdram, &tmp);
        if (tmp.r[2] != 0) g_exb_sticky[i] = true;
    }
}
// PSPRECOMP_EXB_LOG=1: one line per controller update (before the gate) while a burst is active: phase (+632), phase timer (+636),
// +644 flag, the global step, slow-motion object value / time left, whether the update ran the controller.
void exb_log(const char* what, uint32_t ctl, bool ran) {
    static const bool on = std::getenv("PSPRECOMP_EXB_LOG") != nullptr;
    if (!on || !ok(ctl)) return;
    const uint32_t ph = r32(ctl + 632u);
    static uint32_t last_ph = 0xFFFFFFFFu;
    static int n = 0;
    if (ph == 0 && last_ph == 0) return;
    const uint32_t sm = r32(SLOWMO_PTR);
    if (ph != last_ph) n = 0;
    last_ph = ph;
    if (n++ > 400) return;
    std::fprintf(stderr, "[EXB] gt=%.0f %s ran=%d phase=%u t636=%d f644=%u pointer=(pos %.2f vel %.2f held %u) step=%.3f slow=(%.3f,%.3f) ticks=%.2f\n", g_gt_ms, what, ran ? 1 : 0, ph,
                 static_cast<int>(r32(ctl + 636u)), r32(ctl + 644u), rdf(ctl + 596u), rdf(ctl + 604u), r32(ctl + 593u) & 0xFFu, rdf(FRAME_STEP), ok(sm) ? rdf(sm) : -1.f, ok(sm) ? rdf(sm + 4u) : -1.f, g_ticks);
}
// Log only (PSPRECOMP_EXB_LOG=1): the prompt object (r4+20 -> state words +0 state, +4 stage, +8 / +12 button counters, +16 / +20 gauges, +28 timer).
void exb_prompt_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t id = overlay_id();
    if (id != 1) {
        FuncPtr f = id == 2 ? ovl_menu_lookup(EXB_PROMPT) : id == 3 ? ovl_extra_lookup(EXB_PROMPT) : nullptr;
        if (f) f(rdram, ctx);
        return;
    }
    static const bool on = std::getenv("PSPRECOMP_EXB_LOG") != nullptr;
    const uint32_t self = static_cast<uint32_t>(ctx->r[4]);
    g_orig_exb_prompt(rdram, ctx);
    if (!on || !ok(self)) return;
    const uint32_t o = r32(self + 20u);
    if (!ok(o)) return;
    static int n = 0;
    static uint32_t last_state = 0xFFFFFFFFu, last_stage = 0xFFFFFFFFu;
    const uint32_t st = r32(o), sg = r32(o + 4u);
    if (st != last_state || sg != last_stage) n = 0;
    last_state = st; last_stage = sg;
    if (n++ > 150) return;
    std::fprintf(stderr, "[EXBP] state=%u stage=%u cnt=(%u,%u) g=(%.1f,%.1f) timer=%.2f step=%.3f ticks=%.2f\n", st, sg, r32(o + 8u), r32(o + 12u),
                 rdf(o + 16u), rdf(o + 20u), rdf(o + 28u), rdf(FRAME_STEP), g_ticks);
}
// The quicktime that starts the burst (FUN_08A40E60 and its 08A41558 / 08A41774 states) is the same kind of code: fixed steps per call,
// no dt. Same treatment as the controller (PSPRECOMP_EXB_UI=0 turns it off).
// Scales the frame time the gated EX Burst code reads: the global step and the frame timer's dt (+0x18, what menus and 2D code
// use, e.g. the countdown of the quicktime), both to what a 30 Hz frame has.
struct ExbStep {
    float step, dt; uint32_t timer;
    ExbStep() : step(rdf(FRAME_STEP)), dt(0.f), timer(r32(TIMER_PTR)) {
        const float k = 2.f / g_ticks;
        wrf(FRAME_STEP, step * k);
        if (ok(timer)) { dt = rdf(timer + 0x18u); wrf(timer + 0x18u, dt * k); }
    }
    ~ExbStep() { wrf(FRAME_STEP, step); if (ok(timer)) wrf(timer + 0x18u, dt); }
};
bool g_exb_ui = true;
void exb_ui_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t id = overlay_id();
    if (id != 1) {
        FuncPtr f = id == 2 ? ovl_menu_lookup(EXB_UI) : id == 3 ? ovl_extra_lookup(EXB_UI) : nullptr;
        if (f) f(rdram, ctx);
        return;
    }
    static float acc = 1.f;
    if (g_ticks == 2.0f || !g_exb_ui || !g_orig_exb_pressed || g_exb_in) { g_orig_exb_ui(rdram, ctx); return; }   // inside a gated call: already at 30 Hz, do not gate twice
    acc += g_ticks * 0.5f;
    if (acc < 1.f - 1e-4f) { exb_poll_held(rdram, ctx); return; }
    acc -= 1.f;
    if (acc > 1.f) acc = 0.f;
    ExbStep scaled;
    const bool was_in = g_exb_in;
    g_exb_in = true;
    g_orig_exb_ui(rdram, ctx);
    g_exb_in = was_in;
}
void exb_ctrl_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t id = overlay_id();
    if (id != 1) {
        FuncPtr f = id == 2 ? ovl_menu_lookup(EXB_CTRL) : id == 3 ? ovl_extra_lookup(EXB_CTRL) : nullptr;
        if (f) f(rdram, ctx);
        return;
    }
    static float acc = 1.f;
    const uint32_t ctl = static_cast<uint32_t>(ctx->r[4]);
    if (g_ticks == 2.0f || !g_exb30 || !g_orig_exb_pressed || g_exb_in) { exb_log("all", ctl, true); g_orig_exb_ctrl(rdram, ctx); return; }   // g_exb_in: called from the gated UI, already at 30 Hz
    acc += g_ticks * 0.5f;
    if (acc < 1.f - 1e-4f) { exb_poll_held(rdram, ctx); exb_log("held", ctl, false); return; }
    exb_log("run ", ctl, true);
    acc -= 1.f;
    if (acc > 1.f) acc = 0.f;
    ExbStep scaled;
    g_exb_in = true;
    g_orig_exb_ctrl(rdram, ctx);
    g_exb_in = false;
}

// ---- Assist gauge ---------------------------------------------------------------------------------------------------------
// ovl FUN_08AEC1EC(this, ctrl-ish, float amount in f12): gauge (+0x18 of [ctrl+0x52C]) = clamp(gauge + amount, 0, 10000).
// PSPRECOMP_ASSIST_GAUGE=1 logs every call: amount and the return address (who adds).
constexpr uint32_t GAUGE_ADD = 0x08AEC1ECu;
FuncPtr g_orig_gauge_add = nullptr;
void gauge_add_hook(uint8_t* rdram, recomp_context* ctx) {
    static const char* e = std::getenv("PSPRECOMP_ASSIST_GAUGE");
    if (e && e[0] && e[0] != '0') {
        static int left = 20000;
        if (left > 0) { left--; std::fprintf(stderr, "[GAUGE_ADD] gt=%.0f amount=%.3f ra=%08X this=%08X step=%.3f\n", g_gt_ms, ctx->f[12], (unsigned)ctx->r[31], (unsigned)ctx->r[4], rdf(FRAME_STEP)); }
    }
    g_orig_gauge_add(rdram, ctx);
}

// ovl FUN_08AEC7D8(this): per update, for each assist on the field, gauge -= table * 0.01 * K (no frame step in sight).
constexpr uint32_t GAUGE_DRAIN = 0x08AEC7D8u;
FuncPtr g_orig_gauge_drain = nullptr;
uint32_t gauge_of_this(uint32_t self) {
    if (!self || !ok(self)) return 0;
    const uint32_t pc = r32(self + 8u);
    if (!pc || !ok(pc)) return 0;
    const uint32_t gp = r32(pc + 0x52Cu);
    return gp && ok(gp) ? gp + 0x18u : 0;
}
void gauge_drain_hook(uint8_t* rdram, recomp_context* ctx) {
    static const char* e = std::getenv("PSPRECOMP_ASSIST_GAUGE");
    const bool log = e && e[0] && e[0] != '0';
    const uint32_t self = static_cast<uint32_t>(ctx->r[4]);
    const uint32_t ga = log ? gauge_of_this(self) : 0;
    const float before = ga ? rdf(ga) : 0.f;
    g_orig_gauge_drain(rdram, ctx);
    if (ga) {
        static int left = 20000;
        const float after = rdf(ga);
        if (after != before && left > 0) { left--; std::fprintf(stderr, "[GAUGE_DRAIN] gt=%.0f %.3f -> %.3f (%.3f) step=%.3f\n", g_gt_ms, before, after, after - before, rdf(FRAME_STEP)); }
    }
}

// ovl FUN_089E97FC(side): the HUD's read of the gauge, returns the percent in f0. PSPRECOMP_ASSIST_GAUGE=1 logs it when it changes.
constexpr uint32_t GAUGE_HUD = 0x089E97FCu;
FuncPtr g_orig_gauge_hud = nullptr;
void gauge_hud_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t side = static_cast<uint32_t>(ctx->r[4]);
    g_orig_gauge_hud(rdram, ctx);
    static const char* e = std::getenv("PSPRECOMP_ASSIST_GAUGE");
    if (e && e[0] && e[0] != '0' && side < 2) {
        static float last[2] = {-1, -1};
        static int left = 20000;
        if (ctx->f[0] != last[side] && left > 0) {
            left--;
            const uint32_t ga = assist_gauge_addr(static_cast<int>(side));
            std::fprintf(stderr, "[GAUGE_HUD] gt=%.0f side=%u pct=%.0f raw=%.2f addr=%08X step=%.3f\n", g_gt_ms, side, ctx->f[0], ga ? rdf(ga) : -1.f, ga, rdf(FRAME_STEP));
        }
        last[side] = ctx->f[0];
    }
}

// ---- Effect components ------------------------------------------------------------------------------------------
// The effect runtime advances its components by dt = instance+352 (the global step x the handle's scale, written by
// FUN_0894B48C): 2.0 per update on the PSP, 1.0 per update at 60 Hz. Most components scale by it. These step once
// per *update* instead (F.md, fx.md item 7), so at 60 Hz they ran twice as fast / lived half as long:
//   ch61  envelope     m2 0x08953A50  mult += rate per update while timer += 0.5 dt: each ramp lasts twice as many
//                                     updates with the same rate, overshoots 2x, and ch62's clamp makes it strobe
//   ch95  points       m2 0x0897F3E4  life -1, motion and history per update; only emission (acc += rate dt) by dt
//   ch100 streaks      m2 0x0897DE38  (-> FUN_08984508: emission by dt, FUN_08984B40 steps the sparks per update)
//   ch102 line burst   m2 0x08974DCC  as ch95
//   ch103 line rays    m2 0x089760E8  as ch95
//   ch68  trail path   m2 0x08966018  (-> FUN_0896B710) one ring-buffer sample per update: ch80 ribbons half as long
//   ch70  smoke path   m2 0x08967438  (-> FUN_0896C9B0) shift + drift per update
//   ch105 locus UVs    m2 0x08964B40  u -= P+48, v -= P+52 per update
// At 60 Hz each runs on every other update of its own state block (a3), with instance+352 doubled for the call: on
// those updates it does exactly what the PSP does on a 30 Hz frame (dt 2, one step), and in between it holds, as the
// PSP's picture does. Emitters read dt through the current runner (*0x09B6F478 +136), which is the same instance;
// both are scaled (once each) to be safe.
//   ch106 locus history (FUN_0896808C, from m2 0x089672C4; the weapon / body trails): `t += dt`, one shift
// (pos[i] = pos[i-1] + vel[i-1]) when t >= last + 1 -- at dt 1 that is every update, so the trail was half as long --
// and pos[0] = the current point on every update. Here the shift updates run with dt x 2; the updates between run
// with dt 0 (no shift, t unchanged, but pos[0] still follows the bone, so the trail head stays on the weapon); and
// before each shift update pos[0] of both point buffers (object +0 / +4, n x {vec4 pos, vec4 vel}) is put back to
// the point of the previous shift update, so the shift copies the sample the PSP would have. Wrapped at the inner
// function so roster.cpp's lingering-trail hook on the m2 still decides first whether it runs at all.
// A state block whose memory is reused by a new element within one update of the old one's last step only delays
// the new one's first step by one 60 Hz update.
constexpr uint32_t CUR_RUNNER = 0x09B6F478u;   // EBOOT: runner being updated (+136 instance); read by the emitters
constexpr uint32_t LOCUS_HIST = 0x0896808Cu;   // EBOOT: ch106 history update (a0 object, a1 runner, a2 params)
struct FxHookDef { uint32_t addr; const char* what; };
constexpr FxHookDef k_fx_hooks[] = {
    {0x08953A50u, "ch61 envelope"},   {0x0897F3E4u, "ch95 points"},     {0x0897DE38u, "ch100 streaks"},
    {0x08974DCCu, "ch102 lines"},     {0x089760E8u, "ch103 rays"},      {0x08966018u, "ch68 trail path"},
    {0x08967438u, "ch70 smoke path"}, {0x08964B40u, "ch105 locus UVs"},
};
constexpr int kFxHooks = static_cast<int>(sizeof(k_fx_hooks) / sizeof(k_fx_hooks[0]));
FuncPtr g_fx_orig[kFxHooks] = {};
FuncPtr g_orig_locus_hist = nullptr;
bool g_fx60 = true;

struct FxStep { uint64_t tick = 0; int phase = 0; bool ran = false, have = false; uint8_t pos0[32] = {}; };
std::unordered_map<uint32_t, FxStep> g_fx_step;
uint64_t g_fx_pruned = 0;
int g_fx_runs = 0, g_fx_holds = 0;

// Run this update? One update in every g_fx_period (2 at 60 Hz, 3 at 90 Hz, 4 at 120 Hz...) per state block; a new
// block, or one not seen on the previous update, runs at once. A second call in the same frame repeats the decision.
bool fx_step_due(FxStep& s) {
    if (s.tick == g_tick) return s.ran;
    if (s.tick + 1 == g_tick) s.phase = (s.phase + 1) % (g_fx_period > 1 ? g_fx_period : 1);
    else s.phase = 0;
    s.tick = g_tick;
    s.ran = s.phase == 0;
    return s.ran;
}
FxStep& fx_step_of(uint32_t key) {
    if (g_tick - g_fx_pruned >= 1024) {   // forget blocks not seen for a while (ended effects)
        g_fx_pruned = g_tick;
        for (auto it = g_fx_step.begin(); it != g_fx_step.end();) it = g_tick - it->second.tick > 8 ? g_fx_step.erase(it) : std::next(it);
    }
    return g_fx_step[key];
}
// Scale instance+352 of the runner's instance (and the current runner's, if different) for one call.
struct DtScale {
    uint32_t addr[2] = {};
    float was[2] = {};
    int n = 0;
    DtScale(uint32_t runner, float k) {
        const uint32_t cur = r32(CUR_RUNNER);
        const uint32_t inst[2] = {ok(runner) ? r32(runner + 136u) : 0u, ok(cur) ? r32(cur + 136u) : 0u};
        for (uint32_t in : inst) {
            if (!ok(in) || (n && addr[0] == in + 352u)) continue;
            addr[n] = in + 352u;
            was[n] = rdf(addr[n]);
            wrf(addr[n], was[n] * k);
            n++;
        }
    }
    ~DtScale() { for (int i = n - 1; i >= 0; i--) wrf(addr[i], was[i]); }
};
bool fx_fix_on() { return g_rate > 30 && g_fx60; }

template <int I>
void fx_every_other_hook(uint8_t* rdram, recomp_context* ctx) {
    if (!fx_fix_on()) { g_fx_orig[I](rdram, ctx); return; }
    if (!fx_step_due(fx_step_of(static_cast<uint32_t>(ctx->r[7])))) { g_fx_holds++; return; }
    g_fx_runs++;
    DtScale dt(static_cast<uint32_t>(ctx->r[5]), 2.0f);
    g_fx_orig[I](rdram, ctx);
}
template <int... I> struct FxHookFns { static constexpr FuncPtr fns[] = {fx_every_other_hook<I>...}; };
using FxFns = FxHookFns<0, 1, 2, 3, 4, 5, 6, 7>;
static_assert(sizeof(FxFns::fns) / sizeof(FxFns::fns[0]) == kFxHooks, "one hook function per address");

void locus_hist_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t obj = static_cast<uint32_t>(ctx->r[4]);
    if (!fx_fix_on() || !ok(obj)) { g_orig_locus_hist(rdram, ctx); return; }
    FxStep& s = fx_step_of(obj);
    const bool first = g_ram[(obj + 8u) & 0x07FFFFFFu] != 0;   // the history is (re)filled on its next shift
    bool run = fx_step_due(s);
    if (first && !run) run = s.ran = true;
    const uint32_t a = r32(obj), b = r32(obj + 4u);
    const bool bufs = ok(a) && ok(b);
    if (!run) {
        g_fx_holds++;
        DtScale dt(static_cast<uint32_t>(ctx->r[5]), 0.0f);
        g_orig_locus_hist(rdram, ctx);
        return;
    }
    g_fx_runs++;
    if (bufs && s.have && !first) {
        std::memcpy(g_ram + (a & 0x07FFFFFFu), s.pos0, 16);
        std::memcpy(g_ram + (b & 0x07FFFFFFu), s.pos0 + 16, 16);
    }
    {
        DtScale dt(static_cast<uint32_t>(ctx->r[5]), 2.0f);
        g_orig_locus_hist(rdram, ctx);
    }
    s.have = bufs;
    if (bufs) {
        std::memcpy(s.pos0, g_ram + (a & 0x07FFFFFFu), 16);
        std::memcpy(s.pos0 + 16, g_ram + (b & 0x07FFFFFFu), 16);
    }
}

void fx_log_window() {
    if (!g_fx_runs && !g_fx_holds) return;
    std::fprintf(stderr, "[FPS] effect components stepped at 30 Hz: %d steps, %d held updates, %zu state blocks\n", g_fx_runs,
                 g_fx_holds, g_fx_step.size());
    g_fx_runs = g_fx_holds = 0;
}

void fx_install() {
    const char* e = std::getenv("PSPRECOMP_FX60");
    g_fx60 = !(e && e[0] == '0');
    if (const char* x = std::getenv("PSPRECOMP_EXB30"); x && x[0] == '0') g_exb30 = false;
    int n = 0;
    for (int i = 0; i < kFxHooks; i++) {
        g_fx_orig[i] = psp_dispatch_probe_lookup(k_fx_hooks[i].addr);
        if (g_fx_orig[i]) { psp_dispatch_register(k_fx_hooks[i].addr, FxFns::fns[i]); n++; }
        else std::fprintf(stderr, "[FPS] effect hook %s NOT installed (no function at %08X)\n", k_fx_hooks[i].what, k_fx_hooks[i].addr);
    }
    g_orig_locus_hist = psp_dispatch_probe_lookup(LOCUS_HIST);
    if (g_orig_locus_hist) { psp_dispatch_register(LOCUS_HIST, locus_hist_hook); n++; }
    else std::fprintf(stderr, "[FPS] effect hook ch106 locus history NOT installed (no function at %08X)\n", LOCUS_HIST);
    std::fprintf(stderr, "[FPS] %d/%d effect component hooks installed (60 Hz: stepped at 30 Hz)%s\n", n, kFxHooks + 1,
                 g_fx60 ? "" : "; off (PSPRECOMP_FX60=0)");
}

int fps_get() { return g_target.load(); }
void fps_set(int fps) {
    fps = fps < 30 ? 30 : fps > 240 ? 240 : (fps + 15) / 30 * 30;   // a multiple of 30
    if (g_target.exchange(fps) != fps) std::fprintf(stderr, "[FPS] target frame rate %d\n", fps);
    psp_adhoc_set_local_fps(fps);   // told to the other players
}
const int k_rates[] = {30, 60, 90, 120, 150, 180, 210, 240};

}  // namespace

// The position (actor+0x80, vec3) of the fighter nearest to `eye` among those that were updated in the last two
// seconds, for camera.cpp: the game's own camera sits behind the fighter you control, so that is the one the free camera
// pivots on. false when no fighter is known.
bool dissidia_nearest_fighter(const float eye[3], float pos[3]) {
    float best = 1e30f;
    bool found = false;
    for (int i = 0; i < 4; i++) {
        const uint32_t a = g_fighters[i];
        if (!a || !ok(a) || g_tick - g_fighter_seen[i] > 120) continue;
        float p[3] = {rdf(a + 0x80u), rdf(a + 0x84u), rdf(a + 0x88u)};
        const float d = (p[0] - eye[0]) * (p[0] - eye[0]) + (p[1] - eye[1]) * (p[1] - eye[1]) + (p[2] - eye[2]) * (p[2] - eye[2]);
        if (d < best) { best = d; found = true; pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2]; }
    }
    return found;
}

// Ticks (1/60 s) one game frame advances: 2 at 30 Hz, 1 at 60 Hz, 2/3 at 90 Hz, 0.5 at 120 Hz.
float dissidia_frame_ticks() { return g_ticks; }
// Factor for a value the game keeps as "ticks per 30 Hz frame" (the actor own clock): 1 at 30 Hz, 0.5 at 60 Hz.
float dissidia_own_clock_scale() { return g_ticks * 0.5f; }

void dissidia_framerate_install(uint8_t* rdram) {
    g_ram = rdram;
    const char* e = std::getenv("PSPRECOMP_FPS");
    if (e && std::atoi(e) >= 30) g_target = (std::min(std::atoi(e), 240) + 15) / 30 * 30;
    g_scope_all = (e = std::getenv("PSPRECOMP_FPS_SCOPE")) && std::strcmp(e, "all") == 0;
    g_log = (e = std::getenv("PSPRECOMP_FPS_LOG")) && e[0] && e[0] != '0';
    g_actor_log = (e = std::getenv("PSPRECOMP_ASSIST_LOG")) && e[0] && e[0] != '0';
    g_fighter_log = (e = std::getenv("PSPRECOMP_FIGHTER_LOG")) && e[0] && e[0] != '0';
    if ((e = std::getenv("PSPRECOMP_PRESS_LATCH")) && e[0]) { g_press_ms = float(std::atof(e)); if (g_press_ms == 1.0f) g_press_ms = 33.4f; }
    g_press_latch = g_press_ms > 0.5f;
    testdrive_install();
    g_fighter_dump = g_fighter_log && (e = std::getenv("PSPRECOMP_FIGHTER_DUMP")) && e[0] && e[0] != '0';
    g_orig_frame_end = psp_dispatch_probe_lookup(FRAME_END);
    g_orig_pad = psp_dispatch_probe_lookup(PAD_SET);
    g_orig_actor_dt = psp_dispatch_probe_lookup(ACTOR_DT);
    g_orig_cam_dt = ovl_battle_lookup(CAM_DT);
    g_orig_pressed = ovl_battle_lookup(PRESSED);
    if (!g_orig_frame_end || !g_orig_pad || !g_orig_actor_dt || !g_orig_cam_dt) {
        std::fprintf(stderr, "[FPS] hooks NOT installed (frame end %p pad %p actor dt %p camera dt %p): 30 Hz only\n",
                     (void*)g_orig_frame_end, (void*)g_orig_pad, (void*)g_orig_actor_dt, (void*)g_orig_cam_dt);
        return;
    }
    psp_dispatch_register(FRAME_END, frame_end_hook);
    psp_dispatch_register(PAD_SET, pad_set_hook);
    psp_dispatch_register(ACTOR_DT, actor_dt_hook);
    psp_dispatch_register(CAM_DT, cam_dt_hook);
    if ((g_orig_exb_pressed = ovl_battle_lookup(EXB_PRESSED))) psp_dispatch_register(EXB_PRESSED, exb_pressed_hook);
    if (std::getenv("PSPRECOMP_EXB_LOG") && (g_orig_exb_prompt = ovl_battle_lookup(EXB_PROMPT))) psp_dispatch_register(EXB_PROMPT, exb_prompt_hook);
    if (const char* x = std::getenv("PSPRECOMP_EXB_UI"); x && x[0] == '0') g_exb_ui = false;
    if ((g_orig_exb_ui = ovl_battle_lookup(EXB_UI))) psp_dispatch_register(EXB_UI, exb_ui_hook);
    if ((g_orig_exb_ctrl = ovl_battle_lookup(EXB_CTRL))) psp_dispatch_register(EXB_CTRL, exb_ctrl_hook);
    else std::fprintf(stderr, "[FPS] EX Burst controller hook NOT installed\n");
    if ((g_orig_gauge_add = ovl_battle_lookup(GAUGE_ADD))) psp_dispatch_register(GAUGE_ADD, gauge_add_hook);
    if ((g_orig_gauge_drain = ovl_battle_lookup(GAUGE_DRAIN))) psp_dispatch_register(GAUGE_DRAIN, gauge_drain_hook);
    if ((g_orig_gauge_hud = ovl_battle_lookup(GAUGE_HUD))) psp_dispatch_register(GAUGE_HUD, gauge_hud_hook);
    if (g_orig_pressed) psp_dispatch_register(PRESSED, pressed_hook);
    std::fprintf(stderr, "[FPS] press latch %s (pressed-test hook %s)\n", g_press_latch ? "on" : "off", g_orig_pressed ? "installed" : "NOT FOUND");
    if ((g_orig_rootmotion = psp_dispatch_probe_lookup(ROOT_MOTION))) psp_dispatch_register(ROOT_MOTION, root_motion_hook);
    else std::fprintf(stderr, "[FPS] root motion hook NOT installed (no function at %08X)\n", ROOT_MOTION);
    if (const char* us = std::getenv("PSPRECOMP_UI_STEP"); us && us[0] == '0') g_ui_step = false;
    if ((g_orig_ui_param = psp_dispatch_probe_lookup(UI_PARAM_STEP))) psp_dispatch_register(UI_PARAM_STEP, ui_param_hook);
    else std::fprintf(stderr, "[FPS] UI step hook NOT installed (no function at %08X)\n", UI_PARAM_STEP);
    if ((g_orig_slowmo = psp_dispatch_probe_lookup(SLOWMO_FRAME))) psp_dispatch_register(SLOWMO_FRAME, slowmo_frame_hook);
    else std::fprintf(stderr, "[FPS] slow-motion hook NOT installed (no function at %08X)\n", SLOWMO_FRAME);
    if (const char* sm = std::getenv("PSPRECOMP_STEER_MOVE"); !(sm && sm[0] == '0') && (g_orig_steer_move = ovl_battle_lookup(STEER_MOVE))) { psp_dispatch_register(STEER_MOVE, steer_move_hook); std::fprintf(stderr, "[FPS] steered-move hook installed\n"); }
    if (g_log && (g_orig_skel = psp_dispatch_probe_lookup(SKEL_BUILD))) psp_dispatch_register(SKEL_BUILD, skel_build_hook);
    fx_install();
    psp_settings_set_frame_rate_hook({k_rates, static_cast<int>(std::size(k_rates)), fps_get, fps_set});
    psp_adhoc_set_local_fps(g_target.load());
    std::fprintf(stderr, "[FPS] frame rate %d (battle logic; scope %s)\n", g_target.load(), g_scope_all ? "all" : "battle");
}
