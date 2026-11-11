#include "psp_ge_shader.h"
#include "psp_ge.h"
#include "psp_ge_constants.h"

#include "psp_gl.h"
#include "psp_ge_shader_src.h"
#include <cstdio>
#include <cstring>

// ---- Shader source ----

// The vertex stage is shared with the FX shader overrides (psp_fx_shader.cpp),
// which compile the same body with PSP_FX defined to get extra outputs. The
// #version line is passed as a separate source string so a #define can go
// between it and the body.
static const char* const k_vertex_version = PSP_GLSL_VERSION;
float g_psp_gles_point_size = 1.0f;   // see psp_gl.h (read on Android only)
// k_vertex_shader_src / k_fragment_shader_src live in psp_ge_shader_src.h (shared with the Vulkan backend).



// ---- Module state ----
static GLuint g_program = 0;
static GLuint g_used_program = 0;
static ShaderUniforms g_uniforms{};

// ---- Uniform value cache ----
// Redundant glUniform* calls are a measurable per-draw cost (thousands of
// draws per frame); each uniform keeps its last uploaded bytes and skips the
// GL call when nothing changed. Indexed by uniform location.
struct UCache { unsigned char data[512]; unsigned size; bool valid; };
static UCache g_ucache[128];
static bool u_changed(GLint loc, const void* d, size_t n) {
    if (loc < 0) return false;
    if (loc >= 128 || n > sizeof(g_ucache[0].data)) return true;  // uncached
    UCache& c = g_ucache[loc];
    if (c.valid && c.size == n && std::memcmp(c.data, d, n) == 0) return false;
    std::memcpy(c.data, d, n);
    c.size = (unsigned)n;
    c.valid = true;
    return true;
}
void ge_shader_invalidate_cache() { std::memset(g_ucache, 0, sizeof g_ucache); }
static void u1i(GLint loc, int v) { if (u_changed(loc, &v, sizeof v)) glUniform1i(loc, v); }
static void u3i(GLint loc, int a, int b, int c) {
    int v[3] = {a, b, c};
    if (u_changed(loc, v, sizeof v)) glUniform3i(loc, a, b, c);
}
static void u3f(GLint loc, float a, float b, float c) {
    float v[3] = {a, b, c};
    if (u_changed(loc, v, sizeof v)) glUniform3f(loc, a, b, c);
}
static void u4f(GLint loc, float a, float b, float c, float d) {
    float v[4] = {a, b, c, d};
    if (u_changed(loc, v, sizeof v)) glUniform4f(loc, a, b, c, d);
}

// ---- Helpers ----

static GLuint compile_shader_n(GLenum type, const char* const* srcs, int n);
static GLuint compile_shader(
    GLenum type, const char* src
) {
    return compile_shader_n(type, &src, 1);
}
static GLuint compile_shader_n(GLenum type, const char* const* srcs, int n) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, n, srcs, nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log),
                           nullptr, log);
        std::fprintf(stderr,
            "[SHADER] Compile error (%s):\n%s\n",
            type == GL_VERTEX_SHADER ? "vert" : "frag",
            log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// ---- Public API ----

void ge_shader_init() {
    const char* const vsrc[2] = {k_vertex_version, k_vertex_shader_src};
    GLuint vert = compile_shader_n(GL_VERTEX_SHADER, vsrc, 2);
    const char* const fsrc[2] = {k_vertex_version, k_fragment_shader_src};   // same #version (+ ES precisions) as the vertex stage
    GLuint frag = compile_shader_n(GL_FRAGMENT_SHADER, fsrc, 2);

    if (!vert || !frag) {
        std::fprintf(stderr,
            "[SHADER] Failed to compile shaders\n");
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return;
    }

    g_program = glCreateProgram();
    glAttachShader(g_program, vert);
    glAttachShader(g_program, frag);
    {
        // Capture the vertex-stage outputs for PSPRECOMP_GE_VERIFY (no cost
        // unless a transform-feedback pass is actually run).
        static const char* const tf_varyings[] = {"gl_Position", "v_texcoord", "v_color"};
        glTransformFeedbackVaryings(g_program, 3, tf_varyings, GL_INTERLEAVED_ATTRIBS);
    }
    glLinkProgram(g_program);

    GLint success = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(g_program, sizeof(log),
                            nullptr, log);
        std::fprintf(stderr,
            "[SHADER] Link error:\n%s\n", log);
        glDeleteProgram(g_program);
        g_program = 0;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    if (g_program) {
        ge_shader_query_locations(g_program, &g_uniforms);
        ge_shader_invalidate_cache();

        std::fprintf(stderr,
            "[SHADER] Uber-shader compiled and linked "
            "(program=%u)\n", g_program);
    }
}

void ge_shader_shutdown() {
    if (g_program) {
        glDeleteProgram(g_program);
        g_program = 0;
        g_used_program = 0;
    }
}

void ge_shader_use() {
    if (g_program && g_used_program != g_program) {
        glUseProgram(g_program);
        g_used_program = g_program;
    }
}

void ge_shader_set_uniforms(const GeState& state) {
    if (!g_program) return;

    u1i(g_uniforms.u_texture_enable, state.texture_enable ? 1 : 0);
    u1i(g_uniforms.u_texture, 0);  // unit 0
    u1i(g_uniforms.u_tex_func, static_cast<int>(state.tex_func));
    u1i(g_uniforms.u_tex_rgba, state.tex_func_rgba ? 1 : 0);
    u1i(g_uniforms.u_tex_double, state.tex_func_double ? 1 : 0);
    u3f(g_uniforms.u_tex_env,
        (state.tex_env_color & 0xFF) / 255.0f,
        ((state.tex_env_color >> 8) & 0xFF) / 255.0f,
        ((state.tex_env_color >> 16) & 0xFF) / 255.0f);

    // Alpha test
    u1i(g_uniforms.u_alpha_test_enable, state.alpha_test_enable ? 1 : 0);
    if (state.alpha_test_enable) {
        int func = state.alpha_test & 0x7;
        int ref_byte = (state.alpha_test >> 8) & 0xFF;
        int mask_byte = (state.alpha_test >> 16) & 0xFF;
        u1i(g_uniforms.u_alpha_test_ref, ref_byte);
        u1i(g_uniforms.u_alpha_test_mask, mask_byte);
        u1i(g_uniforms.u_alpha_test_func, func);
    }

    // Colour test (0xBBGGRR ref/mask)
    u1i(g_uniforms.u_color_test_enable, state.color_test_enable ? 1 : 0);
    if (state.color_test_enable) {
        const uint32_t r = state.color_ref, m = state.color_test_mask;
        u1i(g_uniforms.u_color_test_func, state.color_test & 0x3);
        u3i(g_uniforms.u_color_test_ref,
            r & 0xFF, (r >> 8) & 0xFF, (r >> 16) & 0xFF);
        u3i(g_uniforms.u_color_test_mask,
            m & 0xFF, (m >> 8) & 0xFF, (m >> 16) & 0xFF);
    }
}

void ge_shader_set_tex_remap(float sx, float sy, bool flip) {
    if (!g_program) return;
    u4f(g_uniforms.u_tex_remap, sx, sy, flip ? 1.0f : 0.0f, 0.0f);
}

static void u1f(GLint loc, float v) { if (u_changed(loc, &v, sizeof v)) glUniform1f(loc, v); }
void ge_shader_set_alpha_override(float a) { u1f(g_uniforms.u_alpha_override, a >= 0.0f ? a + 1.0f : 0.0f); }

void ge_shader_set_alpha_double(bool enable) {
    if (!g_program) return;
    u1i(g_uniforms.u_alpha_double, enable ? 1 : 0);
}

static void upload_point_size(GLint loc) {
#ifdef PSP_GLES
    if (loc >= 0) { const float v = g_psp_gles_point_size; if (u_changed(loc, &v, sizeof v)) glUniform1f(loc, v); }
#else
    (void)loc;
#endif
}

void ge_shader_set_xform_passthrough() {
    if (!g_program) return;
    u1i(g_uniforms.u_xform, 0);
    upload_point_size(g_uniforms.u_psize);
}

// 4x3 column-major (12 floats) -> padded 4x4 column-major (16 floats).
static inline void pad43(const float* m, float* o) {
    for (int c = 0; c < 4; c++) {
        o[c * 4 + 0] = m[c * 3 + 0];
        o[c * 4 + 1] = m[c * 3 + 1];
        o[c * 4 + 2] = m[c * 3 + 2];
        o[c * 4 + 3] = c == 3 ? 1.0f : 0.0f;
    }
}

void ge_shader_set_xform_3d(const GeXform3D& x) {
    if (!g_program) return;
    u1i(g_uniforms.u_xform, 1);
    upload_point_size(g_uniforms.u_psize);
    float m4[16];
    pad43(x.world, m4);
    if (u_changed(g_uniforms.u_world, m4, sizeof m4))
        glUniformMatrix4fv(g_uniforms.u_world, 1, GL_FALSE, m4);
    pad43(x.view, m4);
    if (u_changed(g_uniforms.u_view, m4, sizeof m4))
        glUniformMatrix4fv(g_uniforms.u_view, 1, GL_FALSE, m4);
    if (u_changed(g_uniforms.u_proj, x.proj, 16 * sizeof(float)))
        glUniformMatrix4fv(g_uniforms.u_proj, 1, GL_FALSE, x.proj);
    u4f(g_uniforms.u_uvxf, x.uvxf[0], x.uvxf[1], x.uvxf[2], x.uvxf[3]);
    u1i(g_uniforms.u_skin, x.skin_n);
    if (x.skin_n > 0) {
        // Only the bones in use are uploaded (up to 8 mat4 = 512 bytes, the
        // u_changed cache limit).
        float b4[8 * 16];
        for (int k = 0; k < x.skin_n; k++) pad43(x.bones + k * 12, b4 + k * 16);
        if (u_changed(g_uniforms.u_bones, b4, (size_t)x.skin_n * 16 * sizeof(float)))
            glUniformMatrix4fv(g_uniforms.u_bones, x.skin_n, GL_FALSE, b4);
    }
    u1i(g_uniforms.u_lit, x.lit);
    if (x.lit) {
        u1i(g_uniforms.u_lflags, x.lflags);
        u1i(g_uniforms.u_nl, x.nlights);
        if (u_changed(g_uniforms.u_lc, x.lc, sizeof x.lc))
            glUniform4fv(g_uniforms.u_lc, 4, &x.lc[0][0]);
        if (x.nlights > 0
            && u_changed(g_uniforms.u_lt, x.lt, (size_t)x.nlights * 5 * 4 * sizeof(float)))
            glUniform4fv(g_uniforms.u_lt, x.nlights * 5, &x.lt[0][0]);
    }
}

GLuint ge_shader_get_program() {
    return g_program;
}

// ---- Shared with the FX shader overrides (psp_fx_shader.cpp) ----

const char* ge_shader_vertex_version() { return k_vertex_version; }
const char* ge_shader_vertex_body() { return k_vertex_shader_src; }

void ge_shader_query_locations(GLuint prog, ShaderUniforms* u) {
    auto L = [&](const char* n) { return glGetUniformLocation(prog, n); };
    u->u_texture_enable = L("u_texture_enable");
    u->u_texture = L("u_texture");
    u->u_tex_func = L("u_tex_func");
    u->u_tex_rgba = L("u_tex_rgba");
    u->u_tex_double = L("u_tex_double");
    u->u_tex_env = L("u_tex_env");
    u->u_alpha_test_enable = L("u_alpha_test_enable");
    u->u_alpha_test_ref = L("u_alpha_test_ref");
    u->u_alpha_test_func = L("u_alpha_test_func");
    u->u_alpha_test_mask = L("u_alpha_test_mask");
    u->u_color_test_enable = L("u_color_test_enable");
    u->u_color_test_func = L("u_color_test_func");
    u->u_color_test_ref = L("u_color_test_ref");
    u->u_color_test_mask = L("u_color_test_mask");
    u->u_tex_remap = L("u_tex_remap");
    u->u_alpha_double = L("u_alpha_double");
    u->u_alpha_override = L("u_alpha_override");
    u->u_xform = L("u_xform");
    u->u_psize = L("u_psize");
    u->u_world = L("u_world");
    u->u_view = L("u_view");
    u->u_proj = L("u_proj");
    u->u_uvxf = L("u_uvxf");
    u->u_skin = L("u_skin");
    u->u_bones = L("u_bones[0]");
    u->u_lit = L("u_lit");
    u->u_lflags = L("u_lflags");
    u->u_nl = L("u_nl");
    u->u_lc = L("u_lc[0]");
    u->u_lt = L("u_lt[0]");
}

void ge_shader_note_external_program() { g_used_program = 0; }

void ge_shader_upload_to(const ShaderUniforms& u, const GeState& state, const GeXform3D* x,
                         bool alpha_double) {
    // Uncached: an override program is bound for a handful of batches a frame.
    if (u.u_texture_enable >= 0) glUniform1i(u.u_texture_enable, state.texture_enable ? 1 : 0);
    if (u.u_texture >= 0) glUniform1i(u.u_texture, 0);
    if (u.u_tex_func >= 0) glUniform1i(u.u_tex_func, static_cast<int>(state.tex_func));
    if (u.u_tex_rgba >= 0) glUniform1i(u.u_tex_rgba, state.tex_func_rgba ? 1 : 0);
    if (u.u_tex_double >= 0) glUniform1i(u.u_tex_double, state.tex_func_double ? 1 : 0);
    if (u.u_tex_env >= 0)
        glUniform3f(u.u_tex_env, (state.tex_env_color & 0xFF) / 255.0f,
                    ((state.tex_env_color >> 8) & 0xFF) / 255.0f,
                    ((state.tex_env_color >> 16) & 0xFF) / 255.0f);
    if (u.u_alpha_double >= 0) glUniform1i(u.u_alpha_double, alpha_double ? 1 : 0);
#ifdef PSP_GLES
    if (u.u_psize >= 0) glUniform1f(u.u_psize, g_psp_gles_point_size);
#endif
    if (!x) {
        if (u.u_xform >= 0) glUniform1i(u.u_xform, 0);
        return;
    }
    if (u.u_xform >= 0) glUniform1i(u.u_xform, 1);
    float m4[16];
    pad43(x->world, m4);
    if (u.u_world >= 0) glUniformMatrix4fv(u.u_world, 1, GL_FALSE, m4);
    pad43(x->view, m4);
    if (u.u_view >= 0) glUniformMatrix4fv(u.u_view, 1, GL_FALSE, m4);
    if (u.u_proj >= 0) glUniformMatrix4fv(u.u_proj, 1, GL_FALSE, x->proj);
    if (u.u_uvxf >= 0) glUniform4f(u.u_uvxf, x->uvxf[0], x->uvxf[1], x->uvxf[2], x->uvxf[3]);
    if (u.u_skin >= 0) glUniform1i(u.u_skin, x->skin_n);
    if (x->skin_n > 0 && u.u_bones >= 0) {
        float b4[8 * 16];
        for (int k = 0; k < x->skin_n; k++) pad43(x->bones + k * 12, b4 + k * 16);
        glUniformMatrix4fv(u.u_bones, x->skin_n, GL_FALSE, b4);
    }
    if (u.u_lit >= 0) glUniform1i(u.u_lit, x->lit);
    if (x->lit) {
        if (u.u_lflags >= 0) glUniform1i(u.u_lflags, x->lflags);
        if (u.u_nl >= 0) glUniform1i(u.u_nl, x->nlights);
        if (u.u_lc >= 0) glUniform4fv(u.u_lc, 4, &x->lc[0][0]);
        if (x->nlights > 0 && u.u_lt >= 0) glUniform4fv(u.u_lt, x->nlights * 5, &x->lt[0][0]);
    }
}
