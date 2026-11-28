#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    vec2 u_size;
    vec2 u_step;
    mat4 u_proj;
    mat4 u_inv;
    vec2 u_dr;
    float u_clearz;
    float u_sky;
    vec2 u_fade;
    int u_depth_snap;
    vec2 u_dir;
    float u_rad;
};





layout(location = 0) in vec2 v_uv;
layout(set = 1, binding = 0) uniform sampler2D u_depth;










// The depth at uv, from one definite full-resolution texel. The passes run at half resolution, where a pixel's centre lies exactly on
// the border between two depth texels: nearest sampling then picks either one depending on the GPU's rounding (Adreno / Turnip did
// not do it consistently), so the normals rebuilt from neighbouring depths jumped in bands along pixel columns and depth contours,
// and AO and the ray-traced shadow's facing term drew evenly spaced lines on walls (RP6, render scale 2). The quarter-texel pull-back
// makes every read land inside a texel.
float wd_at(vec2 uv) {
    if (u_depth_snap == 0) return texture(u_depth, uv).r;
    return texture(u_depth, (floor(uv * u_size - 0.25) + 0.5) / u_size).r;
}
float depth_t(float wd) { return (wd - u_dr.x) / (u_dr.y - u_dr.x); }   // 0 at the near plane, 1 at the far plane
bool is_far(float wd) { return abs(wd - u_clearz) < 3.0 / 65535.0 || depth_t(wd) > u_sky; }
vec3 vpos(vec2 uv, float wd) {
    float z = depth_t(wd) * 2.0 - 1.0;
    vec4 p = u_inv * vec4(uv * 2.0 - 1.0, z, 1.0);
    return p.xyz / p.w;
}
vec3 vpos(vec2 uv) { return vpos(uv, wd_at(uv)); }
// View-space z straight from the projection (no matrix product): used where only the depth along the view axis matters.
float vz(float wd) {
    float ndc = depth_t(wd) * 2.0 - 1.0;
    return (u_proj[3][2] - ndc * u_proj[3][3]) / (ndc * u_proj[2][3] - u_proj[2][2]);
}
float dist_fade(float lp) { return 1.0 - smoothstep(u_fade.x, u_fade.y, lp); }
// Surface normal from the depth neighbours (the side with the smaller jump on each axis), facing the camera.
vec3 normal_at(vec2 uv, vec3 P) {
    vec2 px = u_step;
    vec3 a = vpos(uv + vec2(px.x, 0.0)), b = vpos(uv - vec2(px.x, 0.0));
    vec3 c = vpos(uv + vec2(0.0, px.y)), d = vpos(uv - vec2(0.0, px.y));
    vec3 dx = abs(length(a) - length(P)) < abs(length(b) - length(P)) ? a - P : P - b;
    vec3 dy = abs(length(c) - length(P)) < abs(length(d) - length(P)) ? c - P : P - d;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, P) > 0.0) n = -n;      // the camera is at the origin
    return n;
}
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

layout(location = 0) out vec4 o_color;
layout(set = 1, binding = 1) uniform sampler2D u_src;
layout(set = 1, binding = 2) uniform sampler2D u_color;


float rough_at(vec2 uv) { float b = floor(texture(u_color, uv).a * 255.0 + 0.5); return b < 32.0 ? -1.0 : mod(floor(b / 4.0), 8.0) / 7.0; }
void main() {
    vec4 c0 = texture(u_src, v_uv);
    float rough = rough_at(v_uv);
    float wd = wd_at(v_uv);
    if (rough <= 0.0 || is_far(wd)) { o_color = c0; return; }
    float z0 = vz(wd);
    float r = rough * u_rad;
    vec4 sum = c0;
    float ws = 1.0;
    for (int i = 1; i <= 6; ++i) {
        float t = float(i) / 6.0;
        for (int sd = -1; sd <= 1; sd += 2) {
            vec2 uv = v_uv + u_dir * (float(sd) * t * r);
            float w2 = wd_at(uv);
            if (is_far(w2) || rough_at(uv) < 0.0) continue;
            float dl = abs(vz(w2) - z0) / abs(z0);
            float w = exp(-t * t * 2.5) * exp(-dl * 40.0);
            sum += texture(u_src, uv) * w;
            ws += w;
        }
    }
    o_color = sum / ws;
}
