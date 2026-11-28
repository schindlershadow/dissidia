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
    int u_have_fx;
    int u_fx_dbg;
    float u_fx_gain;
    float u_fx_thick;
    float u_lscale;
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











layout(set = 1, binding = 2) uniform sampler2D u_fxc;
layout(set = 1, binding = 3) uniform sampler2D u_fxd;




//@RT_DECL
// The effect at uv floats in front of the scene there: its depth is clearly nearer than the scene's. A blended sheet lying on a
// surface (the water layer of the floor, decals, shadows) is at the scene's own depth and is not an effect to reflect.
bool fx_floats(vec2 uv, float wd_scene, float efd) {
    return length(vpos(uv, wd_scene)) - length(vpos(uv, efd)) > 0.2 + 0.01 * length(vpos(uv, wd_scene));
}

void main() {
    o_fx = vec4(1.0, 0.0, 0.0, 1.0);
    o_refl = vec4(0.0);
    float wd = wd_at(v_uv);
    if (u_fx_dbg == 3 && u_have_fx != 0) {   // debug: where the effects layer holds anything (sky included), no floor test
        vec4 fc = texture(u_fxc, v_uv);
        if (fc.a > 0.03 || max(fc.r, max(fc.g, fc.b)) > 0.06) { o_refl = vec4(fc.rgb * 0.5 + vec3(0.5, 0.0, 0.5), 1.0); return; }
    }
    vec3 mk = vec3(0.0);
//@RT_MARK
    if (is_far(wd)) { o_refl.rgb = mk * u_lscale; return; }
    vec3 P = vpos(v_uv, wd);
    float lp = length(P);
    float fd = dist_fade(lp);
    if (fd <= 0.0) { o_refl.rgb = mk * u_lscale; return; }
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
//@RT_CODE
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
    if (u_fx_dbg == 2 && u_have_fx != 0) {   // debug: the effects layer itself, wherever the layer holds something
        vec4 fc = texture(u_fxc, v_uv);
        if ((fc.a > 0.03 || max(fc.r, max(fc.g, fc.b)) > 0.06) && fx_floats(v_uv, wd, texture(u_fxd, v_uv).r)) { o_refl = vec4(fc.rgb + vec3(0.4, 0.0, 0.4), 1.0); return; }
    }

    if (u_n_ssr > 0 && dot(n, u_up) > 0.7) {
//@RT_REFL
        {
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
        bool fx_hit = false;
        float fx_t = 0.0;
        vec4 fx_c = vec4(0.0);
        vec2 fx_uv = vec2(0.5);
        for (int i = 0; i < u_n_ssr; ++i) {
            float t = (float(i) + jit) * stepl;
            vec3 S = O + R * t;
            vec4 cl = u_proj * vec4(S, 1.0);
            if (cl.w <= 1e-5) break;
            vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
            if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) break;
            last_in = uv2;
            float wd2 = wd_at(uv2);
            if (u_have_fx != 0 && !fx_hit) {
                // effects layer: orbs, beams, auras, glass. They write no scene depth, so the ray is tested against the nearest effect
                // fragment's own depth; an effect later covered by opaque geometry (its depth behind the scene's) is not there.
                vec4 fc = texture(u_fxc, uv2);
                if (fc.a > 0.03 || max(fc.r, max(fc.g, fc.b)) > 0.06) {
                    float efd = texture(u_fxd, uv2).r;
                    if ((is_far(wd2) || fx_floats(uv2, wd2, efd))) {
                        float de = length(vpos(uv2, efd)), ds = length(S);
                        if (ds > de - 0.02 * lp && ds < de + stepl * 2.5 + u_fx_thick) { fx_hit = true; fx_t = t; fx_c = fc; fx_uv = uv2; }
                    }
                }
            }
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
        if (hit.x < 0.0 && last_in.x >= 0.0 && is_far(wd_at(last_in))) {   // the ray left the scene into the sky
            hit = last_in; hit_t = maxd;
            // The sky (and the beams / clouds painted on it) lies in the ray's direction, at infinity: take the picture where that
            // direction projects, not where the short march happened to end (that showed the sky straight above the floor point).
            vec4 cs = u_proj * vec4(R, 0.0);
            if (cs.w > 1e-5) {
                vec2 us = cs.xy / cs.w * 0.5 + 0.5;
                if (us.x > 0.0 && us.x < 1.0 && us.y > 0.0 && us.y < 1.0 && is_far(wd_at(us))) hit = us;
            }
        }
        if (hit.x >= 0.0 || fx_hit) {
            vec3 col = vec3(0.0);
            vec2 euv = fx_uv;
            float ht = hit_t;
            if (hit.x >= 0.0) { col = texture(u_color, hit).rgb; euv = hit; }
            vec2 e = min(euv, 1.0 - euv);
            float edge = smoothstep(0.0, 0.12, min(e.x, e.y));
            float fres = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, -V), 0.0, 1.0), 5.0);
            float refl_k = smoothstep(0.7, 0.95, upness) * fd * u_ssr_amt;
            float k = hit.x >= 0.0 ? clamp(fres * 1.6 + 0.08, 0.0, 1.0) * edge * (1.0 - ht / maxd * 0.6) * refl_k : 0.0;
            o_refl = vec4(col * k, k);
            if (fx_hit) {
                // Effects are glowing / see-through things that read as bright reflections even at a steep angle on a wet floor, so they
                // get a stronger base than the Fresnel-only opaque reflection (an orb or beam at 14% was invisible).
                float efe = min(min(fx_uv.x, 1.0 - fx_uv.x), min(fx_uv.y, 1.0 - fx_uv.y));
                float kf = clamp(fres * 1.6 + u_fx_gain, 0.0, 1.0) * smoothstep(0.0, 0.12, efe) * (1.0 - fx_t / maxd * 0.5) * refl_k;
                float cov = clamp(max(fx_c.a, max(fx_c.r, max(fx_c.g, fx_c.b))), 0.0, 1.0);
                float af = cov * kf;
                o_refl = vec4(fx_c.rgb * kf + o_refl.rgb * (1.0 - fx_c.a * kf), af + o_refl.a * (1.0 - af));
            }
            if (u_fx_dbg == 1 && fx_hit) o_refl = vec4(0.8, 0.0, 0.8, 0.8);
        }
        }
    }
//@RT_LIGHTS
}
