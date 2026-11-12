#version 460
#extension GL_EXT_ray_query : require
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    vec2 u_size;
    vec2 u_step;
    mat4 u_proj;
    mat4 u_inv;
    vec2 u_dr;
    float u_clearz;
    float u_sky;
    vec2 u_fade;
    float u_radius;
    int u_n_ao;
    int u_n_cs;
    int u_n_ssr;
    vec3 u_light;
    vec3 u_up;
    float u_cs_len;
    float u_cs_thick;
    float u_ssr_len;
    float u_ssr_amt;
    int u_use_rt;
    mat4 u_view_inv;
    vec3 u_light_w;
    float u_rt_len;
    int u_use_rt_refl;
    int u_rt_dbg;
    int u_rt_fx;
    float u_rt_hidden;
    float u_rt_sky_amt;
    float u_rt_sky_dist;
    int u_refl_split;
    float u_rt_refl_len;
    vec3 u_rt_sky;
};





layout(location = 0) out vec2 v_uv;
void ge_gl_main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

void main() {
    ge_gl_main();
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
