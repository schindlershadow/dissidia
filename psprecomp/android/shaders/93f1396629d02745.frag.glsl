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
    mat4 u_view;
    float u_w;
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

layout(location = 0) out vec4 o_env;
layout(set = 1, binding = 1) uniform sampler2D u_color;


void main() {
    float az = (v_uv.x - 0.5) * 6.2831853;
    float el = (v_uv.y - 0.5) * 3.14159265;
    vec3 d = vec3(cos(el) * cos(az), sin(el), cos(el) * sin(az));
    vec3 dv = (u_view * vec4(d, 0.0)).xyz;
    if (dv.z > -0.02) discard;
    vec4 c = u_proj * vec4(dv * 1000.0, 1.0);
    if (c.w <= 1e-5) discard;
    vec2 uv = c.xy / c.w * 0.5 + 0.5;
    if (uv.x < 0.01 || uv.x > 0.99 || uv.y < 0.01 || uv.y > 0.99) discard;
    vec2 o = 3.0 / u_size;      // the colour copy is half resolution: keep clear of geometry so its edge does not bleed in
    if (!is_far(wd_at(uv)) || !is_far(wd_at(uv + vec2(o.x, 0.0))) || !is_far(wd_at(uv - vec2(o.x, 0.0)))
        || !is_far(wd_at(uv + vec2(0.0, o.y))) || !is_far(wd_at(uv - vec2(0.0, o.y)))) discard;
    o_env = vec4(texture(u_color, uv).rgb * u_w, u_w);
}
