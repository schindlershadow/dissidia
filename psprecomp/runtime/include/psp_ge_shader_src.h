// The uber-shader GLSL (GL-free: shared by the OpenGL backend, psp_ge_shader.cpp, and the Vulkan backend, which converts
// it with pspvk::convert_glsl_to_vulkan). Body only: the "#version" line comes from the backend.
#pragma once

inline constexpr const char* const k_vertex_shader_src = R"GLSL(
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec2 a_texcoord;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec3 a_normal;
layout(location = 4) in vec4 a_weights0;
layout(location = 5) in vec4 a_weights1;

out vec2 v_texcoord;
out vec4 v_color;
#ifdef PSP_FX
out vec3 v_fx_opos;     // model space (clip-space xyz on the passthrough path)
out vec3 v_fx_wpos;     // world space (0 on the passthrough path)
out vec3 v_fx_wnormal;  // world-space normal (0 without vertex normals)
out vec2 v_fx_uv;       // raw vertex UV
#endif

// u_xform == 0: a_position is already the final clip-space position (CPU
// transformed: through mode, rectangles, degenerate-matrix fallback).
// u_xform == 1: a_position is model space; the GE transform pipeline runs
// here (bone skinning -> world -> lighting -> view -> proj -> tex xform).
uniform int u_xform;
uniform float u_psize;        // point size (Android: ES has no glPointSize); 0 = unset -> 1
// GE matrices are 4x3; they are uploaded padded to square mat4 (last row
// 0,0,0,1). Square mat4 uniforms and plain vec4 math are the best-trodden
// path on every driver (Apple's GLSL compiler has a history of non-square
// matrix bugs); the padding costs 4 floats per matrix.
uniform mat4 u_world;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform vec4 u_uvxf;          // scale u, scale v, offset u, offset v
uniform int u_skin;           // number of skinning weights used (0 = none)
uniform mat4 u_bones[8];
uniform int u_lit;            // GE lighting enabled
uniform int u_lflags;         // bit0 matAmbient<-vertex, bit1 matDiffuse<-vertex, bit2 has normal
uniform int u_nl;             // number of enabled lights (packed first)
// u_lc: 0 emissive.rgb, 1 material ambient rgb + alpha, 2 material diffuse rgb,
//       3 global ambient rgb + ambient alpha
uniform vec4 u_lc[4];
// u_lt per light: 5 x vec4: pos.xyz+kind, dir.xyz+spotExp, att.xyz+spotCut,
//                 ambient.rgb, diffuse.rgb
uniform vec4 u_lt[20];

vec4 ge_lighting(vec3 wpos, vec3 nrm, vec4 vcol) {
    bool upd_amb = (u_lflags & 1) != 0;
    bool upd_dif = (u_lflags & 2) != 0;
    bool have_n  = (u_lflags & 4) != 0;
    vec3 mamb = upd_amb ? vcol.rgb : u_lc[1].rgb;
    float malpha = upd_amb ? vcol.a : u_lc[1].a;
    vec3 mdif = upd_dif ? vcol.rgb : u_lc[2].rgb;
    vec3 outc = u_lc[0].rgb + u_lc[3].rgb * mamb;
    float alpha = u_lc[3].a * malpha;

    vec3 n = vec3(0.0);
    if (have_n) {
        n = mat3(u_world) * nrm;
        float len = length(n);
        if (len > 1e-8) n /= len;
    }
    for (int i = 0; i < u_nl; i++) {
        vec4 lp = u_lt[i * 5 + 0];
        vec4 ld = u_lt[i * 5 + 1];
        vec4 la = u_lt[i * 5 + 2];
        int kind = int(lp.w + 0.5);
        vec3 L;
        float scale = 1.0;
        if (kind == 0) {
            L = lp.xyz;
            float len = length(L);
            if (len > 1e-8) L /= len;
        } else {
            L = lp.xyz - wpos;
            float d = length(L);
            if (d > 1e-8) L /= d;
            float att = la.x + la.y * d + la.z * d * d;
            scale = att > 1e-8 ? min(1.0 / att, 1.0) : 1.0;
            if (kind == 2) {
                float dl = length(ld.xyz);
                float ang = 0.0;
                if (dl > 1e-8) ang = -dot(L, ld.xyz) / dl;
                if (ang >= la.w) {
                    float a = max(ang, 0.0);
                    scale *= (ld.w == 0.0) ? 1.0 : pow(a, ld.w);
                } else {
                    scale = 0.0;
                }
            }
        }
        float ndl = have_n ? max(dot(n, L), 0.0) : 0.0;
        outc += scale * (u_lt[i * 5 + 3].rgb * mamb + u_lt[i * 5 + 4].rgb * mdif * ndl);
    }
    // Quantise to 8 bits exactly like the CPU path did.
    outc = floor(clamp(outc, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
    alpha = floor(clamp(alpha, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
    return vec4(outc, alpha);
}

void main() {
    gl_PointSize = u_psize > 0.0 ? u_psize : 1.0;
    if (u_xform == 0) {
        gl_Position = a_position;
        v_texcoord = a_texcoord;
        v_color = a_color;
#ifdef PSP_FX
        v_fx_opos = a_position.xyz;
        v_fx_wpos = vec3(0.0);
        v_fx_wnormal = vec3(0.0);
        v_fx_uv = a_texcoord;
#endif
        return;
    }
    vec3 p = a_position.xyz;
    vec3 n = a_normal;
    if (u_skin > 0) {
        vec3 sp = vec3(0.0);
        vec3 sn = vec3(0.0);
        for (int k = 0; k < u_skin; k++) {
            float w = (k < 4) ? a_weights0[k] : a_weights1[k - 4];
            if (w == 0.0) continue;
            mat4 m = u_bones[k];
            sp += w * (m * vec4(p, 1.0)).xyz;
            sn += w * (mat3(m) * n);
        }
        p = sp;
        n = sn;
    }
    vec3 wpos = (u_world * vec4(p, 1.0)).xyz;
    vec4 col = a_color;
    if (u_lit != 0) col = ge_lighting(wpos, n, a_color);
    vec3 vpos = (u_view * vec4(wpos, 1.0)).xyz;
    gl_Position = u_proj * vec4(vpos, 1.0);
    v_texcoord = a_texcoord * u_uvxf.xy + u_uvxf.zw;
    v_color = col;
#ifdef PSP_FX
    v_fx_opos = p;
    v_fx_wpos = wpos;
    v_fx_wnormal = mat3(u_world) * n;
    v_fx_uv = a_texcoord;
#endif
}
)GLSL";

inline constexpr const char* const k_fragment_shader_src = R"GLSL(

in vec2 v_texcoord;
in vec4 v_color;

uniform bool u_texture_enable;
uniform sampler2D u_texture;
uniform int u_tex_func;
uniform bool u_tex_rgba;
uniform bool u_tex_double;
uniform vec3 u_tex_env;
uniform bool u_alpha_test_enable;
uniform int u_alpha_test_ref;    // 0..255
uniform int u_alpha_test_mask;   // 0..255
uniform int u_alpha_test_func;
uniform bool u_color_test_enable;
uniform int u_color_test_func;   // 0 NEVER 1 ALWAYS 2 EQUAL 3 NOTEQUAL
uniform ivec3 u_color_test_ref;  // 0..255 per channel
uniform ivec3 u_color_test_mask;
uniform bool u_alpha_double;
// Stencil-to-alpha: >= 0 replaces the fragment alpha (GE stencil REPLACE/ZERO).
uniform float u_alpha_override;   // 0 = none, else 1 + replacement alpha (default-safe)
// FBO-as-texture remap: (sx, sy, flip, 0). Identity = (1,1,0,0).
uniform vec4 u_tex_remap;

out vec4 frag_color;

void main() {
    vec4 color = v_color;

    if (u_texture_enable) {
        vec2 tc = v_texcoord * u_tex_remap.xy;
        if (u_tex_remap.z > 0.5) tc.y = 1.0 - tc.y;
        vec4 tex = texture(u_texture, tc);

        // Texture function (GE tex func), PPSSPP semantics: the RGBA
        // bit says whether texture alpha participates; colour doubling
        // scales the result (0x80 == 1.0 convention, used by Square Enix).
        if (u_tex_func == 0) {
            // MODULATE
            color.rgb = color.rgb * tex.rgb;
            if (u_tex_rgba) color.a *= tex.a;
        } else if (u_tex_func == 1) {
            // DECAL
            color.rgb = u_tex_rgba ? mix(color.rgb, tex.rgb, tex.a) : tex.rgb;
        } else if (u_tex_func == 2) {
            // BLEND with env colour
            color.rgb = mix(color.rgb, u_tex_env, tex.rgb);
            if (u_tex_rgba) color.a *= tex.a;
        } else if (u_tex_func == 3) {
            // REPLACE
            color.rgb = tex.rgb;
            if (u_tex_rgba) color.a = tex.a;
        } else if (u_tex_func == 4) {
            // ADD
            color.rgb = color.rgb + tex.rgb;
            if (u_tex_rgba) color.a *= tex.a;
        }
        if (u_tex_double) color.rgb = min(color.rgb * 2.0, vec3(1.0));
    }

    // Alpha test: (alpha & mask) <func> (ref & mask), integer 0..255.
    if (u_alpha_test_enable) {
        int a = int(clamp(color.a, 0.0, 1.0) * 255.0 + 0.5) & u_alpha_test_mask;
        int ref = u_alpha_test_ref & u_alpha_test_mask;
        bool pass = true;

        if (u_alpha_test_func == 0) {
            pass = false;                  // NEVER
        } else if (u_alpha_test_func == 1) {
            pass = true;                   // ALWAYS
        } else if (u_alpha_test_func == 2) {
            pass = (a == ref);             // EQUAL
        } else if (u_alpha_test_func == 3) {
            pass = (a != ref);             // NOTEQUAL
        } else if (u_alpha_test_func == 4) {
            pass = (a < ref);              // LESS
        } else if (u_alpha_test_func == 5) {
            pass = (a <= ref);             // LEQUAL
        } else if (u_alpha_test_func == 6) {
            pass = (a > ref);              // GREATER
        } else if (u_alpha_test_func == 7) {
            pass = (a >= ref);             // GEQUAL
        }

        if (!pass) discard;
    }

    // Colour test: (rgb & mask) <func> (ref & mask), compared as a whole
    // 24-bit colour; only NEVER/ALWAYS/EQUAL/NOTEQUAL exist.
    if (u_color_test_enable) {
        ivec3 c = ivec3(clamp(color.rgb, vec3(0.0), vec3(1.0)) * 255.0 + 0.5)
                  & u_color_test_mask;
        ivec3 r = u_color_test_ref & u_color_test_mask;
        bool eq = (c == r);
        bool pass = true;
        if (u_color_test_func == 0) pass = false;
        else if (u_color_test_func == 2) pass = eq;
        else if (u_color_test_func == 3) pass = !eq;
        if (!pass) discard;
    }

    // GE 2*src-alpha blend factors: GL_SRC_ALPHA with doubled, clamped alpha.
    if (u_alpha_double) color.a = min(color.a * 2.0, 1.0);
    if (u_alpha_override > 0.5) color.a = u_alpha_override - 1.0;

    frag_color = color;
}
)GLSL";
