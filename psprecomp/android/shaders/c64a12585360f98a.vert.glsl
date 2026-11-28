#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    float psp_unused_;
};




layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;
void ge_gl_main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }

void main() {
    ge_gl_main();
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
