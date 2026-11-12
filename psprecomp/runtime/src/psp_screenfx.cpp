// Screen-space scene effects (SSAO, contact shadows, SSR), see include/psp_screenfx.h.
//
// The scene's depth buffer holds the PSP's window depth: ndc z mapped through glDepthRange(near_z, far_z). A pixel's view
// space position is rebuilt with the inverse of the PSP projection matrix, so the passes do not need to know the game's
// near / far planes or field of view.
//
// Passes (all but the last two at half resolution, which is what makes this affordable on a handheld):
//   1. one pass computes AO + contact shadow (RG texture) and the reflection (premultiplied RGBA texture) from the depth,
//   2. a depth-aware blur of the AO / shadow texture in two directions,
//   3. composite: AO * (1 - shadow) multiplied into the scene (depth-aware 2x2 upsample), then the reflection blended
//      over the result (so a shadow darkens the floor but not the reflection in it).
// RGB only: the buffer's alpha is the GE stencil.
#include "psp_screenfx.h"
#include "psp_rt.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
PspRtHooks g_psp_rt;
// Per-stage lighting: the game module reports which stage is being fought (-1 = unknown / not in a battle).
static int (*g_stage_id_fn)() = nullptr;
void psp_set_stage_id_provider(int (*fn)()) { g_stage_id_fn = fn; }
int psp_stage_id() { return g_stage_id_fn ? g_stage_id_fn() : -1; }
// How much of the scene's light a stage has. Enclosed stages have no sun: ray-traced shadows darken the whole picture by `dark` there
// (1 = unchanged). PSPRECOMP_STAGE_DARK=<f> overrides it for every stage (tuning), PSPRECOMP_STAGE_LOG=1 logs the id.
struct StageLook { int id; float dark; };
static const StageLook k_stage_looks[] = {
    {1, 0.3f},   // Pandaemonium: enclosed, no light source (issue #4)
};
static float stage_dark() {
    static const bool has_env = std::getenv("PSPRECOMP_STAGE_DARK") != nullptr;
    const int id = psp_stage_id();
    static int last = -2;
    if (id != last) { last = id; if (std::getenv("PSPRECOMP_STAGE_LOG")) std::fprintf(stderr, "[STAGE] id %d\n", id); }
    if (has_env) return std::getenv("PSPRECOMP_STAGE_DARK") ? float(std::atof(std::getenv("PSPRECOMP_STAGE_DARK"))) : 1.f;
    for (const StageLook& s : k_stage_looks) if (s.id == id) return s.dark;
    return 1.f;
}
static std::atomic<int> g_rt_on{-1};
bool psp_rt_enabled() {
    int v = g_rt_on.load();
    if (v < 0) { const char* e = std::getenv("PSPRECOMP_VK_RT"); v = e && e[0] ? std::min(3, std::max(0, std::atoi(e))) : 0; g_rt_on.store(v); }
    return v != 0;
}
int psp_rt_quality() { return psp_rt_enabled() ? g_rt_on.load() : 0; }
void psp_rt_set_quality(int q) { g_rt_on.store(std::min(3, std::max(0, q))); }
void psp_rt_set_enabled(bool on) { psp_rt_set_quality(on ? 2 : 0); }
static std::atomic<int> g_rt_refl_on{-1};
bool psp_rt_refl_enabled() {
    int v = g_rt_refl_on.load();
    if (v < 0) { const char* e = std::getenv("PSPRECOMP_VK_RT_REFL"); v = e && e[0] ? std::min(3, std::max(0, std::atoi(e))) : 0; g_rt_refl_on.store(v); }
    return v != 0;
}
int psp_rt_refl_quality() { return psp_rt_refl_enabled() ? g_rt_refl_on.load() : 0; }
void psp_rt_refl_set_quality(int q) { g_rt_refl_on.store(std::min(3, std::max(0, q))); }
void psp_rt_refl_set_enabled(bool on) { psp_rt_refl_set_quality(on ? 2 : 0); }
const char* psp_rt_unavailable_reason() {
    if (psp_rt_available()) return "";
    if (g_psp_rt.unavailable_reason) { const char* r = g_psp_rt.unavailable_reason(); if (r && r[0]) return r; }
#ifdef __ANDROID__
    return "Needs the Vulkan renderer and a driver with ray query: on an Adreno 740 or newer, set Graphics API to Vulkan and pick a Turnip driver under GPU Driver, then restart.";
#else
    return "Needs the Vulkan renderer: set Graphics API to Vulkan and restart.";
#endif
}

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::atomic<int> g_ao{-1}, g_cs{-1}, g_sr{-1};

int env_level(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return 0;
    int n = std::atoi(v);
    return n < 0 ? 0 : n > 3 ? 3 : n;
}
float env_float(const char* name, float dflt) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? float(std::atof(v)) : dflt;
}

const char* const k_vs = R"GLSL(
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// Shared by the passes.
const char* const k_common = R"GLSL(
in vec2 v_uv;
uniform sampler2D u_depth;
uniform vec2 u_size;        // full-resolution size of the depth texture
uniform vec2 u_step;        // one texel of the pass being drawn, in uv (the neighbours for normals / blur)
uniform mat4 u_proj;
uniform mat4 u_inv;
uniform vec2 u_dr;          // glDepthRange near, far
uniform float u_clearz;     // the cleared (background) depth
uniform float u_sky;        // depth_t above which a pixel is the sky dome / background
uniform vec2 u_fade;        // view distance where the effects start to fade out / are gone (the 16-bit depth is too coarse far away)

float wd_at(vec2 uv) { return texture(u_depth, uv).r; }
float depth_t(float wd) { return (wd - u_dr.x) / (u_dr.y - u_dr.x); }   // 0 at the near plane, 1 at the far plane
bool is_far(float wd) { return abs(wd - u_clearz) < 3.0 / 65535.0 || depth_t(wd) > u_sky; }
vec3 vpos(vec2 uv, float wd) {
    float z = depth_t(wd) * 2.0 - 1.0;
    vec4 p = u_inv * vec4(uv * 2.0 - 1.0, z, 1.0);
    return p.xyz / p.w;
}
vec3 vpos(vec2 uv) { return vpos(uv, wd_at(uv)); }
// View-space z straight from the projection (no matrix product): used where only the depth along the view axis matters.
float vz(float wd) {
    float ndc = depth_t(wd) * 2.0 - 1.0;
    return (u_proj[3][2] - ndc * u_proj[3][3]) / (ndc * u_proj[2][3] - u_proj[2][2]);
}
float dist_fade(float lp) { return 1.0 - smoothstep(u_fade.x, u_fade.y, lp); }
// Surface normal from the depth neighbours (the side with the smaller jump on each axis), facing the camera.
vec3 normal_at(vec2 uv, vec3 P) {
    vec2 px = u_step;
    vec3 a = vpos(uv + vec2(px.x, 0.0)), b = vpos(uv - vec2(px.x, 0.0));
    vec3 c = vpos(uv + vec2(0.0, px.y)), d = vpos(uv - vec2(0.0, px.y));
    vec3 dx = abs(length(a) - length(P)) < abs(length(b) - length(P)) ? a - P : P - b;
    vec3 dy = abs(length(c) - length(P)) < abs(length(d) - length(P)) ? c - P : P - d;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, P) > 0.0) n = -n;      // the camera is at the origin
    return n;
}
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
)GLSL";

// Pass 1 (half resolution): AO + contact shadow -> o_fx, reflection (premultiplied) -> o_refl.
const char* const k_fs_main = R"GLSL(
layout(location = 0) out vec4 o_fx;
layout(location = 1) out vec4 o_refl;
uniform sampler2D u_color;  // the scene, half resolution (reflection source)
uniform float u_radius;     // AO radius as a fraction of the distance to the camera
uniform int u_n_ao;         // samples per effect; 0 = effect off
uniform int u_n_cs;
uniform int u_n_ssr;
uniform vec3 u_light;       // view space, towards the light
uniform vec3 u_up;          // world up in view space
uniform float u_cs_len;     // contact shadow ray length, view units
uniform float u_cs_thick;   // how far behind a visible surface a ray point still counts as occluded, view units
uniform float u_ssr_len;    // reflection ray length as a fraction of the distance to the camera
uniform float u_ssr_amt;
uniform int u_have_fx;      // 1: the scene has an effects layer (blended draws, ge_draw: fxl_*)
uniform sampler2D u_fxc;    // effects colour (premultiplied by the game's own blend) + coverage in alpha; additive draws leave alpha alone
uniform sampler2D u_fxd;    // effects depth: the nearest effect fragment per pixel
uniform int u_fx_dbg;       // PSPRECOMP_SSR_FX_DEBUG=1: a reflection that hit an effect is magenta
uniform float u_fx_gain;    // base reflectivity of an effect (opaque reflections use Fresnel alone)
uniform float u_fx_thick;   // how far behind an effect's front surface a ray still counts as inside it, view units
//@RT_DECL
// The effect at uv floats in front of the scene there: its depth is clearly nearer than the scene's. A blended sheet lying on a
// surface (the water layer of the floor, decals, shadows) is at the scene's own depth and is not an effect to reflect.
bool fx_floats(vec2 uv, float wd_scene, float efd) {
    return length(vpos(uv, wd_scene)) - length(vpos(uv, efd)) > 0.2 + 0.01 * length(vpos(uv, wd_scene));
}

void main() {
    o_fx = vec4(1.0, 0.0, 0.0, 1.0);
    o_refl = vec4(0.0);
    float wd = wd_at(v_uv);
    if (u_fx_dbg == 3 && u_have_fx != 0) {   // debug: where the effects layer holds anything (sky included), no floor test
        vec4 fc = texture(u_fxc, v_uv);
        if (fc.a > 0.03 || max(fc.r, max(fc.g, fc.b)) > 0.06) { o_refl = vec4(fc.rgb * 0.5 + vec3(0.5, 0.0, 0.5), 1.0); return; }
    }
    if (is_far(wd)) return;
    vec3 P = vpos(v_uv, wd);
    float lp = length(P);
    float fd = dist_fade(lp);
    if (fd <= 0.0) return;
    vec3 n = normal_at(v_uv, P);
    float noise = ign(gl_FragCoord.xy);
    float ao = 1.0;
    if (u_n_ao > 0) {
        float rad = u_radius * lp;
        vec3 up = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 tx = normalize(cross(up, n)), ty = cross(n, tx);
        float occ = 0.0;
        for (int i = 0; i < u_n_ao; ++i) {
            float t = (float(i) + 0.5) / float(u_n_ao);
            float a = noise * 6.2831853 + float(i) * 2.39996323;
            float c = mix(0.12, 1.0, fract(float(i) * 0.618034 + noise));
            float s = sqrt(1.0 - c * c);
            vec3 dir = tx * (cos(a) * s) + ty * (sin(a) * s) + n * c;
            vec3 S = P + dir * (rad * mix(0.15, 1.0, t * t));
            vec4 cl = u_proj * vec4(S, 1.0);
            if (cl.w <= 1e-5) continue;
            vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
            if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) continue;
            float wd2 = wd_at(uv2);
            if (is_far(wd2)) continue;
            float dq = length(vpos(uv2, wd2)), ds = length(S);
            if (ds - dq > 0.02 * rad) occ += smoothstep(0.0, 1.0, rad / max(abs(lp - dq), 1e-4));
        }
        ao = mix(1.0, 1.0 - occ / float(u_n_ao), fd);
    }
    float sh = 0.0;
    if (u_n_cs > 0) {
        float facing = smoothstep(0.15, 0.5, dot(n, u_light));   // surfaces turned away from the light are dark already
        if (facing > 0.0) {
            float jit = ign(gl_FragCoord.xy + 17.0);
            vec3 O = P + n * (0.01 + 0.004 * lp);
//@RT_CODE
            for (int j = 0; j < u_n_cs; ++j) {
                float t = (float(j) + jit) / float(u_n_cs);
                vec3 S = O + u_light * (t * t * u_cs_len);      // denser near the surface
                vec4 cl = u_proj * vec4(S, 1.0);
                if (cl.w <= 1e-5) break;
                vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
                if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) break;
                float wd2 = wd_at(uv2);
                if (is_far(wd2)) continue;
                float diff = length(S) - length(vpos(uv2, wd2));
                if (diff > 0.01 + 0.004 * lp && diff < u_cs_thick) { sh = facing * fd; break; }
            }
        }
    }
    o_fx = vec4(ao, sh, 0.0, 1.0);
    if (u_fx_dbg == 2 && u_have_fx != 0) {   // debug: the effects layer itself, wherever the layer holds something
        vec4 fc = texture(u_fxc, v_uv);
        if ((fc.a > 0.03 || max(fc.r, max(fc.g, fc.b)) > 0.06) && fx_floats(v_uv, wd, texture(u_fxd, v_uv).r)) { o_refl = vec4(fc.rgb + vec3(0.4, 0.0, 0.4), 1.0); return; }
    }

    if (u_n_ssr > 0 && dot(n, u_up) > 0.7) {
//@RT_REFL
        {
        float upness = dot(n, u_up);
        vec3 V = normalize(P);
        vec3 R = reflect(V, n);
        float maxd = u_ssr_len * lp;
        float stepl = maxd / float(u_n_ssr);
        float jit = ign(gl_FragCoord.xy);
        vec3 O = P + n * (0.004 * lp);
        vec2 hit = vec2(-1.0);
        float hit_t = 0.0, prev_t = 0.0;
        vec2 last_in = vec2(-1.0);
        bool fx_hit = false;
        float fx_t = 0.0;
        vec4 fx_c = vec4(0.0);
        vec2 fx_uv = vec2(0.5);
        for (int i = 0; i < u_n_ssr; ++i) {
            float t = (float(i) + jit) * stepl;
            vec3 S = O + R * t;
            vec4 cl = u_proj * vec4(S, 1.0);
            if (cl.w <= 1e-5) break;
            vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
            if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) break;
            last_in = uv2;
            float wd2 = wd_at(uv2);
            if (u_have_fx != 0 && !fx_hit) {
                // effects layer: orbs, beams, auras, glass. They write no scene depth, so the ray is tested against the nearest effect
                // fragment's own depth; an effect later covered by opaque geometry (its depth behind the scene's) is not there.
                vec4 fc = texture(u_fxc, uv2);
                if (fc.a > 0.03 || max(fc.r, max(fc.g, fc.b)) > 0.06) {
                    float efd = texture(u_fxd, uv2).r;
                    if ((is_far(wd2) || fx_floats(uv2, wd2, efd))) {
                        float de = length(vpos(uv2, efd)), ds = length(S);
                        if (ds > de - 0.02 * lp && ds < de + stepl * 2.5 + u_fx_thick) { fx_hit = true; fx_t = t; fx_c = fc; fx_uv = uv2; }
                    }
                }
            }
            if (!is_far(wd2)) {
                float diff = length(S) - length(vpos(uv2, wd2));
                if (diff > 0.0 && diff < stepl * 2.5 + 0.01 * lp) {
                    float lo = prev_t, hi = t;
                    for (int k = 0; k < 4; ++k) {           // refine between the previous and this step
                        float mid = 0.5 * (lo + hi);
                        vec3 Sm = O + R * mid;
                        vec4 cm = u_proj * vec4(Sm, 1.0);
                        vec2 um = cm.xy / cm.w * 0.5 + 0.5;
                        float wm = wd_at(um);
                        if (!is_far(wm) && length(Sm) - length(vpos(um, wm)) > 0.0) hi = mid; else lo = mid;
                    }
                    vec3 Sh = O + R * hi;
                    vec4 ch = u_proj * vec4(Sh, 1.0);
                    hit = ch.xy / ch.w * 0.5 + 0.5;
                    hit_t = hi;
                    break;
                }
            }
            prev_t = t;
        }
        if (hit.x < 0.0 && last_in.x >= 0.0 && is_far(wd_at(last_in))) {   // the ray left the scene into the sky
            hit = last_in; hit_t = maxd;
            // The sky (and the beams / clouds painted on it) lies in the ray's direction, at infinity: take the picture where that
            // direction projects, not where the short march happened to end (that showed the sky straight above the floor point).
            vec4 cs = u_proj * vec4(R, 0.0);
            if (cs.w > 1e-5) {
                vec2 us = cs.xy / cs.w * 0.5 + 0.5;
                if (us.x > 0.0 && us.x < 1.0 && us.y > 0.0 && us.y < 1.0 && is_far(wd_at(us))) hit = us;
            }
        }
        if (hit.x >= 0.0 || fx_hit) {
            vec3 col = vec3(0.0);
            vec2 euv = fx_uv;
            float ht = hit_t;
            if (hit.x >= 0.0) { col = texture(u_color, hit).rgb; euv = hit; }
            vec2 e = min(euv, 1.0 - euv);
            float edge = smoothstep(0.0, 0.12, min(e.x, e.y));
            float fres = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, -V), 0.0, 1.0), 5.0);
            float refl_k = smoothstep(0.7, 0.95, upness) * fd * u_ssr_amt;
            float k = hit.x >= 0.0 ? clamp(fres * 1.6 + 0.08, 0.0, 1.0) * edge * (1.0 - ht / maxd * 0.6) * refl_k : 0.0;
            o_refl = vec4(col * k, k);
            if (fx_hit) {
                // Effects are glowing / see-through things that read as bright reflections even at a steep angle on a wet floor, so they
                // get a stronger base than the Fresnel-only opaque reflection (an orb or beam at 14% was invisible).
                float efe = min(min(fx_uv.x, 1.0 - fx_uv.x), min(fx_uv.y, 1.0 - fx_uv.y));
                float kf = clamp(fres * 1.6 + u_fx_gain, 0.0, 1.0) * smoothstep(0.0, 0.12, efe) * (1.0 - fx_t / maxd * 0.5) * refl_k;
                float cov = clamp(max(fx_c.a, max(fx_c.r, max(fx_c.g, fx_c.b))), 0.0, 1.0);
                float af = cov * kf;
                o_refl = vec4(fx_c.rgb * kf + o_refl.rgb * (1.0 - fx_c.a * kf), af + o_refl.a * (1.0 - af));
            }
            if (u_fx_dbg == 1 && fx_hit) o_refl = vec4(0.8, 0.0, 0.8, 0.8);
        }
        }
    }
}
)GLSL";

// Pass 2: depth-aware blur of o_fx (5 taps, half resolution).
const char* const k_fs_blur = R"GLSL(
out vec4 o_color;
uniform sampler2D u_fx;
uniform vec2 u_dir;         // one texel step
void main() {
    float wd = wd_at(v_uv);
    if (is_far(wd)) { o_color = vec4(1.0, 0.0, 0.0, 1.0); return; }
    float z0 = vz(wd);
    vec2 sum = vec2(0.0);
    float wsum = 0.0;
    for (int i = -2; i <= 2; ++i) {
        vec2 uv = v_uv + u_dir * float(i);
        float w2 = wd_at(uv);
        if (is_far(w2)) continue;
        float dl = abs(vz(w2) - z0) / abs(z0);
        float w = exp(-float(i * i) / 3.0) * exp(-dl * 60.0);
        sum += texture(u_fx, uv).rg * w;
        wsum += w;
    }
    o_color = vec4(wsum > 0.0 ? sum / wsum : vec2(1.0, 0.0), 0.0, 1.0);
}
)GLSL";

// Pass 3a (full resolution, blend dst * src): AO and contact shadow, upsampled from half resolution by weighting the four
// nearest texels by how close their depth is to this pixel's, so nothing bleeds across a silhouette.
const char* const k_fs_mul = R"GLSL(
out vec4 o_color;
uniform sampler2D u_fx;
uniform vec2 u_half;        // half-resolution size
uniform float u_ao_amt;
uniform float u_cs_amt;
uniform float u_dark;       // stage darkening (enclosed stages), 1 = none
void main() {
    float wd = wd_at(v_uv);
    if (is_far(wd)) { o_color = vec4(1.0); return; }
    float z0 = vz(wd);
    vec2 hp = v_uv * u_half - 0.5;
    vec2 base = floor(hp), f = hp - base;
    vec2 sum = vec2(0.0);
    float wsum = 0.0;
    for (int k = 0; k < 4; ++k) {
        vec2 o = vec2(float(k & 1), float(k >> 1));
        vec2 uvc = (base + o + 0.5) / u_half;
        float bw = (o.x > 0.5 ? f.x : 1.0 - f.x) * (o.y > 0.5 ? f.y : 1.0 - f.y);
        float w2 = wd_at(uvc);
        float dl = is_far(w2) ? 1.0 : abs(vz(w2) - z0) / abs(z0);
        float w = (bw + 0.001) * exp(-dl * 60.0) + 1e-5;
        sum += texture(u_fx, uvc).rg * w;
        wsum += w;
    }
    vec2 fx = sum / wsum;
    o_color = vec4(vec3(mix(1.0, fx.r, u_ao_amt) * (1.0 - fx.g * u_cs_amt) * u_dark), 1.0);
}
)GLSL";

// Pass 3b (full resolution, blend one / one-minus-src-alpha): the premultiplied reflection.
const char* const k_fs_refl = R"GLSL(
out vec4 o_color;
uniform sampler2D u_refl;
uniform float u_dbg;        // debug (PSPRECOMP_REFL_ONLY=1): show the reflection layer alone
uniform float u_dark;       // stage darkening: the reflection is light too
void main() {
    if (is_far(wd_at(v_uv))) { o_color = vec4(0.0); return; }
    o_color = texture(u_refl, v_uv);
    o_color.rgb *= u_dark;
    if (u_dbg > 0.0) o_color = vec4(o_color.rgb * 2.0, 1.0);
}
)GLSL";

struct Prog {
    GLuint id = 0;
    bool failed = false;
};
Prog g_main_p, g_blur_p, g_mul_p, g_refl_p, g_main_rt_p;

// Ray-queried contact shadow (PSPRECOMP_VK_RT, Vulkan only; docs/VULKAN_PORT.md): the pass-1 shader with the screen-space march
// replaced by a ray query from the surface toward the light against the frame's world-space triangles (psp_rt.h). The GL
// shaders never see these lines: the markers are comments there.
const char* const k_rt_decl = R"GLSL(
uniform accelerationStructureEXT u_tlas;
uniform int u_use_rt;
uniform mat4 u_view_inv;    // view space -> PSP world space (where the triangles are)
uniform vec3 u_light_w;     // world space, towards the light
uniform float u_rt_len;     // ray length, world units
uniform int u_rt_rays;      // shadow rays per pixel (quality level: 1, 3, 6), jittered across the light disc for a soft edge
uniform float u_rt_soft;    // half angle of the light disc, radians-ish
uniform int u_use_rt_refl;
uniform int u_rt_dbg;       // debug (PSPRECOMP_RT_REFL_DEBUG=1): colour by what the ray found (red tri colour, green visible on screen, blue miss, yellow sky)
uniform int u_rt_refl_rays; // reflection rays per pixel (quality: 1, 1, 3), jittered by u_rt_rough for a glossy blur
uniform float u_rt_rough;
uniform int u_rt_fx;        // the scene has an effects layer
uniform float u_rt_hidden;   // brightness / strength of a hit on a surface that is not visible on screen
uniform float u_rt_sky_amt;  // strength of a reflection of the sky relative to one of geometry
uniform float u_rt_sky_dist; // view units past which a hit counts as the sky dome
uniform int u_refl_split;   // debug (PSPRECOMP_RT_REFL_SPLIT=1): the left half of the picture keeps the screen-space reflection
uniform float u_rt_refl_len; // reflection ray length, world units
uniform vec3 u_rt_sky;      // what a reflection ray that hits nothing shows when the sky is off screen
)GLSL";
// Ray-traced reflection: the closest hit along the reflected view ray. Shading the hit: the geometry has positions only, so a
// hit shows the triangle's colour captured at draw time (texture average x vertex colour / lit material, rt_tri_col[]); where
// the hit point is visible on screen the real, shaded scene colour is used instead (screen-space assisted). A miss shows the
// sky: the scene colour in that direction when it is on screen, else u_rt_sky.
const char* const k_rt_refl = R"GLSL(
        if (u_use_rt_refl != 0 && !(u_refl_split != 0 && v_uv.x < 0.5)) {
            float upness = dot(n, u_up);
            vec3 V = normalize(P);
            vec3 R0 = reflect(V, n);
            vec4 racc = vec4(0.0);
            for (int ri = 0; ri < u_rt_refl_rays; ++ri) {
            vec3 R = R0;
            if (u_rt_refl_rays > 1) {
                float ja = ign(gl_FragCoord.xy + 31.0 * float(ri + 1)) * 6.2831853, jb = ign(gl_FragCoord.yx + 57.0 * float(ri + 1)) * 2.0 - 1.0, jr = sqrt(1.0 - jb * jb);
                R = normalize(R0 + vec3(cos(ja) * jr, jb, sin(ja) * jr) * u_rt_rough);
                R = normalize(R + n * max(0.0, 0.05 - dot(R, n)));
            }
            vec3 O = P + n * (0.01 + 0.004 * lp);
            rayQueryEXT rq;
            rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsOpaqueEXT, 0x1, (u_view_inv * vec4(O, 1.0)).xyz, 0.0,
                                  normalize((u_view_inv * vec4(R, 0.0)).xyz), u_rt_refl_len);
            while (rayQueryProceedEXT(rq)) {}
            vec3 col = u_rt_sky;
            float hit_t = u_rt_refl_len;
            bool hit = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
            vec3 dbg = vec3(0.0, 0.0, 1.0);                                   // miss
            if (hit) {
                hit_t = rayQueryGetIntersectionTEXT(rq, true);
                uint pc = rt_tri_col[rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)];
                col = vec3(float(pc & 255u), float((pc >> 8) & 255u), float((pc >> 16) & 255u)) / 255.0;
                dbg = vec3(1.0, 0.0, 0.0);                                    // hit, shaded by the triangle's colour
            }
            vec3 H = O + R * (hit ? hit_t : 500.0);
            bool hidden = hit;                                                    // cleared below when the hit shows its real on-screen colour
            bool sky = !hit || length(H) >= u_rt_sky_dist;                        // the sky dome (or nothing): the floor already carries a baked sky, so it counts less
            vec4 ch = u_proj * vec4(H, 1.0);
            if (ch.w > 1e-5) {
                vec2 uvh = ch.xy / ch.w * 0.5 + 0.5;
                bool on = uvh.x > 0.0 && uvh.x < 1.0 && uvh.y > 0.0 && uvh.y < 1.0;
                if (hit && length(H) < u_rt_sky_dist) {
                    if (on) {
                        float wdh = wd_at(uvh);
                        if (!is_far(wdh) && abs(length(vpos(uvh, wdh)) - length(H)) < 0.03 * length(H) + 0.05) { col = texture(u_color, uvh).rgb; dbg = vec3(0.0, 1.0, 0.0); hidden = false; }   // visible: the real shaded colour
                    }
                } else {                                                      // the sky (a miss or a hit on the dome): the picture's sky in that direction
                    vec2 uvs = clamp(uvh, vec2(0.02), vec2(0.98));
                    if ((on || !hit) && is_far(wd_at(uvs))) { col = texture(u_color, uvs).rgb; dbg = vec3(1.0, 1.0, 0.0); hidden = false; }   // off screen, a dome hit keeps its triangle colour
                    hit_t = 0.25 * u_rt_refl_len;
                }
            } else if (hit && length(H) >= u_rt_sky_dist) hit_t = 0.25 * u_rt_refl_len;
            // The effects layer (orbs, auras, glass: blended draws that write no depth): the closest one in front of the opaque hit,
            // shown with its real on-screen colour when it is visible there, else its triangle colour, blended over the opaque reflection.
            vec3 fxc = vec3(0.0);
            float fxa = 0.0, fx_t = 0.0;
            if (u_rt_fx != 0 && !(u_rt_fx == 2 && v_uv.x < 0.5)) {   // 2 = debug split: no effects layer in the left half
                rayQueryEXT rf;
                rayQueryInitializeEXT(rf, u_tlas, gl_RayFlagsOpaqueEXT, 0x2, (u_view_inv * vec4(O, 1.0)).xyz, 0.0,
                                      normalize((u_view_inv * vec4(R, 0.0)).xyz), hit ? hit_t : u_rt_refl_len);
                while (rayQueryProceedEXT(rf)) {}
                if (rayQueryGetIntersectionTypeEXT(rf, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
                    fx_t = rayQueryGetIntersectionTEXT(rf, true);
                    uint fc = rt_fx_col[rayQueryGetIntersectionPrimitiveIndexEXT(rf, true)];
                    fxc = vec3(float(fc & 255u), float((fc >> 8) & 255u), float((fc >> 16) & 255u)) / 255.0;
                    fxa = float(fc >> 24) / 255.0;
                    vec4 cf = u_proj * vec4(O + R * fx_t, 1.0);
                    if (cf.w > 1e-5) {
                        vec2 uvf = cf.xy / cf.w * 0.5 + 0.5;
                        if (uvf.x > 0.0 && uvf.x < 1.0 && uvf.y > 0.0 && uvf.y < 1.0) fxc = texture(u_color, uvf).rgb;
                    }
                }
            }
            if (hidden && !sky) col *= u_rt_hidden;      // a surface the camera cannot see (an underside, a back): its flat colour is unlit, so show it dim
            float fres = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, -V), 0.0, 1.0), 5.0);
            float k = clamp(fres * 1.6 + 0.08, 0.0, 1.0) * (hidden && !sky ? u_rt_hidden : 1.0) * (1.0 - hit_t / u_rt_refl_len * 0.5) * smoothstep(0.7, 0.95, upness) * fd * u_ssr_amt * (sky ? u_rt_sky_amt : 1.0);
            o_refl = vec4(col * k, k);
            if (fxa > 0.0) {
                float kf = clamp(fres * 1.6 + 0.08, 0.0, 1.0) * (1.0 - fx_t / u_rt_refl_len * 0.5) * smoothstep(0.7, 0.95, upness) * fd * u_ssr_amt;
                float af = fxa * kf;
                o_refl = vec4(fxc * af + o_refl.rgb * (1.0 - af), af + o_refl.a * (1.0 - af));
                if (u_rt_dbg != 0) dbg = vec3(1.0, 0.0, 1.0);
            }
            if (u_rt_dbg == 1 || (u_rt_dbg == 2 && v_uv.x > 0.5)) o_refl = vec4(dbg * 0.6, 0.6);
            racc += o_refl;
            }
            o_refl = racc / float(u_rt_refl_rays);
        } else
)GLSL";
const char* const k_rt_code = R"GLSL(
            if (u_use_rt != 0) {
                vec3 Ow = (u_view_inv * vec4(O, 1.0)).xyz;
                vec3 lw = u_light_w;
                vec3 lu = normalize(abs(lw.y) < 0.99 ? cross(lw, vec3(0.0, 1.0, 0.0)) : cross(lw, vec3(1.0, 0.0, 0.0))), lv = cross(lw, lu);
                float blocked = 0.0;
                for (int r = 0; r < u_rt_rays; ++r) {
                    vec3 ld = lw;
                    if (u_rt_rays > 1) {
                        float a = (float(r) + jit) / float(u_rt_rays) * 6.2831853 + jit * 6.2831853, rad = sqrt(fract(jit * 7.31 + float(r) * 0.618034)) * u_rt_soft;
                        ld = normalize(lw + lu * (cos(a) * rad) + lv * (sin(a) * rad));
                    }
                    rayQueryEXT rq;
                    rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0x1, Ow, 0.0, ld, u_rt_len);
                    while (rayQueryProceedEXT(rq)) {}
                    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) blocked += 1.0;
                }
                if (blocked > 0.0) sh = facing * fd * blocked / float(u_rt_rays);
            } else
)GLSL";
static int env_int_dbg() { const char* e = std::getenv("PSPRECOMP_RT_REFL_DEBUG"); return e ? (std::atoi(e) == 2 ? 2 : 1) : 0; }
std::string rt_main_source() {
    std::string s = k_fs_main;
    auto rep = [&](const char* marker, const char* with) { size_t i = s.find(marker); if (i != std::string::npos) s.replace(i, std::strlen(marker), with); };
    rep("//@RT_DECL", k_rt_decl);
    rep("//@RT_CODE", k_rt_code);
    rep("//@RT_REFL", k_rt_refl);
    return s;
}
GLuint g_vao = 0, g_smp_nn = 0, g_smp_lin = 0;

// Render targets, sized to the scene buffer (depth) and half of it (everything else).
int g_w = 0, g_h = 0, g_hw = 0, g_hh = 0;
GLuint g_depth_tex = 0, g_depth_fbo = 0;
GLuint g_color_tex = 0, g_color_fbo = 0;                     // half-res copy of the scene
GLuint g_fx_tex[2] = {0, 0}, g_refl_tex = 0;
GLuint g_main_fbo = 0, g_fx_fbo[2] = {0, 0};                 // main: fx[0] + refl; fx_fbo[i]: fx[i] alone

GLuint compile(GLenum type, const char* const* body, int n) {
    const char* src[6];
    int k = 0;
    src[k++] = PSP_GLSL_VERSION;
    for (int i = 0; i < n; ++i) src[k++] = body[i];
    GLuint s = glCreateShader(type);
    glShaderSource(s, k, src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "[SCREENFX] shader failed:\n%s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool build(Prog& p, const char* fs_body) {
    if (p.id) return true;
    if (p.failed) return false;
    const char* vsrc[1] = {k_vs};
    const char* fsrc[2] = {k_common, fs_body};
    GLuint vs = compile(GL_VERTEX_SHADER, vsrc, 1), fs = vs ? compile(GL_FRAGMENT_SHADER, fsrc, 2) : 0;
    if (!vs || !fs) { p.failed = true; if (vs) glDeleteShader(vs); return false; }
    GLuint id = glCreateProgram();
    glAttachShader(id, vs);
    glAttachShader(id, fs);
    glLinkProgram(id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(id, sizeof log, nullptr, log);
        std::fprintf(stderr, "[SCREENFX] link failed:\n%s\n", log);
        glDeleteProgram(id);
        p.failed = true;
        return false;
    }
    p.id = id;
    return true;
}

GLuint make_tex(GLenum ifmt, GLenum fmt, GLenum type, int w, int h, GLint filter) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void free_targets() {
    GLuint f[5] = {g_depth_fbo, g_color_fbo, g_main_fbo, g_fx_fbo[0], g_fx_fbo[1]};
    GLuint t[5] = {g_depth_tex, g_color_tex, g_fx_tex[0], g_fx_tex[1], g_refl_tex};
    for (int i = 0; i < 5; ++i) {
        if (f[i]) glDeleteFramebuffers(1, &f[i]);
        if (t[i]) glDeleteTextures(1, &t[i]);
    }
    g_depth_fbo = g_color_fbo = g_main_fbo = g_fx_fbo[0] = g_fx_fbo[1] = 0;
    g_depth_tex = g_color_tex = g_fx_tex[0] = g_fx_tex[1] = g_refl_tex = 0;
}

bool complete() { return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE; }

bool ensure_targets(int w, int h) {
    if (g_w == w && g_h == h && g_depth_fbo) return true;
    free_targets();
    g_w = w; g_h = h; g_hw = (w + 1) / 2; g_hh = (h + 1) / 2;
    g_depth_tex = make_tex(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, w, h, GL_NEAREST);
    glGenFramebuffers(1, &g_depth_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_depth_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_depth_tex, 0);
    bool ok = complete();
    g_color_tex = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, g_hw, g_hh, GL_LINEAR);
    glGenFramebuffers(1, &g_color_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_color_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color_tex, 0);
    ok = ok && complete();
    for (int i = 0; i < 2; ++i) {
        g_fx_tex[i] = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, g_hw, g_hh, GL_LINEAR);
        glGenFramebuffers(1, &g_fx_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, g_fx_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fx_tex[i], 0);
        ok = ok && complete();
    }
    g_refl_tex = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, g_hw, g_hh, GL_LINEAR);
    glGenFramebuffers(1, &g_main_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_main_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fx_tex[0], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_refl_tex, 0);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    ok = ok && complete();
    if (!ok) std::fprintf(stderr, "[SCREENFX] framebuffer incomplete (%dx%d)\n", w, h);
    return ok;
}

// column-major 4x4 inverse (general)
bool invert4(const float* m, float* out) {
    double a[16];
    for (int i = 0; i < 16; ++i) a[i] = m[i];
    double inv[16];
    inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
    double det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    if (std::fabs(det) < 1e-30) return false;
    det = 1.0 / det;
    for (int i = 0; i < 16; ++i) out[i] = float(inv[i] * det);
    return true;
}

void normalize3(float* v) {
    float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

// view matrix (PSP 4x3, columns of 3) applied to a world direction
void view_dir(const float* vm, const float* d, float* out) {
    for (int r = 0; r < 3; ++r) out[r] = vm[r] * d[0] + vm[3 + r] * d[1] + vm[6 + r] * d[2];
    normalize3(out);
}

struct Common {
    GLint depth, size, step, proj, inv, dr, clearz, sky, fade;
};
Common common_locs(GLuint p) {
    Common c;
    c.depth = glGetUniformLocation(p, "u_depth");
    c.size = glGetUniformLocation(p, "u_size");
    c.step = glGetUniformLocation(p, "u_step");
    c.proj = glGetUniformLocation(p, "u_proj");
    c.inv = glGetUniformLocation(p, "u_inv");
    c.dr = glGetUniformLocation(p, "u_dr");
    c.clearz = glGetUniformLocation(p, "u_clearz");
    c.sky = glGetUniformLocation(p, "u_sky");
    c.fade = glGetUniformLocation(p, "u_fade");
    return c;
}

#ifndef PSP_GLES
// per-pass GPU timestamps (PSPRECOMP_FX_DEBUG): marks 0 start, 1 copies, 2 main pass, 3 blur, 4 AO/shadow composite, 5 reflection composite
GLuint g_ts[6] = {0, 0, 0, 0, 0, 0};
bool g_ts_busy = false;
double g_ts_sum[5] = {0, 0, 0, 0, 0};
int g_ts_n = 0;
void ts_begin() {
    static const bool on = std::getenv("PSPRECOMP_FX_DEBUG") != nullptr;
    if (!on) return;
    if (!g_ts[0]) glGenQueries(6, g_ts);
    if (g_ts_busy) {
        GLuint ok = 0;
        glGetQueryObjectuiv(g_ts[5], GL_QUERY_RESULT_AVAILABLE, &ok);
        if (ok) {
            GLuint64 t[6];
            for (int i = 0; i < 6; ++i) glGetQueryObjectui64v(g_ts[i], GL_QUERY_RESULT, &t[i]);
            for (int i = 0; i < 5; ++i) g_ts_sum[i] += double(t[i + 1] - t[i]) * 1e-6;
            if (++g_ts_n == 120) {
                std::fprintf(stderr, "[SCREENFX] passes ms: copies %.2f main %.2f blur %.2f mul %.2f refl %.2f\n", g_ts_sum[0] / 120, g_ts_sum[1] / 120,
                             g_ts_sum[2] / 120, g_ts_sum[3] / 120, g_ts_sum[4] / 120);
                for (double& v : g_ts_sum) v = 0;
                g_ts_n = 0;
            }
            g_ts_busy = false;
        }
    }
    if (!g_ts_busy) { glQueryCounter(g_ts[0], GL_TIMESTAMP); g_ts_busy = true; }
}
void ts_mark(int i) {
    static const bool on = std::getenv("PSPRECOMP_FX_DEBUG") != nullptr;
    if (on && g_ts_busy && g_ts[0] && i > 0) glQueryCounter(g_ts[i], GL_TIMESTAMP);
}
#else
inline void ts_begin() {}   // GLES has no timestamp queries here
inline void ts_mark(int) {}
#endif

}  // namespace

int psp_fx_ssao() {
    int v = g_ao.load();
    if (v < 0) { v = env_level("PSPRECOMP_SSAO"); g_ao.store(v); }
    return v;
}
void psp_fx_set_ssao(int level) { g_ao.store(level < 0 ? 0 : level > 3 ? 3 : level); }
int psp_fx_contact() {
    int v = g_cs.load();
    if (v < 0) { v = env_level("PSPRECOMP_CONTACT_SHADOWS"); g_cs.store(v); }
    return v;
}
void psp_fx_set_contact(int level) { g_cs.store(level < 0 ? 0 : level > 3 ? 3 : level); }
int psp_fx_ssr() {
    int v = g_sr.load();
    if (v < 0) { v = env_level("PSPRECOMP_SSR"); g_sr.store(v); }
    return v;
}
void psp_fx_set_ssr(int level) { g_sr.store(level < 0 ? 0 : level > 3 ? 3 : level); }

bool psp_screenfx_active() { return psp_fx_ssao() || psp_fx_contact() || psp_fx_ssr() || psp_rt_active() || psp_rt_refl_active(); }

void psp_screenfx_run(GLuint fbo, GLuint color_tex, int w, int h, const ScreenFxScene& sc) {
    (void)color_tex;
#ifndef PSP_GLES
    ts_begin();
#endif
    if (!build(g_main_p, k_fs_main) || !build(g_blur_p, k_fs_blur) || !build(g_mul_p, k_fs_mul) || !build(g_refl_p, k_fs_refl)) return;
    if (!ensure_targets(w, h)) return;
    if (!g_vao) {
        glGenVertexArrays(1, &g_vao);
        glGenSamplers(1, &g_smp_nn);
        glGenSamplers(1, &g_smp_lin);
        for (int i = 0; i < 2; ++i) {
            GLuint s = i ? g_smp_lin : g_smp_nn;
            GLint f = i ? GL_LINEAR : GL_NEAREST;
            glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, f);
            glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, f);
            glSamplerParameteri(s, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(s, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    static const bool dbg_print = std::getenv("PSPRECOMP_FX_DEBUG") != nullptr;
    float inv[16];
    if (!invert4(sc.proj, inv)) return;
    const bool rt_refl_on = psp_rt_refl_active();
    const int ao = psp_fx_ssao(), sr = rt_refl_on ? 3 : psp_fx_ssr();   // ray-traced reflections replace the screen-space ones (full strength)
    const int cs = psp_rt_active() ? 3 : psp_fx_contact();   // ray-traced shadows replace the contact shadows (full contact-shadow strength; a ray query has no sample count)
    static const float k_ao_amt[4] = {0.f, 0.55f, 0.8f, 1.0f};
    static const float k_cs_amt[4] = {0.f, 0.3f, 0.45f, 0.6f};
    static const float k_sr_amt[4] = {0.f, 0.45f, 0.7f, 1.0f};
    // samples per effect and level: the cost of the pass is mostly these loops
    static const int k_n_ao[4] = {0, 5, 8, 12};
    static const int k_n_cs[4] = {0, 6, 10, 14};
    static const int k_n_ssr[4] = {0, 10, 16, 22};
    const float radius = env_float("PSPRECOMP_SSAO_RADIUS", 0.09f);
    const float cs_len = env_float("PSPRECOMP_CONTACT_LEN", 2.5f);       // view units (a fighter is about 2.4 tall)
    const float cs_thick = env_float("PSPRECOMP_CONTACT_THICK", 0.5f);
    // The sky dome is real geometry far away (about 250 units here, the farthest floor and ruins about 80): everything
    // past PSPRECOMP_FX_SKY_DIST view units counts as background. The depth_t that distance has under this projection:
    const float fade0 = env_float("PSPRECOMP_FX_FADE_START", 30.f), fade1 = env_float("PSPRECOMP_FX_FADE_END", 70.f);
    float sky_t = 0.97f;
    {
        const float D = env_float("PSPRECOMP_FX_SKY_DIST", 150.f), zv = -D;
        const float den = sc.proj[11] * zv;
        if (std::fabs(den) > 1e-6f) {
            const float ndc = (sc.proj[10] * zv + sc.proj[14]) / den;
            sky_t = ndc * 0.5f + 0.5f;
        }
    }
    const float ssr_len = env_float("PSPRECOMP_SSR_LEN", 0.6f);
    // Light: mostly overhead (world space, so the shadows stay put while the camera moves), a little to one side.
    float wl[3] = {env_float("PSPRECOMP_LIGHT_X", 0.4f), env_float("PSPRECOMP_LIGHT_Y", 1.0f), env_float("PSPRECOMP_LIGHT_Z", 0.3f)};
    float lv[3], up[3], wup[3] = {0.f, 1.f, 0.f};
    view_dir(sc.view, wl, lv);
    float wl_n[3] = {wl[0], wl[1], wl[2]};
    { const float l = std::sqrt(wl[0] * wl[0] + wl[1] * wl[1] + wl[2] * wl[2]); if (l > 1e-6f) for (float& c : wl_n) c /= l; }
    bool use_rt = false;
    const bool use_rt_sh = sc.rt_ready && cs > 0 && psp_rt_enabled(), use_rt_refl = sc.rt_ready && rt_refl_on;
    if (use_rt_sh || use_rt_refl) {
        if (!g_main_rt_p.id && !g_main_rt_p.failed) { static const std::string src = rt_main_source(); build(g_main_rt_p, src.c_str()); }
        use_rt = g_main_rt_p.id != 0;
    }
    view_dir(sc.view, wup, up);
    if (dbg_print) {
        static int n = 0;
        if (n++ % 120 == 0) {
            std::fprintf(stderr, "[SCREENFX] %dx%d near/far %.4f %.4f clear %.4f\n  P:", w, h, sc.near_z, sc.far_z, sc.clear_z);
            for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %g", sc.proj[i]);
            std::fprintf(stderr, "\n  V:");
            for (int i = 0; i < 12; ++i) std::fprintf(stderr, " %g", sc.view[i]);
            std::fprintf(stderr, "\n  light(view) %g %g %g up(view) %g %g %g\n", lv[0], lv[1], lv[2], up[0], up[1], up[2]);
        }
    }

    // copies of the scene's depth (full resolution) and colour (half resolution, only the reflections read it)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_depth_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    if (sr) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_color_fbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, g_hw, g_hh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    ts_mark(1);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(g_vao);

    auto bind_tex = [&](int unit, GLuint tex, GLuint smp) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindSampler(unit, smp);
    };
    auto set_common = [&](GLuint prog, int pw, int ph) {
        const Common c = common_locs(prog);
        glUseProgram(prog);
        bind_tex(0, g_depth_tex, g_smp_nn);
        glUniform1i(c.depth, 0);
        glUniform2f(c.size, float(w), float(h));
        glUniform2f(c.step, 1.f / float(pw), 1.f / float(ph));
        glUniformMatrix4fv(c.proj, 1, GL_FALSE, sc.proj);
        glUniformMatrix4fv(c.inv, 1, GL_FALSE, inv);
        glUniform2f(c.dr, sc.near_z, sc.far_z);
        glUniform1f(c.clearz, sc.clear_z);
        glUniform1f(c.sky, sky_t);
        glUniform2f(c.fade, fade0, fade1);
    };
    auto loc = [](GLuint p, const char* n) { return glGetUniformLocation(p, n); };

    // 1: AO + contact shadow + reflection (half resolution)
    Prog& mp = use_rt ? g_main_rt_p : g_main_p;
    glBindFramebuffer(GL_FRAMEBUFFER, g_main_fbo);
    glViewport(0, 0, g_hw, g_hh);
    set_common(mp.id, g_hw, g_hh);
    bind_tex(1, g_color_tex, g_smp_lin);
    glUniform1i(loc(mp.id, "u_color"), 1);
    glUniform1f(loc(mp.id, "u_radius"), radius);
    glUniform1i(loc(mp.id, "u_n_ao"), k_n_ao[ao]);
    glUniform1i(loc(mp.id, "u_n_cs"), k_n_cs[cs]);
    glUniform1i(loc(mp.id, "u_n_ssr"), k_n_ssr[sr]);
    glUniform3f(loc(mp.id, "u_light"), lv[0], lv[1], lv[2]);
    glUniform3f(loc(mp.id, "u_up"), up[0], up[1], up[2]);
    glUniform1f(loc(mp.id, "u_cs_len"), cs_len);
    glUniform1f(loc(mp.id, "u_cs_thick"), cs_thick);
    glUniform1f(loc(mp.id, "u_ssr_len"), ssr_len);
    glUniform1f(loc(mp.id, "u_ssr_amt"), k_sr_amt[sr]);
    static const bool fxl_off = std::getenv("PSPRECOMP_SSR_FX_OFF") != nullptr;
    const bool have_fx = sr && sc.fx_color && sc.fx_depth && !use_rt_refl && !fxl_off;
    glUniform1i(loc(mp.id, "u_have_fx"), have_fx ? 1 : 0);
    glUniform1i(loc(mp.id, "u_fx_dbg"), std::getenv("PSPRECOMP_SSR_FX_DEBUG") ? std::atoi(std::getenv("PSPRECOMP_SSR_FX_DEBUG")) : 0);
    if (have_fx) {
        bind_tex(2, sc.fx_color, g_smp_lin);
        bind_tex(3, sc.fx_depth, g_smp_nn);
        glUniform1i(loc(mp.id, "u_fxc"), 2);
        glUniform1i(loc(mp.id, "u_fxd"), 3);
        glUniform1f(loc(mp.id, "u_fx_gain"), env_float("PSPRECOMP_SSR_FX_GAIN", 0.4f));
        glUniform1f(loc(mp.id, "u_fx_thick"), env_float("PSPRECOMP_SSR_FX_THICK", 0.6f));
    }
    if (use_rt) {
        glUniform1i(loc(mp.id, "u_use_rt"), use_rt_sh ? 1 : 0);
        glUniform1i(loc(mp.id, "u_use_rt_refl"), use_rt_refl ? 1 : 0);
        glUniform1i(loc(mp.id, "u_rt_dbg"), env_int_dbg());
        { const int q = psp_rt_refl_quality(); glUniform1i(loc(mp.id, "u_rt_refl_rays"), q >= 3 ? 3 : 1); }
        glUniform1f(loc(mp.id, "u_rt_rough"), env_float("PSPRECOMP_RT_ROUGH", 0.03f));
        glUniform1i(loc(mp.id, "u_rt_fx"), !sc.rt_fx || psp_rt_refl_quality() < 2 || std::getenv("PSPRECOMP_RT_FX_OFF") ? 0 : std::getenv("PSPRECOMP_RT_FX_SPLIT") ? 2 : 1);
        glUniform1f(loc(mp.id, "u_rt_hidden"), env_float("PSPRECOMP_RT_HIDDEN", 0.45f));
        glUniform1f(loc(mp.id, "u_rt_sky_amt"), env_float("PSPRECOMP_RT_SKY_AMT", 0.4f));
        glUniform1f(loc(mp.id, "u_rt_sky_dist"), env_float("PSPRECOMP_FX_SKY_DIST", 150.f));
        glUniform1i(loc(mp.id, "u_refl_split"), std::getenv("PSPRECOMP_RT_REFL_SPLIT") ? 1 : 0);
        glUniform1f(loc(mp.id, "u_rt_refl_len"), env_float("PSPRECOMP_RT_REFL_LEN", 300.f));
        glUniform3f(loc(mp.id, "u_rt_sky"), env_float("PSPRECOMP_RT_SKY_R", 0.45f), env_float("PSPRECOMP_RT_SKY_G", 0.55f), env_float("PSPRECOMP_RT_SKY_B", 0.75f));
        float vm[16] = {sc.view[0], sc.view[1], sc.view[2], 0.f, sc.view[3], sc.view[4], sc.view[5], 0.f,
                        sc.view[6], sc.view[7], sc.view[8], 0.f, sc.view[9], sc.view[10], sc.view[11], 1.f}, vinv[16];
        if (!invert4(vm, vinv)) std::memset(vinv, 0, sizeof vinv);
        glUniformMatrix4fv(loc(mp.id, "u_view_inv"), 1, GL_FALSE, vinv);
        glUniform3f(loc(mp.id, "u_light_w"), wl_n[0], wl_n[1], wl_n[2]);
        glUniform1f(loc(mp.id, "u_rt_len"), env_float("PSPRECOMP_RT_LEN", 40.f));
        { const int q = psp_rt_quality(); glUniform1i(loc(mp.id, "u_rt_rays"), q >= 3 ? 6 : q == 2 ? 3 : 1); }
        glUniform1f(loc(mp.id, "u_rt_soft"), env_float("PSPRECOMP_RT_SOFT", 0.04f));
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ts_mark(2);

    // 2: blur AO / shadow there and back
    if (ao || cs) {
        for (int pass = 0; pass < 2; ++pass) {
            glBindFramebuffer(GL_FRAMEBUFFER, g_fx_fbo[1 - pass]);
            set_common(g_blur_p.id, g_hw, g_hh);
            bind_tex(1, g_fx_tex[pass], g_smp_lin);
            glUniform1i(loc(g_blur_p.id, "u_fx"), 1);
            glUniform2f(loc(g_blur_p.id, "u_dir"), pass ? 0.f : 1.f / g_hw, pass ? 1.f / g_hh : 0.f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    ts_mark(3);
    // 3: composite into the scene (RGB only: alpha is the GE's stencil)
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    const float dark = psp_rt_active() ? stage_dark() : 1.f;   // stage looks ride on the ray-traced lighting
    if (ao || cs || dark < 0.999f) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);              // dst * (AO * (1 - shadow))
        set_common(g_mul_p.id, w, h);
        bind_tex(1, g_fx_tex[0], g_smp_nn);
        glUniform1i(loc(g_mul_p.id, "u_fx"), 1);
        glUniform2f(loc(g_mul_p.id, "u_half"), float(g_hw), float(g_hh));
        glUniform1f(loc(g_mul_p.id, "u_ao_amt"), k_ao_amt[ao]);
        glUniform1f(loc(g_mul_p.id, "u_dark"), dark);
        glUniform1f(loc(g_mul_p.id, "u_cs_amt"), (std::getenv("PSPRECOMP_CONTACT_AMT") ? env_float("PSPRECOMP_CONTACT_AMT", 0.6f) : k_cs_amt[cs]));
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    ts_mark(4);
    if (sr) {
        glEnable(GL_BLEND);
        static const bool refl_only = std::getenv("PSPRECOMP_REFL_ONLY") != nullptr;
        glBlendFunc(refl_only ? GL_ONE : GL_ONE, refl_only ? GL_ZERO : GL_ONE_MINUS_SRC_ALPHA);     // the reflection over the (shadowed) floor
        set_common(g_refl_p.id, w, h);
        bind_tex(1, g_refl_tex, g_smp_lin);
        glUniform1i(loc(g_refl_p.id, "u_refl"), 1);
        glUniform1f(loc(g_refl_p.id, "u_dbg"), refl_only ? 1.f : 0.f);
        glUniform1f(loc(g_refl_p.id, "u_dark"), dark);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    ts_mark(5);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(0);
    for (int u = 3; u >= 0; --u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindSampler(u, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glUseProgram(0);
}
