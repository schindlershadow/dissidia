#version 460
#extension GL_EXT_ray_query : require
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    vec2 u_size;
    vec2 u_step;
    mat4 u_proj;
    mat4 u_inv;
    vec2 u_dr;
    float u_clearz;
    float u_sky;
    vec2 u_fade;
    float u_radius;
    int u_n_ao;
    int u_n_cs;
    int u_n_ssr;
    vec3 u_light;
    vec3 u_up;
    float u_cs_len;
    float u_cs_thick;
    float u_ssr_len;
    float u_ssr_amt;
    int u_use_rt;
    mat4 u_view_inv;
    vec3 u_light_w;
    float u_rt_len;
};





layout(location = 0) in vec2 v_uv;
layout(set = 1, binding = 0) uniform sampler2D u_depth;









float wd_at(vec2 uv) { return texture(u_depth, uv).r; }
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

layout(location = 0) out vec4 o_fx;
layout(location = 1) out vec4 o_refl;
layout(set = 1, binding = 1) uniform sampler2D u_color;











layout(set = 2, binding = 0) uniform accelerationStructureEXT u_tlas;






void main() {
    o_fx = vec4(1.0, 0.0, 0.0, 1.0);
    o_refl = vec4(0.0);
    float wd = wd_at(v_uv);
    if (is_far(wd)) return;
    vec3 P = vpos(v_uv, wd);
    float lp = length(P);
    float fd = dist_fade(lp);
    if (fd <= 0.0) return;
    vec3 n = normal_at(v_uv, P);
    float noise = ign(gl_FragCoord.xy);
    float ao = 1.0;
    if (u_n_ao > 0) {
        float rad = u_radius * lp;
        vec3 up = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 tx = normalize(cross(up, n)), ty = cross(n, tx);
        float occ = 0.0;
        for (int i = 0; i < u_n_ao; ++i) {
            float t = (float(i) + 0.5) / float(u_n_ao);
            float a = noise * 6.2831853 + float(i) * 2.39996323;
            float c = mix(0.12, 1.0, fract(float(i) * 0.618034 + noise));
            float s = sqrt(1.0 - c * c);
            vec3 dir = tx * (cos(a) * s) + ty * (sin(a) * s) + n * c;
            vec3 S = P + dir * (rad * mix(0.15, 1.0, t * t));
            vec4 cl = u_proj * vec4(S, 1.0);
            if (cl.w <= 1e-5) continue;
            vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
            if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) continue;
            float wd2 = wd_at(uv2);
            if (is_far(wd2)) continue;
            float dq = length(vpos(uv2, wd2)), ds = length(S);
            if (ds - dq > 0.02 * rad) occ += smoothstep(0.0, 1.0, rad / max(abs(lp - dq), 1e-4));
        }
        ao = mix(1.0, 1.0 - occ / float(u_n_ao), fd);
    }
    float sh = 0.0;
    if (u_n_cs > 0) {
        float facing = smoothstep(0.15, 0.5, dot(n, u_light));   // surfaces turned away from the light are dark already
        if (facing > 0.0) {
            float jit = ign(gl_FragCoord.xy + 17.0);
            vec3 O = P + n * (0.01 + 0.004 * lp);

            if (u_use_rt != 0) {
                vec3 Ow = (u_view_inv * vec4(O, 1.0)).xyz;
                rayQueryEXT rq;
                rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF, Ow, 0.0, u_light_w, u_rt_len);
                while (rayQueryProceedEXT(rq)) {}
                if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) sh = facing * fd;
            } else

            for (int j = 0; j < u_n_cs; ++j) {
                float t = (float(j) + jit) / float(u_n_cs);
                vec3 S = O + u_light * (t * t * u_cs_len);      // denser near the surface
                vec4 cl = u_proj * vec4(S, 1.0);
                if (cl.w <= 1e-5) break;
                vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
                if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) break;
                float wd2 = wd_at(uv2);
                if (is_far(wd2)) continue;
                float diff = length(S) - length(vpos(uv2, wd2));
                if (diff > 0.01 + 0.004 * lp && diff < u_cs_thick) { sh = facing * fd; break; }
            }
        }
    }
    o_fx = vec4(ao, sh, 0.0, 1.0);

    if (u_n_ssr > 0 && dot(n, u_up) > 0.7) {
        float upness = dot(n, u_up);
        vec3 V = normalize(P);
        vec3 R = reflect(V, n);
        float maxd = u_ssr_len * lp;
        float stepl = maxd / float(u_n_ssr);
        float jit = ign(gl_FragCoord.xy);
        vec3 O = P + n * (0.004 * lp);
        vec2 hit = vec2(-1.0);
        float hit_t = 0.0, prev_t = 0.0;
        vec2 last_in = vec2(-1.0);
        for (int i = 0; i < u_n_ssr; ++i) {
            float t = (float(i) + jit) * stepl;
            vec3 S = O + R * t;
            vec4 cl = u_proj * vec4(S, 1.0);
            if (cl.w <= 1e-5) break;
            vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
            if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) break;
            last_in = uv2;
            float wd2 = wd_at(uv2);
            if (!is_far(wd2)) {
                float diff = length(S) - length(vpos(uv2, wd2));
                if (diff > 0.0 && diff < stepl * 2.5 + 0.01 * lp) {
                    float lo = prev_t, hi = t;
                    for (int k = 0; k < 4; ++k) {           // refine between the previous and this step
                        float mid = 0.5 * (lo + hi);
                        vec3 Sm = O + R * mid;
                        vec4 cm = u_proj * vec4(Sm, 1.0);
                        vec2 um = cm.xy / cm.w * 0.5 + 0.5;
                        float wm = wd_at(um);
                        if (!is_far(wm) && length(Sm) - length(vpos(um, wm)) > 0.0) hi = mid; else lo = mid;
                    }
                    vec3 Sh = O + R * hi;
                    vec4 ch = u_proj * vec4(Sh, 1.0);
                    hit = ch.xy / ch.w * 0.5 + 0.5;
                    hit_t = hi;
                    break;
                }
            }
            prev_t = t;
        }
        if (hit.x < 0.0 && last_in.x >= 0.0 && is_far(wd_at(last_in))) { hit = last_in; hit_t = maxd; }   // the ray left the scene into the sky
        if (hit.x >= 0.0) {
            vec2 e = min(hit, 1.0 - hit);
            float edge = smoothstep(0.0, 0.12, min(e.x, e.y));
            float fres = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, -V), 0.0, 1.0), 5.0);
            float k = clamp(fres * 1.6 + 0.08, 0.0, 1.0) * edge * (1.0 - hit_t / maxd * 0.6) * smoothstep(0.7, 0.95, upness) * fd * u_ssr_amt;
            o_refl = vec4(texture(u_color, hit).rgb * k, k);
        }
    }
}
