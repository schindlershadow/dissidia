// games/dissidia/runtime/camera.cpp — analogue right-stick camera for Dissidia 012 battles.
//
// How the battle camera works (reverse-engineered, see games/dissidia/CAMERA.md):
//   * The camera manager (a0 of ovl_battle FUN_08B3CA24, one per battle; 0x09AE8C70 in our runs)
//     is itself a camera object: view matrix at +0x00, eye +0x40, target +0x50, up +0x60, proj at
//     +0xC0.... FUN_08B3CA24 runs once per frame: it blends up to 8 camera-mode slots
//     (objects at manager+0x6D0[i]; d-pad rotation, lock-on framing, ... all live in those modes),
//     producing a final eye/target/up on its own stack (sp+0xA0 eye, sp+0xB0 target, sp+0x30 up),
//     then calls   SetEye(mgr,&eye) SetTarget(mgr,&tgt) SetUp(mgr,&up) BuildLookAt(mgr)   (FUN_0885EAA8 /
//     EAC4 / EAE0 / E860), adds a shake offset, repeats the four calls, and copies the result to the render
//     camera (FUN_088A5F1C).
//   * We wrap FUN_08B3CA24 (to learn the manager and run once per frame) and the EBOOT's SetEye
//     (FUN_0885EAA8): on the first SetEye(mgr, ...) of each update we rotate the eye vector, in place on
//     the guest stack, about the target (which sits 0x10 after the eye on the same stack frame) by the
//     accumulated stick offset. Everything downstream (lookAt, shake, the copy-out to the render camera FUN_088A5F1C
//     and to the global camera [0x089D5EC0]+0x720 by FUN_08B3C0D8) then sees the rotated camera. The game's own
//     state (d-pad rotation, lock-on, auto-centring, collision pull-in) is untouched: the stick is an offset on top.
//   * Everything this module does is VIEW state (per player, wall-clock smoothed, it runs on frames a lock-step battle
//     holds back) and must never reach the simulation. Checked (gdb read watchpoints, single-player battle with the left
//     stick held): the manager's matrix / eye / target are read only inside FUN_08B3CA24 and its copy-outs; the global
//     copy is read by the render-matrix builders (FUN_08866BA8 ...) and two battle-overlay readers (FUN_08B53C20 eye,
//     FUN_08ABBC48 matrix; not identified). A two-copy battle with one player's view
//     turned by the right stick while moving stays in sync (2100 frames). The one leak was input.cpp turning the
//     left stick by dissidia_camera_turn() for every pad the battle processes; in a lock-step battle that turn is now
//     applied only to the local stick before it is sent (input.cpp, docs/multiplayer.md "Camera and stick").
//
// Options: PSPRECOMP_CAM_FREE=0 disables (stick then acts as D-pad, see psp_hle_ctrl.cpp), PSPRECOMP_CAM_SENS
// (1.0 = 150 deg/s yaw at full deflection), PSPRECOMP_CAM_INVERT_X=1 / PSPRECOMP_CAM_INVERT_Y=1, PSPRECOMP_CAM_PITCH_MIN/MAX (deg,
// elevation of the eye above the target; default -5..80), R3 (right stick click, or the C key) puts the camera back at once;
// PSPRECOMP_CAM_RECENTER=<seconds> (time constant
// for the offset to drift back to zero once the stick has been idle for PSPRECOMP_CAM_RECENTER_DELAY seconds;
// defaults 1.0 and 0.5; 0 = hold the offset),
// Not locked on (issue #20): the camera is held (see g_hold_on) - PSPRECOMP_CAM_UNLOCK_HOLD=0 disables.
// PSPRECOMP_CAM_PIVOT=0 orbits the game's look-at point instead of the fighter (the old way);
// PSPRECOMP_CAM_LOG=1 is the GE-level log; PSPRECOMP_CAMX_LOG=<n> logs n updates of this module.

#include "psp_memory.h"
#include "psp_game_module.h"
#include "psp_runtime.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include "psp_ge_draw.h"
#include "psp_ge.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern FuncPtr dissidia_ovl_battle_lookup(uint32_t vaddr);
extern FuncPtr dissidia_ovl_menu_lookup(uint32_t vaddr);
extern FuncPtr dissidia_ovl_extra_lookup(uint32_t vaddr);
extern void dissidia_tagteam_frame(uint8_t* rdram, recomp_context* ctx);   // tagteam.cpp: the tag swap's safe point (the camera step runs once per frame, outside any fighter's update)
bool dissidia_nearest_fighter(const float eye[3], float pos[3]);   // framerate.cpp
uint32_t dissidia_nearest_actor();   // framerate.cpp
extern uint32_t g_dissidia_cine_actor, g_dissidia_cine_seq;   // roster.cpp (hitcam=)
extern int g_dissidia_cine_ticks;
float dissidia_battle_age();
float dissidia_tagteam_age();   // tagteam.cpp: its battle clock (moves with the simulation)
bool dissidia_tagteam_body(uint32_t A, float* ymin, float* ymax);   // tagteam.cpp: lowest / highest bone of the fighter's model now (model space, metres)
bool dissidia_tagteam_intro_hold();   // tagteam.cpp: the partners' banters hold the retail intro now
float dissidia_tagteam_intro_delay();   // tagteam.cpp: ticks the retail intro was held for the partners' banters
uint32_t dissidia_player_actor();   // framerate.cpp: the human-controlled fighter

float g_dissidia_last_eye[3] = {}, g_dissidia_last_tgt[3] = {}; uint32_t g_dissidia_last_cam = 0;   // the last SetEye (any camera object), for logs
constexpr float kBanterLead = 18.f;   // ticks
float g_pre_fov = 0.f;   // the retail camera's fov before the partners' banters held the intro
float last_bE[3], last_bF[3], last_bfov = 0.f, last_b_age = 0.f; bool have_b = false, grace_done = true;   // the last banter shot drawn
float g_entr_fov = 0.f; bool g_fov_scaled = false;   // the game's own fov before the first banter shot wrote its own: put back when the shots are over   // the fov the current entrance shot wants (0 = the game's)
bool g_dissidia_entr_tight = false;   // the shot is a start-of-battle banter's: as tight as the retail intro close-up (eye ~1.8-2.4 m in front, chest height)
bool g_dissidia_entr_more = false;    // another banter follows this one at once: no ease out to the game's camera between them
uint32_t g_dissidia_entr_actor = 0; float g_dissidia_entr_pose_at = 0.f, g_dissidia_entr_until = 0.f, g_dissidia_entr_t0 = 0.f, g_dissidia_entr_scale = 1.f;
uint32_t g_dissidia_vic_actor = 0;   // the winner of a finished tag battle (tagteam.cpp): vic_frame
namespace {

constexpr uint32_t OVL_BASE = 0x089DD4C0u;
constexpr uint32_t CAM_UPDATE = 0x08B3CA24u;   // ovl_battle: per-frame camera manager update
constexpr uint32_t CAM_SETEYE = 0x0885EAA8u;   // EBOOT: camera->eye = *a1

uint8_t* g_ram = nullptr;
FuncPtr g_orig_update = nullptr, g_orig_seteye = nullptr;
uint32_t g_mgr = 0;
bool g_in_update = false;
int g_eye_calls = 0;
bool g_free = true, g_invert = false;
float g_sens = 1.0f, g_pitch_min = -5.f, g_pitch_max = 80.f, g_recenter = 1.0f, g_recenter_delay = 0.5f, g_idle = 0.f;
float g_yaw = 0.f, g_pitch = 0.f;   // accumulated offsets (radians)
double g_last_t = 0;
int g_log_left = 0;
float g_final_eye[3] = {0, 0, 0};   // the eye as SetEye leaves it (after our own changes)
bool g_blocker_on = true;   // PSPRECOMP_CAM_BLOCKER_FADE=0: no fade of walls that block our own camera
bool g_pivot_on = true, g_invert_x = false;   // orbit the fighter (PSPRECOMP_CAM_PIVOT=0: orbit the game's look-at point, the old way)
// hitcam= framing (roster.cpp posts the request when a hitact= action starts)
bool g_cine_on = true;
uint32_t g_cine_seen = 0, g_cine_actor = 0;
double g_cine_t0 = 0, g_cine_len = 0;
float g_cine_side = 0.f;
int g_cine_logs = 0;
// Not locked on (issue #20): the game's camera follows the fighter and re-centres by itself; the left stick must not move
// the camera then, only the right stick does. While the player has no lock-on target the camera keeps its direction and follows the fighter
// (fixed orientation and distance, following the fighter, the right stick orbits the held look-at point, no auto re-centre). PSPRECOMP_CAM_UNLOCK_HOLD=0 turns it off.
constexpr uint32_t LOCK_TARGET = 0x08B3B280u, ACT_LOCK = 0x5ACu;   // FUN_08B3B280(*(actor + 0x5AC)) = the lock-on target, 0 = none
FuncPtr g_lock_fn = nullptr;
double g_first_lock_t = 0, g_force_unlock_s = -1;   // PSPRECOMP_CAM_FORCE_UNLOCK=<s>: test aid, report 'not locked' that long after the first lock
bool g_ever_locked = false;
float g_arm_ticks = 600.f;   // PSPRECOMP_CAM_HOLD_ARM=<ticks>: with no lock yet, the hold starts this long after the battle began
bool g_hold_on = true, g_locked = true, g_lock_known = false, g_holding = false, g_held_valid = false;
float g_held_p[3] = {0, 0, 0}, g_off[3] = {0, 0, 0};
bool g_held_p_set = false;
float g_held_e[3], g_held_t[3], g_game_e[3], g_game_t[3];
bool g_game_valid = false;
float g_turn = 0.f, g_relock_w = 0.f;
int g_lock_logs = 0;
constexpr uint32_t ACT_POS = 0x80u, ACT_TARGET = 0x2FCu;    // actor position (vec3), its target actor (roster.cpp)
// Smoothing (all frame-rate independent, time constants in seconds):
//  * the stick drives an angular velocity that eases towards the stick (g_stick_tc), so turns start and stop softly;
//  * the orbit pivot (the player's chest) follows the fighter with a short lag (g_pivot_tc): with the view turned, eye =
//    pivot + R(game eye - pivot), so every jitter of a raw pivot (steps, landing, dashes) was multiplied on screen;
//  * the orbit radius follows the game's own distance with a lag (g_dist_tc): the game zooms and pulls in for ITS view
//    direction (collision behind its own eye, lock-on framing), which pumped the turned view in and out;
//  * the held rig (no lock-on) follows the fighter with a lag, more vertically (jumps no longer bob the view 1:1);
//  * re-centring ramps in after the idle delay instead of starting at full speed.
float g_dt = 0.f;
float g_stick_tc = 0.07f, g_pivot_tc = 0.12f, g_dist_tc = 0.35f, g_hold_tc = 0.15f, g_hold_tc_y = 0.4f;
float g_vyaw = 0.f, g_vpitch = 0.f;           // angular velocity (rad/s)
float g_piv[3] = {0, 0, 0}; bool g_piv_ok = false;
float g_rad = 0.f; bool g_rad_ok = false;
float g_off_s[3] = {0, 0, 0}; bool g_off_ok = false;
float ease_k(float tc) { return tc > 0.f ? 1.f - std::exp(-g_dt / tc) : 1.f; }
// Collision of our own view (turned or held) with the stage (ge_draw_camera_probe_*): the game pulls its camera in for walls
// behind ITS eye only. The allowed distance pulls in fast and lets out slowly. PSPRECOMP_CAM_COLLIDE=0 turns it off.
bool g_cc_on = true;
float g_col = 0.f; bool g_col_ok = false;
uint32_t g_cc_id = 0;
bool g_cc_done = false;   // once per update (the game sets the eye a second time with the shake added)

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t now_ms() { return static_cast<int64_t>(now_s() * 1000.0); }

float rdf(uint32_t a) { float f; std::memcpy(&f, g_ram + (a & 0x07FFFFFFu), 4); return f; }
void wrf(uint32_t a, float f) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &f, 4); }

bool ram_ok(uint32_t a, uint32_t n) { return psp_guest_user_ok(a, n); }
bool rdvec(uint32_t a, float* v) {
    if (!ram_ok(a, 12)) return false;
    for (int i = 0; i < 3; i++) { v[i] = rdf(a + 4 * i); if (!std::isfinite(v[i]) || std::fabs(v[i]) > 2000.f) return false; }
    return true;
}
float smooth01(float x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3 - 2 * x); }

// The hitcam framing weight now (0 = the game's camera), and the eye / look-at it wants; false when off.
// A push-in on the game camera's own line: eye = Noctis's chest - (35 % closer) along the game eye's direction, at
// least 2.5 m away, and backed out to stay >= 12 m from the Alterna sphere's centre (foe + 3 m; v8h, v8j, v8k).
// Look-at = his chest + 35 % of the way to the foe's centre, 0.3 m up (Alterna v8f: at 60 % / 0.6 m the look-at
// followed the foe held up in the sphere and Noctis's raised-arm cast and throw left the frame; at 35 % he stays in
// it, the sphere behind him, and the downward slam keeps the foe in view as it falls).
bool cine_frame(const float* eye, const float* tgt, float* E, float* F, float* w) {
    (void)tgt;
    if (!g_cine_on || !g_cine_actor) return false;
    const double t = now_s() - g_cine_t0;
    if (t < 0 || t > g_cine_len) { g_cine_actor = 0; return false; }
    *w = std::fmin(smooth01(static_cast<float>(t / 0.25)), smooth01(static_cast<float>((g_cine_len - t) / 0.4)));
    float P[3], T[3];
    uint32_t tg;
    if (!rdvec(g_cine_actor + ACT_POS, P)) return false;
    if (!ram_ok(g_cine_actor + ACT_TARGET, 4)) return false;
    std::memcpy(&tg, g_ram + ((g_cine_actor + ACT_TARGET) & 0x07FFFFFFu), 4);
    if (!ram_ok(tg, 0x100) || !rdvec(tg + ACT_POS, T)) return false;
    const float C[3] = {P[0], P[1] + 1.2f, P[2]};          // Noctis's chest
    float v[3] = {eye[0] - C[0], eye[1] - C[1], eye[2] - C[2]};
    const float L = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (L < 1e-3f) return false;
    float L2 = std::fmin(L, std::fmax(2.5f, 0.65f * L));         // 35 % closer, not under 2.5 m
    // Alterna v8h-v8k: the glass sphere (radius 3 m, its shards out to 3.15 m) is centred 3 m above the foe;
    // a push-in that brings the eye within kSphereClear of that centre puts the lens in / against the shards
    // (23:20: the screen filled with the crack lattice). Back the eye out along the same line until it clears,
    // at most 30 % past the game's own distance.
    constexpr float kSphereClear = 12.0f;
    const float S[3] = {T[0], T[1] + 3.0f, T[2]};
    auto dist_s = [&](float l) {
        float d2 = 0.f;
        for (int i = 0; i < 3; i++) { const float e = C[i] + v[i] / L * l - S[i]; d2 += e * e; }
        return std::sqrt(d2);
    };
    while (dist_s(L2) < kSphereClear && L2 < 1.3f * L) L2 = std::fmin(1.3f * L, L2 + 0.25f);
    for (int i = 0; i < 3; i++) E[i] = C[i] + v[i] / L * L2;
    for (int i = 0; i < 3; i++) F[i] = C[i] + 0.35f * ((T[i] + (i == 1 ? 1.3f : 0.f)) - C[i]);
    F[1] += 0.3f;
    if (g_cine_logs < 4) {
        g_cine_logs++;
        std::fprintf(stderr, "[HITCAM] push-in on actor %08X: eye %.1f -> %.1f m from him, %.1f m from the sphere, look-at (%.1f,%.1f,%.1f), w %.2f\n",
                     g_cine_actor, L, L2, dist_s(L2), F[0], F[1], F[2], *w);
    }
    return true;
}

// The tag team call-in (tagteam.cpp sets these): while the warp-in runs the camera turns to the entrant; during his battle-start pose it cuts in to
// a front close-up like the opening banter (eye in front of him toward his foe, slightly to the side, pushing in), then eases back.
bool entr_frame(const float* eye, const float* tgt, float* E, float* F, float* w) {
    (void)tgt;
    if (!g_dissidia_entr_actor) return false;
    const float age = dissidia_tagteam_age();   // the tag clock, the one the entrance times are in
    if (age >= g_dissidia_entr_until && g_dissidia_entr_tight && have_b && !grace_done && !std::getenv("PSPRECOMP_BANTER_NOGRACE")) {   // the last banter shot stays for ONE more camera update: the retail camera runs its first update of the "vs" shot in it, unseen (no frozen-camera frame between the cut)
        grace_done = true; for (int i = 0; i < 3; i++) { E[i] = last_bE[i]; F[i] = last_bF[i]; } g_entr_fov = last_bfov; *w = 1.f; return true;
    }
    if (age >= g_dissidia_entr_until) { if (std::getenv("PSPRECOMP_ENTRCAM_LOG")) std::fprintf(stderr, "[ENTRCAM] shot over: actor %08X age %.0f until %.0f\n", g_dissidia_entr_actor, age, g_dissidia_entr_until); g_dissidia_entr_actor = 0; return false; }
    float P[3]; if (!rdvec(g_dissidia_entr_actor + ACT_POS, P)) { if (std::getenv("PSPRECOMP_ENTRCAM_LOG")) std::fprintf(stderr, "[ENTRCAM] no position of %08X\n", g_dissidia_entr_actor); return false; }
    const float C[3] = {P[0], P[1] + 1.1f, P[2]};
    const float out = (g_dissidia_entr_tight && g_dissidia_entr_more) ? 1.f : smooth01((g_dissidia_entr_until - age) / 18.f);   // 0.3 s ease out (ages are 60ths); not between two banters
    if (g_dissidia_entr_pose_at > 0.f && age < g_dissidia_entr_pose_at) {   // warp-in: the game's eye, looking at him
        for (int i = 0; i < 3; i++) { E[i] = eye[i]; F[i] = C[i]; }
        *w = std::fmin(out, 0.85f);
        return true;
    }
    // in front of him: his facing (model -> world matrix at actor +0x330, row 2 = forward), like the battle-start banter shots
    // taken once when the pose starts (the pose itself turns some characters around: a per-frame facing swung the camera behind them)
    static uint32_t latched_for = 0; static float latched_at = -1.f, ld[2] = {0.f, 1.f};
    static float last_E[3], last_F[3], last_age = -1e9f, base_E[3], base_F[3]; static bool have_last = false, chain = false;   // chain: this banter follows another at once (pans from its last shot)
    if (latched_for != g_dissidia_entr_actor || latched_at != g_dissidia_entr_pose_at) {
        latched_for = g_dissidia_entr_actor; latched_at = g_dissidia_entr_pose_at;
        chain = g_dissidia_entr_tight && have_last && age - last_age < 8.f; if (chain) for (int i = 0; i < 3; i++) { base_E[i] = last_E[i]; base_F[i] = last_F[i]; }
        // toward his foe: the battle-start pose turns him to face the opponent (his facing at the start of the pose is still the fly-in's)
        uint32_t foe; std::memcpy(&foe, g_ram + ((g_dissidia_entr_actor + 0x300u) & 0x07FFFFFFu), 4);
        float T[3];
        // his model's facing (the matrix forward row); PSPRECOMP_ENTRCAM_FOE=1: toward the foe
        if (std::getenv("PSPRECOMP_ENTRCAM_FOE") && ram_ok(foe, 0x100) && rdvec(foe + ACT_POS, T)) { ld[0] = T[0] - P[0]; ld[1] = T[2] - P[2]; }
        else {   // (until 19 Nov: the forward row in the object at actor +0x398 (+0x240 x, +0x248 z), chosen because the yaw word +0x9C and the matrix at +0x330
            // did not follow the model of a member called in)
            // (19 Nov: that "row" is not a facing at all - it read NaN, (0.15, 3.14), (0, 0), (1, 0) for different members. The model is drawn with the
            // actor's world matrix +0x00 / +0x330, forward = row 2 (x +0x350, z +0x358), which the game builds every tick from the forward vector +0x90;
            // tagteam.cpp face_foe() -> model_face_yaw() now turns that vector toward the foe. PSPRECOMP_ENTRCAM_OLDDIR=1: the old +0x398 row.)
            uint32_t mo; std::memcpy(&mo, g_ram + ((g_dissidia_entr_actor + 0x398u) & 0x07FFFFFFu), 4);
            if (std::getenv("PSPRECOMP_ENTRCAM_YAW")) { const float yaw = rdf(g_dissidia_entr_actor + 0x9Cu); ld[0] = std::sin(yaw); ld[1] = std::cos(yaw); }
            else if (std::getenv("PSPRECOMP_ENTRCAM_OLDDIR")) { if (ram_ok(mo, 0x300)) { ld[0] = rdf(mo + 0x240u); ld[1] = rdf(mo + 0x248u); } }
            else { ld[0] = rdf(g_dissidia_entr_actor + 0x350u); ld[1] = rdf(g_dissidia_entr_actor + 0x358u); } }
        if (std::getenv("PSPRECOMP_ENTRCAM_LOG")) std::fprintf(stderr, "[ENTRCAM] latched dir %.3f %.3f at %.1f %.1f %.1f (eye %.1f %.1f %.1f)\n", ld[0], ld[1], P[0], P[1], P[2], eye[0], eye[1], eye[2]);
        if (!(ld[0] * ld[0] + ld[1] * ld[1] > 1e-4f)) { ld[0] = eye[0] - P[0]; ld[1] = eye[2] - P[2]; }
    }
    if (!std::getenv("PSPRECOMP_ENTRCAM_OLDDIR") && !std::getenv("PSPRECOMP_ENTRCAM_FOE") && !std::getenv("PSPRECOMP_ENTRCAM_YAW")) {   // the facing the model is drawn with now (the pose start turns him once more)
        const float fx = rdf(g_dissidia_entr_actor + 0x350u), fz = rdf(g_dissidia_entr_actor + 0x358u); if (std::isfinite(fx) && std::isfinite(fz) && fx * fx + fz * fz > 0.25f) { ld[0] = fx; ld[1] = fz; } }
    float d[3] = {ld[0], 0.f, ld[1]};
    const float L = std::sqrt(d[0] * d[0] + d[2] * d[2]); if (L < 1e-3f) return false;
    d[0] /= L; d[2] /= L;
    float since = age - (g_dissidia_entr_pose_at > 0.f ? g_dissidia_entr_pose_at : (g_dissidia_entr_tight && g_dissidia_entr_t0 > 0.f) ? g_dissidia_entr_t0 : (g_dissidia_entr_until - 150.f));   // (a banter whose line made it longer than 2.5 s: its shots run from its start)
    // banter: measured on the retail intro close-ups of the slot fighters (PSPRECOMP_INTROCAM_LOG): eye 2.1 - 2.4 m away horizontally, 1.75 - 2.0 m above his feet, looking at
    // 1.6 - 1.8 m, with a NARROW field of view (camera object + 0xC0: 21 - 26 degrees, the game's own is 50): head and chest fill the picture. The banter shot takes the same
    // (eye 2.2 -> 1.9 m, +1.75 m, look-at +1.55 m, fov 26); the call-in pose keeps its waist-up 2.8 -> 2.3 m at the normal fov.
    const bool tight = g_dissidia_entr_tight && !std::getenv("PSPRECOMP_BANTER_LOOSE");
    const float k = smooth01(since / 150.f), dist = tight ? 2.2f - 0.3f * k : 2.8f - 0.5f * k;          // push in from 2.8 to 2.3 m over the pose (waist-up, like the banter shots)
    static const float side = std::getenv("PSPRECOMP_ENTRCAM_SIDE") ? static_cast<float>(std::atof(std::getenv("PSPRECOMP_ENTRCAM_SIDE"))) : 0.f;   // 0: straight in front (0.7: the old three-quarter view)
    const float sx = -d[2] * side / 0.7f, sz = d[0] * side / 0.7f;
    E[0] = C[0] + d[0] * dist + sx * 0.7f; E[1] = C[1] + (tight ? 0.65f : 0.45f); E[2] = C[2] + d[2] * dist + sz * 0.7f;
    F[0] = C[0]; F[1] = C[1] + (tight ? 0.45f : 0.3f); F[2] = C[2];
    g_entr_fov = tight ? 26.f : 0.f;
    static const bool cam_old = std::getenv("PSPRECOMP_BANTER_CAMOLD") != nullptr;
    if (tight && !cam_old) {
        // 25 Nov (user play-test 3): every banter shot is retail's own intro shot of a fighter, measured on the retail 1v1 intro (PSPRECOMP_INTROCAM_LOG=2, [INTROPATH]; the enemy slot
        // fighter's two shots, ages 168 - 326 of `~/dissidia/tt/s2/ttbanter3/p0`, the same numbers in `q0`): E1, a low wide shot from his right (eye 0.28 m up, 1.7 m in front, 1.6 m to
        // the right, looking at a point 1.1 m in front / 0.9 m right of him, so he stands off-centre; fov 36) for 122 ticks, then E2, a front close-up (eye 1.75 m in front at 1.04 m,
        // look-at 0.8 m in front at 1.27 m, fov 23.1) for the rest. Numbers are in the fighter's frame (forward = matrix row 2, right = (fz, -fx), up from his feet). Same shot for every
        // banter, a HARD cut at the start (retail cuts from the human's shot to the enemy's) and at the end. (Retail's per-character shots differ a little in height; the numbers are one character's.)
        struct K { float t, ef, es, eu, lf, ls, lu, fov; };
        static const K ks[] = {{0, 1.73f, 1.70f, 0.28f, 1.35f, 0.91f, 0.76f, 36.f}, {2, 1.73f, 1.70f, 0.28f, 1.33f, 0.91f, 0.76f, 36.f}, {14, 1.73f, 1.66f, 0.28f, 1.23f, 0.92f, 0.73f, 36.f},
                               {26, 1.75f, 1.59f, 0.28f, 1.12f, 0.94f, 0.70f, 36.f}, {38, 1.76f, 1.56f, 0.28f, 1.08f, 0.94f, 0.68f, 36.f}, {50, 1.77f, 1.55f, 0.29f, 1.08f, 0.94f, 0.68f, 36.f},
                               {62, 1.77f, 1.56f, 0.28f, 1.09f, 0.94f, 0.68f, 36.f}, {121, 1.77f, 1.56f, 0.28f, 1.09f, 0.94f, 0.68f, 36.f},
                               {122, 1.78f, 0.10f, 1.04f, 0.81f, -0.05f, 1.27f, 23.1f}, {158, 1.71f, 0.12f, 1.04f, 0.75f, -0.05f, 1.29f, 23.1f}};
        // The pose starts when the banter starts and its first ~0.3 s are the fighter settling into place (root motion: he is drawn off the shot's spot, sliding in; Vaan: 5.57 - 5.8 s of
        // `~/dissidia/tt/s2/ttbanter4/s1`): the shot starts kBanterLead ticks later; until then the previous shot stays (the last banter's last frame, or retail's frozen camera for the first banter).
        if (since < kBanterLead) { if (have_b && age >= last_b_age && age - last_b_age < 120.f) { for (int i = 0; i < 3; i++) { E[i] = last_bE[i]; F[i] = last_bF[i]; } g_entr_fov = last_bfov; *w = 1.f; return true; } return false; }
        since -= kBanterLead;
        const int nk = static_cast<int>(sizeof ks / sizeof ks[0]);
        K c = since < 0.f ? ks[0] : ks[nk - 1];
        for (int i = 0; i + 1 < nk; i++) if (since >= ks[i].t && since < ks[i + 1].t) { const float u = (since - ks[i].t) / (ks[i + 1].t - ks[i].t);
            c = {since, ks[i].ef + (ks[i + 1].ef - ks[i].ef) * u, ks[i].es + (ks[i + 1].es - ks[i].es) * u, ks[i].eu + (ks[i + 1].eu - ks[i].eu) * u, ks[i].lf + (ks[i + 1].lf - ks[i].lf) * u,
                 ks[i].ls + (ks[i + 1].ls - ks[i].ls) * u, ks[i].lu + (ks[i + 1].lu - ks[i].lu) * u, ks[i].fov + (ks[i + 1].fov - ks[i].fov) * u}; break; }
        // The shot is retail's, measured on one character (Lightning: bones 4% .. 96% percentile span about 1.42 m): distances and heights scale with the speaking fighter's own bone span NOW (his model is not at his actor's
        // origin: Golbez hovers and sinks 3 m during his pose, Shantotto is 0.75 m tall), and heights count from his lowest bone. Smoothed over a few updates (the pose breathes).
        static float sm_s = 1.f, sm_lo = 0.f; static float sm_t0 = -1.f;
        float lo = 0.f, hi = 0.f; float sc = 1.f, base = P[1];
        if (!std::getenv("PSPRECOMP_BANTER_NOSCALE") && dissidia_tagteam_body(g_dissidia_entr_actor, &lo, &hi) && hi - lo > 0.3f) {
            const float s_now = std::fmin(3.f, std::fmax(0.3f, (hi - lo) / 1.42f));
            if (sm_t0 != g_dissidia_entr_t0) { sm_t0 = g_dissidia_entr_t0; sm_s = s_now; sm_lo = lo; } else { sm_s += (s_now - sm_s) * 0.25f; sm_lo += (lo - sm_lo) * 0.25f; }
            sc = sm_s; base = P[1] + sm_lo;
        }
        const float rx = d[2], rz = -d[0];
        // Heights scale with the fighter, the horizontal distances stay retail's (scaling them put a 0.75 m Shantotto's camera 0.9 m away - inside the near plane, she vanished) and the fov
        // narrows / widens by the same factor (tan(fov / 2) * scale), so the framing is the same proportion of the body.
        E[0] = P[0] + (d[0] * c.ef + rx * c.es); E[1] = base + c.eu * sc; E[2] = P[2] + (d[2] * c.ef + rz * c.es);
        F[0] = P[0] + (d[0] * c.lf + rx * c.ls); F[1] = base + c.lu * sc; F[2] = P[2] + (d[2] * c.lf + rz * c.ls);
        c.fov = 2.f * std::atan(std::tan(c.fov * 0.5f * 3.14159265f / 180.f) * sc) * 180.f / 3.14159265f;
        g_entr_fov = std::getenv("PSPRECOMP_BANTER_FOVTEST") ? static_cast<float>(std::atof(std::getenv("PSPRECOMP_BANTER_FOVTEST"))) : c.fov; *w = 1.f;
        for (int k2 = 0; k2 < 3; k2++) { last_bE[k2] = E[k2]; last_bF[k2] = F[k2]; } last_bfov = g_entr_fov; have_b = true; grace_done = false; last_b_age = age;
        if (std::getenv("PSPRECOMP_ENTRCAM_LOG")) { static int nl = 0; if (nl++ % 10 == 0) std::fprintf(stderr, "[ENTRCAM] banter shot age %.0f since %.0f eye %.2f %.2f %.2f look-at %.2f %.2f %.2f fov %.1f fighter %.2f %.2f %.2f yaw9C %.3f yaw320 %.3f row2 %.3f %.3f fwd90 %.3f %.3f\n", age, since, E[0], E[1], E[2], F[0], F[1], F[2], c.fov, P[0], P[1], P[2], rdf(g_dissidia_entr_actor + 0x9Cu), rdf(g_dissidia_entr_actor + 0x320u), rdf(g_dissidia_entr_actor + 0x350u), rdf(g_dissidia_entr_actor + 0x358u), rdf(g_dissidia_entr_actor + 0x90u), rdf(g_dissidia_entr_actor + 0x98u)); }
        return true;
    }
    if (tight) {
        // the second banter follows the first at once: pan from the last close-up to this one, not through the game's wide camera
        const float kin = smooth01(since / (chain ? 18.f : 12.f));
        if (chain) { for (int i = 0; i < 3; i++) { E[i] = base_E[i] + (E[i] - base_E[i]) * kin; F[i] = base_F[i] + (F[i] - base_F[i]) * kin; } *w = out; }
        else *w = std::fmin(out, kin);
        if (*w > 0.99f) { for (int i = 0; i < 3; i++) { last_E[i] = E[i]; last_F[i] = F[i]; } last_age = age; have_last = true; }
        if (std::getenv("PSPRECOMP_ENTRCAM_LOG")) { static int nl = 0; if (nl++ % 10 == 0) std::fprintf(stderr, "[ENTRCAM] banter shot age %.0f w %.2f chain %d dist %.2f eye %.2f %.2f %.2f look-at %.2f %.2f %.2f fighter %.2f %.2f %.2f\n", age, *w, chain ? 1 : 0, dist, E[0], E[1], E[2], F[0], F[1], F[2], P[0], P[1], P[2]); }
        return true;
    }
    *w = std::fmin(out, smooth01(since / 12.f));                            // 0.2 s cut-in
    return true;
}

// The finish of a tag battle: the retail result scene puts the winner at a fixed spot (0, 0, 10) and frames it from a camera that keeps the height of the
// fight's level. When the fight was on a raised level (stage 12: y = 23) the camera looks at an empty floor from far above (seen: after a member called
// into the slot won). tagteam.cpp names the winner once the finish has put him on that spot; if the game's eye is not within a few metres of his height
// the view is replaced by the front close-up the game gives a winner on the ground level (eye 5.6 m in front, 1 m up, looking at him).
bool vic_frame(const float* eye, const float* tgt, float* E, float* F, float* w) {
    (void)tgt;
    if (!g_dissidia_vic_actor) return false;
    float P[3]; if (!rdvec(g_dissidia_vic_actor + ACT_POS, P)) return false;
    if (std::fabs(eye[1] - P[1]) < 5.0f && std::fabs(eye[0] - P[0]) < 40.f) return false;   // the game's own camera is on him
    E[0] = P[0] - 1.0f; E[1] = P[1] + 1.2f; E[2] = P[2] + 5.6f;
    F[0] = P[0]; F[1] = P[1] + 1.1f; F[2] = P[2];
    *w = 1.f;
    return true;
}

uint32_t overlay_id() { uint32_t id; std::memcpy(&id, g_ram + (OVL_BASE & 0x07FFFFFFu), 4); return id; }

// Which fighter this player's VIEW follows, without touching the simulation.
//   The camera manager's slot table (mgr + 0x6D0 + 4 * i, 8 entries; the fighters' lock blocks, actor + 0x5AC) is
//   SIMULATION state: every update FUN_08B3CA24 steps the camera of every lock block listed there (FUN_08B3C760 copies
//   each one's eye / target / up / fov to mgr + 0x110 + 0x30 * i, FUN_08B3B4FC runs its mode), and a fighter's stick is
//   turned into a world direction with that lock-block camera. Re-pointing a slot (to show another fighter) leaves one
//   fighter's camera un-stepped and steps another twice: in a lock-step battle that machine's fighters move differently
//   (seen: positions split at the first stick input). What is per viewer is only WHICH slot is shown: the update picks the
//   slot whose lock block has byte +0x125 set (FUN_08B3B308; the index goes to mgr + 0x8B8), set on the local fighter's block.
//   So to show another fighter, put that slot's camera into the final eye / look-at here (view only), and leave the table,
//   the lock blocks and +0x125 alone. dissidia_camera_set_view_slot(i) (-1 = the game's choice); PSPRECOMP_CAM_VIEW_SLOT=<i>
//   for tests. The slot's camera is read as the update copied it at its start (one update old).
uint32_t rd32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
int g_view_slot = -2;   // -2: not read from the environment yet
uint32_t g_view_actor = 0; float g_vf_dx = 0.f, g_vf_dz = 1.f;
bool view_follow_frame(uint32_t eye) {
    if (!g_view_actor) return false;
    float P[3], F[3]; if (!rdvec(g_view_actor + 0x80u, P)) return false;
    const uint32_t foe = rd32(g_view_actor + 0x300u);
    float dx = g_vf_dx, dz = g_vf_dz;
    if (foe >= 0x08800000u && foe < 0x0A000000u && rdvec(foe + 0x80u, F)) { const float ex = F[0] - P[0], ez = F[2] - P[2], d = std::sqrt(ex * ex + ez * ez); if (d > 0.05f) { dx = ex / d; dz = ez / d; } }
    g_vf_dx += (dx - g_vf_dx) * 0.08f; g_vf_dz += (dz - g_vf_dz) * 0.08f;
    const float n = std::sqrt(g_vf_dx * g_vf_dx + g_vf_dz * g_vf_dz); const float ux = n > 1e-3f ? g_vf_dx / n : 0.f, uz = n > 1e-3f ? g_vf_dz / n : 1.f;
    const float E[3] = {P[0] - ux * 6.5f, P[1] + 2.6f, P[2] - uz * 6.5f}, T[3] = {P[0] + ux * 2.5f, P[1] + 1.2f, P[2] + uz * 2.5f};
    static int logs = std::getenv("PSPRECOMP_CAM_VIEW_LOG") ? 40 : 0, k = 0;
    if (logs > 0 && k++ % 30 == 0) { logs--; std::fprintf(stderr, "[CAMFOLLOW] actor %08X at %.2f,%.2f,%.2f eye %.2f,%.2f,%.2f look-at %.2f,%.2f,%.2f (game eye %.2f,%.2f,%.2f)\n", g_view_actor, P[0], P[1], P[2], E[0], E[1], E[2], T[0], T[1], T[2], rdf(eye), rdf(eye + 4), rdf(eye + 8)); }
    for (int i = 0; i < 3; i++) { wrf(eye + 4u * i, E[i]); wrf(eye + 0x10u + 4u * i, T[i]); }
    return true;
}
void view_slot_frame(uint32_t eye) {
    if (g_view_slot == -2) { const char* e = std::getenv("PSPRECOMP_CAM_VIEW_SLOT"); g_view_slot = e && *e ? std::atoi(e) : -1; }
    if (g_view_slot < 0 || g_view_slot > 7 || !g_mgr) return;
    uint32_t lb; std::memcpy(&lb, g_ram + ((g_mgr + 0x6D0u + 4u * g_view_slot) & 0x07FFFFFFu), 4);
    if (!lb) return;
    const uint32_t s = g_mgr + 0x110u + 0x30u * static_cast<uint32_t>(g_view_slot);
    float E[3], T[3];
    if (!rdvec(s, E) || !rdvec(s + 0x10u, T)) return;
    static int logs = std::getenv("PSPRECOMP_CAM_VIEW_LOG") ? 40 : 0, n = 0;
    if (logs > 0 && n++ % 30 == 0) {
        logs--;
        std::fprintf(stderr, "[CAMVIEW] slot %d (shown by the game: %u) lock block %08X eye %.2f,%.2f,%.2f look-at %.2f,%.2f,%.2f  game eye %.2f,%.2f,%.2f look-at %.2f,%.2f,%.2f\n",
                     g_view_slot, rd32(g_mgr + 0x8B8u), lb, E[0], E[1], E[2], T[0], T[1], T[2], rdf(eye), rdf(eye + 4), rdf(eye + 8), rdf(eye + 0x10), rdf(eye + 0x14), rdf(eye + 0x18));
    }
    for (int i = 0; i < 3; i++) { wrf(eye + 4u * i, E[i]); wrf(eye + 0x10u + 4u * i, T[i]); }
}

}   // namespace

void dissidia_camera_set_view_slot(int slot) { g_view_slot = slot; }   // view only: which camera-manager slot this player sees (-1 = the game's)
// View only: a chase camera on an actor (a human-driven tag extra: its own lock-block camera is stepped but its target stays at the origin;
// see docs/tag-team.md "Multiplayer: handovers"), behind it, looking past it toward its foe (actor +0x300). 0 = off.
void dissidia_camera_set_view_follow(uint32_t actor) { g_view_actor = actor; }

// How far the free camera has turned the view to the RIGHT of the game's own camera, radians (negative: to the left); 0 when
// the camera module is not running (menus, no battle). input.cpp turns the left stick by this so "up" goes into the screen.
// View state of THIS player only: in a lock-step battle input.cpp applies it to the local stick before it is packed and
// sent, never to the pad slots every machine simulates with.
float dissidia_camera_turn() {
    if (!(now_s() - g_last_t < 0.5 && g_last_t != 0)) return 0.f;
    return g_holding ? g_turn : -g_yaw;
}

namespace {

bool pl_pos(float* P) {
    const uint32_t a = dissidia_player_actor();
    return a && rdvec(a + ACT_POS, P);
}

// Field-of-view setting (Display > Field of View). The camera object keeps its vertical FOV at +0xC0 (radians or degrees: told apart by size);
// the game builds the render projection, culls objects and places the lock-on ring from it, so changing it here keeps all of them in
// step (a wider picture drawn by the GE alone would cull at the edges and leave the ring off its target). The game may rewrite the field
// (zoom effects): a value that is not the one we wrote last is taken as the new base.
float g_fov_written = 0.f, g_fov_base = 0.f;
void apply_fov(uint8_t* rdram) {
    uint8_t* p = rdram + ((g_mgr + 0xC0u) & 0x07FFFFFFu);
    float f; std::memcpy(&f, p, sizeof f);
    const int off = ge_fov_offset();
    if (g_fov_written != 0.f && f == g_fov_written) f = g_fov_base;   // still ours: back to the game's value
    const bool deg = f > 3.2f;
    const float lo = deg ? 10.f : 0.17f, hi = deg ? 170.f : 2.97f;
    if (!(f >= lo && f <= hi)) { g_fov_written = 0.f; return; }       // not a plausible FOV: leave it alone
    g_fov_base = f;
    static bool logged = false;
    if (!logged) { logged = true; std::fprintf(stderr, "[CAMFOV] camera fov field %g (%s), offset %d\n", f, deg ? "degrees" : "radians", off); }
    ge_fov_use_projection(false);   // this module owns the setting now (psp_ge.h)
    float n = deg ? f + float(off) : f + float(off) * 3.14159265f / 180.f;
    n = std::fmin(std::fmax(n, lo), hi);
    if (off == 0) n = f;
    std::memcpy(p, &n, sizeof n);
    g_fov_written = n;
    {   // the lock-on ring is placed by the game with its own FOV: tell the drawing the ratio of focal lengths
        const float d2r = deg ? 3.14159265f / 180.f : 1.f;
        ge_draw_set_hud_follow_scale(std::tan(f * d2r * 0.5f) / std::tan(n * d2r * 0.5f));
    }
}

// CAM_UPDATE is a battle-overlay address; the menu/extra overlays map other code there.
void dissidia_cam_update(uint8_t* rdram, recomp_context* ctx) {
    uint32_t id = overlay_id();
    if (id == 1) dissidia_tagteam_frame(rdram, ctx);
    if (id != 1) {
        FuncPtr f = id == 2 ? dissidia_ovl_menu_lookup(CAM_UPDATE) : id == 3 ? dissidia_ovl_extra_lookup(CAM_UPDATE) : nullptr;
        if (f) f(rdram, ctx);
        return;
    }
    const double t = now_s();
    double dt = t - g_last_t;
    if (g_last_t == 0 || dt > 1.0) { g_yaw = g_pitch = g_vyaw = g_vpitch = 0.f; dt = 0; g_ever_locked = g_held_valid = false; g_locked = true; g_piv_ok = g_rad_ok = g_off_ok = false; }   // new battle / long pause: reset
    g_last_t = t;
    if (dt > 0.1) dt = 0.1;
    g_dt = static_cast<float>(dt);
    g_mgr = static_cast<uint32_t>(ctx->r[4]);
    // Widescreen: the camera object keeps its aspect ratio at +0xCC (+0xC0 = {fov, near, far, aspect}, 480/272 on the PSP). The game
    // culls its objects / shadow casters with it, so a wider aspect makes it draw everything the wider picture shows (no pop-in at the
    // edges). It is wide only AFTER the camera update (the update itself, and the logic that projects world positions to the HUD, e.g.
    // the lock-on ring, see the PSP aspect); the runtime's draw_wide_x widens the drawing as before.
    static const bool wide_cull = !(std::getenv("PSPRECOMP_WIDE_CULL") && std::getenv("PSPRECOMP_WIDE_CULL")[0] == '0');   // =0: the PSP's own aspect (A/B tests)
    const bool set_wide = wide_cull && ge_draw_widescreen();
    auto set_aspect = [&](float asp) { std::memcpy(rdram + ((g_mgr + 0xCCu) & 0x07FFFFFFu), &asp, sizeof asp); };
    if (set_wide) set_aspect(480.0f / 272.0f);
    float sx = 0, sy = 0;
    bool active = psp_ctrl_right_stick(&sx, &sy);
    {   // lock-on state of the player's fighter
        bool locked = true, known = false;
        const uint32_t pl = g_hold_on && g_lock_fn ? dissidia_player_actor() : 0;
        if (pl && ram_ok(pl, 0x5B0)) {
            uint32_t blk; std::memcpy(&blk, g_ram + ((pl + ACT_LOCK) & 0x07FFFFFFu), 4);
            if (ram_ok(blk, 0x140)) {
                const recomp_context saved = *ctx;
                ctx->r[4] = static_cast<int32_t>(blk);
                g_lock_fn(rdram, ctx);
                const uint32_t tgt = static_cast<uint32_t>(ctx->r[2]);
                *ctx = saved;
                locked = tgt != 0; known = true;
                if (locked && !g_ever_locked) { g_ever_locked = true; g_first_lock_t = t; }
                if (g_force_unlock_s >= 0 && g_ever_locked && t - g_first_lock_t > g_force_unlock_s) locked = false;
                if (g_lock_logs && (locked != g_locked || !g_lock_known)) {
                    g_lock_logs--;
                    std::fprintf(stderr, "[CAMLOCK] age %.0f player %08X lock block %08X target %08X -> %s\n", dissidia_battle_age(), pl, blk, tgt, locked ? "locked" : "NOT locked");
                }
            }
        }
        if (g_lock_logs > 0 && !known) { static int n = 0; if (n++ % 60 == 0) std::fprintf(stderr, "[CAMLOCK] unknown: player %08X\n", pl); }
        if (!g_ever_locked && dissidia_battle_age() - dissidia_tagteam_intro_delay() < g_arm_ticks) { locked = true; known = false; }   // (25 Nov: the arming counts the time the partners' banters held the intro: the free hold took over the retail "vs" shot, fov and all)   // intro: the game's camera runs
        if (g_locked && !locked && g_game_valid) {
            g_relock_w = 0.f;   // just lost the lock: freeze the camera where the game had it
            std::memcpy(g_held_e, g_game_e, sizeof g_held_e); std::memcpy(g_held_t, g_game_t, sizeof g_held_t); g_held_valid = true; g_held_p_set = false;
        }
        if (!locked && !g_locked && !g_held_valid && g_game_valid) {   // (first update already unlocked)
            std::memcpy(g_held_e, g_game_e, sizeof g_held_e); std::memcpy(g_held_t, g_game_t, sizeof g_held_t); g_held_valid = true; g_held_p_set = false;
        }
        if (!g_locked && locked) g_relock_w = 1.f;   // (the held rig keeps following the fighter meanwhile)   // lock regained: ease from the held camera back to the game's
        g_locked = locked; g_lock_known = known;
        g_holding = known && !locked && g_held_valid;
        if (known && locked && g_relock_w <= 0.f) g_held_valid = false;
    }
    if (psp_ctrl_take_camera_recenter()) { g_yaw = g_pitch = g_vyaw = g_vpitch = 0.f; g_idle = 0.f; }   // R3 / C: back to the game's camera now
    g_rstick_consumed_until_ms.store(now_ms() + 400, std::memory_order_relaxed);
    constexpr float kRate = 2.6f;   // rad/s at full deflection and sens 1.0
    {   // response curve (fine aim near the centre, full speed at the rim), then ease the angular velocity towards it
        const float m = std::sqrt(sx * sx + sy * sy);
        const float c = m > 1e-4f ? std::pow(std::fmin(m, 1.f), 1.6f) / m : 0.f;
        const float wy = active ? -(g_invert_x ? -sx : sx) * c * g_sens * kRate : 0.f;          // stick right: the view turns right
        const float wp = active ? (g_invert ? sy : -sy) * c * g_sens * kRate * 0.6f : 0.f;      // stick up: the view tilts up
        const float k = ease_k(g_stick_tc);
        g_vyaw += (wy - g_vyaw) * k; g_vpitch += (wp - g_vpitch) * k;
        if (!active && std::fabs(g_vyaw) < 1e-3f) g_vyaw = 0.f;
        if (!active && std::fabs(g_vpitch) < 1e-3f) g_vpitch = 0.f;
        g_yaw += g_vyaw * g_dt; g_pitch += g_vpitch * g_dt;
    }
    if (active) g_idle = 0.f;
    else {
        g_idle += static_cast<float>(dt);
        // Let go of the stick: after a short pause (a brief release while adjusting must not snap back) the offset
        // eases back to the game's own camera, so a big turn cannot leave the fighter out of sight. The pull ramps in
        // over half a second instead of starting at full speed.
        if (g_recenter > 0.f && g_idle > g_recenter_delay && dt > 0 && !(g_lock_known && !g_locked)) {
            const float ramp = smooth01((g_idle - g_recenter_delay) / 0.5f);
            float k = std::exp(-static_cast<float>(dt) * ramp / g_recenter);
            g_yaw *= k; g_pitch *= k;
            if (std::fabs(g_yaw) < 0.002f && g_vyaw == 0.f) g_yaw = 0.f;
            if (std::fabs(g_pitch) < 0.002f && g_vpitch == 0.f) g_pitch = 0.f;
        }
    }
    // Test aid (multiplayer determinism): PSPRECOMP_CAM_FORCE_YAW=<radians> holds this machine's free camera at that yaw / PSPRECOMP_CAM_FORCE_PITCH likewise,
    // so machines with different local cameras can be run against each other (the simulation must not notice).
    { static const char* fy = std::getenv("PSPRECOMP_CAM_FORCE_YAW"); static const char* fp = std::getenv("PSPRECOMP_CAM_FORCE_PITCH"); if (fy) g_yaw = static_cast<float>(std::atof(fy)); if (fp) g_pitch = static_cast<float>(std::atof(fp)); }
    // Yaw is unbounded but kept in (-pi, pi].
    const float PI = 3.14159265f;
    if (g_yaw > PI) g_yaw -= 2 * PI; else if (g_yaw < -PI) g_yaw += 2 * PI;
    if (g_dissidia_cine_seq != g_cine_seen) {         // a new hitcam= request
        g_cine_seen = g_dissidia_cine_seq;
        g_cine_actor = g_dissidia_cine_actor; g_cine_t0 = t; g_cine_len = g_dissidia_cine_ticks / 60.0; g_cine_side = 0.f;
    }
    apply_fov(rdram);   // the update (lock-on ring placement) sees the wider view too
    g_in_update = true; g_eye_calls = 0; g_cc_done = false;
    g_orig_update(rdram, ctx);
    g_in_update = false;
    {   // PSPRECOMP_CAMF_LOG=<n>: one line per update for n updates (camera tests: smoothness of the final eye)
        static int left = [] { const char* e = std::getenv("PSPRECOMP_CAMF_LOG"); return e ? std::atoi(e) : 0; }();
        float P[3] = {0, 0, 0};
        if (left > 0 && pl_pos(P)) {
            left--;
            std::fprintf(stderr, "[CAMF] t=%.4f yaw=%.4f pitch=%.4f hold=%d eye=%.4f,%.4f,%.4f game=%.4f,%.4f,%.4f pl=%.4f,%.4f,%.4f\n", t, g_yaw, g_pitch, (int)g_holding,
                         g_final_eye[0], g_final_eye[1], g_final_eye[2], g_game_e[0], g_game_e[1], g_game_e[2], P[0], P[1], P[2]);
        }
    }
    {   // our own camera (turned view or hold): the game's wall fade used its eye, so fade what blocks ours
        float P[3];
        const bool ours = (g_yaw != 0.f || g_pitch != 0.f || g_holding) && g_blocker_on && pl_pos(P);
        if (ours) P[1] += 1.0f;
        ge_draw_set_view_blocker(ours, P, g_final_eye);
    }
    // the partners' spawn leaves the camera object's fov 0 for an update (the intro camera data node reloads): a degenerate projection, one drawn frame of stray polygons. While the intro is held the
    // last retail fov stands in (a write here, unlike one inside SetEye, reaches the projection).
    if (dissidia_tagteam_intro_hold() && g_pre_fov > 1.f && !(rdf(g_mgr + 0xC0u) >= 1.f) && !std::getenv("PSPRECOMP_BANTER_NOFOVFIX")) wrf(g_mgr + 0xC0u, g_pre_fov);
    apply_fov(rdram);   // the render camera copy, culling and the GE projection use it
    if (set_wide) set_aspect(static_cast<float>(ge_draw_widescreen_width()) / 272.0f);
}

// Issue #136 video 3: after a slot fighter's KO swap the game's camera program sometimes computes eye / look-at = NaN (a lock block's +0x40.. words
// are NaN) and keeps them for the rest of the battle: the 3D view is empty sky and the positional audio (listener = camera) turns into full-scale
// static. SetEye gets the NaN vectors, so replace them here: look at the player's fighter from behind the last good eye's side.
// PSPRECOMP_TAGTEAM_NOCAMNAN=1 = old behaviour.
static void nan_guard(uint32_t cam, uint32_t eye) {
    if (!g_in_update || cam != g_mgr) return;
    static const bool off = std::getenv("PSPRECOMP_TAGTEAM_NOCAMNAN") != nullptr;
    if (off) return;
    float v[6]; bool bad = false;
    for (int i = 0; i < 3; i++) { v[i] = rdf(eye + 4u * i); v[3 + i] = rdf(eye + 0x10u + 4u * i); }
    for (float f : v) if (!std::isfinite(f) || std::fabs(f) > 1.0e6f) bad = true;
    static float good[6] = {0, 0, 0, 0, 0, 0}; static bool have = false; static int n = 0;
    if (!bad) { for (int i = 0; i < 6; i++) good[i] = v[i]; have = true; return; }
    float P[3] = {0, 0, 0}; const bool pl = pl_pos(P) && std::isfinite(P[0]) && std::isfinite(P[1]) && std::isfinite(P[2]);
    float T[3], E[3];
    if (pl) { T[0] = P[0]; T[1] = P[1] + 1.4f; T[2] = P[2]; } else for (int i = 0; i < 3; i++) T[i] = have ? good[3 + i] : 0.f;
    float dx = have ? good[0] - T[0] : 0.f, dz = have ? good[2] - T[2] : 6.f;
    float len = std::sqrt(dx * dx + dz * dz);
    if (!(len > 0.5f)) { dx = 0.f; dz = 6.f; len = 6.f; }
    const float r = len < 4.f ? 4.f : (len > 8.f ? 8.f : len);
    E[0] = T[0] + dx / len * r; E[1] = T[1] + 2.0f; E[2] = T[2] + dz / len * r;
    for (int i = 0; i < 3; i++) { wrf(eye + 4u * i, E[i]); wrf(eye + 0x10u + 4u * i, T[i]); good[i] = E[i]; good[3 + i] = T[i]; }
    have = true;
    if (n++ < 5 || n % 600 == 0) std::fprintf(stderr, "[CAMNAN] SetEye got NaN / garbage (%f %f %f | %f %f %f): eye/look-at replaced (player %d) n=%d\n", v[0], v[1], v[2], v[3], v[4], v[5], (int)pl, n);
}

// SetEye(cam, vec): rotate vec about the target stored 0x10 after it, once per update.
void dissidia_cam_seteye(uint8_t* rdram, recomp_context* ctx) {
    uint32_t cam = static_cast<uint32_t>(ctx->r[4]), eye = static_cast<uint32_t>(ctx->r[5]);
    g_ram = rdram;
    nan_guard(cam, eye);
    for (int i = 0; i < 3; i++) { g_dissidia_last_eye[i] = rdf(eye + 4u * i); g_dissidia_last_tgt[i] = rdf(eye + 0x10u + 4u * i); } g_dissidia_last_cam = cam;
    float E[3], F[3], w = 0.f;
    bool held_now = false;
    float game_az = 0.f;
    if (g_in_update && cam == g_mgr && g_eye_calls == 0) { if (!view_follow_frame(eye)) view_slot_frame(eye); }
    if (g_in_update && cam == g_mgr && g_eye_calls == 0 && g_hold_on) {
        const float ge[3] = {rdf(eye), rdf(eye + 4), rdf(eye + 8)}, gt[3] = {rdf(eye + 0x10), rdf(eye + 0x14), rdf(eye + 0x18)};
        std::memcpy(g_game_e, ge, sizeof ge); std::memcpy(g_game_t, gt, sizeof gt); g_game_valid = true;
        game_az = std::atan2(ge[0] - gt[0], ge[2] - gt[2]);
        {   // the held rig keeps its orientation and distance but moves with the player's fighter
            float P[3];
            if (g_held_valid && pl_pos(P)) {
                if (!g_held_p_set) { std::memcpy(g_held_p, P, sizeof P); g_held_p_set = true; g_off_ok = false; }
                float want[3];
                for (int i = 0; i < 3; i++) want[i] = P[i] - g_held_p[i];
                const float d2 = (want[0] - g_off_s[0]) * (want[0] - g_off_s[0]) + (want[1] - g_off_s[1]) * (want[1] - g_off_s[1]) + (want[2] - g_off_s[2]) * (want[2] - g_off_s[2]);
                if (!g_off_ok || d2 > 15.f * 15.f) { std::memcpy(g_off_s, want, sizeof want); g_off_ok = true; }   // first frame / teleport: no swoop
                const float kh = ease_k(g_hold_tc), kv = ease_k(g_hold_tc_y);
                for (int i = 0; i < 3; i++) g_off_s[i] += (want[i] - g_off_s[i]) * (i == 1 ? kv : kh);
                std::memcpy(g_off, g_off_s, sizeof g_off);
            } else { g_off[0] = g_off[1] = g_off[2] = 0.f; g_off_ok = false; }
        }
        float he[3], ht[3];
        for (int i = 0; i < 3; i++) { he[i] = g_held_e[i] + g_off[i]; ht[i] = g_held_t[i] + g_off[i]; }
        if (g_relock_w > 0.f && !g_holding && g_held_valid) {   // easing back to the game's camera after a lock
            g_relock_w -= g_dt / 0.6f;   // 0.6 s at any frame rate (was 20 updates)
            const float k = smooth01(g_relock_w < 0.f ? 0.f : g_relock_w);
            if (g_relock_w <= 0.f) g_held_valid = false;
            for (int i = 0; i < 3; i++) { wrf(eye + 4 * i, ge[i] + (he[i] - ge[i]) * k); wrf(eye + 0x10 + 4 * i, gt[i] + (ht[i] - gt[i]) * k); }
        } else if (g_holding) {
            for (int i = 0; i < 3; i++) { wrf(eye + 4 * i, he[i]); wrf(eye + 0x10 + 4 * i, ht[i]); }
            held_now = true;
        }
    }
    if (g_in_update && cam == g_mgr && g_eye_calls == 0) {
        // The partners' constructors leave one camera update with eye / target 0 while the intro is held: the last retail camera is shown for it.
        static float pre_e[3], pre_t[3]; static bool pre_ok = false, pre_scaled = false; static int hold_updates = 0;
        const float e3[3] = {rdf(eye), rdf(eye + 4), rdf(eye + 8)}, t3[3] = {rdf(eye + 0x10), rdf(eye + 0x14), rdf(eye + 0x18)}, fv = rdf(g_mgr + 0xC0u);
        bool sane = std::isfinite(fv) && fv > 1.f;
        for (int i = 0; i < 3; i++) if (!std::isfinite(e3[i]) || !std::isfinite(t3[i]) || std::fabs(e3[i]) > 5000.f || std::fabs(t3[i]) > 5000.f) sane = false;
        if (std::fabs(e3[0]) < 1e-3f && std::fabs(e3[1]) < 1e-3f && std::fabs(e3[2]) < 1e-3f) sane = false;
        const bool hold = dissidia_tagteam_intro_hold();
        if (std::getenv("PSPRECOMP_CAMUP_LOG") && dissidia_tagteam_age() > 318.f && dissidia_tagteam_age() < 350.f) std::fprintf(stderr, "[CAMUP] age %.0f eye %.2f %.2f %.2f tgt %.2f %.2f %.2f fov %.1f sane %d hold %d entr %08X\n", dissidia_tagteam_age(), e3[0], e3[1], e3[2], t3[0], t3[1], t3[2], fv, sane, hold, g_dissidia_entr_actor);
        if (!hold) { hold_updates = 0; if (sane) { std::memcpy(pre_e, e3, sizeof e3); std::memcpy(pre_t, t3, sizeof t3); g_pre_fov = fv; pre_ok = true; } if (pre_scaled && !g_dissidia_entr_actor) { ge_set_fov_scale(1.f); pre_scaled = false; } }
        else if (pre_ok && !std::getenv("PSPRECOMP_BANTER_NOPRECAM") && (!sane || (++hold_updates <= 3 && std::fabs(fv - g_pre_fov) < 0.05f && (std::fabs(e3[0] - pre_e[0]) + std::fabs(e3[1] - pre_e[1]) + std::fabs(e3[2] - pre_e[2])) > 0.5f))) {   // (also the update after it: the camera is already on the next shot while its fov is still the old shot's - a view into the void)   // (only the broken update: the frozen retail camera behind it is retail's own next shot, the human's wide "vs" start, and is shown as it is)
            for (int i = 0; i < 3; i++) { wrf(eye + 4 * i, pre_e[i]); wrf(eye + 0x10 + 4 * i, pre_t[i]); }
            if (fv > 1.f && g_pre_fov > 1.f) { const float r = fv > 3.2f ? 3.14159265f / 180.f : 1.f; ge_set_fov_scale(std::tan(fv * r * 0.5f) / std::tan(g_pre_fov * r * 0.5f)); pre_scaled = true; }
            if (std::getenv("PSPRECOMP_ENTRCAM_LOG")) { static int np = 0; if (np++ % 6 == 0) std::fprintf(stderr, "[ENTRCAM] held: last retail camera shown at age %.0f (game eye %.1f %.1f %.1f)\n", dissidia_tagteam_age(), e3[0], e3[1], e3[2]); }
        }
    }
    if (g_in_update && cam == g_mgr && g_eye_calls == 0 && !held_now) {   // (25 Nov: the held free camera takes over 10 s into a battle; a long start - 3 v 3 banters run to 16 s - lost the banter close-ups to it)
        float ev[3] = {rdf(eye), rdf(eye + 4), rdf(eye + 8)}, tv[3] = {rdf(eye + 0x10), rdf(eye + 0x14), rdf(eye + 0x18)};
        if (static const bool il = std::getenv("PSPRECOMP_INTROCAM_LOG") != nullptr; il) {   // debug: the game's own eye / look-at against the nearest fighter (the retail intro close-up's framing)
            static int n = 0; float np[3];
            if (n < 6000 && dissidia_nearest_fighter(ev, np)) {
                const float dh = std::sqrt((ev[0] - np[0]) * (ev[0] - np[0]) + (ev[2] - np[2]) * (ev[2] - np[2])), dt = std::sqrt((tv[0] - np[0]) * (tv[0] - np[0]) + (tv[1] - np[1]) * (tv[1] - np[1]) + (tv[2] - np[2]) * (tv[2] - np[2]));
                if (std::getenv("PSPRECOMP_INTROCAM_LOG")[0] == '2') {   // every update: the eye / look-at in the fighter's own frame (forward = matrix row 2, +0x350 / +0x358), for the retail intro's shot path
                    const uint32_t na = dissidia_nearest_actor(); const float fx = rdf(na + 0x350u), fz = rdf(na + 0x358u), fl = std::sqrt(fx * fx + fz * fz) + 1e-6f, ux = fx / fl, uz = fz / fl;
                    const float ex = ev[0] - np[0], ez = ev[2] - np[2], tx = tv[0] - np[0], tz = tv[2] - np[2];
                    std::fprintf(stderr, "[INTROPATH] age=%.1f actor=%08X eye fwd %.2f side %.2f up %.2f | look fwd %.2f side %.2f up %.2f | fov %.1f ra %08X a0 %08X ent %08X | yaw9C %.3f yaw320 %.3f row2 %.3f %.3f fwd90 %.3f %.3f\n", dissidia_tagteam_age(), na, ex * ux + ez * uz, ex * uz - ez * ux, ev[1] - np[1], tx * ux + tz * uz, tx * uz - tz * ux, tv[1] - np[1], rdf(g_mgr + 0xC0u), static_cast<uint32_t>(ctx->r[31]), cam, g_dissidia_entr_actor, rdf(na + 0x9Cu), rdf(na + 0x320u), rdf(na + 0x350u), rdf(na + 0x358u), rdf(na + 0x90u), rdf(na + 0x98u));
                    n++;
                } else
                if (n++ % 3 == 0) std::fprintf(stderr, "[INTROCAM] fov %.3f n=%d age=%.0f entr=%08X eye-fighter horiz %.2f dy %.2f look-at: dy %.2f dist %.2f eye %.1f %.1f %.1f\n", rdf(g_mgr + 0xC0u), n, dissidia_tagteam_age(), g_dissidia_entr_actor, dh, ev[1] - np[1], tv[1] - np[1], dt, ev[0], ev[1], ev[2]);
            }
        }
        if ((entr_frame(ev, tv, E, F, &w) || vic_frame(ev, tv, E, F, &w) || cine_frame(ev, tv, E, F, &w)) && w > 0.f) {    // the call-in close-up, or hitcam=: frame the attacker and his target from the side
            for (int i = 0; i < 3; i++) {
                wrf(eye + 4 * i, ev[i] + (E[i] - ev[i]) * w);
                wrf(eye + 0x10 + 4 * i, tv[i] + (F[i] - tv[i]) * w);
            }
            if (g_entr_fov > 0.f && g_dissidia_entr_actor && !std::getenv("PSPRECOMP_BANTER_NOFOV")) {
                // (25 Nov: writing the fov into the camera object, as the first banter shots did, changed nothing on screen - the game builds its projection before SetEye; checked with fov 12 / 60.)
                // The retail shot's fov is reached with a view-only focal scale on every projection the game uploads (psp_ge_viewport.cpp ge_set_fov_scale).
                const float fv = rdf(g_mgr + 0xC0u), r = fv > 3.2f ? 3.14159265f / 180.f : 1.f;
                const float want = fv + (g_entr_fov - fv) * w;
                if (fv > 1e-3f && want > 1e-3f) { ge_set_fov_scale(std::tan(fv * r * 0.5f) / std::tan(want * r * 0.5f)); g_fov_scaled = true; }
                if (std::getenv("PSPRECOMP_ENTRCAM_LOG")) { static int nf = 0; if (nf++ % 10 == 0) std::fprintf(stderr, "[ENTRCAM] fov: game %.2f, banter wants %.2f (w %.2f)\n", fv, g_entr_fov, w); }
            }
            g_eye_calls++;
        } else if (g_fov_scaled) { ge_set_fov_scale(1.f); g_fov_scaled = false; }
    }
    float pivot[3];
    if (g_in_update && cam == g_mgr && g_eye_calls == 0 && g_yaw == 0.f && g_pitch == 0.f) g_piv_ok = g_rad_ok = false;   // back on the game's camera: start fresh next time
    if (g_in_update && cam == g_mgr && g_eye_calls == 0 && (g_yaw != 0.f || g_pitch != 0.f) && g_pivot_on) {
        // Orbit the fighter, not the game's look-at point (which sits in front of him): the whole rig, eye and look-at,
        // turns about a vertical axis through the fighter, and tilts about a horizontal one, so he keeps his place on the
        // screen and can't be lost behind the lens. The pivot is the fighter nearest to the game's own eye.
        const float ev[3] = {rdf(eye), rdf(eye + 4), rdf(eye + 8)};
        // The player's own fighter (the nearest fighter to the lens could switch to the opponent when he came close and
        // throw the view across the stage), the nearest fighter only when the player is not known.
        if (pl_pos(pivot) || (!held_now && dissidia_nearest_fighter(ev, pivot))) {
            g_eye_calls++;
            pivot[1] += 1.0f;   // about the chest
            {
                const float d2 = (pivot[0] - g_piv[0]) * (pivot[0] - g_piv[0]) + (pivot[1] - g_piv[1]) * (pivot[1] - g_piv[1]) + (pivot[2] - g_piv[2]) * (pivot[2] - g_piv[2]);
                if (!g_piv_ok || d2 > 15.f * 15.f) { std::memcpy(g_piv, pivot, sizeof pivot); g_piv_ok = true; }   // first use / teleport
                const float k = ease_k(g_pivot_tc);
                for (int i = 0; i < 3; i++) { g_piv[i] += (pivot[i] - g_piv[i]) * k; pivot[i] = g_piv[i]; }
            }
            float E[3] = {ev[0] - pivot[0], ev[1] - pivot[1], ev[2] - pivot[2]};
            if (!held_now) {   // the orbit radius eases after the game's distance instead of jumping with it
                const float L = std::sqrt(E[0] * E[0] + E[1] * E[1] + E[2] * E[2]);
                if (L > 1e-3f) {
                    if (!g_rad_ok) { g_rad = L; g_rad_ok = true; }
                    g_rad += (L - g_rad) * ease_k(g_dist_tc);
                    // full effect once the view is turned a little, none at zero offset (no pop when the offset reaches 0)
                    const float w = smooth01(std::sqrt(g_yaw * g_yaw + g_pitch * g_pitch) / 0.15f);
                    const float s = 1.f + (g_rad / L - 1.f) * w;
                    for (int i = 0; i < 3; i++) E[i] *= s;
                }
            }
            float T[3] = {rdf(eye + 0x10) - pivot[0], rdf(eye + 0x14) - pivot[1], rdf(eye + 0x18) - pivot[2]};
            const float horiz = std::sqrt(E[0] * E[0] + E[2] * E[2]);
            if (horiz > 1e-3f) {
                const float d2r = 3.14159265f / 180.f;
                float el = std::atan2(E[1], horiz), nel = el + g_pitch;
                const float lo = g_pitch_min * d2r, hi = g_pitch_max * d2r, lo_e = std::fmin(lo, el), hi_e = std::fmax(hi, el);
                if (nel < lo_e) nel = lo_e; else if (nel > hi_e) nel = hi_e;
                g_pitch = nel - el;                    // keep the accumulator inside the window (no wind-up)
                const float cy = std::cos(g_yaw), sy = std::sin(g_yaw);
                auto yaw = [&](float v[3]) { const float x = v[0] * cy + v[2] * sy, z = v[2] * cy - v[0] * sy; v[0] = x; v[2] = z; };
                yaw(E); yaw(T);
                // tilt about the horizontal axis (perpendicular to the eye's horizontal direction): +angle lowers the eye
                const float hx = E[0] / horiz, hz = E[2] / horiz;
                const float ax = hz, az = -hx;         // (0,1,0) x (hx,0,hz)
                const float th = -(nel - el), ct = std::cos(th), st = std::sin(th);
                auto tilt = [&](float v[3]) {
                    const float dot = ax * v[0] + az * v[2];
                    const float cx = -az * v[1], cyv = az * v[0] - ax * v[2], cz = ax * v[1];   // a x v (a has no y part)
                    float o[3] = {v[0] * ct + cx * st + ax * dot * (1 - ct), v[1] * ct + cyv * st, v[2] * ct + cz * st + az * dot * (1 - ct)};
                    v[0] = o[0]; v[1] = o[1]; v[2] = o[2];
                };
                tilt(E); tilt(T);
                for (int i = 0; i < 3; i++) { wrf(eye + 4 * i, pivot[i] + E[i]); wrf(eye + 0x10 + 4 * i, pivot[i] + T[i]); }
            }
        }
    }
    if (g_in_update && cam == g_mgr && g_eye_calls++ == 0 && (g_yaw != 0.f || g_pitch != 0.f)) {
        const uint32_t tg = eye + 0x10;
        float ex = rdf(eye), ey = rdf(eye + 4), ez = rdf(eye + 8);
        float tx = rdf(tg), ty = rdf(tg + 4), tz = rdf(tg + 8);
        float dx = ex - tx, dy = ey - ty, dz = ez - tz;
        float r = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (r > 1e-3f) {
            float horiz = std::sqrt(dx * dx + dz * dz);
            float az = std::atan2(dx, dz);               // yaw of the eye around the target
            float el = std::atan2(dy, horiz);            // elevation
            const float d2r = 3.14159265f / 180.f;
            float nel = el + g_pitch;
            float lo = g_pitch_min * d2r, hi = g_pitch_max * d2r;
            // Clamp the *result*, but never fight the game if it is already outside our window.
            float lo_e = std::fmin(lo, el), hi_e = std::fmax(hi, el);
            if (nel < lo_e) nel = lo_e; else if (nel > hi_e) nel = hi_e;
            g_pitch = nel - el;   // keep the accumulator inside the window (no wind-up)
            float naz = az + g_yaw;
            float ch = std::cos(nel) * r;
            wrf(eye,     tx + std::sin(naz) * ch);
            wrf(eye + 4, ty + std::sin(nel) * r);
            wrf(eye + 8, tz + std::cos(naz) * ch);
        }
    }
    if (g_in_update && cam == g_mgr && g_cc_on && !g_cc_done) {
        g_cc_done = true;
        const float wgt = held_now ? 1.f : smooth01(std::sqrt(g_yaw * g_yaw + g_pitch * g_pitch) / 0.15f);
        float P[3];
        if (wgt > 0.f && pl_pos(P)) {
            P[1] += 1.0f;   // chest
            const float e[3] = {rdf(eye), rdf(eye + 4), rdf(eye + 8)};
            const float dir[3] = {e[0] - P[0], e[1] - P[1], e[2] - P[2]};
            const float L = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (L > 1.2f) {
                constexpr float kStart = 0.6f, kMargin = 0.35f, kMin = 1.2f;   // the test starts off the body; the eye stays this far off the wall
                const float s0[3] = {P[0] + dir[0] / L * kStart, P[1] + dir[1] / L * kStart, P[2] + dir[2] / L * kStart};
                ge_draw_camera_probe_request(s0, e, ++g_cc_id);
                uint32_t rid = 0;
                const float r = ge_draw_camera_probe_result(&rid);
                float want = L;
                if (rid && g_cc_id - rid < 8 && r < 1.f) want = std::fmax(kMin, kStart + r * (L - kStart) - kMargin);
                if (!g_col_ok) { g_col = L; g_col_ok = true; }
                g_col += (want - g_col) * ease_k(want < g_col ? 0.06f : 0.5f);
                const float dist = L + (std::fmin(L, g_col) - L) * wgt;
                if (dist < L - 1e-3f) for (int i = 0; i < 3; i++) wrf(eye + 4 * i, P[i] + dir[i] / L * dist);
            }
        } else g_col_ok = false;
    }
    if (held_now) {   // the stick is read in the game's own frame: turn it by how far this view is from the game's
        float d = std::atan2(rdf(eye) - rdf(eye + 0x10), rdf(eye + 8) - rdf(eye + 0x18)) - game_az;
        const float PI = 3.14159265f;
        while (d > PI) d -= 2 * PI;
        while (d < -PI) d += 2 * PI;
        g_turn = -d;
    }
    if (g_log_left > 0 && g_in_update && cam == g_mgr && g_eye_calls == 1) {
        g_log_left--;
        float ex = rdf(eye), ey = rdf(eye + 4), ez = rdf(eye + 8);
        float tx = rdf(eye + 0x10), ty = rdf(eye + 0x14), tz = rdf(eye + 0x18);
        std::fprintf(stderr, "[CAMX] hold=%d locked=%d known=%d held_valid=%d game_valid=%d yaw=%.2fdeg pitch_off=%.2fdeg yaw_off=%.2fdeg eye=(%.2f,%.2f,%.2f) tgt=(%.2f,%.2f,%.2f)\n",
            (int)held_now, (int)g_locked, (int)g_lock_known, (int)g_held_valid, (int)g_game_valid, std::atan2(ex - tx, ez - tz) * 57.29578f, g_pitch * 57.29578f, g_yaw * 57.29578f, ex, ey, ez, tx, ty, tz);
    }
    if (g_in_update && cam == g_mgr) for (int i = 0; i < 3; i++) g_final_eye[i] = rdf(eye + 4 * i);
    g_orig_seteye(rdram, ctx);
}

}  // namespace

bool dissidia_player_pos(float* P) { return g_ram && pl_pos(P); }   // stage light editor / effect-light log (psp_screenfx.cpp); g_ram is only set once the camera hooks run (not with PSPRECOMP_CAM_FREE=0)

void dissidia_camera_install(uint8_t* rdram) {
    const char* e = std::getenv("PSPRECOMP_HITCAM");
    g_cine_on = !(e && e[0] == '0');
    e = std::getenv("PSPRECOMP_CAM_FREE");
    g_free = !(e && e[0] == '0');
    g_ram = rdram;   // also for dissidia_player_pos with the analogue camera off
    if (!g_free) { std::fprintf(stderr, "[CAM] analogue camera disabled (PSPRECOMP_CAM_FREE=0)\n"); return; }
    if ((e = std::getenv("PSPRECOMP_CAM_SENS"))) g_sens = static_cast<float>(std::atof(e));
    g_invert = (e = std::getenv("PSPRECOMP_CAM_INVERT_Y")) && e[0] && e[0] != '0';
    if ((e = std::getenv("PSPRECOMP_CAM_PITCH_MIN"))) g_pitch_min = static_cast<float>(std::atof(e));
    if ((e = std::getenv("PSPRECOMP_CAM_PITCH_MAX"))) g_pitch_max = static_cast<float>(std::atof(e));
    if ((e = std::getenv("PSPRECOMP_CAM_BLOCKER_FADE"))) g_blocker_on = e[0] != '0';
    if ((e = std::getenv("PSPRECOMP_CAM_PIVOT"))) g_pivot_on = e[0] != '0';
    g_invert_x = (e = std::getenv("PSPRECOMP_CAM_INVERT_X")) && e[0] && e[0] != '0';
    if ((e = std::getenv("PSPRECOMP_CAM_RECENTER"))) g_recenter = static_cast<float>(std::atof(e));
    if ((e = std::getenv("PSPRECOMP_CAM_RECENTER_DELAY"))) g_recenter_delay = static_cast<float>(std::atof(e));
    if ((e = std::getenv("PSPRECOMP_CAMX_LOG"))) g_log_left = std::atoi(e);
    if ((e = std::getenv("PSPRECOMP_CAM_COLLIDE"))) g_cc_on = e[0] != '0';
    if ((e = std::getenv("PSPRECOMP_CAM_STICK_TC"))) g_stick_tc = static_cast<float>(std::atof(e));
    if ((e = std::getenv("PSPRECOMP_CAM_PIVOT_TC"))) g_pivot_tc = static_cast<float>(std::atof(e));
    if ((e = std::getenv("PSPRECOMP_CAM_DIST_TC"))) g_dist_tc = static_cast<float>(std::atof(e));
    if ((e = std::getenv("PSPRECOMP_CAM_HOLD_TC"))) { g_hold_tc = static_cast<float>(std::atof(e)); g_hold_tc_y = g_hold_tc * 2.6f; }
    g_hold_on = !((e = std::getenv("PSPRECOMP_CAM_UNLOCK_HOLD")) && e[0] == '0');
    if ((e = std::getenv("PSPRECOMP_CAMLOCK_LOG"))) g_lock_logs = std::atoi(e);
    if ((e = std::getenv("PSPRECOMP_CAM_HOLD_ARM"))) g_arm_ticks = static_cast<float>(std::atof(e));
    if ((e = std::getenv("PSPRECOMP_CAM_FORCE_UNLOCK"))) g_force_unlock_s = std::atof(e);
    g_lock_fn = dissidia_ovl_battle_lookup(LOCK_TARGET);
    g_orig_update = dissidia_ovl_battle_lookup(CAM_UPDATE);
    g_orig_seteye = psp_dispatch_probe_lookup(CAM_SETEYE);
    if (!g_orig_update || !g_orig_seteye) {
        std::fprintf(stderr, "[CAM] hooks NOT installed (update %p seteye %p)\n", (void*)g_orig_update, (void*)g_orig_seteye);
        return;
    }
    psp_dispatch_register(CAM_UPDATE, dissidia_cam_update);
    psp_dispatch_register(CAM_SETEYE, dissidia_cam_seteye);
    std::fprintf(stderr, "[CAM] analogue camera installed (sens %.2f%s, pitch %.0f..%.0f deg, recenter %.1fs)\n",
                 g_sens, g_invert ? ", inverted" : "", g_pitch_min, g_pitch_max, g_recenter);
}

uint32_t dissidia_camera_manager() { return g_mgr; }   // the game's camera manager (0 before the first update), for tagteam.cpp
