// games/dissidia/runtime/tagteam_fx.cpp — the tag team's own effects, drawn on the game layer (menu_layer.h) at a fighter's place on the
// screen: the entrance of a member coming in (a column of light, a ring on the floor, rising sparks, then a flash when he strikes his pose).
// World points are projected with the matrices of the game's last 3D draw (ge_draw_last_view_proj). PSPRECOMP_TAGTEAM_NOFX=1 turns it off.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include <algorithm>
#include "menu_layer.h"
#include "psp_ge_draw.h"
#include "tagteam_hud.h"
#include "../../../runtime/src/hle/psp_savedata_dialog.h"

namespace {

dissidia::MenuLayer* g_fxl = nullptr;   // the layer being drawn (tagteam_hud.cpp's, set by tagteam_fx_draw)
#define g_fx (*g_fxl)
float g_fx_age = -1.f; float g_fx_pos[3] = {}; bool g_fx_have = false;   // this frame's entrant feet and battle age (tagteam_fx_frame)

struct Fx {
    uint32_t actor = 0;
    float t0 = 0, pose_at = 0, until = 0;   // battle ages (60ths): warp-in start, pose start, end
} g_ent;

// Projects a world point to 480 x 272 design units; false behind the camera.
bool project(const float p[3], float& sx, float& sy) {
    float V[12], P[16];
    if (!ge_draw_last_view_proj(V, P)) return false;
    const float v[3] = {p[0] * V[0] + p[1] * V[3] + p[2] * V[6] + V[9], p[0] * V[1] + p[1] * V[4] + p[2] * V[7] + V[10], p[0] * V[2] + p[1] * V[5] + p[2] * V[8] + V[11]};
    float c[4];
    for (int i = 0; i < 4; i++) c[i] = P[i] * v[0] + P[4 + i] * v[1] + P[8 + i] * v[2] + P[12 + i];
    if (c[3] <= 0.05f) return false;
    sx = (c[0] / c[3] * 0.5f + 0.5f) * 480.f;
    sy = (0.5f - c[1] / c[3] * 0.5f) * 272.f;
    return std::isfinite(sx) && std::isfinite(sy);
}

float hash01(uint32_t n) { n = (n ^ 61u) ^ (n >> 16); n *= 9u; n ^= n >> 4; n *= 0x27d4eb2du; n ^= n >> 15; return float(n & 0xFFFFFFu) / float(0xFFFFFF); }
float smooth(float x) { x = std::min(1.f, std::max(0.f, x)); return x * x * (3.f - 2.f * x); }

// The entrance: warp-in (t0 .. pose_at) and the pose (pose_at .. until). pos = the fighter's feet.
void draw_entrance(const float pos[3], float age) {
    using dissidia::LayerColor;
    const float t = (age - g_ent.t0) / 60.f, tp = (age - g_ent.pose_at) / 60.f, total = (g_ent.until - g_ent.t0) / 60.f;
    const float fade_out = 1.f - smooth((t - (total - 0.6f)) / 0.6f);
    float fx, fy, hx, hy;   // feet and head (1.7 m up) on the screen
    const float head[3] = {pos[0], pos[1] + 1.7f, pos[2]}, top[3] = {pos[0], pos[1] + 5.f, pos[2]};
    if (!project(pos, fx, fy) || !project(head, hx, hy)) { static int n = 0; if (n++ < 3 && std::getenv("PSPRECOMP_TAGTEAM_FXLOG")) std::fprintf(stderr, "[FX] t=%.2f no projection\n", t); return; }
    if (std::getenv("PSPRECOMP_TAGTEAM_FXLOG")) { static int n = 0; if (n++ % 10 == 0) std::fprintf(stderr, "[FX] t=%.2f tp=%.2f feet %.1f,%.1f head %.1f,%.1f\n", t, tp, fx, fy, hx, hy); }
    float tx, ty; if (!project(top, tx, ty)) { tx = hx; ty = hy - 60.f; }
    const float unit = std::max(4.f, std::fabs(fy - hy) / 1.7f);   // design units per metre at his distance
    const LayerColor glowc{150, 200, 255, 1.f};

    // the column of light: strongest while he arrives, thinning out once he stands
    const float col_a = smooth(t / 0.25f) * (tp < 0 ? 1.f : 1.f - smooth(tp / 0.5f)) * fade_out;
    if (col_a > 0.01f) {
        const float half = 0.85f * unit, x0 = fx - half * 2.4f, x1 = fx + half * 2.4f;
        // The column is separable: core / edge depend on x only, the vertical profile on y only. Each is computed once per pixel column / row (lazily,
        // from the very coordinates the band hands over) instead of once per pixel: at the entrance close-up the band is 100k+ pixels and the exp / pow /
        // sin per pixel cost 25-40 ms a frame (the stutter after a call-in). PSPRECOMP_TAGTEAM_FXSLOW=1: per pixel as before.
        static const bool slow = std::getenv("PSPRECOMP_TAGTEAM_FXSLOW") != nullptr;
        const int sc = g_fx.scale(), nx = 480 * sc + 8, ny = 272 * sc + 8;
        static std::vector<float> cc, ce, cv; cc.assign(size_t(nx), -1.f); ce.assign(size_t(nx), 0.f); cv.assign(size_t(ny), -1.f);
        g_fx.band(x0, std::min(ty, fy) , x1, fy + 2.f, [&](float x, float y) {
            const float h = (fy - y) / std::max(1.f, fy - ty);   // 0 at the feet, 1 at the top
            float core, edge, vert;
            const int ix = int(std::floor(g_fx.PX(x))), iy = int(std::floor(g_fx.PY(y)));
            const bool cached = !slow && ix >= 0 && ix < nx && iy >= 0 && iy < ny;
            if (cached && cc[size_t(ix)] >= 0.f) { core = cc[size_t(ix)]; edge = ce[size_t(ix)]; }
            else { const float d = (x - fx) / half; core = std::exp(-d * d * 4.f); edge = std::exp(-d * d * 0.7f) * 0.5f; if (cached) { cc[size_t(ix)] = core; ce[size_t(ix)] = edge; } }
            if (cached && cv[size_t(iy)] >= 0.f) vert = cv[size_t(iy)];
            else { vert = std::pow(std::max(0.f, 1.f - h), 0.7f) * (0.75f + 0.25f * std::sin(9.f * h - 10.f * t)); if (cached) cv[size_t(iy)] = vert; }
            const float a = (core * 1.0f + edge) * vert * col_a;
            return LayerColor{150.f + 100.f * core, 200.f + 50.f * core, 255.f, std::min(1.f, a)};
        });
    }
    // the ring on the floor: grows out from his feet during the warp-in, a fast wide one at the pose
    auto ring = [&](float radius_m, float a, float dot) {
        if (a <= 0.01f) return;
        const int n = 40;
        for (int i = 0; i < n; i++) {
            const float ang = 6.2831853f * float(i) / float(n);
            const float p[3] = {pos[0] + std::cos(ang) * radius_m, pos[1] + 0.03f, pos[2] + std::sin(ang) * radius_m};
            float x, y; if (project(p, x, y)) g_fx.glow(x, y, dot, LayerColor{glowc.r, glowc.g, glowc.b, a});
        }
    };
    if (tp < 0) ring(0.3f + 1.2f * smooth(t / 1.0f), 0.7f * smooth(t / 0.2f) * fade_out, 2.0f);
    // a soft glow on the floor under him and a flash where he appears
    if (col_a > 0.01f) g_fx.glow(fx, fy - 0.2f * unit, 1.3f * unit, LayerColor{170, 215, 255, 0.55f * col_a});
    if (t < 0.35f) g_fx.glow(fx, (fy + hy) * 0.5f, 1.4f * unit * (0.6f + t * 2.f), LayerColor{235, 245, 255, 0.9f * (1.f - smooth(t / 0.35f))});
    if (tp >= 0 && tp < 0.6f) ring(0.6f + 3.f * smooth(tp / 0.6f), 0.8f * (1.f - smooth(tp / 0.6f)), 2.2f);
    // rising sparks around him
    const int ns = 44;
    for (int i = 0; i < ns; i++) {
        const float life = 0.9f + 0.6f * hash01(i * 7u + 1u), born = hash01(i * 13u + 5u) * 1.2f;
        float k = (t - born) / life; if (k < 0.f) continue;
        k = std::fmod(k, 1.f);
        if (t - born > life && tp > 0.4f) continue;   // the last round dies out after the pose starts
        const float ang = 6.2831853f * hash01(i * 31u + 3u), r = 0.25f + 0.55f * hash01(i * 17u + 9u);
        const float p[3] = {pos[0] + std::cos(ang + k * 1.5f) * r, pos[1] + 0.1f + 2.4f * k, pos[2] + std::sin(ang + k * 1.5f) * r};
        float x, y; if (!project(p, x, y)) continue;
        const float a = std::sin(3.14159f * k) * 0.9f * fade_out;
        g_fx.glow(x, y, 1.6f + 1.4f * hash01(i * 3u), LayerColor{215, 238, 255, a});
    }
    // the flash as he strikes the pose
    if (tp >= 0 && tp < 0.45f) {
        const float a = 0.85f * (1.f - smooth(tp / 0.45f));
        const float cx = (fx + hx) * 0.5f, cy = (fy + hy) * 0.5f;
        g_fx.glow(cx, cy, 1.6f * unit + 30.f * tp, LayerColor{235, 245, 255, a});
    }
}

}  // namespace
bool tagteam_fx_project(const float p[3], float& sx, float& sy) { return project(p, sx, sy); }

// tagteam.cpp: the entrance of a member (warp-in from now, the pose at pose_at, the end at until; battle ages in 60ths)
void tagteam_fx_entrance(uint32_t actor, float now, float pose_at, float until) {
    if (std::getenv("PSPRECOMP_TAGTEAM_NOFX")) return;
    g_ent.actor = actor; g_ent.t0 = now; g_ent.pose_at = pose_at > 0.f ? pose_at : now; g_ent.until = until;
}

// tagteam.cpp, once per frame: the battle age and the entrant's feet (actor + 0x80) for this frame's effects
void tagteam_fx_frame(const uint8_t* rdram, float age) {
    g_fx_have = false; g_fx_age = age;
    if (!g_ent.actor || age < 0.f) return;
    if (age >= g_ent.until) { g_ent.actor = 0; return; }
    if (age < g_ent.t0) return;
    std::memcpy(g_fx_pos, rdram + ((g_ent.actor + 0x80u) & 0x07FFFFFFu), 12);
    { float q[3]; if (tagteam_hud_pos(g_ent.actor, q)) std::memcpy(g_fx_pos, q, 12); }   // the interpolated scene's place (tagteam_hud.cpp)
    g_fx_have = std::isfinite(g_fx_pos[0]) && std::fabs(g_fx_pos[0]) < 2000.f;
}
// tagteam_hud.cpp (tagteam_layer_frame): draws this frame's effects into its layer; true when something was drawn
bool tagteam_fx_draw(dissidia::MenuLayer& layer) {
    if (!g_fx_have) return false;
    g_fxl = &layer;
    draw_entrance(g_fx_pos, g_fx_age);
    return true;
}
