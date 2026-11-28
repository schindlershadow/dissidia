#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    float psp_unused_;
};

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 1, binding = 0) uniform sampler2D u_tex;
void main() { o_col = texture(u_tex, v_uv); }
