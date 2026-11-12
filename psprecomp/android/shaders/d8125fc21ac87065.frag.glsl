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






layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec4 v_color;


layout(set = 1, binding = 0) uniform sampler2D u_texture;













// Stencil-to-alpha: >= 0 replaces the fragment alpha (GE stencil REPLACE/ZERO).

// FBO-as-texture remap: (sx, sy, flip, 0). Identity = (1,1,0,0).


layout(location = 0) out vec4 frag_color;

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
