// Screen-space scene effects (SSAO, contact shadows, SSR), see include/psp_screenfx.h.
//
// The scene's depth buffer holds the PSP's window depth: ndc z mapped through glDepthRange(near_z, far_z). A pixel's view
// space position is rebuilt with the inverse of the PSP projection matrix, so the passes do not need to know the game's
// near / far planes or field of view. Passes: (1) AO + contact shadow into an RG texture, (2) a depth-aware blur of it
// in two directions, (3) the composite back into the scene buffer (RGB only: the buffer's alpha is the GE stencil).
#include "psp_screenfx.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
out vec4 o_color;
uniform sampler2D u_depth;
uniform vec2 u_size;
uniform mat4 u_proj;
uniform mat4 u_inv;
uniform vec2 u_dr;          // glDepthRange near, far
uniform float u_clearz;     // the cleared (background) depth
uniform float u_radius;     // AO radius as a fraction of the distance to the camera
uniform int u_dbg;
uniform vec2 u_fade;        // view distance where the effects start to fade out / are gone (the 16-bit depth is too coarse far away)
uniform float u_sky;        // depth_t above which a pixel is the sky dome / background

float wd_at(vec2 uv) { return texture(u_depth, uv).r; }
float depth_t(float wd) { return (wd - u_dr.x) / (u_dr.y - u_dr.x); }   // 0 at the near plane, 1 at the far plane
bool is_far(float wd) { return abs(wd - u_clearz) < 3.0 / 65535.0 || depth_t(wd) > u_sky; }
vec3 vpos(vec2 uv, float wd) {
    float z = (wd - u_dr.x) / (u_dr.y - u_dr.x) * 2.0 - 1.0;
    vec4 p = u_inv * vec4(uv * 2.0 - 1.0, z, 1.0);
    return p.xyz / p.w;
}
vec3 vpos(vec2 uv) { return vpos(uv, wd_at(uv)); }
// Surface normal from the depth neighbours (the side with the smaller jump on each axis), facing the camera.
vec3 normal_at(vec2 uv, vec3 P) {
    vec2 px = 1.0 / u_size;
    vec3 a = vpos(uv + vec2(px.x, 0.0)), b = vpos(uv - vec2(px.x, 0.0));
    vec3 c = vpos(uv + vec2(0.0, px.y)), d = vpos(uv - vec2(0.0, px.y));
    vec3 dx = abs(length(a) - length(P)) < abs(length(b) - length(P)) ? a - P : P - b;
    vec3 dy = abs(length(c) - length(P)) < abs(length(d) - length(P)) ? c - P : P - d;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, P) > 0.0) n = -n;      // the camera is at the origin
    return n;
}
float dist_fade(float lp) { return 1.0 - smoothstep(u_fade.x, u_fade.y, lp); }
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
)GLSL";

const char* const k_fs_ao = R"GLSL(
uniform int u_do_ao;
uniform int u_do_cs;
uniform vec3 u_light;       // view space, towards the light
uniform float u_cs_len;     // contact shadow ray length as a fraction of the distance to the camera
void main() {
    float wd = wd_at(v_uv);
    if (is_far(wd)) { o_color = vec4(1.0, 0.0, 0.0, 1.0); return; }
    vec3 P = vpos(v_uv, wd);
    float lp = length(P);
    vec3 n = normal_at(v_uv, P);
    float noise = ign(gl_FragCoord.xy);
    float ao = 1.0;
    if (u_do_ao != 0) {
        const int N = 14;
        float rad = u_radius * lp;
        vec3 up = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 tx = normalize(cross(up, n)), ty = cross(n, tx);
        float occ = 0.0;
        for (int i = 0; i < N; ++i) {
            float t = (float(i) + 0.5) / float(N);
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
            float diff = ds - dq;
            if (diff > 0.02 * rad) occ += smoothstep(0.0, 1.0, rad / max(abs(lp - dq), 1e-4));
        }
        ao = 1.0 - occ / float(N);
    }
    float sh = 0.0;
    if (u_do_cs != 0) {
        const int M = 24;
        float maxd = u_cs_len * lp;
        float facing = smoothstep(0.15, 0.5, dot(n, u_light));   // surfaces turned away from the light are dark already
        float jit = ign(gl_FragCoord.xy + 17.0);
        vec3 O = P + n * (0.004 * lp);
        for (int j = 0; j < M; ++j) {
            float t = (float(j) + jit) / float(M);
            vec3 S = O + u_light * (t * maxd);
            vec4 cl = u_proj * vec4(S, 1.0);
            if (cl.w <= 1e-5) break;
            vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
            if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) break;
            float wd2 = wd_at(uv2);
            if (is_far(wd2)) continue;
            float diff = length(S) - length(vpos(uv2, wd2));
            if (diff > 0.004 * lp && diff < 0.5 * maxd) { sh = facing; break; }
        }
    }
    float fd = dist_fade(lp);
    o_color = vec4(mix(1.0, ao, fd), sh * fd, 0.0, 1.0);
}
)GLSL";

const char* const k_fs_blur = R"GLSL(
uniform sampler2D u_fx;
uniform vec2 u_dir;         // one texel step
void main() {
    float wd = wd_at(v_uv);
    if (is_far(wd)) { o_color = vec4(1.0, 0.0, 0.0, 1.0); return; }
    float lp = length(vpos(v_uv, wd));
    vec2 sum = vec2(0.0);
    float wsum = 0.0;
    for (int i = -4; i <= 4; ++i) {
        vec2 uv = v_uv + u_dir * float(i);
        float w2 = wd_at(uv);
        if (is_far(w2)) continue;
        float dl = abs(length(vpos(uv, w2)) - lp) / lp;
        float w = exp(-float(i * i) / 10.0) * exp(-dl * 60.0);
        sum += texture(u_fx, uv).rg * w;
        wsum += w;
    }
    o_color = vec4(wsum > 0.0 ? sum / wsum : vec2(1.0, 0.0), 0.0, 1.0);
}
)GLSL";

const char* const k_fs_comp = R"GLSL(
uniform sampler2D u_color;
uniform sampler2D u_fx;
uniform float u_ao_amt;
uniform float u_cs_amt;
uniform float u_ssr_amt;
uniform vec3 u_up;          // world up in view space
uniform float u_ssr_len;
void main() {
    vec4 col = texture(u_color, v_uv);
    float wd = wd_at(v_uv);
    if (is_far(wd)) { o_color = (u_dbg >= 6) ? vec4(vec3(depth_t(wd)), 1.0) : col; return; }
    vec2 f = texture(u_fx, v_uv).rg;
    vec3 rgb = col.rgb;
    float dbg_refl = 0.0;
    if (u_ssr_amt > 0.0) {
        vec3 P = vpos(v_uv, wd);
        float lp = length(P);
        vec3 n = normal_at(v_uv, P);
        float upness = dot(n, u_up);
        if (upness > 0.7) {
            vec3 V = normalize(P);
            vec3 R = reflect(V, n);
            float maxd = u_ssr_len * lp;
            const int STEPS = 40;
            float stepl = maxd / float(STEPS);
            float jit = ign(gl_FragCoord.xy);
            vec3 O = P + n * (0.004 * lp);
            vec2 hit = vec2(-1.0);
            float hit_t = 0.0;
            float prev_t = 0.0;
            vec2 last_in = vec2(-1.0);
            for (int i = 0; i < STEPS; ++i) {
                float t = (float(i) + jit) * stepl;
                vec3 S = O + R * t;
                vec4 cl = u_proj * vec4(S, 1.0);
                if (cl.w <= 1e-5) break;
                vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
                if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) break;
                last_in = uv2;
                float wd2 = wd_at(uv2);
                if (!is_far(wd2)) {
                    float diff = length(S) - length(vpos(uv2, wd2));
                    if (diff > 0.0 && diff < stepl * 2.5 + 0.01 * lp) {
                        // refine between the previous and this step
                        float lo = prev_t, hi = t;
                        for (int k = 0; k < 6; ++k) {
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
            if (hit.x < 0.0 && last_in.x >= 0.0 && is_far(wd_at(last_in))) { hit = last_in; hit_t = maxd; }   // the ray left the scene into the sky
            if (hit.x >= 0.0) {
                vec2 e = min(hit, 1.0 - hit);
                float edge = smoothstep(0.0, 0.12, min(e.x, e.y));
                float fres = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, -V), 0.0, 1.0), 5.0);
                float k = clamp(fres * 1.6 + 0.08, 0.0, 1.0) * edge * (1.0 - hit_t / maxd * 0.6) * smoothstep(0.7, 0.95, upness) * dist_fade(lp) * u_ssr_amt;
                vec3 refl = texture(u_color, hit).rgb;
                rgb = mix(rgb, refl, k);
                dbg_refl = k;
            }
        }
    }
    float ao = mix(1.0, f.r, u_ao_amt);
    float sh = f.g * u_cs_amt;
    rgb *= ao * (1.0 - sh);
    if (u_dbg == 1) rgb = vec3(f.r);
    else if (u_dbg == 2) rgb = vec3(1.0 - f.g);
    else if (u_dbg == 3) rgb = vec3(dbg_refl);
    else if (u_dbg == 4) rgb = normal_at(v_uv, vpos(v_uv, wd)) * 0.5 + 0.5;
    else if (u_dbg == 5) rgb = vec3(fract(length(vpos(v_uv, wd)) * 0.1));
    else if (u_dbg == 6) rgb = vec3(depth_t(wd));
    else if (u_dbg == 8) rgb = vec3(log2(max(length(vpos(v_uv, wd)), 1e-3)) / 16.0 + 0.25);
    else if (u_dbg == 7) rgb = vec3(fract(depth_t(wd) * 50.0));
    o_color = vec4(rgb, col.a);
}
)GLSL";

struct Prog {
    GLuint id = 0;
    bool failed = false;
};
Prog g_ao_p, g_blur_p, g_comp_p;
GLuint g_vao = 0, g_smp_nn = 0, g_smp_lin = 0;

// Render targets, sized to the scene buffer.
int g_w = 0, g_h = 0;
GLuint g_depth_tex = 0, g_depth_fbo = 0;
GLuint g_color_tex = 0, g_color_fbo = 0;
GLuint g_fx_tex[2] = {0, 0}, g_fx_fbo[2] = {0, 0};

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
    GLuint f[4] = {g_depth_fbo, g_color_fbo, g_fx_fbo[0], g_fx_fbo[1]};
    GLuint t[4] = {g_depth_tex, g_color_tex, g_fx_tex[0], g_fx_tex[1]};
    for (int i = 0; i < 4; ++i) {
        if (f[i]) glDeleteFramebuffers(1, &f[i]);
        if (t[i]) glDeleteTextures(1, &t[i]);
    }
    g_depth_fbo = g_color_fbo = g_fx_fbo[0] = g_fx_fbo[1] = 0;
    g_depth_tex = g_color_tex = g_fx_tex[0] = g_fx_tex[1] = 0;
}

bool ensure_targets(int w, int h) {
    if (g_w == w && g_h == h && g_depth_fbo) return true;
    free_targets();
    g_w = w; g_h = h;
    g_depth_tex = make_tex(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, w, h, GL_NEAREST);
    glGenFramebuffers(1, &g_depth_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_depth_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_depth_tex, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    g_color_tex = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h, GL_LINEAR);
    glGenFramebuffers(1, &g_color_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_color_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color_tex, 0);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    for (int i = 0; i < 2; ++i) {
        g_fx_tex[i] = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h, GL_LINEAR);
        glGenFramebuffers(1, &g_fx_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, g_fx_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fx_tex[i], 0);
        ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
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
    GLint depth, size, proj, inv, dr, clearz, radius, dbg, sky, fade;
};
Common common_locs(GLuint p) {
    Common c;
    c.depth = glGetUniformLocation(p, "u_depth");
    c.size = glGetUniformLocation(p, "u_size");
    c.proj = glGetUniformLocation(p, "u_proj");
    c.inv = glGetUniformLocation(p, "u_inv");
    c.dr = glGetUniformLocation(p, "u_dr");
    c.clearz = glGetUniformLocation(p, "u_clearz");
    c.radius = glGetUniformLocation(p, "u_radius");
    c.dbg = glGetUniformLocation(p, "u_dbg");
    c.sky = glGetUniformLocation(p, "u_sky");
    c.fade = glGetUniformLocation(p, "u_fade");
    return c;
}

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

bool psp_screenfx_active() { return psp_fx_ssao() || psp_fx_contact() || psp_fx_ssr(); }

void psp_screenfx_run(GLuint fbo, GLuint color_tex, int w, int h, const ScreenFxScene& sc) {
    (void)color_tex;
    if (!build(g_ao_p, k_fs_ao) || !build(g_blur_p, k_fs_blur) || !build(g_comp_p, k_fs_comp)) return;
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
    static const int dbg_mode = std::getenv("PSPRECOMP_FX_VIEW") ? std::atoi(std::getenv("PSPRECOMP_FX_VIEW")) : 0;   // 1 AO, 2 shadow, 3 reflection, 4 normals, 5 distance rings
    float inv[16];
    if (!invert4(sc.proj, inv)) return;
    const int ao = psp_fx_ssao(), cs = psp_fx_contact(), sr = psp_fx_ssr();
    static const float k_ao_amt[4] = {0.f, 0.55f, 0.8f, 1.0f};
    static const float k_cs_amt[4] = {0.f, 0.25f, 0.4f, 0.55f};
    static const float k_sr_amt[4] = {0.f, 0.45f, 0.7f, 1.0f};
    const float radius = env_float("PSPRECOMP_SSAO_RADIUS", 0.09f);
    const float cs_len = env_float("PSPRECOMP_CONTACT_LEN", 0.14f);
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
    static const float k_world_light[3] = {0.35f, 0.85f, 0.4f};   // towards the light, world space (from above, a little toward +x/+z)
    float wl[3] = {env_float("PSPRECOMP_LIGHT_X", k_world_light[0]), env_float("PSPRECOMP_LIGHT_Y", k_world_light[1]),
                   env_float("PSPRECOMP_LIGHT_Z", k_world_light[2])};
    float lv[3], up[3], wup[3] = {0.f, 1.f, 0.f};
    view_dir(sc.view, wl, lv);
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

    // copies of the scene's depth and colour
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_depth_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_color_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, w, h);
    glBindVertexArray(g_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);

    auto set_common = [&](GLuint prog) {
        const Common c = common_locs(prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_depth_tex);
        glBindSampler(0, g_smp_nn);
        glUniform1i(c.depth, 0);
        glUniform2f(c.size, float(w), float(h));
        glUniformMatrix4fv(c.proj, 1, GL_FALSE, sc.proj);
        glUniformMatrix4fv(c.inv, 1, GL_FALSE, inv);
        glUniform2f(c.dr, sc.near_z, sc.far_z);
        glUniform1f(c.clearz, sc.clear_z);
        glUniform1f(c.radius, radius);
        glUniform1i(c.dbg, dbg_mode);
        glUniform1f(c.sky, sky_t);
        glUniform2f(c.fade, fade0, fade1);
    };

    // 1: AO + contact shadow
    if (ao || cs) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_fx_fbo[0]);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(g_ao_p.id);
        set_common(g_ao_p.id);
        glUniform1i(glGetUniformLocation(g_ao_p.id, "u_do_ao"), ao ? 1 : 0);
        glUniform1i(glGetUniformLocation(g_ao_p.id, "u_do_cs"), cs ? 1 : 0);
        glUniform3f(glGetUniformLocation(g_ao_p.id, "u_light"), lv[0], lv[1], lv[2]);
        glUniform1f(glGetUniformLocation(g_ao_p.id, "u_cs_len"), cs_len);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        // 2: blur there and back
        glUseProgram(g_blur_p.id);
        for (int pass = 0; pass < 2; ++pass) {
            glBindFramebuffer(GL_FRAMEBUFFER, g_fx_fbo[1 - pass]);
            set_common(g_blur_p.id);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g_fx_tex[pass]);
            glBindSampler(1, g_smp_lin);
            glUniform1i(glGetUniformLocation(g_blur_p.id, "u_fx"), 1);
            glUniform2f(glGetUniformLocation(g_blur_p.id, "u_dir"), pass ? 0.f : 1.f / w, pass ? 1.f / h : 0.f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    // 3: composite into the scene (RGB only: alpha is the GE's stencil)
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glUseProgram(g_comp_p.id);
    set_common(g_comp_p.id);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_fx_tex[0]);
    glBindSampler(1, g_smp_lin);
    glUniform1i(glGetUniformLocation(g_comp_p.id, "u_fx"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_color_tex);
    glBindSampler(2, g_smp_lin);
    glUniform1i(glGetUniformLocation(g_comp_p.id, "u_color"), 2);
    glUniform1f(glGetUniformLocation(g_comp_p.id, "u_ao_amt"), k_ao_amt[ao]);
    glUniform1f(glGetUniformLocation(g_comp_p.id, "u_cs_amt"), k_cs_amt[cs]);
    glUniform1f(glGetUniformLocation(g_comp_p.id, "u_ssr_amt"), k_sr_amt[sr]);
    glUniform3f(glGetUniformLocation(g_comp_p.id, "u_up"), up[0], up[1], up[2]);
    glUniform1f(glGetUniformLocation(g_comp_p.id, "u_ssr_len"), ssr_len);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(0);
    for (int u = 2; u >= 0; --u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindSampler(u, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glUseProgram(0);
}
