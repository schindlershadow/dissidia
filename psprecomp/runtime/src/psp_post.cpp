// Present-time post-processing, see include/psp_post.h.
#include "psp_post.h"
#include "psp_smaa_data.h"

#include <string>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

std::atomic<int> g_aa{-1}, g_sharp{-1}, g_up{-1};

int env_int(const char* name, int dflt) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return dflt;
    return std::atoi(v);
}

const char* const k_vs = R"GLSL(
out vec2 v_uv;
void main() {
    // One oversized triangle covering the viewport.
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

const char* const k_fs = R"GLSL(
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_tex;
uniform vec2 u_size;        // source size in texels
uniform float u_scale;      // output pixels per source texel (horizontal)
uniform int u_mode;         // 0 bilinear, 1 sharp bilinear, 2 bicubic (Catmull-Rom)
uniform int u_aa;           // 1 = FXAA
uniform float u_sharp;      // 0 = off, else contrast-adaptive sharpening amount 0..1

vec3 fetch(vec2 uv) { return texture(u_tex, uv).rgb; }
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec3 bicubic(vec2 uv) {
    vec2 tc = uv * u_size;
    vec2 t1 = floor(tc - 0.5) + 0.5;
    vec2 f = tc - t1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 s12 = w1 + w2;
    vec2 t12 = (t1 + w2 / s12) / u_size;
    vec2 t0 = (t1 - 1.0) / u_size;
    vec2 t3 = (t1 + 2.0) / u_size;
    vec3 c = fetch(vec2(t0.x, t0.y)) * (w0.x * w0.y) + fetch(vec2(t12.x, t0.y)) * (s12.x * w0.y) + fetch(vec2(t3.x, t0.y)) * (w3.x * w0.y)
           + fetch(vec2(t0.x, t12.y)) * (w0.x * s12.y) + fetch(vec2(t12.x, t12.y)) * (s12.x * s12.y) + fetch(vec2(t3.x, t12.y)) * (w3.x * s12.y)
           + fetch(vec2(t0.x, t3.y)) * (w0.x * w3.y) + fetch(vec2(t12.x, t3.y)) * (s12.x * w3.y) + fetch(vec2(t3.x, t3.y)) * (w3.x * w3.y);
    return max(c, vec3(0.0));
}

vec3 center(vec2 uv) {
    if (u_mode == 2) return bicubic(uv);
    if (u_mode == 1 && u_scale > 1.0) {
        // Sharp bilinear: the blend between two texels is squeezed into about one output pixel.
        vec2 t = uv * u_size;
        vec2 fl = floor(t);
        vec2 cd = fract(t) - 0.5;                       // distance from the texel centre
        float rr = 0.5 - 0.5 / u_scale;                 // flat region around the centre
        vec2 f = (cd - clamp(cd, -rr, rr)) * u_scale + 0.5;
        return fetch((fl + f) / u_size);
    }
    return fetch(uv);
}

void main() {
    vec2 inv = 1.0 / u_size;
    vec3 m = center(v_uv);
    if (u_aa != 0) {
        // FXAA (the compact edge-directed blend): taps one texel away along the detected edge.
        vec3 nw = fetch(v_uv + vec2(-1.0, -1.0) * inv), ne = fetch(v_uv + vec2(1.0, -1.0) * inv);
        vec3 sw = fetch(v_uv + vec2(-1.0, 1.0) * inv),  se = fetch(v_uv + vec2(1.0, 1.0) * inv);
        float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lm = luma(m);
        float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
        float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
        vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
        float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);
        float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
        dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * inv;
        vec3 a = 0.5 * (fetch(v_uv + dir * (1.0 / 3.0 - 0.5)) + fetch(v_uv + dir * (2.0 / 3.0 - 0.5)));
        vec3 b = a * 0.5 + 0.25 * (fetch(v_uv + dir * -0.5) + fetch(v_uv + dir * 0.5));
        float lb = luma(b);
        m = (lb < lmin || lb > lmax) ? a : b;
    }
    if (u_sharp > 0.0) {
        // Contrast-adaptive sharpening (after AMD CAS): sharpen less where the neighbourhood is already contrasty.
        vec3 n = fetch(v_uv + vec2(0.0, -1.0) * inv), s = fetch(v_uv + vec2(0.0, 1.0) * inv);
        vec3 w = fetch(v_uv + vec2(-1.0, 0.0) * inv), e = fetch(v_uv + vec2(1.0, 0.0) * inv);
        vec3 mn = min(m, min(min(n, s), min(w, e)));
        vec3 mx = max(m, max(max(n, s), max(w, e)));
        vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
        vec3 wgt = amp * (-1.0 / mix(8.0, 5.0, u_sharp));
        m = clamp((m + wgt * (n + s + w + e)) / (1.0 + 4.0 * wgt), 0.0, 1.0);
    }
    o_color = vec4(m, 1.0);
}
)GLSL";

GLuint g_prog = 0, g_vao = 0, g_sampler_lin = 0, g_sampler_nn = 0;
bool g_failed = false;
GLint u_tex = -1, u_size = -1, u_scale = -1, u_mode = -1, u_aa = -1, u_sharp = -1;

GLuint compile(GLenum type, const char* body) {
    const char* src[2] = {PSP_GLSL_VERSION, body};
    GLuint s = glCreateShader(type);
    glShaderSource(s, 2, src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "[POST] %s shader failed:\n%s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool ensure_program() {
    if (g_prog) return true;
    if (g_failed) return false;
    GLuint vs = compile(GL_VERTEX_SHADER, k_vs), fs = vs ? compile(GL_FRAGMENT_SHADER, k_fs) : 0;
    if (!vs || !fs) { g_failed = true; if (vs) glDeleteShader(vs); return false; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "[POST] link failed:\n%s\n", log);
        glDeleteProgram(p);
        g_failed = true;
        return false;
    }
    g_prog = p;
    u_tex = glGetUniformLocation(p, "u_tex");
    u_size = glGetUniformLocation(p, "u_size");
    u_scale = glGetUniformLocation(p, "u_scale");
    u_mode = glGetUniformLocation(p, "u_mode");
    u_aa = glGetUniformLocation(p, "u_aa");
    u_sharp = glGetUniformLocation(p, "u_sharp");
    glGenVertexArrays(1, &g_vao);
    glGenSamplers(1, &g_sampler_lin);
    glGenSamplers(1, &g_sampler_nn);
    for (int i = 0; i < 2; ++i) {
        const GLuint s = i ? g_sampler_nn : g_sampler_lin;
        const GLint f = i ? GL_NEAREST : GL_LINEAR;
        glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, f);
        glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, f);
        glSamplerParameteri(s, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    std::fprintf(stderr, "[POST] program ready\n");
    return true;
}

// ---- SMAA 1x (Jimenez, Echevarria, Sousa, Gutierrez, Masia; reference shaders embedded by tools/gen_smaa.py) ----------------
// Three passes at the source (render target) resolution, before the upscale / sharpen pass: luma edge detection, blend weight
// calculation (area + search lookup textures), neighbourhood blending. The result replaces the source texture of psp_post_draw.

GLuint g_sm_prog[3] = {0, 0, 0};
GLuint g_sm_edges = 0, g_sm_blend = 0, g_sm_out = 0, g_sm_area = 0, g_sm_search = 0;
GLuint g_sm_fbo[3] = {0, 0, 0};
int g_sm_w = 0, g_sm_h = 0;
bool g_sm_hdr = false;
bool g_sm_failed = false;

const char* const k_sm_main[3] = {
R"GLSL(
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_color;
void main() {
    vec4 off[3]; SMAAEdgeDetectionVS(v_uv, off);
    o_color = vec4(SMAALumaEdgeDetectionPS(v_uv, off, u_color), 0.0, 0.0);
}
)GLSL",
R"GLSL(
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_edges;
uniform sampler2D u_area;
uniform sampler2D u_search;
void main() {
    vec2 pix; vec4 off[3]; SMAABlendingWeightCalculationVS(v_uv, pix, off);
    o_color = SMAABlendingWeightCalculationPS(v_uv, pix, off, u_edges, u_area, u_search, vec4(0.0));
}
)GLSL",
R"GLSL(
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_color;
uniform sampler2D u_blend;
void main() {
    vec4 off; SMAANeighborhoodBlendingVS(v_uv, off);
    o_color = vec4(SMAANeighborhoodBlendingPS(v_uv, off, u_color, u_blend).rgb, 1.0);
}
)GLSL"};

GLuint build_program(const std::string& vs_src, const std::string& fs_src, const char* what) {
    auto comp = [&](GLenum type, const std::string& body) {
        const char* src[2] = {PSP_GLSL_VERSION, body.c_str()};
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 2, src, nullptr);
        glCompileShader(sh);
        GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048]; glGetShaderInfoLog(sh, sizeof log, nullptr, log);
            std::fprintf(stderr, "[POST] %s shader failed:\n%s\n", what, log);
            glDeleteShader(sh); return GLuint(0);
        }
        return sh;
    };
    GLuint vs = comp(GL_VERTEX_SHADER, vs_src), fs = vs ? comp(GL_FRAGMENT_SHADER, fs_src) : 0;
    if (!vs || !fs) { if (vs) glDeleteShader(vs); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]; glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "[POST] %s link failed:\n%s\n", what, log);
        glDeleteProgram(p); return 0;
    }
    return p;
}

void make_sm_tex(GLuint& tex, GLenum ifmt, GLenum fmt, int w, int h, const void* data) {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool ensure_smaa(int w, int h) {
    if (g_sm_failed) return false;
    if (!g_sm_prog[0]) {
        std::string lib = "#define SMAA_GLSL_3 1\n#define SMAA_PRESET_HIGH 1\n#define SMAA_RT_METRICS u_rt\nuniform vec4 u_rt;\n";
        for (int i = 0; psp_smaa_lib[i]; ++i) lib += psp_smaa_lib[i];
        const std::string vs = std::string(k_vs);
        for (int i = 0; i < 3; ++i) {
            g_sm_prog[i] = build_program(vs, lib + k_sm_main[i], i == 0 ? "SMAA edges" : i == 1 ? "SMAA weights" : "SMAA blend");
            if (!g_sm_prog[i]) { g_sm_failed = true; return false; }
        }
        make_sm_tex(g_sm_area, GL_RG8, GL_RG, 160, 560, psp_smaa_area);
        make_sm_tex(g_sm_search, GL_R8, GL_RED, 64, 16, psp_smaa_search);
        glBindTexture(GL_TEXTURE_2D, g_sm_search);   // the search lookup is read with exact texel coordinates
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        std::fprintf(stderr, "[POST] SMAA ready\n");
    }
    const bool want_hdr = psp_hdr_scene_live();
    if (g_sm_w != w || g_sm_h != h || g_sm_hdr != want_hdr) {
        g_sm_hdr = want_hdr;   // fp16 intermediates keep the scene's values above 1.0 through the AA pass
        GLuint* texs[3] = {&g_sm_edges, &g_sm_blend, &g_sm_out};
        for (int i = 0; i < 3; ++i) {
            if (*texs[i]) glDeleteTextures(1, texs[i]);
            make_sm_tex(*texs[i], g_sm_hdr ? GL_RGBA16F : GL_RGBA8, GL_RGBA, w, h, nullptr);
            if (!g_sm_fbo[i]) glGenFramebuffers(1, &g_sm_fbo[i]);
            glBindFramebuffer(GL_FRAMEBUFFER, g_sm_fbo[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texs[i], 0);
        }
        g_sm_w = w; g_sm_h = h;
    }
    return true;
}

// Runs the three passes on `tex` (w x h); returns the anti-aliased texture, or 0 on failure (the caller then uses FXAA / none).
GLuint smaa_run(GLuint tex, int w, int h) {
    if (!ensure_smaa(w, h)) return 0;
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, w, h);
    glBindVertexArray(g_vao);
    auto bind = [&](int unit, GLuint t, GLuint samp) { glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(GL_TEXTURE_2D, t); glBindSampler(unit, samp); };
    auto pass = [&](int i, GLuint target_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
        glUseProgram(g_sm_prog[i]);
        glUniform4f(glGetUniformLocation(g_sm_prog[i], "u_rt"), 1.f / w, 1.f / h, float(w), float(h));
        return g_sm_prog[i];
    };
    GLuint p = pass(0, g_sm_fbo[0]);                                   // 1: edges
    bind(0, tex, g_sampler_lin); glUniform1i(glGetUniformLocation(p, "u_color"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p = pass(1, g_sm_fbo[1]);                                          // 2: blend weights
    bind(0, g_sm_edges, g_sampler_lin); bind(1, g_sm_area, g_sampler_lin); bind(2, g_sm_search, g_sampler_nn);
    glUniform1i(glGetUniformLocation(p, "u_edges"), 0); glUniform1i(glGetUniformLocation(p, "u_area"), 1); glUniform1i(glGetUniformLocation(p, "u_search"), 2);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p = pass(2, g_sm_fbo[2]);                                          // 3: neighbourhood blending
    bind(0, tex, g_sampler_lin); bind(1, g_sm_blend, g_sampler_lin);
    glUniform1i(glGetUniformLocation(p, "u_color"), 0); glUniform1i(glGetUniformLocation(p, "u_blend"), 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    for (int u = 2; u >= 0; --u) { glActiveTexture(GL_TEXTURE0 + u); glBindSampler(u, 0); glBindTexture(GL_TEXTURE_2D, 0); }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    return g_sm_out;
}

// ---- Final pass: colour grade + HDR ------------------------------------------------------------------------------------

std::atomic<int> g_sat{-1}, g_bright{-1}, g_contrast{-1}, g_hdr{-1}, g_peak{-1}, g_paper{-1}, g_hboost{-1}, g_hgamut{-1}, g_hscene{-1};

int env_clamped(const char* name, int dflt, int lo, int hi) {
    int v = env_int(name, dflt);
    return v < lo ? lo : v > hi ? hi : v;
}

const char* const k_final_fs = R"GLSL(
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_scene;   // the picture before the HUD / overlays
uniform sampler2D u_comp;    // the finished window picture
uniform float u_sat;         // 1 = unchanged
uniform float u_bright;      // gamma exponent applied as pow(c, 1 / u_bright)
uniform float u_contrast;    // 1 = unchanged
uniform int u_enc;           // 0 SDR out, 1 scRGB linear, 2 HDR10 PQ
uniform float u_peak;        // nits
uniform float u_paper;       // nits
uniform float u_scrgb_nits;  // nits that scRGB 1.0 stands for: 80 per the spec (desktop, gamescope); Android makes 1.0 the system's SDR white
uniform float u_boost;       // 0..1 how much of the headroom small highlights may use
uniform float u_gamut;       // 0..1 how far the picture is stretched from sRGB towards the Display-P3 primaries
uniform vec2 u_texel;        // one window pixel in uv
uniform int u_hscene;        // 1 = the scene copies are fp16 and may hold real values above 1.0 (HDR Scene): keep them

vec3 srgb_to_lin(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }

vec3 pq_encode(vec3 nits) {
    vec3 y = clamp(nits / 10000.0, 0.0, 1.0);
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 p = pow(y, vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

void main() {
    vec3 c = texture(u_comp, v_uv).rgb;
    vec3 sc = texture(u_scene, v_uv).rgb;
    // Pixels the overlays changed (HUD, menus, dialogs) stay at SDR level; only the 3D picture is expanded.
    const bool scene_px = max(abs(c.r - sc.r), max(abs(c.g - sc.g), abs(c.b - sc.b))) < (2.5 / 255.0);
    // Grade, in display (gamma) space.
    // With HDR Scene on, values above 1.0 are real light (additive effects) and are kept (extended sRGB); the grade
    // never lifts a value that was at or below 1.0 above it, so the LDR look is unchanged.
    const float hi = u_hscene != 0 ? 64.0 : 1.0;
    c = clamp((c - 0.5) * u_contrast + 0.5, 0.0, hi);
    c = pow(max(c, vec3(0.0)), vec3(1.0 / u_bright));
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = clamp(mix(vec3(l), c, u_sat), 0.0, hi);
    if (u_enc == 0) { o_color = vec4(c, 1.0); return; }
    // HDR. The game is SDR: linearise, then lift only the bright end (glows, spell flashes, the sky, specular) so that
    // SDR white sits at paper white and the brightest pixels reach the display's peak.
    vec3 lin = srgb_to_lin(c);
    float m = max(lin.r, max(lin.g, lin.b));
    float gain = 1.0;
    if (scene_px && u_boost > 0.0) {
        // Only SMALL bright features (glows, sparks, spell flashes, specular) may rise above paper white. Large bright areas
        // (menus, sky, white stages) are already at their intended level in SDR, so a pixel is lifted only as far as it
        // stands out from the local average around it.
        float lm = 0.0;
        for (int i = 0; i < 8; ++i) {
            float a = 0.785398 * float(i);
            vec3 n = texture(u_comp, v_uv + vec2(cos(a), sin(a)) * u_texel * (i % 2 == 0 ? 6.0 : 11.0)).rgb;
            lm += dot(srgb_to_lin(n), vec3(0.2126, 0.7152, 0.0722));
        }
        lm *= 0.125;
        float lum = dot(lin, vec3(0.2126, 0.7152, 0.0722));
        float stand = smoothstep(0.10, 0.55, lum - lm);       // how much it stands out from its surroundings
        float bright = smoothstep(0.50, 0.95, m);             // and that it is actually bright
        gain = 1.0 + max(u_peak / u_paper - 1.0, 0.0) * u_boost * stand * bright;
    }
    // Colour: a wide-gamut panel shows plain sRGB colour dull next to its own SDR mode (the compositor stretches SDR colour to the
    // panel gamut, but not HDR content). u_gamut stretches the picture the same way, treating the PSP's RGB as Display-P3 by that amount.
    // Matrices are written column by column. scRGB (enc 1) takes BT.709 primaries and may go negative; HDR10 (enc 2) takes BT.2020.
    const mat3 p3_to_709 = mat3(1.2249401, -0.0420569, -0.0196376,  -0.2249404, 1.0420571, -0.0786361,  0.0, 0.0, 1.0982735);
    const mat3 p3_to_2020 = mat3(0.7538330, 0.0457438, -0.0012110,  0.1985970, 0.9417770, 0.0176042,  0.0475700, 0.0124792, 0.9836070);
    const mat3 srgb_to_2020 = mat3(0.6274040, 0.0690970, 0.0163916,  0.3292820, 0.9195400, 0.0880132,  0.0433136, 0.0113612, 0.8955950);
    if (u_enc == 1) lin = mix(lin, p3_to_709 * lin, u_gamut);
    else lin = mix(srgb_to_2020 * lin, p3_to_2020 * lin, u_gamut);
    vec3 nits = lin * u_paper * gain;
    if (u_hscene != 0) nits *= min(1.0, u_peak / max(max(nits.r, max(nits.g, nits.b)), 1.0));   // real HDR data stops at the display peak
    if (u_enc == 1) o_color = vec4(nits / u_scrgb_nits, 1.0);   // scRGB: 1.0 = 80 nits (Android: 1.0 = SDR white, so paper white is the system's)
    else o_color = vec4(pq_encode(nits), 1.0);
}
)GLSL";

GLuint g_fprog = 0, g_scene_tex = 0, g_comp_tex = 0, g_scene_fbo = 0, g_comp_fbo = 0;
int g_ftw = 0, g_fth = 0;
bool g_ffailed = false, g_scene_valid = false;
GLint fu_scene = -1, fu_comp = -1, fu_sat = -1, fu_bright = -1, fu_contrast = -1, fu_enc = -1, fu_peak = -1, fu_paper = -1, fu_boost = -1, fu_texel = -1, fu_gamut = -1, fu_scrgb = -1, fu_hscene = -1;

bool ensure_final_program() {
    if (g_fprog) return true;
    if (g_ffailed) return false;
    if (!ensure_program()) return false;   // shares the vertex shader / VAO / samplers
    GLuint vs = compile(GL_VERTEX_SHADER, k_vs), fs = vs ? compile(GL_FRAGMENT_SHADER, k_final_fs) : 0;
    if (!vs || !fs) { g_ffailed = true; if (vs) glDeleteShader(vs); return false; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { std::fprintf(stderr, "[POST] final pass link failed\n"); glDeleteProgram(p); g_ffailed = true; return false; }
    g_fprog = p;
    fu_scene = glGetUniformLocation(p, "u_scene"); fu_comp = glGetUniformLocation(p, "u_comp");
    fu_sat = glGetUniformLocation(p, "u_sat"); fu_bright = glGetUniformLocation(p, "u_bright");
    fu_contrast = glGetUniformLocation(p, "u_contrast"); fu_enc = glGetUniformLocation(p, "u_enc");
    fu_peak = glGetUniformLocation(p, "u_peak"); fu_paper = glGetUniformLocation(p, "u_paper");
    fu_boost = glGetUniformLocation(p, "u_boost"); fu_scrgb = glGetUniformLocation(p, "u_scrgb_nits"); fu_gamut = glGetUniformLocation(p, "u_gamut"); fu_texel = glGetUniformLocation(p, "u_texel"); fu_hscene = glGetUniformLocation(p, "u_hscene");
    std::fprintf(stderr, "[POST] final pass ready\n");
    return true;
}

bool g_copy_hdr = false;   // the scene / window copies are fp16 (HDR Scene on): values above 1.0 survive to the final pass

void make_copy_target(GLuint& tex, GLuint& fbo, int w, int h) {
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (g_copy_hdr) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!fbo) glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
}

bool ensure_copy_targets(int w, int h) {
    const bool hdr = psp_hdr_scene_live();
    if (g_scene_tex && g_ftw == w && g_fth == h && g_copy_hdr == hdr) return true;
    g_copy_hdr = hdr;
    make_copy_target(g_scene_tex, g_scene_fbo, w, h);
    make_copy_target(g_comp_tex, g_comp_fbo, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_ftw = w; g_fth = h;
    return true;
}

}  // namespace

PspHdrHooks g_psp_hdr;

int psp_post_saturation() {
    int v = g_sat.load();
    if (v < 0) { v = env_clamped("PSPRECOMP_SATURATION", 100, 0, 200); g_sat.store(v); }
    return v;
}
void psp_post_set_saturation(int pct) { g_sat.store(pct < 0 ? 0 : pct > 200 ? 200 : pct); }
int psp_post_brightness() {
    int v = g_bright.load();
    if (v < 0) { v = env_clamped("PSPRECOMP_BRIGHTNESS", 100, 50, 150); g_bright.store(v); }
    return v;
}
void psp_post_set_brightness(int pct) { g_bright.store(pct < 50 ? 50 : pct > 150 ? 150 : pct); }
int psp_post_contrast() {
    int v = g_contrast.load();
    if (v < 0) { v = env_clamped("PSPRECOMP_CONTRAST", 100, 50, 150); g_contrast.store(v); }
    return v;
}
void psp_post_set_contrast(int pct) { g_contrast.store(pct < 50 ? 50 : pct > 150 ? 150 : pct); }

bool psp_hdr_wanted() {
    int v = g_hdr.load();
    if (v < 0) { v = env_int("PSPRECOMP_HDR", 0) ? 1 : 0; g_hdr.store(v); }
    return v != 0;
}
void psp_hdr_set_wanted(bool on) { g_hdr.store(on ? 1 : 0); if (g_psp_hdr.set_wanted) g_psp_hdr.set_wanted(on); }
bool psp_hdr_available() { return g_psp_hdr.available && g_psp_hdr.available(); }
int psp_hdr_encoding() { return g_psp_hdr.encoding ? g_psp_hdr.encoding() : 0; }
bool psp_hdr_scene_wanted() {
    int v = g_hscene.load();
    if (v < 0) { v = env_int("PSPRECOMP_HDR_SCENE", 0) ? 1 : 0; g_hscene.store(v); }
    return v != 0;
}
void psp_hdr_scene_set_wanted(bool on) { g_hscene.store(on ? 1 : 0); }
bool psp_hdr_scene_live() { return psp_hdr_scene_wanted() && psp_hdr_encoding() != 0; }
int psp_hdr_peak() {
    int v = g_peak.load();
    if (v < 0) { v = env_clamped("PSPRECOMP_HDR_PEAK", 1000, 400, 4000); g_peak.store(v); }
    return v;
}
void psp_hdr_set_peak(int nits) { g_peak.store(nits < 400 ? 400 : nits > 4000 ? 4000 : nits); }
int psp_hdr_paper() {
    int v = g_paper.load();
    if (v < 0) { v = env_clamped("PSPRECOMP_HDR_PAPER", 203, 80, 1000); g_paper.store(v); }
    return v;
}
int psp_hdr_boost() {
    int v = g_hboost.load();
    if (v < 0) { v = env_clamped("PSPRECOMP_HDR_BOOST", 1, 0, 3); g_hboost.store(v); }
    return v;
}
void psp_hdr_set_boost(int level) { g_hboost.store(level < 0 ? 0 : level > 3 ? 3 : level); }
int psp_hdr_gamut() {
    int v = g_hgamut.load();
    if (v < 0) {
#ifdef __ANDROID__
        v = env_clamped("PSPRECOMP_HDR_GAMUT", 4, 0, 4);   // a phone's SDR mode boosts colour well past P3
#else
        v = env_clamped("PSPRECOMP_HDR_GAMUT", 2, 0, 4);
#endif
        g_hgamut.store(v);
    }
    return v;
}
void psp_hdr_set_gamut(int level) { g_hgamut.store(level < 0 ? 0 : level > 4 ? 4 : level); }
void psp_hdr_set_paper(int nits) { g_paper.store(nits < 80 ? 80 : nits > 1000 ? 1000 : nits); }

bool psp_post_final_active() {
    return psp_post_saturation() != 100 || psp_post_brightness() != 100 || psp_post_contrast() != 100 || psp_hdr_encoding() != 0;
}

void psp_post_scene_snapshot(int dw, int dh) {
    g_scene_valid = false;
    if (dw <= 0 || dh <= 0 || psp_hdr_encoding() == 0 || !ensure_final_program() || !ensure_copy_targets(dw, dh)) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_scene_fbo);
    glBlitFramebuffer(0, 0, dw, dh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_scene_valid = true;
}

bool psp_post_final(int dw, int dh) {
    if (dw <= 0 || dh <= 0 || !psp_post_final_active() || !ensure_final_program() || !ensure_copy_targets(dw, dh)) return false;
    const int enc = psp_hdr_encoding();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_comp_fbo);
    glBlitFramebuffer(0, 0, dw, dh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, dw, dh);
    glUseProgram(g_fprog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_comp_tex);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g_scene_valid ? g_scene_tex : g_comp_tex);
    glUniform1i(fu_comp, 0); glUniform1i(fu_scene, 1);
    glUniform1f(fu_sat, psp_post_saturation() / 100.f);
    glUniform1f(fu_bright, psp_post_brightness() / 100.f);
    glUniform1f(fu_contrast, psp_post_contrast() / 100.f);
    glUniform1i(fu_enc, enc);
    glUniform1i(fu_hscene, g_copy_hdr ? 1 : 0);
    glUniform1f(fu_peak, float(psp_hdr_peak()));
    glUniform1f(fu_paper, float(psp_hdr_paper()));
#ifdef __ANDROID__
    glUniform1f(fu_scrgb, 203.f);   // Android: scRGB 1.0 is the SDR white the user's brightness slider sets (taken as the 203-nit reference): paper white 350 shows white at 1.7x that
#else
    glUniform1f(fu_scrgb, 80.f);
#endif
    { static const float k_boost[4] = {0.f, 0.35f, 0.65f, 1.f}; glUniform1f(fu_boost, k_boost[psp_hdr_boost()]); }
    glUniform2f(fu_texel, 1.f / float(dw), 1.f / float(dh));
    { static const float k_gamut[5] = {0.f, 0.4f, 0.7f, 1.f, 1.7f}; glUniform1f(fu_gamut, k_gamut[psp_hdr_gamut()]); }   // level 4 = 1.7: extrapolated past Display-P3
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    g_scene_valid = false;
    return true;
}

int psp_post_aa() {
    int v = g_aa.load();
    if (v < 0) { v = env_int("PSPRECOMP_POST_AA", 0); v = v < 0 ? 0 : v > POST_AA_SMAA ? POST_AA_SMAA : v; g_aa.store(v); }
    return v;
}
void psp_post_set_aa(int mode) { g_aa.store(mode < 0 ? 0 : mode > POST_AA_SMAA ? POST_AA_SMAA : mode); }

int psp_post_sharpen() {
    int v = g_sharp.load();
    if (v < 0) { v = env_int("PSPRECOMP_SHARPEN", 0); if (v < 0) v = 0; if (v > 4) v = 4; g_sharp.store(v); }
    return v;
}
void psp_post_set_sharpen(int level) { g_sharp.store(level < 0 ? 0 : level > 4 ? 4 : level); }

int psp_post_upscale() {
    int v = g_up.load();
    if (v < 0) {
        const char* e = std::getenv("PSPRECOMP_UPSCALE");
        v = POST_UP_BILINEAR;
        if (e && !std::strcmp(e, "sharp")) v = POST_UP_SHARP;
        else if (e && !std::strcmp(e, "bicubic")) v = POST_UP_BICUBIC;
        g_up.store(v);
    }
    return v;
}
void psp_post_set_upscale(int mode) { g_up.store(mode < 0 ? 0 : mode > 2 ? 2 : mode); }

bool psp_post_active() {
    return psp_post_aa() != POST_AA_OFF || psp_post_sharpen() > 0 || psp_post_upscale() != POST_UP_BILINEAR;
}

bool psp_post_draw(GLuint tex, int sw, int sh, int x0, int y0, int x1, int y1, bool nearest) {
    if (!ensure_program() || x1 <= x0 || y1 <= y0) return false;
    int aa = psp_post_aa();
    if (aa == POST_AA_SMAA) {
        const GLuint sm = smaa_run(tex, sw, sh);   // restores the default framebuffer; a failed build falls back to FXAA
        if (sm) { tex = sm; aa = POST_AA_OFF; } else aa = POST_AA_FXAA;
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(x0, y0, x1 - x0, y1 - y0);
    glUseProgram(g_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, nearest ? g_sampler_nn : g_sampler_lin);
    glUniform1i(u_tex, 0);
    glUniform2f(u_size, float(sw), float(sh));
    glUniform1f(u_scale, float(x1 - x0) / float(sw));
    // A nearest present (integer scale) keeps its hard pixels: the smoothing modes do not apply to it.
    glUniform1i(u_mode, nearest ? 0 : psp_post_upscale());
    glUniform1i(u_aa, aa);
    static const float k_amt[5] = {0.f, 0.25f, 0.5f, 0.75f, 1.0f};
    glUniform1f(u_sharp, k_amt[psp_post_sharpen()]);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    return true;
}
