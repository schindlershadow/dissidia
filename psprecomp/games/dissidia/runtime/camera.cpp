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
//     accumulated stick offset. Everything downstream (lookAt, shake, render copy, movement-relative-to-camera
//     code that reads the manager's matrix) then sees the rotated camera. The game's own state (d-pad
//     rotation, lock-on, auto-centring, collision pull-in) is untouched: the stick is an offset on top.
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

#include "psp_game_module.h"
#include "psp_runtime.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include "psp_ge_draw.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
extern FuncPtr ovl_menu_lookup(uint32_t vaddr);
extern FuncPtr ovl_extra_lookup(uint32_t vaddr);
bool dissidia_nearest_fighter(const float eye[3], float pos[3]);   // framerate.cpp
extern uint32_t g_dissidia_cine_actor, g_dissidia_cine_seq;   // roster.cpp (hitcam=)
extern int g_dissidia_cine_ticks;
float dissidia_battle_age();
uint32_t dissidia_player_actor();   // framerate.cpp: the human-controlled fighter

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

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t now_ms() { return static_cast<int64_t>(now_s() * 1000.0); }

float rdf(uint32_t a) { float f; std::memcpy(&f, g_ram + (a & 0x07FFFFFFu), 4); return f; }
void wrf(uint32_t a, float f) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &f, 4); }

bool ram_ok(uint32_t a, uint32_t n) { return a >= 0x08800000u && a + n <= 0x0C000000u; }
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

uint32_t overlay_id() { uint32_t id; std::memcpy(&id, g_ram + (OVL_BASE & 0x07FFFFFFu), 4); return id; }

}   // namespace

// How far the free camera has turned the view to the RIGHT of the game's own camera, radians (negative: to the left); 0 when
// the camera module is not running (menus, no battle). input.cpp turns the left stick by this so "up" goes into the screen.
float dissidia_camera_turn() {
    if (!(now_s() - g_last_t < 0.5 && g_last_t != 0)) return 0.f;
    return g_holding ? g_turn : -g_yaw;
}

namespace {

bool pl_pos(float* P) {
    const uint32_t a = dissidia_player_actor();
    return a && rdvec(a + ACT_POS, P);
}

// CAM_UPDATE is a battle-overlay address; the menu/extra overlays map other code there.
void dissidia_cam_update(uint8_t* rdram, recomp_context* ctx) {
    uint32_t id = overlay_id();
    if (id != 1) {
        FuncPtr f = id == 2 ? ovl_menu_lookup(CAM_UPDATE) : id == 3 ? ovl_extra_lookup(CAM_UPDATE) : nullptr;
        if (f) f(rdram, ctx);
        return;
    }
    const double t = now_s();
    double dt = t - g_last_t;
    if (g_last_t == 0 || dt > 1.0) { g_yaw = g_pitch = 0.f; dt = 0; g_ever_locked = g_held_valid = false; g_locked = true; }   // new battle / long pause: reset
    g_last_t = t;
    if (dt > 0.1) dt = 0.1;
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
        if (!g_ever_locked && dissidia_battle_age() < g_arm_ticks) { locked = true; known = false; }   // intro: the game's camera runs
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
    if (psp_ctrl_take_camera_recenter()) { g_yaw = g_pitch = 0.f; g_idle = 0.f; }   // R3 / C: back to the game's camera now
    g_rstick_consumed_until_ms.store(now_ms() + 400, std::memory_order_relaxed);
    constexpr float kRate = 2.6f;   // rad/s at full deflection and sens 1.0
    if (active) {
        g_idle = 0.f;
        g_yaw   -= (g_invert_x ? -sx : sx) * g_sens * kRate * static_cast<float>(dt);   // stick right: the view turns right
        g_pitch += (g_invert ? sy : -sy) * g_sens * kRate * 0.6f * static_cast<float>(dt);   // stick up: the view tilts up
    } else {
        g_idle += static_cast<float>(dt);
        // Let go of the stick: after a short pause (a brief release while adjusting must not snap back) the offset
        // eases back to the game's own camera, so a big turn cannot leave the fighter out of sight.
        if (g_recenter > 0.f && g_idle > g_recenter_delay && dt > 0 && !(g_lock_known && !g_locked)) {
            float k = std::exp(-static_cast<float>(dt) / g_recenter);
            g_yaw *= k; g_pitch *= k;
            if (std::fabs(g_yaw) < 0.002f) g_yaw = 0.f;
            if (std::fabs(g_pitch) < 0.002f) g_pitch = 0.f;
        }
    }
    // Yaw is unbounded but kept in (-pi, pi].
    const float PI = 3.14159265f;
    if (g_yaw > PI) g_yaw -= 2 * PI; else if (g_yaw < -PI) g_yaw += 2 * PI;
    if (g_dissidia_cine_seq != g_cine_seen) {         // a new hitcam= request
        g_cine_seen = g_dissidia_cine_seq;
        g_cine_actor = g_dissidia_cine_actor; g_cine_t0 = t; g_cine_len = g_dissidia_cine_ticks / 60.0; g_cine_side = 0.f;
    }
    g_in_update = true; g_eye_calls = 0;
    g_orig_update(rdram, ctx);
    g_in_update = false;
    {   // our own camera (turned view or hold): the game's wall fade used its eye, so fade what blocks ours
        float P[3];
        const bool ours = (g_yaw != 0.f || g_pitch != 0.f || g_holding) && g_blocker_on && pl_pos(P);
        if (ours) P[1] += 1.0f;
        ge_draw_set_view_blocker(ours, P, g_final_eye);
    }
    if (set_wide) set_aspect(static_cast<float>(ge_draw_widescreen_width()) / 272.0f);
}

// SetEye(cam, vec): rotate vec about the target stored 0x10 after it, once per update.
void dissidia_cam_seteye(uint8_t* rdram, recomp_context* ctx) {
    uint32_t cam = static_cast<uint32_t>(ctx->r[4]), eye = static_cast<uint32_t>(ctx->r[5]);
    float E[3], F[3], w = 0.f;
    bool held_now = false;
    float game_az = 0.f;
    if (g_in_update && cam == g_mgr && g_eye_calls == 0 && g_hold_on) {
        const float ge[3] = {rdf(eye), rdf(eye + 4), rdf(eye + 8)}, gt[3] = {rdf(eye + 0x10), rdf(eye + 0x14), rdf(eye + 0x18)};
        std::memcpy(g_game_e, ge, sizeof ge); std::memcpy(g_game_t, gt, sizeof gt); g_game_valid = true;
        game_az = std::atan2(ge[0] - gt[0], ge[2] - gt[2]);
        {   // the held rig keeps its orientation and distance but moves with the player's fighter
            float P[3];
            if (g_held_valid && pl_pos(P)) {
                if (!g_held_p_set) { std::memcpy(g_held_p, P, sizeof P); g_held_p_set = true; }
                for (int i = 0; i < 3; i++) g_off[i] = P[i] - g_held_p[i];
            } else g_off[0] = g_off[1] = g_off[2] = 0.f;
        }
        float he[3], ht[3];
        for (int i = 0; i < 3; i++) { he[i] = g_held_e[i] + g_off[i]; ht[i] = g_held_t[i] + g_off[i]; }
        if (g_relock_w > 0.f && !g_holding && g_held_valid) {   // easing back to the game's camera after a lock
            g_relock_w -= 0.05f;
            const float k = smooth01(g_relock_w < 0.f ? 0.f : g_relock_w);
            if (g_relock_w <= 0.f) g_held_valid = false;
            for (int i = 0; i < 3; i++) { wrf(eye + 4 * i, ge[i] + (he[i] - ge[i]) * k); wrf(eye + 0x10 + 4 * i, gt[i] + (ht[i] - gt[i]) * k); }
        } else if (g_holding) {
            for (int i = 0; i < 3; i++) { wrf(eye + 4 * i, he[i]); wrf(eye + 0x10 + 4 * i, ht[i]); }
            held_now = true;
        }
    }
    if (g_in_update && cam == g_mgr && g_eye_calls == 0 && !held_now) {
        float ev[3] = {rdf(eye), rdf(eye + 4), rdf(eye + 8)}, tv[3] = {rdf(eye + 0x10), rdf(eye + 0x14), rdf(eye + 0x18)};
        if (cine_frame(ev, tv, E, F, &w) && w > 0.f) {    // hitcam=: frame the attacker and his target from the side
            for (int i = 0; i < 3; i++) {
                wrf(eye + 4 * i, ev[i] + (E[i] - ev[i]) * w);
                wrf(eye + 0x10 + 4 * i, tv[i] + (F[i] - tv[i]) * w);
            }
            g_eye_calls++;
        }
    }
    float pivot[3];
    if (g_in_update && cam == g_mgr && g_eye_calls == 0 && (g_yaw != 0.f || g_pitch != 0.f) && g_pivot_on) {
        // Orbit the fighter, not the game's look-at point (which sits in front of him): the whole rig, eye and look-at,
        // turns about a vertical axis through the fighter, and tilts about a horizontal one, so he keeps his place on the
        // screen and can't be lost behind the lens. The pivot is the fighter nearest to the game's own eye.
        const float ev[3] = {rdf(eye), rdf(eye + 4), rdf(eye + 8)};
        if (held_now ? pl_pos(pivot) : dissidia_nearest_fighter(ev, pivot)) {
            g_eye_calls++;
            pivot[1] += 1.0f;   // about the chest
            float E[3] = {ev[0] - pivot[0], ev[1] - pivot[1], ev[2] - pivot[2]};
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

bool dissidia_player_pos(float* P) { return pl_pos(P); }   // for the stage light editor (runtime/src/psp_screenfx.cpp)

void dissidia_camera_install(uint8_t* rdram) {
    const char* e = std::getenv("PSPRECOMP_HITCAM");
    g_cine_on = !(e && e[0] == '0');
    e = std::getenv("PSPRECOMP_CAM_FREE");
    g_free = !(e && e[0] == '0');
    if (!g_free) { std::fprintf(stderr, "[CAM] analogue camera disabled (PSPRECOMP_CAM_FREE=0)\n"); return; }
    g_ram = rdram;
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
    g_hold_on = !((e = std::getenv("PSPRECOMP_CAM_UNLOCK_HOLD")) && e[0] == '0');
    if ((e = std::getenv("PSPRECOMP_CAMLOCK_LOG"))) g_lock_logs = std::atoi(e);
    if ((e = std::getenv("PSPRECOMP_CAM_HOLD_ARM"))) g_arm_ticks = static_cast<float>(std::atof(e));
    if ((e = std::getenv("PSPRECOMP_CAM_FORCE_UNLOCK"))) g_force_unlock_s = std::atof(e);
    g_lock_fn = ovl_battle_lookup(LOCK_TARGET);
    g_orig_update = ovl_battle_lookup(CAM_UPDATE);
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
