#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    vec2 u_res;
};





layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_col;

layout(location = 0) out vec4 v_col;
void ge_gl_main() {
    v_col = a_col;
    gl_Position = vec4(a_pos.x / u_res.x * 2.0 - 1.0, 1.0 - a_pos.y / u_res.y * 2.0, 0.0, 1.0);
}

void main() {
    ge_gl_main();
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
