#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    vec2 u_res;
};





layout(location = 0) in vec4 v_col;
layout(location = 0) out vec4 o_col;
void main() { o_col = v_col; }
