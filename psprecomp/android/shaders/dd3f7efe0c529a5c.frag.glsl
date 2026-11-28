#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    float u_byte;
};





layout(location = 0) in float v_mat;

layout(location = 0) out vec4 o_color;
void main() {
    o_color = vec4(0.0, 0.0, 0.0, v_mat * u_byte);
}
