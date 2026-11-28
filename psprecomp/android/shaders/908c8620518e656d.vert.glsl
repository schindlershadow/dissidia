#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    float u_byte;
};





layout(location = 0) in vec4 a_clip;
layout(location = 1) in float a_mat;
layout(location = 0) out float v_mat;
void ge_gl_main() {
    v_mat = a_mat;
    gl_Position = a_clip;
}

void main() {
    ge_gl_main();
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
