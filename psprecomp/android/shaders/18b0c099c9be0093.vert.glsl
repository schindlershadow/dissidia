#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    int u_xform;
    float u_psize;
    mat4 u_world;
    mat4 u_view;
    mat4 u_proj;
    vec4 u_uvxf;
    int u_skin;
    mat4 u_bones[8];
    int u_lit;
    int u_lflags;
    int u_nl;
    vec4 u_lc[4];
    vec4 u_lt[20];
    int u_texture_enable__i;
    int u_tex_func;
    int u_tex_rgba__i;
    int u_tex_double__i;
    vec3 u_tex_env;
    int u_alpha_test_enable__i;
    int u_alpha_test_ref;
    int u_alpha_test_mask;
    int u_alpha_test_func;
    int u_color_test_enable__i;
    int u_color_test_func;
    ivec3 u_color_test_ref;
    ivec3 u_color_test_mask;
    int u_alpha_double__i;
    float u_alpha_override;
    vec4 u_tex_remap;
};
#define u_texture_enable (u_texture_enable__i != 0)
#define u_tex_rgba (u_tex_rgba__i != 0)
#define u_tex_double (u_tex_double__i != 0)
#define u_alpha_test_enable (u_alpha_test_enable__i != 0)
#define u_color_test_enable (u_color_test_enable__i != 0)
#define u_alpha_double (u_alpha_double__i != 0)





layout(location = 0) in vec4 a_position;
layout(location = 1) in vec2 a_texcoord;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec3 a_normal;
layout(location = 4) in vec4 a_weights0;
layout(location = 5) in vec4 a_weights1;

layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec4 v_color;
#ifdef PSP_FX
layout(location = 2) out vec3 v_fx_opos;     // model space (clip-space xyz on the passthrough path)
layout(location = 3) out vec3 v_fx_wpos;     // world space (0 on the passthrough path)
layout(location = 4) out vec3 v_fx_wnormal;  // world-space normal (0 without vertex normals)
layout(location = 5) out vec2 v_fx_uv;       // raw vertex UV
#endif

// u_xform == 0: a_position is already the final clip-space position (CPU
// transformed: through mode, rectangles, degenerate-matrix fallback).
// u_xform == 1: a_position is model space; the GE transform pipeline runs
// here (bone skinning -> world -> lighting -> view -> proj -> tex xform).


// GE matrices are 4x3; they are uploaded padded to square mat4 (last row
// 0,0,0,1). Square mat4 uniforms and plain vec4 math are the best-trodden
// path on every driver (Apple's GLSL compiler has a history of non-square
// matrix bugs); the padding costs 4 floats per matrix.









// u_lc: 0 emissive.rgb, 1 material ambient rgb + alpha, 2 material diffuse rgb,
//       3 global ambient rgb + ambient alpha

// u_lt per light: 5 x vec4: pos.xyz+kind, dir.xyz+spotExp, att.xyz+spotCut,
//                 ambient.rgb, diffuse.rgb


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

void ge_gl_main() {
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

void main() {
    ge_gl_main();
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
