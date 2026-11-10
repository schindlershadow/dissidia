// games/dissidia/runtime/input.cpp — continuous analogue stick direction for Dissidia 012.
//
// The EBOOT's stick processor FUN_08872138 (called from the pad poll FUN_08871E24 each frame with
// a0 = pad.stick vector (0x08C15BB0 in our runs, pad object 0x08C15B90 + 0x20) and a1 = {lx, ly}
// as floats 0..255) works PER AXIS:
//     v = a1[i] - 127.5;  v -= pad.deadzone (float at pad+0x258, default 100.0 = 78% of travel);
//     clamp at 0 (and set the virtual D-pad bit for that axis);  v /= (127.5 - deadzone)
// then normalises the 2-vector: stick vector = (dir.x, dir.y, 0, min(|v|, 1)).
// With a 78% per-axis dead zone, any direction whose weaker axis is below 100/127.5 collapses onto the
// other axis: on a PSP nub (square gate) that leaves 8 usable directions; on a modern circular-gate stick
// (diagonals never exceed ~90/90 of 127.5) it leaves only 4 cardinal directions. That is the "characters
// snap to fixed angles" effect, and it is entirely in the game, not in our pass-through of the 8-bit
// stick.
//
// Fix: wrap FUN_08872138, let the original run (it still produces the virtual D-pad bits returned in
// v0/v1 that menus use, and any side effects), then overwrite the stick vector with a radial version:
//     m = |{lx,ly} - 127.5| / 127.5
//     m < PSPRECOMP_ANALOG_DEADZONE (default 0.20)  ->  (0,0,0,0)
//     else  (dx/|d|, dy/|d|, 0, min(1, (m - dz) / (1 - dz)))
// i.e. the same vector layout/semantics (unit direction + magnitude 0..1; y down-positive), but with the
// true angle. Consumers keep mapping magnitude to speed as the game always did.
//
// PSPRECOMP_ANALOG_FULL=0 restores the original 8-way behaviour. PSPRECOMP_ANALOG_LOG=<n> logs n stick
// updates (raw, game vector, replacement).

#include "psp_game_module.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

float dissidia_camera_turn();   // camera.cpp

namespace {

constexpr uint32_t STICK_PROCESS = 0x08872138u;
FuncPtr g_orig = nullptr;
uint8_t* g_ram = nullptr;
float g_dz = 0.20f;
int g_log = 0;

float rdf(uint32_t a) { float f; std::memcpy(&f, g_ram + (a & 0x07FFFFFFu), 4); return f; }
void wrf(uint32_t a, float f) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &f, 4); }

// The game's own 8-way processing (the virtual D-pad bits it returns, which battle movement and the dash / step
// commands read) needs a PSP nub's square gate: a modern stick's circular gate never reaches 78% on both axes at
// a diagonal, so only the four cardinal bits ever fire. In battle the original is run on a square-gate version of
// the stick (the circle stretched to the square: same angle, same size by the max norm) with the game's per-axis
// dead zone lowered to PSPRECOMP_ANALOG_DPAD_ZONE (default 0.35 of travel), which makes eight even sectors.
// PSPRECOMP_ANALOG_8WAY=0 turns this off.
constexpr uint32_t OVL_BASE = 0x089DD4C0u;      // resident overlay id (1 = battle)
constexpr uint32_t PAD_DEADZONE = 0x258u;       // float in the pad object (stick vector is at pad + 0x20)
bool g_8way = true, g_move_rel = true;   // g_move_rel: PSPRECOMP_CAM_MOVE_REL=0 leaves the stick in the game's own frame
float g_dpad_zone = 0.35f;

void dissidia_stick_process(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t out = static_cast<uint32_t>(ctx->r[4]), in = static_cast<uint32_t>(ctx->r[5]);
    float lx = rdf(in), ly = rdf(in + 4);
    uint32_t id = 0;
    std::memcpy(&id, g_ram + (OVL_BASE & 0x07FFFFFFu), 4);
    if (g_move_rel && id == 1) {
        // The free camera (camera.cpp) turns the view away from the game's own camera, but the game still reads the stick
        // in its own frame (behind the fighter, towards the opponent): without this "up" would not go into the screen.
        // The stick is turned by the same angle the view was turned to the right (right and forward are the stick's x and -y).
        const float psi = dissidia_camera_turn();
        if (psi != 0.f) {
            const float sx = (lx - 127.5f) / 127.5f, sf = -(ly - 127.5f) / 127.5f;   // right, forward
            const float c = std::cos(psi), sn = std::sin(psi);
            const float mx = sx * c + sf * sn, mf = -sx * sn + sf * c;
            lx = 127.5f + mx * 127.5f;
            ly = 127.5f - mf * 127.5f;
            wrf(in, lx); wrf(in + 4, ly);
        }
    }
    if (g_8way && id == 1) {
        const float dx = lx - 127.5f, dy = ly - 127.5f;
        const float ax = std::fabs(dx), ay = std::fabs(dy), linf = ax > ay ? ax : ay;
        float sx = dx, sy = dy;
        if (linf > 1e-3f) {
            const float k = std::sqrt(dx * dx + dy * dy) / linf;   // 1 on an axis, up to sqrt 2 on a diagonal
            sx = dx * k; sy = dy * k;
            if (sx > 127.5f) sx = 127.5f; else if (sx < -127.5f) sx = -127.5f;
            if (sy > 127.5f) sy = 127.5f; else if (sy < -127.5f) sy = -127.5f;
        }
        const uint32_t dz_addr = out - 0x20u + PAD_DEADZONE;
        const float dz_was = rdf(dz_addr);
        wrf(in, 127.5f + sx); wrf(in + 4, 127.5f + sy);
        wrf(dz_addr, g_dpad_zone * 127.5f);
        g_orig(rdram, ctx);
        wrf(dz_addr, dz_was);
        wrf(in, lx); wrf(in + 4, ly);
    } else {
        g_orig(rdram, ctx);   // v0/v1 (virtual D-pad bits) are left exactly as the game computed them
    }
    const float dx = lx - 127.5f, dy = ly - 127.5f;
    const float len = std::sqrt(dx * dx + dy * dy);
    const float m = len / 127.5f;
    float ox = 0.f, oy = 0.f, w = 0.f;
    if (m >= g_dz && len > 1e-3f) {
        ox = dx / len; oy = dy / len;
        w = (m - g_dz) / (1.f - g_dz);
        if (w > 1.f) w = 1.f;
    }
    if (g_log > 0 && (std::fabs(dx) > 3.f || std::fabs(dy) > 3.f)) {
        g_log--;
        std::fprintf(stderr, "[STICK] raw=(%.0f,%.0f) angle=%.1f game=(%.3f,%.3f,%.3f) -> (%.3f,%.3f,%.3f)\n", lx, ly,
            std::atan2(dx, -dy) * 57.29578f, rdf(out), rdf(out + 4), rdf(out + 12), ox, oy, w);
    }
    wrf(out, ox); wrf(out + 4, oy); wrf(out + 8, 0.f); wrf(out + 12, w);
}

}  // namespace

void dissidia_input_install(uint8_t* rdram) {
    const char* e = std::getenv("PSPRECOMP_ANALOG_FULL");
    if (e && e[0] == '0') { std::fprintf(stderr, "[INPUT] analogue stick: original 8-way processing (PSPRECOMP_ANALOG_FULL=0)\n"); return; }
    g_ram = rdram;
    if ((e = std::getenv("PSPRECOMP_ANALOG_8WAY")) && e[0] == '0') g_8way = false;
    if ((e = std::getenv("PSPRECOMP_CAM_MOVE_REL")) && e[0] == '0') g_move_rel = false;
    if ((e = std::getenv("PSPRECOMP_ANALOG_DPAD_ZONE"))) g_dpad_zone = static_cast<float>(std::atof(e));
    if (g_dpad_zone < 0.05f) g_dpad_zone = 0.05f; else if (g_dpad_zone > 0.78f) g_dpad_zone = 0.78f;
    if ((e = std::getenv("PSPRECOMP_ANALOG_DEADZONE"))) g_dz = static_cast<float>(std::atof(e));
    if (g_dz < 0.f) g_dz = 0.f; else if (g_dz > 0.9f) g_dz = 0.9f;
    if ((e = std::getenv("PSPRECOMP_ANALOG_LOG"))) g_log = std::atoi(e);
    g_orig = psp_dispatch_probe_lookup(STICK_PROCESS);
    if (!g_orig) { std::fprintf(stderr, "[INPUT] stick hook NOT installed\n"); return; }
    psp_dispatch_register(STICK_PROCESS, dissidia_stick_process);
    std::fprintf(stderr, "[INPUT] continuous analogue stick direction (dead zone %.2f)\n", g_dz);
}
