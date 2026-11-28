#include "psp_ge.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "psp_ge_draw.h"

#include <cmath>
#include <algorithm>

// PSP viewport scale/offset + depth-range registers -> GL viewport + depth
// range (issue #23). Pure math, no GL dependency, so it is unit-testable in
// isolation (tests/test_ge_viewport.cpp).
//
// Mirrors PPSSPP's ConvertViewportAndScissor
// (GPU/Common/GPUStateUtils.cpp:656-798) for the 1:1, non-supersampled,
// non-accurate-depth FBO case the runtime renders into. PPSSPP documents the
// PSP viewport transform as:
//   Xscreen = -offsetX + vpXCenter + vpXScale * Xview
//   Yscreen = -offsetY + vpYCenter + vpYScale * Yview
//   Zscreen =            vpZCenter + vpZScale * Zview
// so the equivalent glViewport(left, top, w, h) is
//   left = vpXCenter - offsetX - |vpXScale|,  w = |2 * vpXScale|
//   top  = vpYCenter - offsetY - |vpYScale|,  h = |2 * vpYScale|
// offsetX/Y are 1/16-subpixel u16 register words (PPSSPP getOffsetX:
// (offsetx & 0xFFFF) / 16). PSP screen origin is top-left; the runtime FBO
// uses GL's bottom-left origin without a vertex Y-flip, so `top` is flipped:
//   gl_y = fb_height - top - h.
// Depth maps the PSP Z window [vpZCenter - vpZScale, vpZCenter + vpZScale]
// (encoded in [0, 65535]) to glDepthRange's [0, 1] -- the same as PPSSPP's
// non-accurate-depth branch (GPUStateUtils.cpp:790-794).
GeViewportDepth ge_compute_viewport_depth(
    float vp_x_scale, float vp_y_scale,
    float vp_x_center, float vp_y_center,
    float vp_z_scale, float vp_z_center,
    uint32_t off_x_raw, uint32_t off_y_raw,
    int fb_height) {
    const float off_x = static_cast<float>(off_x_raw & 0xFFFFu) / 16.0f;
    const float off_y = static_cast<float>(off_y_raw & 0xFFFFu) / 16.0f;

    const float ax = std::fabs(vp_x_scale);
    const float ay = std::fabs(vp_y_scale);
    const float left = vp_x_center - off_x - ax;
    const float top = vp_y_center - off_y - ay;
    const float w = ax * 2.0f;
    const float h = ay * 2.0f;
    const float gl_y = static_cast<float>(fb_height) - top - h;

    float near_z = (vp_z_center - vp_z_scale) / 65535.0f;
    float far_z = (vp_z_center + vp_z_scale) / 65535.0f;
    near_z = std::min(std::max(near_z, 0.0f), 1.0f);
    far_z = std::min(std::max(far_z, 0.0f), 1.0f);

    GeViewportDepth out;
    out.x = static_cast<int>(std::lround(left));
    out.y = static_cast<int>(std::lround(gl_y));
    out.w = static_cast<int>(std::lround(w));
    out.h = static_cast<int>(std::lround(h));
    out.near_z = near_z;
    out.far_z = far_z;
    return out;
}

// Field-of-view setting: a perspective projection (clip w = -z) has its focal scales (x, y) changed so its vertical FOV is the game's
// plus the offset; aspect (y / x) is unchanged. Orthographic and degenerate matrices are left alone.
static std::atomic<int> g_fov_off{-1000};
int ge_fov_offset() {
    int v = g_fov_off.load();
    if (v == -1000) {
        const char* e = std::getenv("PSPRECOMP_FOV");
        v = e && e[0] ? std::atoi(e) : 0;
        v = std::clamp(v, -20, 40);
        g_fov_off.store(v);
    }
    return v;
}
void ge_set_fov_offset(int deg) { g_fov_off.store(std::clamp(deg, -20, 40)); }
static std::atomic<bool> g_fov_proj{true};
void ge_fov_use_projection(bool on) { g_fov_proj.store(on); }
// A game module's per-frame view-only zoom (Dissidia: the tag team's banter close-ups, whose own fov writes reach the game's camera object after its projection was built):
// every perspective projection has its focal scales multiplied by k (1 = off), on top of the offset.
static std::atomic<float> g_fov_k{1.0f};
void ge_set_fov_scale(float k) { g_fov_k.store(std::clamp(k, 0.2f, 5.0f)); }
void ge_apply_fov(float* p) {
    {
        const float k = g_fov_k.load();
        if (k != 1.0f && (p[11] < -0.5f && p[11] > -1.5f) && p[15] == 0.0f && p[5] > 1e-3f && p[0] > 1e-3f) { p[0] *= k; p[5] *= k; }
    }
    const int off = ge_fov_offset();
    if (off == 0 || !g_fov_proj.load() || !(p[11] < -0.5f && p[11] > -1.5f) || p[15] != 0.0f || !(p[5] > 1e-3f) || !(p[0] > 1e-3f)) return;
    const float kPi = 3.14159265f;
    const float fov0 = 2.0f * std::atan(1.0f / p[5]);
    const float fov = std::clamp(fov0 + float(off) * kPi / 180.0f, 10.0f * kPi / 180.0f, 150.0f * kPi / 180.0f);
    const float k = std::tan(fov0 * 0.5f) / std::tan(fov * 0.5f);
    p[0] *= k; p[5] *= k;
}
