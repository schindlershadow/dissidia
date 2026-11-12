#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    vec2 u_size;
    float u_scale;
    int u_mode;
    int u_aa;
    float u_sharp;
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
