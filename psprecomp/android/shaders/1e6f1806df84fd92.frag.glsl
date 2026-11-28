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
    int u_snap_split;
    vec2 u_half;
    float u_ao_amt;
    float u_cs_amt;
    float u_dark;
};





layout(location = 0) in vec2 v_uv;
layout(set = 1, binding = 0) uniform sampler2D u_depth;










// The depth at uv, from one definite full-resolution texel. The passes run at half resolution, where a pixel's centre lies exactly on
// the border between two depth texels: nearest sampling then picks either one depending on the GPU's rounding (Adreno / Turnip did
// not do it consistently), so the normals rebuilt from neighbouring depths jumped in bands along pixel columns and depth contours,
// and AO and the ray-traced shadow's facing term drew evenly spaced lines on walls (RP6, render scale 2). The quarter-texel pull-back
// makes every read land inside a texel.
// The position is rebuilt at the same texel centre the depth came from (vpos): a depth from one place and a screen position from another
// put a point off its surface by up to half a texel's depth change, which is large on a floor seen at a low angle (lines across floors).

vec2 snap_uv(vec2 uv) { return u_depth_snap == 0 ? uv : (floor(uv * u_size - 0.25) + 0.5) / u_size; }
float wd_at(vec2 uv) { return texture(u_depth, snap_uv(uv)).r; }
float depth_t(float wd) { return (wd - u_dr.x) / (u_dr.y - u_dr.x); }   // 0 at the near plane, 1 at the far plane
bool is_far(float wd) { return abs(wd - u_clearz) < 3.0 / 65535.0 || depth_t(wd) > u_sky; }
vec3 vpos(vec2 uv, float wd) {
    float z = depth_t(wd) * 2.0 - 1.0;
    vec2 ps = (u_snap_split != 0 && v_uv.x < 0.5) ? uv : snap_uv(uv);   // debug split: the left half rebuilds at the unsnapped point
    vec4 p = u_inv * vec4(ps * 2.0 - 1.0, z, 1.0);
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
layout(set = 1, binding = 1) uniform sampler2D u_fx;




void main() {
    float wd = wd_at(v_uv);
    if (is_far(wd)) { o_color = vec4(1.0); return; }
    float z0 = vz(wd);
    vec2 hp = v_uv * u_half - 0.5;
    vec2 base = floor(hp), f = hp - base;
    vec2 sum = vec2(0.0);
    float wsum = 0.0;
    for (int k = 0; k < 4; ++k) {
        vec2 o = vec2(float(k & 1), float(k >> 1));
        vec2 uvc = (base + o + 0.5) / u_half;
        float bw = (o.x > 0.5 ? f.x : 1.0 - f.x) * (o.y > 0.5 ? f.y : 1.0 - f.y);
        float w2 = wd_at(uvc);
        float dl = is_far(w2) ? 1.0 : abs(vz(w2) - z0) / abs(z0);
        float w = (bw + 0.001) * exp(-dl * 60.0) + 1e-5;
        sum += texture(u_fx, uvc).rg * w;
        wsum += w;
    }
    vec2 fx = sum / wsum;
    o_color = vec4(vec3(mix(1.0, fx.r, u_ao_amt) * (1.0 - fx.g * u_cs_amt) * u_dark), 1.0);
}
