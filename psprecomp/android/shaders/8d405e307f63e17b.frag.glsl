#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    float u_alpha;
};




layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 1, binding = 0) uniform sampler2D u_tex;

void main() { vec4 c = texture(u_tex, v_uv); o_col = vec4(c.rgb, c.a * u_alpha); }
