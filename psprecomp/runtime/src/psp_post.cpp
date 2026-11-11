// Present-time post-processing, see include/psp_post.h.
#include "psp_post.h"

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

}  // namespace

int psp_post_aa() {
    int v = g_aa.load();
    if (v < 0) { v = env_int("PSPRECOMP_POST_AA", 0) ? POST_AA_FXAA : POST_AA_OFF; g_aa.store(v); }
    return v;
}
void psp_post_set_aa(int mode) { g_aa.store(mode ? POST_AA_FXAA : POST_AA_OFF); }

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
    glUniform1i(u_aa, psp_post_aa());
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
