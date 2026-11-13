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
constexpr uint32_t CHAIN_CHECK = 0x08AB81E0u;   // battle overlay: chain check (a0 = chain table, a1 = actor, a2 = u16* requested action id); window = element start <= actor+0x1A8 < end
constexpr uint32_t GATE_37 = 0x08B334D0u, GATE_36 = 0x08B33508u, GATE_3A = 0x08B33544u;   // battle overlay: input-window gates (a0 = actor): +0x195&1 / +0x194&0x80 / +0x195&2, 0 while the lockout latch +0x5B4&2 is set
constexpr uint32_t CHAIN_TABLE_PTR = 0x08AB8388u;   // battle overlay data: pointer to the chain table (the move picker FUN_08848160 loads it as *(0x08AC0000 - 31864))
constexpr uint32_t CPU_TICK = 0x08A64400u;      // battle overlay: brain tick (a0 = brain = actor +0x5C4); NO_AI skips it for CPU brains
constexpr uint32_t CPU_AI = 0x08A75D38u;        // battle overlay: CPU AI action chooser (returns -1 / -4: no action)
constexpr uint32_t STEER_MOVE = 0x08A2C960u;   // battle overlay: ground steered move controller (Squall's Revolver Drive rush): a0 = out vector (metres to move this update)
constexpr uint32_t UI_PARAM_STEP = 0x088DA520u; // EBOOT: UI animation parameter stepper (value += rate x UI step toward a target)
constexpr uint32_t UI_MGR_PTR = 0x089D7830u;    // EBOOT: pointer to the UI manager (+33: use the frame dt as the UI step, +34: paused)
constexpr uint32_t UI_STEP = 0x089D7834u;       // EBOOT: the UI step FUN_088CF550 sets every update (0 paused, 1.0 fixed, or the frame dt)
constexpr uint32_t OWN_MASK0 = 0x089C7FD8u, OWN_MASK1 = 0x089C7FDCu;
constexpr uint32_t EXB_PRESSED = 0x08B326C4u;
constexpr uint32_t EXB_PRESSED2 = 0x08B324CCu;  // the same test for another virtual button (5; 08B326C4 asks for 1): the button prompt (08A3D1C4) counts presses of both   // battle overlay: 'virtual button pressed this update' test of an actor (r4), used by the EX Burst code
constexpr uint32_t EXB_PROMPT = 0x08A3D1C4u;    // battle overlay: EX Burst button prompt update (state machine with a timer at +28; log only)
constexpr uint32_t EXB_UI = 0x08A40E60u;        // battle overlay: EX Burst quicktime UI update (calls the 08A41558 / 08A41774 states)
constexpr uint32_t EXB_CTRL = 0x08A3FE10u;      // battle overlay: EX Burst controller update (prompt, release gauge, slow motion)
constexpr uint32_t CAM_DT = 0x08B3B4FCu;        // battle overlay: a camera's time step

uint8_t* g_ram = nullptr;
bool g_exb_in = false;   // inside a gated EX Burst call (see exb_ctrl_hook)
double g_exb_t0 = 0;   // play time the current burst began (log only)
const bool g_exb_log_on = std::getenv("PSPRECOMP_EXB_LOG") != nullptr;
bool g_exb_poll = false;   // polling the burst's button tests on a held update (exb_poll_held)
FuncPtr g_orig_ui_param = nullptr, g_orig_steer_move = nullptr, g_orig_pressed = nullptr, g_orig_rootmotion = nullptr, g_orig_slowmo = nullptr, g_orig_frame_end = nullptr, g_orig_pad = nullptr, g_orig_actor_dt = nullptr, g_orig_cam_dt = nullptr, g_orig_exb_ctrl = nullptr, g_orig_exb_pressed = nullptr, g_orig_exb_pressed2 = nullptr, g_orig_exb_prompt = nullptr, g_orig_exb_ui = nullptr;
std::atomic<int> g_target{30};     // requested rate (settings / env)
int g_interval = 2;                // interval written on the last frame end (2 = 30 Hz, 1 = 60 Hz and above)
int g_rate = 30;                   // logic rate in effect (30, 60, 90, ... one frame per vblank above 30)
double g_play_ms = 0;              // wall-independent play time: advances by the frame's ticks (every frame, menus and battle)
bool g_input_dbg = false;          // PSPRECOMP_INPUT_DEBUG=1: pressed buttons on screen and in the log
float g_ticks = 2.0f;              // 1/60 s ticks one frame advances: 60 / g_rate
int g_fx_period = 1;               // updates per effect-component step at 30 Hz pacing: g_rate / 30
bool g_scope_all = false, g_log = false;
int g_switch_logs = 0;
int g_rm_log_left = 300;
bool g_fighter_log = false;   // PSPRECOMP_FIGHTER_LOG (see fighter_log_frame)
uint32_t g_prev_scr[4] = {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
bool g_action_new[4] = {false, false, false, false};   // the script id changed during the previous update: this update is the move's first one with a step   // each fighter's script id at the end of the previous update
// 30 Hz grid (Jecht's timed windows): above 30 Hz the human Jecht's Circle / Square are only seen on every P-th update (P = 2 / ticks: 4 at 120 Hz, 2 at 60), counted from
// the first update after his last move change (g_grid_anchor): reads come right after the move's first visible frame and then every 33 ms, as on the PSP. With the
// first-update step (the update after a move starts advances 2 ticks, see actor_dt_hook) the move's clock at read n is 2 + 2n at every rate (30 Hz: 2 ticks per update; 120 Hz: 0.5 + 2.0 + 3 x 0.5 + ...),
// so the windows (clock ranges) are read at the same real times at 30, 60 and 120 Hz.
bool g_jecht_grid = true;   // PSPRECOMP_JECHT_GRID=0 turns the input half off for A/B (the first update step stays)
int grid_period() { return std::max(1, static_cast<int>(2.0f / g_ticks + 0.5f)); }
float first_step() { return 2.0f; }
uint64_t g_grid_anchor = 0;   // g_tick of the first update after Jecht's last move change: the grid is every P-th update counted from here
bool g_dbg_no_ai = false;      // debug menu: Enemy AI off (the CPU brain tick is skipped once the fight has been running a while)
bool g_dbg_combo = false;      // debug menu: Jecht combo timing helper (flash when a chain window opens, messages for early / late presses)
bool g_env_no_ai = false;      // PSPRECOMP_TEST_NO_AI (the test harnesses)
float g_battle_age = 0.f;      // ticks (1/60 s) the battle has been running
bool g_action_start_step = true;   // see actor_dt_hook (PSPRECOMP_ACTION_START_STEP=0 turns it off for A/B)
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
//                                               <ms> sync [timeout] [scr]        hold the script until the player's action (script id) changes (or becomes `scr`); that update is T0
//                                               <ms> batch <file>                hold it until <file> exists, then play (and delete) the lines in it: a driver feeds trials into a running battle
//                                               <ms> waitidle [timeout]          hold it until the player is out of any action (script id 0xFFFF); that update is T0
//                                               <ms> place <m>                   put the player <m> metres from the dummy (same bearing), teleport
//                                               +<ms> <command>                  any command with a '+' time runs <ms> of game time after T0 (a script with such lines is
//                                                                                played in file order). The player's action changes are logged as '[TEST] gt=<ms> scr <old> -> <new>'.
// BUTTONS: CROSS CIRCLE SQUARE TRIANGLE L R UP DOWN LEFT RIGHT START SELECT joined by '+', or a 0x hex mask. The clock is game
// time in ms since the battle began (the first fighter in an action), advanced by 1/60 s x dt per update, so the same script
// plays identically at 30 and 120 fps and the [FIGHTER] logs (gt=) of two rates can be compared line by line (tools/rate_test.py).
// The scripted pad replaces every other input while a script runs in battle. PSPRECOMP_INPUT_RECORD=<file> writes the pad
// state the game was given as such a script (play the move once by hand, replay it at any rate).
struct TestEvent { double t; int kind; uint32_t a, b; std::string text; bool rel = false; };   // kind 0 set, 1 analog, 2 mark, 3 quit, 4 action, 10 sync, 11 waitidle, 12 place, 13 batch, 14 scan (5 gauge and 6 shot are the assist-gauge / screenshot commands); rel: t counts from the last sync
std::vector<TestEvent> g_test_events;
size_t g_test_next = 0;
double g_gt_ms = 0;                     // game time in the battle, ms
bool g_gt_started = false, g_test_on = false;
uint32_t g_test_buttons = 0, g_test_analog = 0x8080;
bool g_test_has_rel = false;
double g_ref_gt = 0;                    // game time of the last `sync` (the update the player's action changed on): `+<ms>` events count from it
int g_wait_kind = 0;                    // 5 = waiting in `sync`, 6 = in `waitidle`; the script is held until it ends
uint32_t g_wait_scr = 0;
double g_wait_since = 0, g_wait_timeout = 0;
int g_wait_target = -1;
bool g_scan_seeded = false;   // `scan`: the candidate lists exist (second scan on)
bool g_test_ai_off = false;      // PSPRECOMP_TEST_AI_OFF=1: the CPU fighter's pad never gets a button (no AI input) but runs at the normal time step, so it takes hits and reacts
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

// parse script lines into `ev` (rel events keep file order)
void parse_script(std::istream& in, std::vector<TestEvent>& ev) {
    std::string line;
    while (std::getline(in, line)) {
        const size_t h = line.find('#');
        if (h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        double t; std::string cmd;
        const size_t f0 = line.find_first_not_of(" \t");
        const bool rel = f0 != std::string::npos && line[f0] == '+';   // +<ms>: relative to the last sync
        if (rel) line[f0] = ' ', ss.str(line);
        if (!(ss >> t >> cmd)) continue;
        const size_t before = ev.size();
        if (cmd == "sync" || cmd == "waitidle") {   // sync [timeout ms]: hold the script until the player's action (script id) changes, then +ms counts from that update
            double to = 3000; int tgt = -1; ss >> to >> tgt;   // sync [timeout ms] [script id]: ... changes (or becomes that script id)
            ev.push_back({t, cmd == "sync" ? 10 : 11, static_cast<uint32_t>(tgt), static_cast<uint32_t>(to), "", rel});   // waitidle [timeout ms]: hold it until the player is out of any action
        } else if (cmd == "batch") {   // batch <file>: hold the script until <file> exists, then play the lines in it (and delete it): lets a driver feed trials into a running battle
            std::string f; ss >> f;
            ev.push_back({t, 13, 0, 0, f, rel});
        } else if (cmd == "scan") {   // scan <min drop> <max drop> <max value>: user RAM words that dropped by min..max since the last scan, and kept dropping (finds a stat the HUD shows)
            double lo = 150, hi = 400, mx = 2000; ss >> lo >> hi >> mx;
            ev.push_back({t, 14, static_cast<uint32_t>(lo), static_cast<uint32_t>(hi), std::to_string(static_cast<int>(mx)), rel});
        } else if (cmd == "poke") {   // poke <player|cpu> <hex offset into the fighter> <int value>: write one word of a fighter (finding which stat a number on the HUD comes from)
            std::string who = "player", off; long v = 0; ss >> who >> off >> v;
            ev.push_back({t, 15, static_cast<uint32_t>(std::strtoul(off.c_str(), nullptr, 16)), static_cast<uint32_t>(v), who, rel});
        } else if (cmd == "place") {   // place <metres>: the player stands that far from the dummy (on the line they were on), at once
            double d = 4; ss >> d;
            ev.push_back({t, 12, static_cast<uint32_t>(d * 100.0 + 0.5), 0, "", rel});
        } else if (cmd == "set" || cmd == "tap") {
            std::string b; ss >> b;
            const uint32_t m = parse_buttons(b);
            if (cmd == "tap") {
                double d = 100; ss >> d;
                ev.push_back({t, 0, m, 0x8080, ""});
                ev.push_back({t + d, 0, 0, 0x8080, "tap"});   // 'tap' release keeps the stick: marked by text
            } else {
                unsigned ax = 128, ay = 128; ss >> ax >> ay;
                ev.push_back({t, 0, m, (ay & 255u) << 8 | (ax & 255u), "set"});
            }
        } else if (cmd == "analog") {
            unsigned ax = 128, ay = 128; ss >> ax >> ay;
            ev.push_back({t, 1, 0, (ay & 255u) << 8 | (ax & 255u), ""});
        } else if (cmd == "mark") {
            std::string rest; std::getline(ss, rest);
            ev.push_back({t, 2, 0, 0, rest});
        } else if (cmd == "action") {   // action <id> [who]: start the action; who = player (default) | cpu | <slot>; id -1 lists them
            int id = -1; std::string who = "player"; ss >> id >> who;
            ev.push_back({t, 4, static_cast<uint32_t>(id), who == "cpu" ? 1u : (who == "player" ? 0u : 2u + static_cast<uint32_t>(std::atoi(who.c_str()))), ""});
        } else if (cmd == "shot") {   // shot <path>: save a screenshot (TGA) of the picture shown at this game time (renderer comparisons)
            std::string path; ss >> path;
            ev.push_back({t, 6, 0, 0, path});
        } else if (cmd == "gauge") {   // gauge <side> <0..10000>: set a side's assist gauge (side 0 / 1 = the controller table order of the HUD)
            unsigned side = 1; double v = 0; ss >> side >> v;
            ev.push_back({t, 5, side, 0, std::to_string(v)});
        } else if (cmd == "exburst") {   // exburst [who]: start the EX Burst (controller FUN_08A3EB70) of a fighter without the EX gauge / trigger
            std::string who = "player"; ss >> who;
            ev.push_back({t, 7, 0, who == "cpu" ? 1u : (who == "player" ? 0u : 2u + static_cast<uint32_t>(std::atoi(who.c_str()))), ""});
        } else if (cmd == "exstate") {   // exstate <n> [who]: write the EX Mode state (actor +1924 +4); logs the state table
            unsigned n = 0; std::string who = "player"; ss >> n >> who;
            ev.push_back({t, 8, n, who == "cpu" ? 1u : (who == "player" ? 0u : 2u + static_cast<uint32_t>(std::atoi(who.c_str()))), ""});
        } else if (cmd == "quit") ev.push_back({t, 3, 0, 0, ""});
        else std::fprintf(stderr, "[TEST] unknown command '%s'\n", cmd.c_str());
        for (size_t i = before; i < ev.size(); i++) ev[i].rel = rel;
        if (rel) g_test_has_rel = true;
    }
}

void testdrive_load(const char* path) {
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "[TEST] cannot read script %s\n", path); return; }
    parse_script(in, g_test_events);
    // a script with relative events keeps its file order (the times only mean something after the sync before them)
    if (!g_test_has_rel) std::stable_sort(g_test_events.begin(), g_test_events.end(), [](const TestEvent& x, const TestEvent& y) { return x.t < y.t; });
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

// ---- Input debug (PSPRECOMP_INPUT_DEBUG=1) ---------------------------------------------------------------------------
// What the game is handed each frame: pressed buttons and the stick, drawn at the top centre and logged ("[INPUT] t=<play ms>
// down=CROSS+L stick=x,y" on every button change, plus "up" when released). Same text in any frame rate, so 30 and 120 fps logs
// can be compared press by press.
std::string button_names(uint32_t b) {
    static const struct { uint32_t m; const char* n; } k[] = {{0x4000, "CROSS"}, {0x2000, "CIRCLE"}, {0x8000, "SQUARE"}, {0x1000, "TRIANGLE"},
        {0x100, "L"}, {0x200, "R"}, {0x10, "UP"}, {0x40, "DOWN"}, {0x80, "LEFT"}, {0x20, "RIGHT"}, {0x8, "START"}, {0x1, "SELECT"}};
    std::string out;
    for (const auto& e : k) if (b & e.m) { if (!out.empty()) out += "+"; out += e.n; }
    return out;
}
void input_debug_frame() {
    if (!g_input_dbg) return;
    uint32_t b = 0, a = 0x8080;
    psp_ctrl_last_sample(&b, &a);
    const uint32_t face = b & 0xF3F9u;   // buttons only: the stick has its own fields
    static uint32_t last = 0xFFFFFFFFu;
    static uint64_t presses = 0;
    static std::string shown;
    static double shown_at = -1e9;
    if (face != last) {
        const uint32_t newly = face & ~(last == 0xFFFFFFFFu ? 0u : last);
        for (uint32_t bit = 1; bit; bit <<= 1) if (newly & bit) presses++;
        std::fprintf(stderr, "[INPUT] t=%.0f %s=%s stick=%u,%u presses=%llu\n", g_play_ms, face ? "down" : "up", face ? button_names(face).c_str() : "-",
                     a & 255u, (a >> 8) & 255u, (unsigned long long)presses);
        last = face;
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s  stick %u,%u  presses %llu", face ? button_names(face).c_str() : "-", a & 255u, (a >> 8) & 255u, (unsigned long long)presses);
    if (shown != buf || g_play_ms - shown_at > 100.0) { shown = buf; shown_at = g_play_ms; ge_draw_toast(buf, 400); }
}

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
        g_gt_started = true; g_gt_ms = 0; g_test_next = 0; g_test_buttons = 0; g_test_analog = 0x8080; g_wait_kind = 0; g_ref_gt = 0;
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
    if (g_test_freeze_cpu || g_test_ai_off) {   // the dummy keeps the HP it started with (actor +0x10B0): a hit never ends the fight
        static uint32_t keep_actor = 0, keep_hp = 0;
        for (int i = 0; i < 4; i++) {
            const uint32_t a = g_fighters[i];
            if (!a || !ok(a) || g_tick - g_fighter_seen[i] > 60 || a == test_player()) continue;
            if (a != keep_actor || !g_gt_started || g_gt_ms < 34) { keep_actor = a; keep_hp = r32(a + 0x10B0u); }
            else if (keep_hp) w32(a + 0x10B0u, g_test_ai_off ? 100000000u : keep_hp);   // AI-off dummies get a huge pool: once broken, hits do HP damage and the KO check runs inside the update, before this write
        }
    }
    // the player's action (script id): every change is logged, and sync / waitidle watch it
    static uint32_t prev_scr = 0xFFFFu;
    uint32_t cur_scr = 0xFFFFu;
    if (const uint32_t pa = test_player(); pa && ok(pa)) cur_scr = r32(pa + 0x540u) & 0xFFFFu;
    if (!g_gt_started) prev_scr = 0xFFFFu;
    else if (cur_scr != prev_scr) {
        const uint32_t da = test_fighter(1), pa2 = test_player();   // HP / bravery words of the player and the dummy (offsets 0x10A8..0x10B8)
        std::fprintf(stderr, "[TEST] gt=%.0f scr %d -> %d  me %d %d %d %d %d  dummy %d %d %d %d %d\n", g_gt_ms, prev_scr == 0xFFFFu ? -1 : static_cast<int>(prev_scr), cur_scr == 0xFFFFu ? -1 : static_cast<int>(cur_scr),
                     (int)r32(pa2 + 0x10A8u), (int)r32(pa2 + 0x10ACu), (int)r32(pa2 + 0x10B0u), (int)r32(pa2 + 0x10B4u), (int)r32(pa2 + 0x10B8u),
                     (int)r32(da + 0x10A8u), (int)r32(da + 0x10ACu), (int)r32(da + 0x10B0u), (int)r32(da + 0x10B4u), (int)r32(da + 0x10B8u));
        prev_scr = cur_scr;
        if (std::getenv("PSPRECOMP_TEST_DUMP")) {   // raw words 0x1000..0x1400 of the player and the dummy: finds the HP / bravery fields by what changes on a hit
            for (int w = 0; w < 2; w++) {
                const uint32_t a = w ? test_fighter(1) : test_player();
                std::fprintf(stderr, "[TESTDUMP] gt=%.0f %s", g_gt_ms, w ? "dummy" : "me");
                static uint32_t lo = 0x1000, hi = 0x1400;
                static bool rd = false;
                if (!rd) { rd = true; if (const char* r = std::getenv("PSPRECOMP_TEST_DUMP_RANGE")) std::sscanf(r, "%x,%x", &lo, &hi); }
                for (uint32_t o = lo; o < hi; o += 4) std::fprintf(stderr, " %X", r32(a + o));
                std::fprintf(stderr, "\n");
                if (w == 1) {   // the info block the actor points to (+0x2F8) and the one it points to in turn (info +0 ...), 0x600 bytes each
                    for (uint32_t po : {0x2F8u, 0x2FCu, 0x300u, 0x304u}) {
                        const uint32_t q = r32(a + po);
                        if (!ok(q)) continue;
                        std::fprintf(stderr, "[TESTDUMP] gt=%.0f info%X", g_gt_ms, po);
                        for (uint32_t o = 0; o < 0x600; o += 4) std::fprintf(stderr, " %X", r32(q + o));
                        std::fprintf(stderr, "\n");
                    }
                }
            }
        }
    }
    if (g_test_events.empty()) return;
    for (;;) {
        if (g_wait_kind) {
            bool dummy_neutral = true;   // waitidle also waits for the CPU fighter to be out of its hit reactions (motion ids 1 / 2 are its standing idle): a hit on a dummy that is still recovering does not connect, so no chain
        if (g_wait_kind == 11) if (const uint32_t da = test_fighter(1)) { const int dc = static_cast<int16_t>(r32(da + 0x180u) & 0xFFFFu); dummy_neutral = dc == 1 || dc == 2; }
        const bool done = g_wait_kind == 10 ? (g_wait_target >= 0 ? static_cast<int>(cur_scr) == g_wait_target : cur_scr != g_wait_scr) : (cur_scr == 0xFFFFu && dummy_neutral);
            const bool late = g_gt_ms - g_wait_since > g_wait_timeout;
            if (!done && !late) break;
            std::fprintf(stderr, "[TEST] gt=%.0f %s %s (scr %d)\n", g_gt_ms, g_wait_kind == 10 ? "sync" : "waitidle", done ? "done" : "TIMEOUT", cur_scr == 0xFFFFu ? -1 : static_cast<int>(cur_scr));
            g_ref_gt = g_gt_ms;
            g_wait_kind = 0;
        }
        if (g_test_next >= g_test_events.size()) break;
        const TestEvent& e = g_test_events[g_test_next];
        if (g_gt_ms < (e.rel ? g_ref_gt + e.t : e.t)) break;
        g_test_next++;
        if (e.kind == 13) {   // batch: wait for the file, then splice its events in here
            if (FILE* f = std::fopen(e.text.c_str(), "r")) {
                std::fclose(f);
                std::ifstream in(e.text);
                std::vector<TestEvent> more;
                parse_script(in, more);
                std::remove(e.text.c_str());
                std::fprintf(stderr, "[TEST] gt=%.0f batch %s: %zu events\n", g_gt_ms, e.text.c_str(), more.size());
                g_test_events.insert(g_test_events.begin() + g_test_next, more.begin(), more.end());
            } else { g_test_next--; break; }   // not yet: look again next update
            continue;
        }
        if (e.kind == 14) {   // scan: see the `scan` command. Followed at once: 32-bit int, 16-bit halves, float; the baseline is only stored, the first comparison builds the candidate lists
            static std::vector<uint32_t> prev32, cand[3];   // previous snapshot of user RAM (0x08800000..0x0A000000), candidate indexes per encoding (32-bit words, 16-bit halves, floats)
            static bool have = false;
            const uint32_t base = 0x08800000u, words = (0x0A000000u - 0x08800000u) / 4;
            std::vector<uint32_t> now(words);
            std::memcpy(now.data(), g_ram + (base & 0x07FFFFFFu), words * 4ull);
            const double mx = std::atof(e.text.c_str());
            auto val = [](const std::vector<uint32_t>& m, uint32_t i, int enc) -> double {
                if (enc == 0) return static_cast<double>(static_cast<int32_t>(m[i]));
                if (enc == 1) return static_cast<double>(static_cast<uint16_t>((m[i >> 1] >> ((i & 1) * 16)) & 0xFFFFu));
                float f; std::memcpy(&f, &m[i], 4); return f;
            };
            const uint32_t counts[3] = {words, words * 2, words};
            for (int enc = 0; enc < 3; enc++) {
                std::vector<uint32_t> next;
                auto test = [&](uint32_t i) {
                    const double d = val(prev32, i, enc) - val(now, i, enc), nv = val(now, i, enc);
                    if (d >= e.a && d <= e.b && nv >= 0 && nv <= mx) next.push_back(i);
                };
                if (have) { if (cand[enc].empty() && !g_scan_seeded) { for (uint32_t i = 0; i < counts[enc]; i++) test(i); } else for (uint32_t i : cand[enc]) test(i); }
                if (have) cand[enc].swap(next);
                std::fprintf(stderr, "[TEST] gt=%.0f scan enc%d: %zu candidates\n", g_gt_ms, enc, have ? cand[enc].size() : size_t(0));
                if (have && cand[enc].size() <= 30) for (uint32_t i : cand[enc]) std::fprintf(stderr, "[TEST]   scan enc%d %08X = %g (was %g)\n", enc, base + i * (enc == 1 ? 2 : 4), val(now, i, enc), val(prev32, i, enc));
            }
            if (have) g_scan_seeded = true;
            prev32.swap(now); have = true;
            continue;
        }
        if (e.kind == 10 || e.kind == 11) {
            g_wait_kind = e.kind; g_wait_scr = cur_scr; g_wait_since = g_gt_ms; g_wait_timeout = e.b; g_wait_target = static_cast<int>(e.a);
            continue;
        }
        if (e.kind == 15) {
            const uint32_t a = test_fighter(e.text == "cpu" ? 1u : 0u);
            if (a) { w32(a + e.a, e.b); std::fprintf(stderr, "[TEST] gt=%.0f poke %s +%X = %d\n", g_gt_ms, e.text.c_str(), e.a, static_cast<int>(e.b)); }
            continue;
        }
        if (e.kind == 12) {   // place: both fighters back at their start spot (the dummy where it stood at the first `place`, the player <m> metres from it on the x axis);
                             // every hit pushes the dummy on, 42 m after 18 trials and out of the arena. x/z of the body root and its copies (+0x30/+0x38, +0x80/+0x88, +0x100/+0x108, +0x110/+0x118)
            static bool have_home = false;
            static float hx = 0, hz = 0;
            const uint32_t me = test_player(), dm = test_fighter(1);
            if (me && dm) {
                if (!have_home) { hx = rdf(dm + 0x110u); hz = rdf(dm + 0x118u); have_home = true; }
                auto put = [](uint32_t a, float x, float z) {
                    for (uint32_t o : {0x30u, 0x80u, 0x100u, 0x110u}) { uint32_t u; std::memcpy(&u, &x, 4); w32(a + o, u); }
                    for (uint32_t o : {0x38u, 0x88u, 0x108u, 0x118u}) { uint32_t u; std::memcpy(&u, &z, 4); w32(a + o, u); }
                };
                const float d = e.a / 100.0f;
                put(dm, hx, hz);
                put(me, hx - d, hz);
                std::fprintf(stderr, "[TEST] gt=%.0f place: dummy at %.2f,%.2f, player %.2f m away\n", g_gt_ms, hx, hz, d);
            }
        }
        else if (e.kind == 0) { g_test_buttons = e.a; if (e.text == "set") g_test_analog = e.b; }
        else if (e.kind == 1) g_test_analog = e.b;
        else if (e.kind == 2) std::fprintf(stderr, "[TEST] gt=%.0f mark%s\n", g_gt_ms, e.text.c_str());
        else if (e.kind == 4) {
            const uint32_t a = test_fighter(e.b);
            const int r = a ? dissidia_test_action(rdram, ctx, a, static_cast<int>(e.a)) : -2;
            std::fprintf(stderr, "[TEST] gt=%.0f action %d on %08X (%s): %s\n", g_gt_ms, static_cast<int>(e.a), a, e.b == 1 ? "cpu" : "player",
                         r == 1 ? "started" : r == 0 ? "NOT started" : r == -1 ? "no such record" : "no such fighter");
        }
        else if (e.kind == 6) { const bool ok = ge_draw_capture_screenshot(e.text.c_str(), 10000); std::fprintf(stderr, "[TEST] gt=%.0f shot %s: %s\n", g_gt_ms, e.text.c_str(), ok ? "saved" : "FAILED"); }
        else if (e.kind == 5) {
            const uint32_t ga = assist_gauge_addr(static_cast<int>(e.a));
            if (ga) wrf(ga, static_cast<float>(std::atof(e.text.c_str())));
            std::fprintf(stderr, "[TEST] gt=%.0f gauge side %u = %s (%s)\n", g_gt_ms, e.a, e.text.c_str(), ga ? "set" : "no gauge");
        }
        else if (e.kind == 8) {
            const uint32_t a = test_fighter(e.b);
            if (a && ok(a)) {
                for (int i = 0; i < 7; i++) std::fprintf(stderr, "[TEST] ex state table %d -> %08X\n", i + 1, r32(0x08ACBBA8u + 4u * i));
                std::fprintf(stderr, "[TEST] gt=%.0f EX obj %08X state %u -> %u\n", g_gt_ms, a + 1924u, r32(a + 1928u), e.a);
                w32(a + 1928u, e.a);
            }
        }
        else if (e.kind == 7) {
            const uint32_t a = test_fighter(e.b);
            FuncPtr start = ovl_battle_lookup(0x08A3EB70u);
            const uint32_t ctl = r32(0x08A405E0u);   // the EX Burst controller object (global, the caller in FUN_08ACB208 reads it at 0x08A40000 + 1504)
            bool started = false;
            if (a && start && ok(ctl)) {
                const recomp_context after = *ctx;
                const uint32_t info = r32(a + 1348u) + 288u;   // the caller's own query: the foe, through the actor's vtable-like entry
                recomp_context c = *ctx;
                c.r[4] = static_cast<int32_t>(a + static_cast<uint32_t>(static_cast<int16_t>(r32(info) & 0xFFFFu)));
                FuncPtr getter = ok(info) ? (ovl_battle_lookup(r32(info + 4u)) ? ovl_battle_lookup(r32(info + 4u)) : psp_dispatch_probe_lookup(r32(info + 4u))) : nullptr;
                if (getter) getter(rdram, &c);
                const uint32_t foe = getter ? static_cast<uint32_t>(c.r[2]) : 0u;
                c = *ctx;
                c.r[4] = static_cast<int32_t>(ctl); c.r[5] = static_cast<int32_t>(a); c.r[6] = static_cast<int32_t>(foe);
                start(rdram, &c);
                *ctx = after;
                started = true;
            }
            std::fprintf(stderr, "[TEST] gt=%.0f exburst on %08X: %s\n", g_gt_ms, a, started ? "started" : "NOT started");
        }
        else if (e.kind == 3) { std::fprintf(stderr, "[TEST] quit at gt=%.0f ms\n", g_gt_ms); g_should_exit.store(true); }
    }
    psp_ctrl_set_script(true, g_test_buttons, g_test_analog);
}

// ---- Debug menu: Jecht combo timing helper ------------------------------------------------------------------------------------------
// Reads the game's own timing rule. The move picker asks the chain check FUN_08AB81E0(table, actor, &requested action id) for every candidate
// follow-up of a press (call sites: docs/archive/event-ops/D.md). The table is a list of groups, each a list of 8-byte elements
// {s16 action id, s16 start, s16 end, ...}; element j is reachable from the action the fighter is in (the u16 behind actor +0x101C) when
// element j-1 has that id, and only while start <= actor +0x1A8 (the move's motion time, ticks) and (end < 0 or motion time < end).
// Jecht's timed chain steps are elements with a bounded window. Here that table is read live:
//   window opens (some bounded element's start is reached)  -> green edge flash
//   a press while a bounded element is open                 -> GOOD;  before its start -> TOO EARLY;  after its end -> TOO LATE
// The verdict of a press is taken inside the chain check itself (the motion time and window of the element it tested), so it is exactly what
// the game compared. Works for every Jecht move incl. the air ones. Presses are the host pad's (psp_ctrl_last_sample): Circle, Square, Triangle.
constexpr int JECHT_CHAR = 20;
constexpr uint32_t CMB_BUTTONS = 0x2000u | 0x8000u;   // Circle (Brave), Square (HP): the attack buttons (Triangle is the dash with R, Cross the dodge with R)
FuncPtr g_orig_chain_check = nullptr;
uint32_t g_chain_table = 0;               // chain table pointer (a0), cached from the last call
struct CmbVerdict { uint64_t tick = ~0ull; int kind = 0; int start = 0, end = 0; float mt = 0; uint32_t id = 0; };   // kind 1 GOOD, 2 EARLY, 3 LATE
CmbVerdict g_cmb_verdict;                 // best verdict of the update (GOOD beats the others)
struct CmbElem { uint32_t id; int start, end; };
int16_t rs16(uint32_t a) { int16_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 2); return v; }
// elements reachable from action `cur` in the table: calls f(next id, start, end)
template <class F> void chain_next(uint32_t table, uint32_t cur, F f) {
    const uint32_t base = r32(table);
    if (!ok(base)) return;
    const int groups = rs16(base + 2);
    for (int g = 0; g < groups && g < 256; g++) {
        if (!(r32(table + 36u + 4u * static_cast<uint32_t>(g >> 5)) & (1u << (g & 31)))) continue;   // group enabled for this fighter
        const uint32_t grp = base + r32(base + 4u + 4u * static_cast<uint32_t>(g));
        if (!ok(grp)) continue;
        const int count = rs16(grp + 2);
        for (int j = 1; j < count && j < 64; j++) {
            const uint32_t prev = grp + 8u + 8u * static_cast<uint32_t>(j - 1), el = prev + 8u;
            if (rs16(el) == -1) break;
            if (static_cast<uint16_t>(rs16(prev)) == cur) f(static_cast<uint32_t>(static_cast<uint16_t>(rs16(el))), static_cast<int>(rs16(el + 2)), static_cast<int>(rs16(el + 4)));
        }
    }
}
bool in_action(uint32_t a) { const uint32_t scr = r32(a + 0x540u) & 0xFFFFu; return scr >= 10000u && scr < 10200u; }   // in one of Jecht's own attack moves (the dodge, dash, guard... have timeline windows of their own and other script ids)   // the fighter is in an action (the "current action" fields keep the last move once it is over)
bool human_jecht(uint32_t a) {
    if (!a || !ok(a) || fighter_char(a) != JECHT_CHAR) return false;
    const uint32_t brain = r32(a + 0x5C4u);
    return ok(brain) && static_cast<int32_t>(r32(brain + 0x54u)) != 0;   // CPU brains have +0x54 == 0 (see cpu_tick_noai_hook)
}
// 30 Hz input grid for Jecht (see the g_jecht_grid comment at the top): the Circle / Square the game is handed change only on every P-th update (the
// grid: g_tick % P == 0), and a tap that came and went between two grid updates is latched, so it is seen once, as a 33 ms PSP frame would show it.
// Only while the human player is Jecht; the other buttons and the stick pass through.
constexpr uint32_t GRID_MASK = 0x2000u | 0x8000u;
uint32_t jecht_grid_filter(uint32_t raw) {
    static uint32_t held = 0, pend = 0;
    static uint64_t decided = ~0ull;
    bool jecht = false;
    if (g_jecht_grid && g_ticks < 2.0f && overlay_id() == 1) for (uint32_t f : g_fighters) if (f && human_jecht(f)) jecht = true;
    if (!jecht) { held = raw & GRID_MASK; pend = 0; return raw; }
    pend |= raw & GRID_MASK;
    if ((g_tick - g_grid_anchor) % static_cast<uint64_t>(grid_period()) == 0 && decided != g_tick) { decided = g_tick; held = pend; pend = 0; }
    return (raw & ~GRID_MASK) | held;
}

// The timed window of the fighter's current action `cur`: the ticks (motion time) in which the chain move is the one that gets played. The table
// lists, for Jecht's Rush for example, 1629 [8,20) (the repeat move: an early press), 1664 [18,28) (Neutral 2), 1665 [18,28) (Up 2) and 1629 [26,38) (the
// repeat again: a late press); where the repeat move's window overlaps, it wins (measured: presses at motion time 18-19 and 26-27 start the repeat), so
// the real window is [20,26). The repeat move is the id that appears with more than one window; the rest are the chain moves.
bool chain_window(uint32_t table, uint32_t cur, int* lo, int* hi) {
    CmbElem els[64];
    int n = 0;
    chain_next(table, cur, [&](uint32_t id, int st, int en) { if (en >= 0 && n < 64) els[n++] = {id, st, en}; });
    auto is_rep = [&](uint32_t id) { for (int i = 0; i < n; i++) if (els[i].id == id) for (int j = 0; j < n; j++) if (els[j].id == id && (els[j].start != els[i].start || els[j].end != els[i].end)) return true; return false; };
    int gs = 1 << 30, ge = -1;
    for (int i = 0; i < n; i++) if (!is_rep(els[i].id)) { gs = std::min(gs, els[i].start); ge = std::max(ge, els[i].end); }
    if (ge <= gs) return false;
    int first = -1, last = -1;
    for (int t = gs; t < ge; t++) {
        bool good = false, repeat = false;
        for (int i = 0; i < n; i++) if (t >= els[i].start && t < els[i].end) (is_rep(els[i].id) ? repeat : good) = true;
        if (good && !repeat) { if (first < 0) first = t; last = t; }
    }
    if (first < 0) return false;
    *lo = first; *hi = last + 1;
    return true;
}
void chain_check_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t table = static_cast<uint32_t>(ctx->r[4]), actor = static_cast<uint32_t>(ctx->r[5]);
    {
        static const bool dump = std::getenv("PSPRECOMP_PRESS_LOG") != nullptr;
        static uint64_t dumped = ~0ull;
        if (dump && dumped != g_tick && ok(actor) && human_jecht(actor) && ok(r32(actor + 0x101Cu))) {
            dumped = g_tick;
            const uint32_t cur = static_cast<uint16_t>(rs16(r32(actor + 0x101Cu)));
            std::fprintf(stderr, "[CHAIN] gt=%.0f current action %u motion time %.1f requested %u reachable:", g_gt_ms, cur, rdf(actor + 0x1A8u), static_cast<uint16_t>(rs16(static_cast<uint32_t>(ctx->r[6]))));
            chain_next(table, cur, [&](uint32_t id, int st, int en) { std::fprintf(stderr, " %u[%d,%d)", id, st, en); });
            std::fprintf(stderr, "\n");
        }
    }
    if (g_dbg_combo && ok(table) && ok(actor) && g_cmb_verdict.tick != g_tick && human_jecht(actor) && in_action(actor)) {   // once per update: the first candidate tested for a press, in a move (from neutral the "current action" is the last move's)
        const uint32_t cur_p = r32(actor + 0x101Cu);
        int lo = 0, hi = 0;
        if (ok(cur_p) && chain_window(table, static_cast<uint16_t>(rs16(cur_p)), &lo, &hi)) {
            const float mt = rdf(actor + 0x1A8u);
            const int kind = static_cast<float>(lo) > mt ? 2 : (static_cast<float>(hi) <= mt ? 3 : 1);
            g_cmb_verdict = CmbVerdict{g_tick, kind, lo, hi, mt, static_cast<uint32_t>(static_cast<uint16_t>(rs16(cur_p)))};
        }
    }
    g_orig_chain_check(rdram, ctx);
}
// The timed button windows that are not chain-table elements (the HP finishers after Neutral 3 / Up 3 / Down 3, ...) are timeline flags: op 0x004C
// holds +0x196 bit 0x04 (lockout range) and a command press in it latches +0x5B4 bit 2; ops 0x36 / 0x37 / 0x3A set +0x194 bit 7, +0x195 bits 0 / 1 (the window).
// The input code asks three tiny gate functions, which answer 0 while the latch is set. Their answers are taken as the game gave them.
struct GateSeen { uint64_t tick = ~0ull; bool open = false, lock = false, latched = false; };
GateSeen g_gate;
FuncPtr g_orig_gate_37 = nullptr, g_orig_gate_36 = nullptr, g_orig_gate_3a = nullptr;
void gate_note(uint32_t actor, uint32_t result) {
    if (!g_dbg_combo || !ok(actor) || !human_jecht(actor)) return;
    if (g_gate.tick != g_tick) g_gate = GateSeen{g_tick, false, false, false};
    g_gate.open |= (result & 1) != 0;
    g_gate.lock = (g_ram[(actor + 0x196u) & 0x07FFFFFFu] & 4) != 0;
    g_gate.latched = (g_ram[(actor + 0x5B4u) & 0x07FFFFFFu] & 2) != 0;
}
void gate_37_hook(uint8_t* rdram, recomp_context* ctx) { const uint32_t a = static_cast<uint32_t>(ctx->r[4]); g_orig_gate_37(rdram, ctx); gate_note(a, static_cast<uint32_t>(ctx->r[2])); }
void gate_36_hook(uint8_t* rdram, recomp_context* ctx) { const uint32_t a = static_cast<uint32_t>(ctx->r[4]); g_orig_gate_36(rdram, ctx); gate_note(a, static_cast<uint32_t>(ctx->r[2])); }
void gate_3a_hook(uint8_t* rdram, recomp_context* ctx) { const uint32_t a = static_cast<uint32_t>(ctx->r[4]); g_orig_gate_3a(rdram, ctx); gate_note(a, static_cast<uint32_t>(ctx->r[2])); }
bool flag_window(uint32_t a) { return (g_ram[(a + 0x195u) & 0x07FFFFFFu] & 3) != 0 || (g_ram[(a + 0x194u) & 0x07FFFFFFu] & 0x80) != 0; }
uint32_t g_cmb_scr[4] = {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
bool g_cmb_seen_win[4] = {};
bool g_cmb_flag_open[4] = {};
bool g_cmb_was_in_action[4] = {};   // the fighter was in an action at the start of the update that just ended (a press from neutral starts a move, it is not a chain press)
bool g_cmb_in_action_now[4] = {};
float g_cmb_prev_mt[4] = {}, g_cmb_mt_now[4] = {};   // motion time at the end of the previous update (= at the start of the update that just ended) / now

uint32_t g_cmb_prev_buttons = 0;
bool g_cmb_open[4] = {};
uint64_t g_cmb_open_scr[4] = {};
// once per update, before g_action_new is recomputed
void combo_frame() {
    if (!g_dbg_combo) return;
    uint32_t buttons = 0, stick = 0;
    psp_ctrl_last_sample(&buttons, &stick);
    const uint32_t edge = (buttons & 0x300u) ? 0u : (buttons & ~g_cmb_prev_buttons & CMB_BUTTONS);   // pressed in the update that just ended; not with L / R held (dodge, dash, dodge cancel)
    g_cmb_prev_buttons = buttons;
    if (const uint32_t t0 = r32(CHAIN_TABLE_PTR); ok(t0)) g_chain_table = t0;   // the table the move picker reads (a global; the same one the chain check gets inside a move)
    for (int i = 0; i < 4; i++) { g_cmb_prev_mt[i] = g_cmb_mt_now[i]; g_cmb_was_in_action[i] = g_cmb_in_action_now[i]; }
    for (int i = 0; i < 4; i++) {
        const uint32_t a = g_fighters[i];
        if (!human_jecht(a) || !g_chain_table) { g_cmb_open[i] = false; continue; }
        {
            static const bool flog = std::getenv("PSPRECOMP_PRESS_LOG") != nullptr;
            static uint8_t last[4][16] = {};
            if (flog) {   // the per-update flag bytes +0x190..+0x19B, the follow-up list count (+0x1068) and the lockout latch, whenever they change or a button is pressed
                uint8_t cur[16];
                for (int k = 0; k < 12; k++) cur[k] = g_ram[(a + 0x190u + k) & 0x07FFFFFFu];
                cur[12] = static_cast<uint8_t>(r32(a + 0x1068u) & 0xFFu); cur[13] = g_ram[(a + 0x5B4u) & 0x07FFFFFFu]; cur[14] = cur[15] = 0;
                if (std::memcmp(cur, last[i], 16) != 0 || edge) {
                    std::memcpy(last[i], cur, 16);
                    std::fprintf(stderr, "[FLAGS] gt=%.0f scr=%u mt=%.1f edge=%04X b190=", g_gt_ms, r32(a + 0x540u) & 0xFFFFu, rdf(a + 0x1A8u), edge);
                    for (int k = 0; k < 12; k++) std::fprintf(stderr, "%02X", cur[k]);
                    std::fprintf(stderr, " list=%d 5B4=%02X\n", cur[12], cur[13]);
                }
            }
        }
        const uint32_t cur_p = r32(a + 0x101Cu);
        bool open = false;
        if (ok(cur_p) && in_action(a)) {   // (out of a move the "current action" and its motion time stay at the last move's, frozen inside its window)
            int lo = 0, hi = 0;
            const float mt = rdf(a + 0x1A8u);
            if (chain_window(g_chain_table, static_cast<uint16_t>(rs16(cur_p)), &lo, &hi) && static_cast<float>(lo) <= mt && mt < static_cast<float>(hi)) open = true;
        }
        const uint32_t scr_now = r32(a + 0x540u) & 0xFFFFu;
        const bool same_move = scr_now == g_cmb_scr[i] && in_action(a);   // (a press that starts the next move, or the move ending, is not a window closing)
        if (g_cmb_open[i] && !open && same_move) { ge_draw_flash(0xFF8A30, 160); static const bool lg2 = std::getenv("PSPRECOMP_PRESS_LOG") != nullptr; if (lg2) std::fprintf(stderr, "[COMBO] gt=%.0f window closes (motion time %.1f)\n", g_gt_ms, rdf(a + 0x1A8u)); }
        if (g_cmb_flag_open[i] && !(in_action(a) && flag_window(a)) && same_move) ge_draw_flash(0xFF8A30, 160);
        if (open && !g_cmb_open[i]) {
            ge_draw_flash(0x40FF70, 160);
            static const bool log = std::getenv("PSPRECOMP_PRESS_LOG") != nullptr;
            if (log) std::fprintf(stderr, "[COMBO] gt=%.0f window opens (motion time %.1f)\n", g_gt_ms, rdf(a + 0x1A8u));
        }
        g_cmb_open[i] = open;
        const uint32_t scr = r32(a + 0x540u) & 0xFFFFu;
        if (scr != g_cmb_scr[i]) { g_cmb_scr[i] = scr; g_cmb_seen_win[i] = false; g_cmb_flag_open[i] = false; }
        const bool fw = in_action(a) && flag_window(a);
        if (fw && !g_cmb_flag_open[i]) { ge_draw_flash(0x40FF70, 160); static const bool lg = std::getenv("PSPRECOMP_PRESS_LOG") != nullptr; if (lg) std::fprintf(stderr, "[COMBO] gt=%.0f flag window opens (motion time %.1f)\n", g_gt_ms, rdf(a + 0x1A8u)); }
        g_cmb_flag_open[i] = fw;
        if (fw) g_cmb_seen_win[i] = true;
        g_cmb_mt_now[i] = rdf(a + 0x1A8u);
        g_cmb_in_action_now[i] = in_action(a);
    }
    bool was_in_move = false;   // a press from neutral starts a move: it is not a chain press, whatever the chain check was asked
    for (int i = 0; i < 4; i++) if (human_jecht(g_fighters[i]) && g_cmb_was_in_action[i]) was_in_move = true;
    if (!was_in_move) g_cmb_verdict.kind = 0;
    bool shown = edge && g_cmb_verdict.kind && g_cmb_verdict.tick + 1 == g_tick;   // (the chain verdict is shown below)
    if (edge && !shown) {   // not a chain-table press: the flag windows (HP finishers)
        for (int i = 0; i < 4; i++) {
            if (!human_jecht(g_fighters[i]) || !g_cmb_was_in_action[i]) continue;
            int kind = 0;
            if (g_gate.tick + 1 == g_tick) kind = g_gate.open ? 1 : (g_gate.lock || g_gate.latched) ? 2 : (g_cmb_seen_win[i] ? 3 : 0);
            else if (g_cmb_seen_win[i] && !g_cmb_flag_open[i]) kind = 3;
            if (!kind) break;
            const char* msg = kind == 1 ? "GOOD" : kind == 2 ? "TOO EARLY" : "TOO LATE";
            char buf[64]; std::snprintf(buf, sizeof buf, "Jecht combo: %s", msg);
            ge_draw_toast(buf, 900, 28);
            ge_draw_flash(kind == 1 ? 0x40FF70 : 0xFF3030, 220);
            std::fprintf(stderr, "[COMBO] gt=%.0f %s (flag window; open=%d lock=%d latched=%d)\n", g_gt_ms, buf, g_gate.open, g_gate.lock, g_gate.latched);
            shown = true;
            break;
        }
    }
    if (edge & 0x2000u && !shown) {   // a Circle the chain check never saw (pressed before the move accepts any input): judged against the table with the motion time at the start of the update
        for (int i = 0; i < 4; i++) {
            const uint32_t a = g_fighters[i];
            if (!human_jecht(a) || !g_chain_table || !ok(r32(a + 0x101Cu)) || !g_cmb_was_in_action[i]) continue;
            int lo = 0, hi = 0;
            static const bool dbg = std::getenv("PSPRECOMP_PRESS_LOG") != nullptr;
            if (dbg) std::fprintf(stderr, "[COMBO] gt=%.0f unseen Circle: current action %u prev mt %.1f window %s\n", g_gt_ms, static_cast<uint16_t>(rs16(r32(a + 0x101Cu))), g_cmb_prev_mt[i], chain_window(g_chain_table, static_cast<uint16_t>(rs16(r32(a + 0x101Cu))), &lo, &hi) ? "found" : "none");
            if (!chain_window(g_chain_table, static_cast<uint16_t>(rs16(r32(a + 0x101Cu))), &lo, &hi)) continue;
            const float mt = g_cmb_prev_mt[i];
            if (static_cast<float>(lo) <= mt && mt < static_cast<float>(hi)) continue;   // inside the window yet unseen by the check: say nothing
            const bool early = mt < static_cast<float>(lo);
            const int ms = static_cast<int>(((early ? static_cast<float>(lo) - mt : mt - static_cast<float>(hi)) * 1000.f / 60.f) + 0.5f);
            char buf[96];
            if (ms < 8) std::snprintf(buf, sizeof buf, "Jecht combo: %s (just)", early ? "TOO EARLY" : "TOO LATE");
            else std::snprintf(buf, sizeof buf, "Jecht combo: %s (%d ms)", early ? "TOO EARLY" : "TOO LATE", ms);
            ge_draw_toast(buf, 900, 28);
            ge_draw_flash(0xFF3030, 220);
            std::fprintf(stderr, "[COMBO] gt=%.0f %s (unseen by the chain check; motion time %.1f window %d..%d)\n", g_gt_ms, buf, mt, lo, hi);
            break;
        }
    }
    if (edge && g_cmb_verdict.kind && g_cmb_verdict.tick + 1 == g_tick) {   // the verdict of the update that just ended (g_tick was advanced at the start of this frame end)
        const CmbVerdict& v = g_cmb_verdict;
        const char* msg = v.kind == 1 ? "GOOD" : v.kind == 2 ? "TOO EARLY" : "TOO LATE";
        const float ms = (v.kind == 2 ? (static_cast<float>(v.start) - v.mt) : (v.mt - static_cast<float>(v.end))) * 1000.f / 60.f;
        char buf[96];
        if (v.kind == 1) std::snprintf(buf, sizeof buf, "Jecht combo: GOOD");
        else if (ms < 8.f) std::snprintf(buf, sizeof buf, "Jecht combo: %s (just)", msg);
        else std::snprintf(buf, sizeof buf, "Jecht combo: %s (%d ms)", msg, static_cast<int>(ms + 0.5f));
        ge_draw_toast(buf, 900, 28);
        ge_draw_flash(v.kind == 1 ? 0x40FF70 : 0xFF3030, 220);
        std::fprintf(stderr, "[COMBO] gt=%.0f %s action=%u motion time %.1f window %d..%d\n", g_gt_ms, buf, v.id, v.mt, v.start, v.end);
        g_cmb_verdict.kind = 0;
    }
}

void testdrive_install() {
#ifdef __linux__
    // PSPRECOMP_ALLOW_PTRACE=1: let any process of this user attach a debugger (Yama ptrace_scope 1 only allows a parent); for
    // `gdb -p` write traces ("which function stores this guest word") on an unstripped build
    if (const char* ap = std::getenv("PSPRECOMP_ALLOW_PTRACE"); ap && ap[0] && ap[0] != '0') prctl(PR_SET_PTRACER, (unsigned long)-1 /* PR_SET_PTRACER_ANY */, 0, 0, 0);
#endif
    if (const char* ao = std::getenv("PSPRECOMP_TEST_AI_OFF"); ao && ao[0] && ao[0] != '0') g_test_ai_off = true;
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
    g_play_ms += g_ticks * (1000.0 / 60.0);
    input_debug_frame();
    testdrive_frame(rdram, ctx);
    if (battle_running()) g_battle_age += g_ticks; else g_battle_age = 0.f;
    fighter_log_frame();
    combo_frame();
    if (battle_running()) g_battle_age += g_ticks; else g_battle_age = 0.f;
    for (int i = 0; i < 4; i++) {
        const uint32_t scr = (g_fighters[i] && ok(g_fighters[i])) ? (r32(g_fighters[i] + 0x540u) & 0xFFFFu) : 0xFFFFu;
        g_action_new[i] = scr != 0xFFFFu && scr != g_prev_scr[i];
        if (scr != g_prev_scr[i] && g_fighters[i] && human_jecht(g_fighters[i])) g_grid_anchor = g_tick;   // the next update is the first read of the new move's grid
        g_prev_scr[i] = scr;
    }
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
// AI off (PSPRECOMP_TEST_AI_OFF=1): the virtual pad (obj: +4 state, +8 pending, +12 previous) is fed by the human's pad and by the CPU's
// AI through the same bit setters, FUN_08AB387C (state) and FUN_08AB38C4 (pending). For the CPU fighter's pad they do nothing.
constexpr uint32_t PAD_SET_STATE = 0x08AB387Cu, PAD_SET_PENDING = 0x08AB38C4u;
FuncPtr g_orig_pad_set_state = nullptr, g_orig_pad_set_pending = nullptr;
bool ai_off_blocks(uint32_t pad) {
    if (!g_test_ai_off || !g_gt_started) return false;
    const uint32_t a = actor_of_pad(pad);
    return a && a != test_player();
}
void pad_set_state_hook(uint8_t* rdram, recomp_context* ctx) {
    if (ai_off_blocks(static_cast<uint32_t>(ctx->r[4]))) { ctx->r[2] = 0; return; }
    g_orig_pad_set_state(rdram, ctx);
}
void pad_set_pending_hook(uint8_t* rdram, recomp_context* ctx) {
    if (ai_off_blocks(static_cast<uint32_t>(ctx->r[4]))) { ctx->r[2] = 0; return; }
    g_orig_pad_set_pending(rdram, ctx);
}

void pressed_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t obj = static_cast<uint32_t>(ctx->r[4]), bit = static_cast<uint32_t>(ctx->r[5]);
    g_orig_pressed(rdram, ctx);
    {   static const bool lg = std::getenv("PSPRECOMP_EXB_LOG") != nullptr; static uint32_t seen[64]; static int ns = 0;
        if (lg && overlay_id() == 1 && ok(r32(0x08A405E0u))) {
            const uint32_t ph = r32(r32(0x08A405E0u) + 632u); const uint32_t key = ph * 64u + (bit & 63u) + (g_exb_in ? 4096u : 0u);
            bool k = false; for (int i = 0; i < ns; i++) if (seen[i] == key) k = true;
            if (!k && ph && ns < 64) { seen[ns++] = key; std::fprintf(stderr, "[EXBT] pressed(08AB399C) obj=%08X bit=%u phase=%u in=%d gt=%.0f\n", obj, bit, ph, g_exb_in ? 1 : 0, g_gt_ms); } } }
    if (g_exb_log_on && (ctx->r[2] & 1) && overlay_id() == 1 && ok(r32(0x08A405E0u)) && r32(r32(0x08A405E0u) + 632u) != 0)
        std::fprintf(stderr, "[EXBG] bt=%.0f pad-edge pad=%08X(ctrl %08X) bit=%u owner=%08X in=%d\n", g_play_ms - g_exb_t0, obj, obj - 0x53Cu, bit, actor_of_pad(obj), g_exb_in ? 1 : 0);
    g_press_calls++;
    if (g_exb_in || g_exb_poll) return;   // the EX Burst keeps its own edges (sticky): a latched press would count once per run and again from the sticky flag
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
        if (e[0] == '2' && !armed && g_test_on && i == 0 && g_gt_ms > 100) { armed = 1; psp_mem_watch_set(gp + 0x1Cu); }   // =2: watch side 0's +0x1C
        { // whole gauge struct (0x40 bytes before / after the value), logged when any word changes
            static uint32_t prev[2][24] = {};
            bool diff = false;
            for (int k = 0; k < 24; k++) { const uint32_t w = r32(gp + 4u * k); if (w != prev[i][k]) diff = true; }
            if (diff) {
                std::fprintf(stderr, "[GSTRUCT] gt=%.0f side=%d step=%.3f", g_gt_ms, i, rdf(FRAME_STEP));
                for (int k = 0; k < 24; k++) { const uint32_t w = r32(gp + 4u * k); prev[i][k] = w; float f; std::memcpy(&f, &w, 4); std::fprintf(stderr, " +%02X=%g", 4 * k, std::fabs(f) > 1e-6f && std::fabs(f) < 1e7f ? f : (float)(int32_t)w); }
                std::fprintf(stderr, "\n");
            }
        }
        const float v = rdf(gp + 0x18u);
        if (v == last[i]) continue;
        const bool changed = last[i] >= 0;
        last[i] = v;
        std::fprintf(stderr, "[ASSIST] gt=%.0f side=%d ctrl=%08X gauge=%.2f addr=%08X step=%.3f\n", g_gt_ms, i, pc, v, gp + 0x18u, rdf(FRAME_STEP));
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
// Action start step (PSPRECOMP_ACTION_START_STEP=0 turns it off for A/B). On the PSP a move's first update advances its clock by one whole 30 Hz
// frame (2 ticks), so what the player sees as the move's first frame is already 33 ms into it, and the chain windows (Jecht's 3 frame
// presses, measured from the first visible frame of the move) open after (a - 2) ticks. At 120 Hz the first update advanced it by 0.5 ticks: every
// window sat 25 ms (1.5 ticks) later than at 30 Hz, at both edges, for every Jecht chain step (tools/combo_timing.py combos, 11 Nov). The first update
// after a fighter's script id changed therefore runs with a 30 Hz sized step (the same as at 30 Hz), the rest as before.
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
    if (g_action_start_step && g_ticks < 2.0f && ok(actor) && g_ram[(actor + 0x542u) & 0x07FFFFFFu] == 0) {
        for (int i = 0; i < 4; i++) {
            if (g_fighters[i] != actor) continue;
            if (g_action_new[i]) ctx->f[0] *= first_step() / g_ticks;
            break;
        }
    }
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
uint32_t g_exb_args[8] = {};
bool g_exb_sticky[8] = {};
int g_exb_nargs = 0;
// Two tests are polled (08B326C4 and 08B324CC); each actor keeps one sticky flag per test.
bool g_exb_sticky2[8] = {};
int g_exb_edges = 0, g_exb_used = 0;
void exb_pressed_common(FuncPtr orig, bool* sticky, uint32_t addr, uint8_t* rdram, recomp_context* ctx) {
    const uint32_t id = overlay_id();
    if (id != 1) {
        FuncPtr f = id == 2 ? ovl_menu_lookup(addr) : id == 3 ? ovl_extra_lookup(addr) : nullptr;
        if (f) f(rdram, ctx);
        return;
    }
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]);
    {   static const bool lg = std::getenv("PSPRECOMP_EXB_LOG") != nullptr; static uint32_t seen[32]; static int ns = 0;
        const uint32_t key = (ok(r32(0x08A405E0u)) ? r32(r32(0x08A405E0u) + 632u) : 99u) * 4u + (g_exb_in ? 1u : 0u);
        bool k = false; for (int i = 0; i < ns; i++) if (seen[i] == key) k = true;
        if (lg && !k && ns < 32) { seen[ns++] = key; std::fprintf(stderr, "[EXBT] test %08X ra=%08X gt=%.0f phase=%u in=%d\n", addr, static_cast<uint32_t>(ctx->r[31]), g_gt_ms, ok(r32(0x08A405E0u)) ? r32(r32(0x08A405E0u) + 632u) : 99u, g_exb_in ? 1 : 0); } }
    orig(rdram, ctx);
    if (ctx->r[2] != 0 && g_exb_in) { g_exb_edges++; if (g_exb_log_on) std::fprintf(stderr, "[EXBG] bt=%.0f edge test=%08X actor=%08X src=run\n", g_play_ms - g_exb_t0, addr, actor); }
    if (!g_exb_in) return;
    int i = 0;
    while (i < g_exb_nargs && g_exb_args[i] != actor) i++;
    if (i == g_exb_nargs && i < 8) { g_exb_args[i] = actor; g_exb_sticky[i] = g_exb_sticky2[i] = false; g_exb_nargs++; }
    if (i < 8) {
        if (sticky[i]) ctx->r[2] = 1;
        sticky[i] = false;
    }
    if (ctx->r[2] != 0) g_exb_used++;
}
void exb_pressed_hook(uint8_t* rdram, recomp_context* ctx) { exb_pressed_common(g_orig_exb_pressed, g_exb_sticky, EXB_PRESSED, rdram, ctx); }
void exb_pressed2_hook(uint8_t* rdram, recomp_context* ctx) { exb_pressed_common(g_orig_exb_pressed2, g_exb_sticky2, EXB_PRESSED2, rdram, ctx); }
void vt_poll_held(uint8_t* rdram, recomp_context* ctx);
void exb_poll_held(uint8_t* rdram, recomp_context* ctx) {
    struct Guard { Guard() { g_exb_poll = true; } ~Guard() { g_exb_poll = false; } } guard;
    vt_poll_held(rdram, ctx);
    for (int i = 0; i < g_exb_nargs; i++) {
        recomp_context tmp = *ctx;
        tmp.r[4] = static_cast<int32_t>(g_exb_args[i]);
        g_orig_exb_pressed(rdram, &tmp);
        if (tmp.r[2] != 0) { g_exb_sticky[i] = true; g_exb_edges++; if (g_exb_log_on) std::fprintf(stderr, "[EXBG] bt=%.0f edge test=%08X actor=%08X src=held\n", g_play_ms - g_exb_t0, EXB_PRESSED, g_exb_args[i]); }
        if (g_orig_exb_pressed2) {
            tmp = *ctx;
            tmp.r[4] = static_cast<int32_t>(g_exb_args[i]);
            g_orig_exb_pressed2(rdram, &tmp);
            if (tmp.r[2] != 0) { g_exb_sticky2[i] = true; if (g_exb_log_on) std::fprintf(stderr, "[EXBG] bt=%.0f edge test=%08X actor=%08X src=held\n", g_play_ms - g_exb_t0, EXB_PRESSED2, g_exb_args[i]); }
        }
    }
}
// PSPRECOMP_EXB_LOG=1: one line per controller update (before the gate) while a burst is active: phase (+632), phase timer (+636),
// +644 flag, the global step, slow-motion object value / time left, whether the update ran the controller.

// PSPRECOMP_EXB_LOG=1: every virtual-button test function (FUN_08B30B54 ... 08B3315C, 37 of them) is counted per burst phase: calls, and
// calls that returned true ("ph3=12/80": 12 true of 80 calls in phase 3). Printed at the end of a burst.
constexpr uint32_t k_vtests[] = {0x08B30B54u, 0x08B30CC0u, 0x08B30D68u, 0x08B30F9Cu, 0x08B31244u, 0x08B313BCu, 0x08B31640u, 0x08B316E8u, 0x08B318BCu, 0x08B31964u, 0x08B31A0Cu, 0x08B31CFCu, 0x08B31DA4u, 0x08B31F54u, 0x08B31FFCu, 0x08B3237Cu, 0x08B32424u, 0x08B324CCu, 0x08B32574u, 0x08B3261Cu, 0x08B326C4u, 0x08B32774u, 0x08B32824u, 0x08B328CCu, 0x08B32974u, 0x08B32A1Cu, 0x08B32AC4u, 0x08B32B6Cu, 0x08B32C14u, 0x08B32CBCu, 0x08B32D64u, 0x08B32E14u, 0x08B32EBCu, 0x08B32F64u, 0x08B3300Cu, 0x08B330B4u, 0x08B3315Cu};
constexpr int kNV = static_cast<int>(sizeof(k_vtests) / sizeof(k_vtests[0]));
FuncPtr g_vt_orig[kNV] = {};
uint32_t g_vt_calls[kNV][6] = {}, g_vt_true[kNV][6] = {};
// Sticky presses for every virtual-button test the gated burst code calls (the two 08B326C4 / 08B324CC have their own hooks above): the
// first gated call of a (test, actor) pair registers it; on skipped updates exb_poll_held polls every registered pair and keeps an edge
// until the next gated call asks. The mash prompt of Kain / Terra uses the test of the displayed button (08B328CC = Square, ...).
struct VtPair { int test; uint32_t actor; bool sticky; };
VtPair g_vt_pairs[48];
int g_vt_npairs = 0;
void vt_poll_held(uint8_t* rdram, recomp_context* ctx) {
    for (int i = 0; i < g_vt_npairs; i++) {
        recomp_context tmp = *ctx;
        tmp.r[4] = static_cast<int32_t>(g_vt_pairs[i].actor);
        g_vt_orig[g_vt_pairs[i].test](rdram, &tmp);
        if (tmp.r[2] & 1) {
            g_vt_pairs[i].sticky = true;
            if (g_exb_log_on) std::fprintf(stderr, "[EXBG] bt=%.0f edge test=%08X actor=%08X src=held\n", g_play_ms - g_exb_t0, k_vtests[g_vt_pairs[i].test], g_vt_pairs[i].actor);
        }
    }
}
template <int I> void vt_probe(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t id = overlay_id();
    if (id != 1) { FuncPtr f = id == 2 ? ovl_menu_lookup(k_vtests[I]) : id == 3 ? ovl_extra_lookup(k_vtests[I]) : nullptr; if (f) f(rdram, ctx); return; }
    const uint32_t who = static_cast<uint32_t>(ctx->r[4]);
    g_vt_orig[I](rdram, ctx);
    const uint32_t c = r32(0x08A405E0u);
    const uint32_t ph = ok(c) ? r32(c + 632u) : 0u;
    if (g_exb_in) {
        int k = 0;
        while (k < g_vt_npairs && !(g_vt_pairs[k].test == I && g_vt_pairs[k].actor == who)) k++;
        if (k == g_vt_npairs && k < 48) { g_vt_pairs[k] = VtPair{I, who, false}; g_vt_npairs++; }
        if (k < 48) {
            if (g_vt_pairs[k].sticky) ctx->r[2] = 1;
            g_vt_pairs[k].sticky = false;
        }
    }
    if (ph < 6) {
        g_vt_calls[I][ph]++;
        if (ctx->r[2] & 1) {
            g_vt_true[I][ph]++;
            if (g_exb_log_on && ph) std::fprintf(stderr, "[EXBG] bt=%.0f ph=%u true test=%08X on=%08X gauge=%.2f in=%d\n", g_play_ms - g_exb_t0, ph, k_vtests[I], who, rdf(c + 596u), g_exb_in ? 1 : 0);
        }
        if (g_exb_log_on && ph && k_vtests[I] == 0x08B328CCu) std::fprintf(stderr, "[EXBG] bt=%.0f ph=%u call test=08B328CC in=%d result=%d\n", g_play_ms - g_exb_t0, ph, g_exb_in ? 1 : 0, static_cast<int>(ctx->r[2] & 1));
    }
}
template <int I> struct VtReg { static void go() { VtReg<I - 1>::go(); if (k_vtests[I] != 0x08B326C4u && k_vtests[I] != 0x08B324CCu && (g_vt_orig[I] = ovl_battle_lookup(k_vtests[I]))) psp_dispatch_register(k_vtests[I], vt_probe<I>); } };
template <> struct VtReg<-1> { static void go() {} };
void vt_summary() {
    for (int i = 0; i < kNV; i++) {
        bool any = false;
        for (int ph = 1; ph < 6; ph++) any = any || g_vt_calls[i][ph];
        if (any) {
            std::fprintf(stderr, "[EXBG] test %08X:", k_vtests[i]);
            for (int ph = 1; ph < 6; ph++) if (g_vt_calls[i][ph]) std::fprintf(stderr, " ph%d=%u/%u", ph, g_vt_true[i][ph], g_vt_calls[i][ph]);
            std::fprintf(stderr, "\n");
        }
        for (int ph = 0; ph < 6; ph++) g_vt_calls[i][ph] = g_vt_true[i][ph] = 0;
    }
}

void exb_log(const char* what, uint32_t ctl, bool ran) {
    static const char* lv = std::getenv("PSPRECOMP_EXB_LOG");
    if (!lv || !ok(ctl)) return;
    const bool verbose = lv[0] == '2';   // =2: also the held (skipped) updates, with the full [EXB] line
    const uint32_t ph = r32(ctl + 632u);
    static uint32_t last_ph = 0xFFFFFFFFu;
    static int n = 0, runs = 0;
    double& t0 = g_exb_t0; static double p0 = 0;
    static int used0 = 0, edges0 = 0;
    if (ph == 0 && last_ph == 0) return;
    const uint32_t sm = r32(SLOWMO_PTR);
    if (ph != last_ph) {
        n = 0;
        if (last_ph == 0xFFFFFFFFu || last_ph == 0) { t0 = g_play_ms; runs = 0; used0 = g_exb_used; edges0 = g_exb_edges; }   // a burst begins
        if (last_ph == 0xFFFFFFFFu || last_ph == 0) {
            for (int w = 0; w < 2; w++) {
                const uint32_t a = r32(ctl + 620u + 4u * w);
                const uint32_t info = ok(a) ? r32(a + 0x2F8u) : 0u;
                std::fprintf(stderr, "[EXBG] burst actor%d=%08X char=%d pad-owner-ctrl=%08X\n", w, a, ok(info) ? static_cast<int8_t>(g_ram[(info + 6u) & 0x07FFFFFFu]) : -99, ok(a) ? r32(a + 0x5C4u) : 0u);
            }
        }
        std::fprintf(stderr, "[EXBG] bt=%.0f phase %u -> %u (fps %d)\n", g_play_ms - t0, last_ph == 0xFFFFFFFFu ? 0u : last_ph, ph, g_rate);
        if (ph == 4) p0 = g_play_ms;
        if (ph == 0 && last_ph != 0xFFFFFFFFu && last_ph != 0) vt_summary();
        if (ph == 0 && last_ph == 4) std::fprintf(stderr, "[EXBG] bt=%.0f burst end: gauge %.2f, presses handed to the burst %d, edges seen %d, runs %d, fps %d\n", g_play_ms - t0,
                                                   rdf(ctl + 596u), g_exb_used - used0, g_exb_edges - edges0, runs, g_rate);
    }
    last_ph = ph;
    if (ran) {
        runs++;
        uint32_t b = 0, an = 0x8080; psp_ctrl_last_sample(&b, &an);
        // one line per burst update at the burst's own 30 Hz rate (identical rows at any frame rate): bt = ms since the burst began,
        // pt = ms since the gauge phase began, gauge = the bar (0..100), vel = its speed, used/edges = presses seen so far
        std::fprintf(stderr, "[EXBG] bt=%.0f pt=%.0f phase=%u gauge=%.2f vel=%.2f used=%d edges=%d btn=%s\n", g_play_ms - t0, ph == 4 ? g_play_ms - p0 : 0.0, ph,
                     rdf(ctl + 596u), rdf(ctl + 604u), g_exb_used - used0, g_exb_edges - edges0, button_names(b & 0xF3F9u).c_str());
    }
    if (!verbose) return;
    if (n++ > 4000) return;
    std::fprintf(stderr, "[EXB] gt=%.0f %s ran=%d phase=%u t636=%d f644=%u pointer=(pos %.2f vel %.2f held %u) step=%.3f slow=(%.3f,%.3f) ticks=%.2f edges=%d used=%d\n", g_gt_ms, what, ran ? 1 : 0, ph,
                 static_cast<int>(r32(ctl + 636u)), r32(ctl + 644u), rdf(ctl + 596u), rdf(ctl + 604u), r32(ctl + 593u) & 0xFFu, rdf(FRAME_STEP), ok(sm) ? rdf(sm) : -1.f, ok(sm) ? rdf(sm + 4u) : -1.f, g_ticks, g_exb_edges, g_exb_used);
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

// ovl FUN_08AEC000(this, ...): the gauge's per-update tick (passive gain via FUN_08AEC338, idle drain scaled by the global step).
constexpr uint32_t GAUGE_TICK = 0x08AEC000u;
FuncPtr g_orig_gauge_tick = nullptr;
void gauge_tick_hook(uint8_t* rdram, recomp_context* ctx) {
    static const char* e = std::getenv("PSPRECOMP_ASSIST_GAUGE");
    const bool log = e && e[0] && e[0] != '0';
    const uint32_t self = static_cast<uint32_t>(ctx->r[4]);
    const uint32_t ga = log ? gauge_of_this(self) : 0;
    const float before = ga ? rdf(ga) : 0.f;
    g_orig_gauge_tick(rdram, ctx);
    if (ga) {
        static int n = 0, left = 6000;
        const float after = rdf(ga);
        n++;
        if ((after != before || n % 60 == 0) && left > 0) {
            left--;
            const uint32_t pc = r32(self + 8u);
            std::fprintf(stderr, "[GAUGE_TICK] gt=%.0f this=%08X %.2f -> %.2f (%+.3f) step=%.3f flags=%02X %02X %02X\n", g_gt_ms, self, before, after, after - before,
                         rdf(FRAME_STEP), (unsigned)(g_ram[(pc + 1456u + 2u) & 0x07FFFFFFu]), (unsigned)(g_ram[(pc + 1456u + 5u) & 0x07FFFFFFu]), (unsigned)(g_ram[(pc + 1456u + 4u) & 0x07FFFFFFu]));
        }
    }
}

// ovl FUN_08AED330(machine, owner): the assist gauge's state machine, one call per update per side. Its drain of the second gauge word
// (+0x1C, the bar that starts at the battle's initial gauge and runs down while nothing is done) is partly per update, not per step
// (FUN_08AEE124: delta = table x 0.01 x ... with no step factor; only some terms use the global step 0x089D5EAC), so the bar emptied
// ~2.5-3x sooner at 120 fps. Above 30 fps it runs every rate/30-th update with the step of the skipped updates added, i.e. exactly
// what the PSP's 30 Hz frame does. PSPRECOMP_GAUGE30=0 turns it off for A/B checks.
constexpr uint32_t GAUGE_SM = 0x08AED330u;
FuncPtr g_orig_gauge_sm = nullptr;
bool g_gauge30 = true;
void gauge_sm_hook(uint8_t* rdram, recomp_context* ctx) {
    if (!g_gauge30 || g_ticks >= 2.0f || overlay_id() != 1) { g_orig_gauge_sm(rdram, ctx); return; }
    static uint32_t key[4] = {}; static float acc[4] = {};
    const uint32_t self = static_cast<uint32_t>(ctx->r[4]);
    int slot = -1;
    for (int i = 0; i < 4; i++) if (key[i] == self) slot = i;
    if (slot < 0) { for (int i = 0; i < 4 && slot < 0; i++) if (!key[i]) slot = i; if (slot < 0) slot = 0; key[slot] = self; acc[slot] = 0; }
    const float step = rdf(FRAME_STEP);
    acc[slot] += step;
    if (acc[slot] < 2.0f - 1e-3f) return;                 // not this update: the PSP frame has not "elapsed" yet
    const float run_step = acc[slot];
    acc[slot] = 0;
    wrf(FRAME_STEP, run_step);
    g_orig_gauge_sm(rdram, ctx);
    wrf(FRAME_STEP, step);
}

// PSPRECOMP_EXB_LOG=1: which of the button-mash style updaters (all six virtual-button tests) the burst calls (log only)
#define EXB_PROBE(NAME, ADDR) \
    FuncPtr g_orig_probe_##NAME = nullptr; \
    void probe_##NAME(uint8_t* rdram, recomp_context* ctx) { \
        const uint32_t id = overlay_id(); \
        if (id != 1) { FuncPtr f = id == 2 ? ovl_menu_lookup(ADDR) : id == 3 ? ovl_extra_lookup(ADDR) : nullptr; if (f) f(rdram, ctx); return; } \
        static int n = 0; if (n++ < 6 || n % 200 == 0) std::fprintf(stderr, "[EXBX] %08X called gt=%.0f n=%d\n", ADDR, g_gt_ms, n); \
        g_orig_probe_##NAME(rdram, ctx); }
EXB_PROBE(a, 0x08A32EBCu)
EXB_PROBE(b, 0x08A3419Cu)
EXB_PROBE(c, 0x08A35B20u)
EXB_PROBE(d, 0x08A36830u)
EXB_PROBE(e, 0x08A38F50u)
EXB_PROBE(f, 0x08A37A00u)


// PSPRECOMP_EXB_LOG=1: every distinct (test function, return address) of the virtual-button tests while a burst runs (log only)
#define EXB_TPROBE(NAME, ADDR) \
    FuncPtr g_orig_tp_##NAME = nullptr; \
    void tp_##NAME(uint8_t* rdram, recomp_context* ctx) { \
        const uint32_t id = overlay_id(); \
        if (id != 1) { FuncPtr f = id == 2 ? ovl_menu_lookup(ADDR) : id == 3 ? ovl_extra_lookup(ADDR) : nullptr; if (f) f(rdram, ctx); return; } \
        static uint32_t seen[16]; static int ns = 0; const uint32_t ra = ok(r32(0x08A405E0u)) ? r32(r32(0x08A405E0u) + 632u) : 99u; bool k = false; \
        for (int i = 0; i < ns; i++) if (seen[i] == ra) k = true; \
        if (!k && ns < 16) { seen[ns++] = ra; std::fprintf(stderr, "[EXBT] test %08X ra=%08X gt=%.0f phase=%u\n", ADDR, ra, g_gt_ms, r32(r32(0x08A405E0u) + 632u)); } \
        g_orig_tp_##NAME(rdram, ctx); }
EXB_TPROBE(t0, 0x08B320A4u)
EXB_TPROBE(t1, 0x08B3214Cu)
EXB_TPROBE(t2, 0x08B321F4u)
EXB_TPROBE(t3, 0x08B322B8u)
EXB_TPROBE(t4, 0x08B3237Cu)
EXB_TPROBE(t5, 0x08B32424u)
EXB_TPROBE(t7, 0x08B32574u)
EXB_TPROBE(t8, 0x08B3261Cu)

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

// PSPRECOMP_TEST_NO_AI=1 (the test harnesses): the CPU brain tick (FUN_08A64400, a0 = brain = actor +0x5C4) returns at once for brains in CPU
// mode (brain +0x54 == 0) once the test clock runs (g_gt_started: skipping it earlier leaves the fight stuck in its intro), so the CPU fighter never chooses an action. The player's brain (human) still ticks.
// The debug menu's Enemy AI option (Settings > Debug) does the same live: Off skips the CPU brains once the fight has been running 7 s (the intro waits for them).
// PSPRECOMP_DBG_NOAI=1 starts with it off.
FuncPtr g_orig_cpu_tick = nullptr;
void cpu_tick_noai_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t brain = static_cast<uint32_t>(ctx->r[4]);
    const bool off = (g_env_no_ai && g_gt_started) || (g_dbg_no_ai && g_battle_age > 420.f);
    if (off && ok(brain) && static_cast<int32_t>(r32(brain + 0x54u)) == 0) return;   // only once the fight runs: the intro waits for the brains
    g_orig_cpu_tick(rdram, ctx);
}

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
    if ((e = std::getenv("PSPRECOMP_ACTION_START_STEP")) && e[0] == '0') g_action_start_step = false;
    testdrive_install();
    if (const char* id = std::getenv("PSPRECOMP_INPUT_DEBUG"); id && id[0] && id[0] != '0') { g_input_dbg = true; std::fprintf(stderr, "[FPS] input debug on (buttons on screen, [INPUT] log)\n"); }
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
    if ((g_orig_exb_pressed2 = ovl_battle_lookup(EXB_PRESSED2))) psp_dispatch_register(EXB_PRESSED2, exb_pressed2_hook);
    VtReg<kNV - 1>::go();   // sticky presses for every virtual-button test the gated burst code calls (+ counts with PSPRECOMP_EXB_LOG)
    if (std::getenv("PSPRECOMP_EXB_LOG")) {
        if ((g_orig_probe_a = ovl_battle_lookup(0x08A32EBCu))) psp_dispatch_register(0x08A32EBCu, probe_a);
        if ((g_orig_probe_b = ovl_battle_lookup(0x08A3419Cu))) psp_dispatch_register(0x08A3419Cu, probe_b);
        if ((g_orig_probe_c = ovl_battle_lookup(0x08A35B20u))) psp_dispatch_register(0x08A35B20u, probe_c);
        if ((g_orig_probe_d = ovl_battle_lookup(0x08A36830u))) psp_dispatch_register(0x08A36830u, probe_d);
        if ((g_orig_probe_e = ovl_battle_lookup(0x08A38F50u))) psp_dispatch_register(0x08A38F50u, probe_e);
        if ((g_orig_probe_f = ovl_battle_lookup(0x08A37A00u))) psp_dispatch_register(0x08A37A00u, probe_f);
    }
    if (std::getenv("PSPRECOMP_EXB_LOG") && (g_orig_exb_prompt = ovl_battle_lookup(EXB_PROMPT))) psp_dispatch_register(EXB_PROMPT, exb_prompt_hook);
    if (const char* x = std::getenv("PSPRECOMP_EXB_UI"); x && x[0] == '0') g_exb_ui = false;
    if ((g_orig_exb_ui = ovl_battle_lookup(EXB_UI))) psp_dispatch_register(EXB_UI, exb_ui_hook);
    if ((g_orig_exb_ctrl = ovl_battle_lookup(EXB_CTRL))) psp_dispatch_register(EXB_CTRL, exb_ctrl_hook);
    else std::fprintf(stderr, "[FPS] EX Burst controller hook NOT installed\n");
    if ((g_orig_gauge_add = ovl_battle_lookup(GAUGE_ADD))) psp_dispatch_register(GAUGE_ADD, gauge_add_hook);
    if ((g_orig_gauge_drain = ovl_battle_lookup(GAUGE_DRAIN))) psp_dispatch_register(GAUGE_DRAIN, gauge_drain_hook);
    if ((g_orig_gauge_hud = ovl_battle_lookup(GAUGE_HUD))) psp_dispatch_register(GAUGE_HUD, gauge_hud_hook);
    if ((g_orig_gauge_tick = ovl_battle_lookup(GAUGE_TICK))) psp_dispatch_register(GAUGE_TICK, gauge_tick_hook);
    if (const char* x = std::getenv("PSPRECOMP_GAUGE30"); x && x[0] == '0') g_gauge30 = false;
    if ((g_orig_gauge_sm = ovl_battle_lookup(GAUGE_SM))) psp_dispatch_register(GAUGE_SM, gauge_sm_hook);
    if (g_orig_pressed) psp_dispatch_register(PRESSED, pressed_hook);
    if (const char* g = std::getenv("PSPRECOMP_JECHT_GRID"); g && g[0] == '0') g_jecht_grid = false;
    psp_ctrl_set_filter(jecht_grid_filter);
    if ((g_orig_chain_check = ovl_battle_lookup(CHAIN_CHECK))) psp_dispatch_register(CHAIN_CHECK, chain_check_hook);
    if ((g_orig_gate_37 = ovl_battle_lookup(GATE_37))) psp_dispatch_register(GATE_37, gate_37_hook);
    if ((g_orig_gate_36 = ovl_battle_lookup(GATE_36))) psp_dispatch_register(GATE_36, gate_36_hook);
    if ((g_orig_gate_3a = ovl_battle_lookup(GATE_3A))) psp_dispatch_register(GATE_3A, gate_3a_hook);
    if (const char* d = std::getenv("PSPRECOMP_DBG_COMBO"); d && d[0] && d[0] != '0') g_dbg_combo = true;   // start with the debug options on (tests)
    if (const char* d = std::getenv("PSPRECOMP_DBG_NOAI"); d && d[0] && d[0] != '0') g_dbg_no_ai = true;
    if (const char* na = std::getenv("PSPRECOMP_TEST_NO_AI"); na && na[0] && na[0] != '0') g_env_no_ai = true;
    if ((g_orig_cpu_tick = ovl_battle_lookup(CPU_TICK))) psp_dispatch_register(CPU_TICK, cpu_tick_noai_hook);   // always: the debug menu switches it live
    else std::fprintf(stderr, "[FPS] CPU brain tick not found: no Enemy AI option\n");
    if (g_orig_cpu_tick) {
        psp_settings_add_debug_toggle({"Enemy AI", "Enemy AI (debug)\nOff: the CPU fighter never chooses an action; it stands and takes hits. Starts to apply about 7 s into a fight. This session only.",
                                       +[]() { return !g_dbg_no_ai; }, +[](bool on) { g_dbg_no_ai = !on; }});
    }
    psp_settings_add_debug_toggle({"Jecht Combo Timing", "Jecht combo helper (debug)\nThe screen edge flashes green when the window for his next chain press opens, and a press says GOOD, TOO EARLY or TOO LATE with the milliseconds off. Rush, Neutral / Up 2 and 3, HP. This session only.",
                                   +[]() { return g_dbg_combo; }, +[](bool on) { g_dbg_combo = on; }});
    if (g_test_ai_off && (g_orig_pad_set_state = ovl_battle_lookup(PAD_SET_STATE)) && (g_orig_pad_set_pending = ovl_battle_lookup(PAD_SET_PENDING))) {
        psp_dispatch_register(PAD_SET_STATE, pad_set_state_hook); psp_dispatch_register(PAD_SET_PENDING, pad_set_pending_hook);
        std::fprintf(stderr, "[FPS] test: CPU AI input off\n");
    }
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
