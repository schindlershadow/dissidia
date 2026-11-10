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
#include "psp_settings.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_adhoc.h"
#include "psp_ge_draw.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <unordered_map>

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
extern FuncPtr ovl_menu_lookup(uint32_t vaddr);
extern FuncPtr ovl_extra_lookup(uint32_t vaddr);

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
constexpr uint32_t OWN_MASK0 = 0x089C7FD8u, OWN_MASK1 = 0x089C7FDCu;
constexpr uint32_t CAM_DT = 0x08B3B4FCu;        // battle overlay: a camera's time step

uint8_t* g_ram = nullptr;
FuncPtr g_orig_pressed = nullptr, g_orig_rootmotion = nullptr, g_orig_slowmo = nullptr, g_orig_frame_end = nullptr, g_orig_pad = nullptr, g_orig_actor_dt = nullptr, g_orig_cam_dt = nullptr;
std::atomic<int> g_target{30};     // requested rate (settings / env)
int g_interval = 2;                // interval written on the last frame end (2 = 30 Hz, 1 = 60 Hz and above)
int g_rate = 30;                   // logic rate in effect (30, 60, 90, ... one frame per vblank above 30)
float g_ticks = 2.0f;              // 1/60 s ticks one frame advances: 60 / g_rate
int g_fx_period = 1;               // updates per effect-component step at 30 Hz pacing: g_rate / 30
bool g_scope_all = false, g_log = false;
int g_switch_logs = 0;
int g_rm_log_left = 300;
bool g_fighter_log = false;   // PSPRECOMP_FIGHTER_LOG (see fighter_log_frame)
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

void fx_log_window();
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
uint32_t g_fighters[4] = {};
uint64_t g_fighter_seen[4] = {};   // g_tick when the fighter's step was last asked for (stale = left the battle)

// Press latch (PSPRECOMP_PRESS_LATCH=0 turns it off). The fighter code asks the virtual pad "was bit B pressed this update"
// (FUN_08AB399C: state & ~previous state). The PSP ran 30 updates a second, so a press was seen for 33 ms; at 120 it is seen for
// 8 ms, and the timed chain inputs (Jecht: 3 to 5 frame windows) are four times harder to hit. A press is therefore reported as
// pressed for 2 / step updates in total (4 at 120 Hz), as long as the fighter has not started another action (script id) since:
// the press that starts a move must not count again for that move's own early-press lockout (event 0x004C).
bool g_press_latch = true;
uint32_t g_press_calls = 0, g_press_edges = 0, g_press_latched = 0, g_press_dropped = 0;   // counters for the [FPS] log
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
    const uint64_t window = static_cast<uint64_t>(2.0f / g_ticks + 0.5f);   // updates a press stays "pressed"
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
uint32_t g_dump_prev[4][0x700 / 4] = {};
uint32_t g_dump_prev_actor[4] = {};
void fighter_dump_frame() {
    if (!g_fighter_dump) return;
    static const uint32_t ranges[][2] = {{0x20, 0x60}, {0x180, 0x200}, {0x380, 0x3A0}, {0x540, 0x600}, {0x1A0, 0x1C0}};
    for (int i = 0; i < 4 && g_fighter_dump_left > 0; i++) {
        const uint32_t a = g_fighters[i];
        if (!a || !ok(a)) continue;
        const uint32_t scr = r32(a + 0x540u) & 0xFFFFu;
        if (scr == 0xFFFFu) continue;
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
void fighter_log_frame() {
    if (!g_fighter_log) return;
    for (int i = 0; i < 12 && g_object_log_left > 0; i++) {   // motion time of the other actors, whenever it changes
        const uint32_t a = g_objects[i];
        if (!a || !ok(a)) continue;
        const float t = rdf(a + 0x1A8u);   // motion player (actor+0x130) time
        if (t == g_object_time[i]) continue;
        g_object_time[i] = t;
        g_object_log_left--;
        std::fprintf(stderr, "[MOTION] t=%llu %08X kind=%u step=%.3f time=%.3f prev=%.3f end=%.3f len=%.3f scr=%u\n", (unsigned long long)g_tick, a,
                     (unsigned)g_ram[(a + 0x542u) & 0x07FFFFFFu], rdf(FRAME_STEP), t, rdf(a + 0x1A4u), rdf(a + 0x1A0u), rdf(a + 0x1BCu),
                     r32(a + 0x540u) & 0xFFFFu);
    }
    fighter_dump_frame();
    if (g_fighter_log_left <= 0) return;
    for (uint32_t a : g_fighters) {
        if (!a || !ok(a)) continue;
        const uint32_t scr = r32(a + 0x540u) & 0xFFFFu;
        if (scr == 0xFFFFu) continue;
        g_fighter_log_left--;
        std::fprintf(stderr, "[FIGHTER] t=%llu %08X scr=%u step=%.3f y=%.3f floor=%.3f fl=%08X f110=%.3f f114=%.3f f134=%.3f f0F4=%.3f f104=%.3f f144=%.3f vel=%.3f,%.3f,%.3f\n",
                     (unsigned long long)g_tick, a, scr, rdf(FRAME_STEP), rdf(a + 0x34u), rdf(a + 0x2E4u), r32(a + 0x380u), rdf(a + 0x110u),
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
    g_press_latch = !((e = std::getenv("PSPRECOMP_PRESS_LATCH")) && e[0] == '0');
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
    if (g_orig_pressed) psp_dispatch_register(PRESSED, pressed_hook);
    std::fprintf(stderr, "[FPS] press latch %s (pressed-test hook %s)\n", g_press_latch ? "on" : "off", g_orig_pressed ? "installed" : "NOT FOUND");
    if ((g_orig_rootmotion = psp_dispatch_probe_lookup(ROOT_MOTION))) psp_dispatch_register(ROOT_MOTION, root_motion_hook);
    else std::fprintf(stderr, "[FPS] root motion hook NOT installed (no function at %08X)\n", ROOT_MOTION);
    if ((g_orig_slowmo = psp_dispatch_probe_lookup(SLOWMO_FRAME))) psp_dispatch_register(SLOWMO_FRAME, slowmo_frame_hook);
    else std::fprintf(stderr, "[FPS] slow-motion hook NOT installed (no function at %08X)\n", SLOWMO_FRAME);
    if (g_log && (g_orig_skel = psp_dispatch_probe_lookup(SKEL_BUILD))) psp_dispatch_register(SKEL_BUILD, skel_build_hook);
    fx_install();
    psp_settings_set_frame_rate_hook({k_rates, static_cast<int>(std::size(k_rates)), fps_get, fps_set});
    psp_adhoc_set_local_fps(g_target.load());
    std::fprintf(stderr, "[FPS] frame rate %d (battle logic; scope %s)\n", g_target.load(), g_scope_all ? "all" : "battle");
}
