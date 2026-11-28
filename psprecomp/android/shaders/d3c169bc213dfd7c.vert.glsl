#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    float u_sat;
    float u_bright;
    float u_contrast;
    int u_enc;
    float u_peak;
    float u_paper;
    float u_scrgb_nits;
    float u_boost;
    float u_gamut;
    vec2 u_texel;
    int u_hscene;
};





layout(location = 0) out vec2 v_uv;
void ge_gl_main() {
    // One oversized triangle covering the viewport.
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

void main() {
    ge_gl_main();
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
